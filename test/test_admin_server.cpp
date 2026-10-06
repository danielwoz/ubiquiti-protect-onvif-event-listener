// Copyright 2026 Daniel W
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * test_admin_server.cpp
 *
 * Boots AdminServer on a loopback ephemeral port and exercises the two GET
 * endpoints: /api/status (JSON) and / (HTML).  POST endpoints are not covered
 * because they shell out to systemctl / apt-get which are not available in
 * the test environment; handler dispatch is exercised via the GET paths.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "admin_server.hpp"

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* label) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << label << '\n';
  }
}

static std::string temp_dir() {
  const char* d = std::getenv("TEST_TMPDIR");
  return d ? d : "/tmp";
}

// Open a TCP socket to 127.0.0.1:port, send the raw request, read the full
// response until the peer closes.  Returns the response or empty on failure.
static std::string http_exchange(uint16_t port, const std::string& request) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return "";

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)) != 0) {
    ::close(fd);
    return "";
  }

  const char* p = request.data();
  size_t remaining = request.size();
  while (remaining > 0) {
    ssize_t w = ::send(fd, p, remaining, 0);
    if (w <= 0) {
      ::close(fd);
      return "";
    }
    p += w;
    remaining -= static_cast<size_t>(w);
  }

  std::string resp;
  char buf[4096];
  while (true) {
    ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
    if (r <= 0) break;
    resp.append(buf, static_cast<size_t>(r));
  }
  ::close(fd);
  return resp;
}

int main() {
  // Set up a channel file so /api/status has something to read.
  const std::string channel_path = temp_dir() + "/admin_server_test_channel";
  {
    std::ofstream f(channel_path);
    f << "rc\n";
  }

  onvif::AdminServer server;
  const bool started = server.start("9.9.9-test", channel_path, 0);
  check(started, "server starts on ephemeral port");
  if (!started) {
    std::cerr << "PASS " << g_pass << " / FAIL " << g_fail << '\n';
    return 1;
  }

  const uint16_t port = server.port();
  check(port != 0, "port() returns the OS-assigned port");

  // GET / should return the embedded HTML admin page.
  {
    const std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n\r\n";
    const std::string resp = http_exchange(port, req);
    check(resp.find("HTTP/1.1 200") != std::string::npos,
          "GET / returns 200");
    check(resp.find("text/html") != std::string::npos,
          "GET / advertises text/html");
    check(resp.find("<title>onvif-recorder admin</title>") != std::string::npos,
          "GET / body contains admin title");
  }

  // GET /api/status should return JSON with the version and channel.
  {
    const std::string req =
        "GET /api/status HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n\r\n";
    const std::string resp = http_exchange(port, req);
    check(resp.find("HTTP/1.1 200") != std::string::npos,
          "GET /api/status returns 200");
    check(resp.find("application/json") != std::string::npos,
          "GET /api/status advertises application/json");
    // Either the dpkg-query-derived value or the compile-time fallback.
    // In the test env dpkg-query misses so the fallback is our version_.
    check(resp.find("\"version\":") != std::string::npos,
          "status body has version key");
    // The channel file we wrote says "rc" — dpkg-query fallback path writes
    // the fallback string ctx.version ("9.9.9-test") so the channel comes
    // from the file we control.
    check(resp.find("\"channel\":\"rc\"") != std::string::npos,
          "status body reports channel=rc from our file");
  }

  // Unknown path should 404.
  {
    const std::string req =
        "GET /nope HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n\r\n";
    const std::string resp = http_exchange(port, req);
    check(resp.find("HTTP/1.1 404") != std::string::npos,
          "unknown GET path returns 404");
  }

  // GET /api/config returns the schema + entries array.
  {
    const std::string req =
        "GET /api/config HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n\r\n";
    const std::string resp = http_exchange(port, req);
    check(resp.find("HTTP/1.1 200") != std::string::npos,
          "GET /api/config returns 200");
    check(resp.find("\"entries\":[") != std::string::npos,
          "config response contains entries[]");
    // verbose, detect_override, msr_url all present in the schema (added
    // across v1.4.7 and v1.4.8).
    check(resp.find("\"name\":\"verbose\"") != std::string::npos,
          "schema contains verbose");
    check(resp.find("\"name\":\"detect_override\"") != std::string::npos,
          "schema contains detect_override");
    check(resp.find("\"name\":\"msr_url\"") != std::string::npos,
          "schema contains msr_url");
    check(resp.find("\"name\":\"backfill_apply\"") != std::string::npos,
          "schema contains backfill_apply");
  }

  // GET /api/camera_health: with no DB context the handler returns an
  // empty cameras array but still 200 (degraded, not error).
  {
    const std::string req =
        "GET /api/camera_health HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n\r\n";
    const std::string resp = http_exchange(port, req);
    check(resp.find("HTTP/1.1 200") != std::string::npos,
          "GET /api/camera_health returns 200 even without DB");
    check(resp.find("\"cameras\":[]") != std::string::npos,
          "camera_health returns empty array when DB absent");
    check(resp.find("\"now_ms\":") != std::string::npos,
          "camera_health includes now_ms timestamp");
  }

  server.stop();
  ::unlink(channel_path.c_str());

  // The thumbnail hook URL's query string (often a token) is redacted in
  // the diagnostic dump's config.json; other keys are untouched.
  {
    const std::string cfg =
        "{\"detect\":\"true\",\"thumbnail_hook_url\":"
        "\"https://hook.example/t?token=s3cret&x=1\",\"msr_url\":\"a?b\"}";
    const std::string out =
        onvif::redact_url_queries(cfg, {"thumbnail_hook_url"});
    check(out.find("s3cret") == std::string::npos,
          "redact_url_queries removes the hook token");
    check(out.find("\"https://hook.example/t?[REDACTED]\"") !=
              std::string::npos,
          "redact_url_queries keeps scheme, host and path");
    check(out.find("\"msr_url\":\"a?b\"") != std::string::npos,
          "redact_url_queries leaves other keys alone");
    check(onvif::redact_url_queries("{\"thumbnail_hook_url\":\"\"}",
                                    {"thumbnail_hook_url"}) ==
              "{\"thumbnail_hook_url\":\"\"}",
          "redact_url_queries handles an empty URL");
  }

  std::cerr << "PASS " << g_pass << " / FAIL " << g_fail << '\n';
  return g_fail == 0 ? 0 : 1;
}
