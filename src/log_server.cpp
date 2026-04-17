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

#include "log_server.hpp"

#include <arpa/inet.h>
#include <microhttpd.h>
#include <netinet/in.h>

// MHD_Result was introduced in 0.9.71; older sysroots use plain int.
#if MHD_VERSION < 0x00097100
typedef int MHD_Result;
#endif

#include <cstring>
#include <string>

#include "log_ring.hpp"

// HTML template lives outside the namespace to avoid cpplint indent warnings.
// The log content is inserted between the <pre> tags.
// Auto-refreshes every 5 seconds.  Scrolls to the bottom on load.
static const char kHtmlHead[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset=\"utf-8\">"
    "<title>ONVIF Recorder Log</title>"
    "<meta http-equiv=\"refresh\" content=\"5\">"
    "<style>"
    "body{background:#1a1a2e;color:#e0e0e0;"
    "font-family:monospace;margin:0;padding:16px}"
    "pre{white-space:pre-wrap;word-break:break-all;"
    "font-size:12px;line-height:1.4}"
    "h1{font-size:16px;color:#8be9fd;margin:0 0 8px 0}"
    ".I{color:#8be9fd}.W{color:#f1fa8c}"
    ".E{color:#ff5555}.F{color:#ff79c6}"
    "</style></head><body>"
    "<h1>ONVIF Recorder Log "
    "(1 MiB ring buffer, 5s refresh)</h1><pre>";
static const char kHtmlTail[] =
    "</pre>"
    "<script>window.scrollTo(0,document.body.scrollHeight)</script>"
    "</body></html>";

namespace onvif {

// Escape HTML entities in log text.
static std::string html_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + s.size() / 8);
  for (char c : s) {
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      default:   out += c;        break;
    }
  }
  return out;
}

// Add colour spans for severity markers.
static std::string colorize(const std::string& escaped) {
  std::string out;
  out.reserve(escaped.size() + escaped.size() / 4);
  size_t pos = 0;
  while (pos < escaped.size()) {
    size_t nl = escaped.find('\n', pos);
    if (nl == std::string::npos) nl = escaped.size();
    // Each line starts with "YYYY-MM-DD HH:MM:SS.mmm X " where X is
    // the severity letter at offset 24.
    if (nl - pos > 25) {
      char sev = escaped[pos + 24];
      if (sev == 'I' || sev == 'W' || sev == 'E' || sev == 'F') {
        out += "<span class=\"";
        out += sev;
        out += "\">";
        out.append(escaped, pos, nl - pos);
        out += "</span>";
      } else {
        out.append(escaped, pos, nl - pos);
      }
    } else {
      out.append(escaped, pos, nl - pos);
    }
    if (nl < escaped.size()) out += '\n';
    pos = nl + 1;
  }
  return out;
}

static MHD_Result handler(
    void* cls,
    struct MHD_Connection* connection,
    const char* /*url*/,
    const char* /*method*/,
    const char* /*version*/,
    const char* /*upload_data*/,
    size_t* /*upload_data_size*/,
    void** /*con_cls*/) {
  auto* ring = static_cast<const LogRing*>(cls);
  std::string body = kHtmlHead;
  body += colorize(html_escape(ring->dump()));
  body += kHtmlTail;

  struct MHD_Response* response = MHD_create_response_from_buffer(
      body.size(),
      const_cast<char*>(body.data()),
      MHD_RESPMEM_MUST_COPY);
  MHD_add_response_header(response,
      "Content-Type", "text/html; charset=utf-8");
  MHD_add_response_header(response, "Cache-Control", "no-store");
  MHD_Result ret =
      MHD_queue_response(connection, MHD_HTTP_OK, response);
  MHD_destroy_response(response);
  return ret;
}

bool LogServer::start(const LogRing* ring, uint16_t port) {
  ring_ = ring;
  // Bind to loopback only — nginx handles external auth and TLS.
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  daemon_ = MHD_start_daemon(
      MHD_USE_INTERNAL_POLLING_THREAD,
      port,
      nullptr, nullptr,
      &handler, const_cast<LogRing*>(ring_),
      MHD_OPTION_SOCK_ADDR,
      reinterpret_cast<struct sockaddr*>(&addr),
      MHD_OPTION_END);
  return daemon_ != nullptr;
}

void LogServer::stop() {
  if (daemon_) {
    MHD_stop_daemon(daemon_);
    daemon_ = nullptr;
  }
}

LogServer::~LogServer() {
  stop();
}

}  // namespace onvif
