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
#include <vector>

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

static std::string http_get(uint16_t port, const std::string& path) {
  return http_exchange(port,
                       "GET " + path + " HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Connection: close\r\n\r\n");
}

// The body of a raw HTTP response (everything after the blank line).
static std::string http_body(const std::string& resp) {
  const size_t p = resp.find("\r\n\r\n");
  return p == std::string::npos ? std::string() : resp.substr(p + 4);
}

static bool write_text(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f.is_open()) return false;
  f << content;
  return f.good();
}

// The JSON object of the camera with id @p id in a camera_health body.
static std::string camera_object(const std::string& json,
                                 const std::string& id) {
  const size_t p = json.find("{\"id\":\"" + id + "\"");
  if (p == std::string::npos) return {};
  const size_t e = json.find('}', p);
  return e == std::string::npos ? std::string() : json.substr(p, e - p + 1);
}

static unifi::CameraHealth health_row(const std::string& id,
                                      const std::string& host,
                                      const std::string& mac,
                                      bool third_party) {
  unifi::CameraHealth r;
  r.id = id;
  r.name = "cam-" + id;
  r.host = host;
  r.mac = mac;
  r.is_third_party = third_party;
  return r;
}

// camera_health_json: the Share with Protect state comes from the saved
// protect_event_proxy_cameras, matched on normalised MACs, and is only
// reported for third-party cameras.
static void test_camera_health_event_proxy() {
  const std::string cfg = temp_dir() + "/admin_server_test_event_proxy.json";
  check(write_text(cfg,
                   "{\"protect_event_proxy_cameras\":"
                   "\"f0-00-00-fd-ae-d9, 112233445566\","
                   "\"excluded_cameras\":\"aabbccddeeff\"}"),
        "write event proxy config");
  const std::vector<unifi::CameraHealth> rows = {
      health_row("ptz", "192.168.1.109", "F0:00:00:FD:AE:D9", true),
      health_row("door", "192.168.1.108", "AABBCCDDEEFF", true),
      health_row("g4", "192.168.1.20", "112233445566", false),
      health_row("nomac", "192.168.1.50", "", true),
  };
  const std::string j = onvif::camera_health_json(rows, cfg, 42);
  check(j.find("{\"now_ms\":42,\"cameras\":[") == 0,
        "camera_health_json starts with now_ms and cameras");

  const std::string ptz = camera_object(j, "ptz");
  check(ptz.find("\"event_proxy\":true") != std::string::npos,
        "listed third-party camera is shared (MAC normalised)");
  const std::string door = camera_object(j, "door");
  check(door.find("\"event_proxy\":false") != std::string::npos,
        "unlisted third-party camera is not shared");
  check(door.find("\"excluded\":true") != std::string::npos,
        "excluded_cameras still reported next to event_proxy");
  const std::string g4 = camera_object(j, "g4");
  check(!g4.empty() && g4.find("event_proxy") == std::string::npos,
        "first-party camera has no event_proxy key even when listed");
  const std::string nomac = camera_object(j, "nomac");
  check(nomac.find("\"event_proxy\":false") != std::string::npos,
        "third-party camera without a MAC is never shared");

  const std::string missing = onvif::camera_health_json(
      rows, temp_dir() + "/admin_server_test_no_such_config.json", 1);
  check(camera_object(missing, "ptz").find("\"event_proxy\":false") !=
            std::string::npos,
        "missing config file shares nothing");
  check(onvif::camera_health_json({}, cfg, 7) == "{\"now_ms\":7,\"cameras\":[]}",
        "no rows gives an empty cameras array");
  ::unlink(cfg.c_str());
}

int main() {
  test_camera_health_event_proxy();

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

  // The page has the Share with Protect column, and the generic config
  // form leaves protect_event_proxy_cameras to its tickboxes.
  {
    const std::string page = http_body(http_get(port, "/"));
    check(page.find(">Share with Protect<") != std::string::npos,
          "camera health card has a Share with Protect column");
    check(page.find("if (e.name === 'protect_event_proxy_cameras') continue;") !=
              std::string::npos,
          "config form skips protect_event_proxy_cameras");
    check(page.find("colspan=\"8\"") == std::string::npos,
          "no camera health row still spans the old column count");
  }

  // GET /api/event_proxy without a status provider reports sharing off.
  {
    const std::string resp = http_get(port, "/api/event_proxy");
    check(resp.find("HTTP/1.1 200") != std::string::npos,
          "GET /api/event_proxy returns 200 without a provider");
    check(resp.find("application/json") != std::string::npos,
          "GET /api/event_proxy advertises application/json");
    check(http_body(resp) == "{\"mode\":\"off\"}",
          "event_proxy without a provider is {\"mode\":\"off\"}");
  }

  // A provider set after start() is served as-is; an empty reply from it
  // still reads as off.
  const std::string proxy_json =
      "{\"mode\":\"serve\",\"port\":7892,\"bound\":true,"
      "\"patch\":\"applied\",\"protect_restart_pending\":false,"
      "\"hub\":{\"ready\":true,\"cameras\":[{\"key\":\"192.168.1.109\","
      "\"ip\":\"192.168.1.109\",\"mac\":\"F00000FDAED9\",\"proxied\":true,"
      "\"accepting\":true,\"pull_points\":[]}]}}";
  server.set_event_proxy_status_fn([] { return std::string(); });
  check(http_body(http_get(port, "/api/event_proxy")) == "{\"mode\":\"off\"}",
        "empty provider reply reads as {\"mode\":\"off\"}");
  server.set_event_proxy_status_fn([proxy_json] { return proxy_json; });
  check(http_body(http_get(port, "/api/event_proxy")) == proxy_json,
        "event_proxy returns the provider's JSON");

  // The diagnostic dump carries event_proxy.json, sanitised like the
  // other files.
  {
    const std::string resp = http_get(port, "/api/diagnostic_dump");
    check(resp.find("HTTP/1.1 200") != std::string::npos,
          "GET /api/diagnostic_dump returns 200");
    const std::string tgz = temp_dir() + "/admin_server_test_dump.tar.gz";
    check(write_text(tgz, http_body(resp)), "write dump archive");
    std::string ep;
    FILE* p = ::popen(("tar -xzOf " + tgz + " ./event_proxy.json 2>/dev/null")
                          .c_str(), "r");
    if (p != nullptr) {
      char buf[4096];
      size_t n;
      while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) ep.append(buf, n);
      ::pclose(p);
    }
    check(ep.find("\"mode\":\"serve\"") != std::string::npos,
          "dump contains event_proxy.json with the provider's status");
    check(ep.find("192.168.1.109") == std::string::npos,
          "dump's event_proxy.json has camera addresses redacted");
    check(ep.find("F00000FDAED9") == std::string::npos,
          "dump's event_proxy.json has camera MACs redacted");
    ::unlink(tgz.c_str());
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
