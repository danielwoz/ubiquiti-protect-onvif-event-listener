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
 * onvif_listener.cpp -- implementation of the onvif::OnvifListener library.
 *
 * Deps: libcurl, libssl/libcrypto (OpenSSL), libxml2
 */

#include "onvif_listener.hpp"

#include <curl/curl.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace onvif {

// ============================================================
// Library lifecycle
// ============================================================
void global_init() {
  curl_global_init(CURL_GLOBAL_ALL);
  xmlInitParser();
  LIBXML_TEST_VERSION
}

void global_cleanup() {
  curl_global_cleanup();
  xmlCleanupParser();
}

// ============================================================
// Internal helpers (all anonymous-namespace / file-local)
// ============================================================
namespace {

// -------------------------------------------------------
// Base64
// -------------------------------------------------------
static const char B64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";  // NOLINT

std::string base64_encode(const unsigned char* d, size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    unsigned char b0 =            d[i];
    unsigned char b1 = (i + 1 < len) ? d[i + 1] : 0;
    unsigned char b2 = (i + 2 < len) ? d[i + 2] : 0;
    out += B64[b0 >> 2];
    out += B64[((b0 & 0x03) << 4) | (b1 >> 4)];
    out += (i + 1 < len) ? B64[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=';
    out += (i + 2 < len) ? B64[b2 & 0x3f] : '=';
  }
  return out;
}

// -------------------------------------------------------
// Timestamps
// -------------------------------------------------------
std::string utc_now_iso8601() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string utc_now_iso8601_ms() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  std::ostringstream oss;
  oss << buf << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
  return oss.str();
}

// -------------------------------------------------------
// JSON string escaping (used by RawRecorder)
// -------------------------------------------------------
std::string json_str(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 4);
  out += '"';
  for (unsigned char c : s) {
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  out += '"';
  return out;
}

// -------------------------------------------------------
// RawRecorder -- thread-safe JSON Lines sink for raw HTTP exchanges
// -------------------------------------------------------
class RawRecorder {
 public:
  static absl::StatusOr<std::unique_ptr<RawRecorder>> Create(
      const std::string& path) {
    auto r = std::unique_ptr<RawRecorder>(new RawRecorder(path));
    if (!r->file_.is_open())
      return absl::InternalError("Cannot open raw recording file: " + path);
    return r;
  }

  // Record one complete request/response exchange.
  void record(const std::string& camera_ip,
              const std::string& url,
              const std::string& soap_action,
              const std::string& request,
              int64_t            response_status,
              const std::string& response) {
    std::string line;
    line += '{';
    line += json_str("timestamp")       + ':' + json_str(utc_now_iso8601_ms()) + ',';
    line += json_str("camera_ip")       + ':' + json_str(camera_ip)            + ',';
    line += json_str("url")             + ':' + json_str(url)                  + ',';
    line += json_str("soap_action")     + ':' + json_str(soap_action)          + ',';
    line += json_str("request")         + ':' + json_str(request)              + ',';
    line += json_str("response_status") + ':' + std::to_string(response_status)+ ',';
    line += json_str("response")        + ':' + json_str(response);
    line += "}\n";

    std::lock_guard<std::mutex> lk(mu_);
    file_ << line;
    file_.flush();
  }

 private:
  explicit RawRecorder(const std::string& path) {
    file_.open(path, std::ios::app);
  }

  std::ofstream file_;
  std::mutex    mu_;
};

// -------------------------------------------------------
// WS-Security UsernameToken (PasswordDigest)
// -------------------------------------------------------
struct WSSecurity {
  std::string nonce_b64;
  std::string created;
  std::string digest;
};

WSSecurity make_wssecurity(const std::string& password) {
  unsigned char nonce[16];
  RAND_bytes(nonce, sizeof(nonce));

  std::string created  = utc_now_iso8601();
  std::string nonce_b64 = base64_encode(nonce, sizeof(nonce));

  // digest = Base64(SHA1(nonce_bytes || created || password))
  std::vector<unsigned char> pre;
  pre.reserve(16 + created.size() + password.size());
  pre.insert(pre.end(), nonce, nonce + 16);
  pre.insert(pre.end(), created.begin(), created.end());
  pre.insert(pre.end(), password.begin(), password.end());

  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(pre.data(), pre.size(), hash);

  return {nonce_b64, created, base64_encode(hash, SHA_DIGEST_LENGTH)};
}

// -------------------------------------------------------
// SOAP envelope builder
// -------------------------------------------------------
std::string build_soap(
  const std::string& username,
  const std::string& password,
  const std::string& body_xml,
  const std::string& wsa_to     = "",
  const std::string& wsa_action = "") {
  WSSecurity ws = make_wssecurity(password);

  std::ostringstream s;
  s << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       "<SOAP-ENV:Envelope\n"
       "  xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\n"
       "  xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\"\n"  // NOLINT(whitespace/line_length)
       "  xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\"\n"  // NOLINT(whitespace/line_length)
       "  xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\"\n"
       "  xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\"\n"
       "  xmlns:wsa5=\"http://www.w3.org/2005/08/addressing\"\n"
       "  xmlns:tt=\"http://www.onvif.org/ver10/schema\"\n"
       "  xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">\n"
       "  <SOAP-ENV:Header>\n"
       "    <wsse:Security SOAP-ENV:mustUnderstand=\"true\">\n"
       "      <wsu:Timestamp wsu:Id=\"Time\">\n"
       "        <wsu:Created>" << ws.created << "</wsu:Created>\n"
       "      </wsu:Timestamp>\n"
       "      <wsse:UsernameToken>\n"
       "        <wsse:Username>" << username << "</wsse:Username>\n"
       "        <wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">"  // NOLINT(whitespace/line_length)
    << ws.digest << "</wsse:Password>\n"
       "        <wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">"  // NOLINT(whitespace/line_length)
    << ws.nonce_b64 << "</wsse:Nonce>\n"
       "        <wsu:Created>" << ws.created << "</wsu:Created>\n"
       "      </wsse:UsernameToken>\n"
       "    </wsse:Security>\n";

  if (!wsa_to.empty())
    s << "    <wsa5:To>" << wsa_to << "</wsa5:To>\n";
  if (!wsa_action.empty())
    s << "    <wsa5:Action>" << wsa_action << "</wsa5:Action>\n";

  s << "  </SOAP-ENV:Header>\n"
       "  <SOAP-ENV:Body>\n"
    << body_xml
    << "  </SOAP-ENV:Body>\n"
       "</SOAP-ENV:Envelope>\n";

  return s.str();
}

// -------------------------------------------------------
// HTTP POST via libcurl
// -------------------------------------------------------
struct HttpResponse {
  int64_t     status_code{0};
  std::string body;
};

size_t curl_write_cb(void* ptr, size_t sz, size_t nmemb, std::string* out) {
  out->append(static_cast<char*>(ptr), sz * nmemb);
  return sz * nmemb;
}

absl::StatusOr<HttpResponse> soap_post(
  const std::string& url,
  const std::string& body,
  const std::string& action,
  int timeout_sec = 30) {
  CURL* c = curl_easy_init();
  if (!c) return absl::InternalError("curl_easy_init failed");

  HttpResponse resp;

  std::string ct = "Content-Type: application/soap+xml; charset=utf-8";
  if (!action.empty()) ct += "; action=\"" + action + "\"";
  std::string act_hdr = "SOAPAction: \"" + action + "\"";

  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, ct.c_str());
  hdrs = curl_slist_append(hdrs, act_hdr.c_str());

  curl_easy_setopt(c, CURLOPT_URL,            url.c_str());
  curl_easy_setopt(c, CURLOPT_POSTFIELDS,     body.c_str());
  // libcurl requires long for size/timeout params  // NOLINT(runtime/int)
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE,  static_cast<long>(body.size()));  // NOLINT
  curl_easy_setopt(c, CURLOPT_HTTPHEADER,     hdrs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  curl_write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA,      &resp.body);
  curl_easy_setopt(c, CURLOPT_TIMEOUT,        static_cast<long>(timeout_sec));  // NOLINT
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);  // NOLINT(runtime/int)

  CURLcode rc = curl_easy_perform(c);
  curl_slist_free_all(hdrs);

  if (rc != CURLE_OK) {
    curl_easy_cleanup(c);
    return absl::InternalError(std::string("curl: ") + curl_easy_strerror(rc));
  }

  long code = 0;  // NOLINT(runtime/int)
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  resp.status_code = static_cast<int64_t>(code);
  curl_easy_cleanup(c);
  return resp;
}

// -------------------------------------------------------
// libxml2 XPath helper
// -------------------------------------------------------
struct XmlDoc {
  xmlDocPtr          doc{nullptr};
  xmlXPathContextPtr ctx{nullptr};

  static absl::StatusOr<XmlDoc> Create(const std::string& xml) {
    XmlDoc xd;
    xd.doc = xmlParseDoc(reinterpret_cast<const xmlChar*>(xml.c_str()));
    if (!xd.doc) return absl::InternalError("XML parse error");
    xd.ctx = xmlXPathNewContext(xd.doc);
    if (!xd.ctx) {
      xmlFreeDoc(xd.doc);
      xd.doc = nullptr;
      return absl::InternalError("XPath ctx error");
    }
    register_ns(xd.ctx);
    return xd;
  }

  ~XmlDoc() {
    if (ctx) xmlXPathFreeContext(ctx);
    if (doc) xmlFreeDoc(doc);
  }

  XmlDoc() = default;
  XmlDoc(const XmlDoc&)            = delete;
  XmlDoc& operator=(const XmlDoc&) = delete;

  XmlDoc(XmlDoc&& o) noexcept : doc(o.doc), ctx(o.ctx) {
    o.doc = nullptr;
    o.ctx = nullptr;
  }
  XmlDoc& operator=(XmlDoc&& o) noexcept {
    if (this != &o) {
      if (ctx) xmlXPathFreeContext(ctx);
      if (doc) xmlFreeDoc(doc);
      doc = o.doc; ctx = o.ctx;
      o.doc = nullptr; o.ctx = nullptr;
    }
    return *this;
  }

  static void register_ns(xmlXPathContextPtr c) {
    xmlXPathRegisterNs(c, BAD_CAST "s",
      BAD_CAST "http://www.w3.org/2003/05/soap-envelope");
    xmlXPathRegisterNs(c, BAD_CAST "tev",
      BAD_CAST "http://www.onvif.org/ver10/events/wsdl");
    xmlXPathRegisterNs(c, BAD_CAST "wsnt",
      BAD_CAST "http://docs.oasis-open.org/wsn/b-2");
    xmlXPathRegisterNs(c, BAD_CAST "wsa5",
      BAD_CAST "http://www.w3.org/2005/08/addressing");
    xmlXPathRegisterNs(c, BAD_CAST "wsa",
      BAD_CAST "http://schemas.xmlsoap.org/ws/2004/08/addressing");
    xmlXPathRegisterNs(c, BAD_CAST "tt",
      BAD_CAST "http://www.onvif.org/ver10/schema");
    xmlXPathRegisterNs(c, BAD_CAST "tds",
      BAD_CAST "http://www.onvif.org/ver10/device/wsdl");
  }

  using XPathObj = std::unique_ptr<xmlXPathObject, decltype(&xmlXPathFreeObject)>;

  XPathObj xpath(const std::string& expr,
                 xmlNodePtr context_node = nullptr) const {
    xmlNodePtr old_node = ctx->node;
    if (context_node) ctx->node = context_node;

    xmlXPathObjectPtr obj =
      xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>(expr.c_str()), ctx);

    if (context_node) ctx->node = old_node;

    return {obj, xmlXPathFreeObject};
  }

  std::string text(const std::string& expr,
                   xmlNodePtr context_node = nullptr) const {
    auto obj = xpath(expr, context_node);
    if (!obj) return {};
    if (obj->type == XPATH_NODESET &&
        obj->nodesetval && obj->nodesetval->nodeNr > 0) {
      xmlChar* c = xmlNodeGetContent(obj->nodesetval->nodeTab[0]);
      if (!c) return {};
      std::string s(reinterpret_cast<char*>(c));
      xmlFree(c);
      return s;
    }
    if (obj->type == XPATH_STRING && obj->stringval)
      return reinterpret_cast<char*>(obj->stringval);
    return {};
  }

  static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
  }
};

// -------------------------------------------------------
// Pair of URLs discovered via GetServices
// -------------------------------------------------------
struct DiscoveredServices {
  std::string event_url;   // events service XAddr (fallback: /onvif/event_service)
  std::string alarm_url;   // alarm service XAddr; empty = no alarm service found
};

// -------------------------------------------------------
// Internal notification struct (parsed from XML)
// -------------------------------------------------------
struct Notification {
  std::string topic;
  std::string event_time;
  std::string property_op;
  std::map<std::string, std::string> source;
  std::map<std::string, std::string> data;
  std::optional<jpeg_crop::BoundingBox> bbox;
};

// -------------------------------------------------------
// CameraWorker -- manages one camera's PullPoint lifecycle
// -------------------------------------------------------
class CameraWorker {
 public:
  CameraWorker(const CameraConfig& cfg,
               const EventCallback& cb,
               const std::atomic<bool>& running,
               RawRecorder* raw)
    : cfg_(cfg), cb_(cb), running_(running), raw_(raw) {}

  void run() {
    LOG(INFO) << '[' << cfg_.ip << "] started";
    const int max_failures = cfg_.max_consecutive_failures;  // 0 = unlimited
    const int window_sec   = cfg_.failure_window_sec;
    int consecutive_failures = 0;
    std::chrono::steady_clock::time_point streak_start;

    while (running_) {
      // Discover event and alarm service URLs via GetServices (IncludeCapability=true).
      // Re-runs on every outer loop iteration so a camera restart is handled cleanly.
      const DiscoveredServices sv = discover_services();

      auto sub_or = create_subscription(sv.event_url);
      if (!sub_or.ok()) {
        if (consecutive_failures == 0)
          streak_start = std::chrono::steady_clock::now();
        ++consecutive_failures;
        if (max_failures > 0 && consecutive_failures >= max_failures) {
          pause_and_reset(&consecutive_failures, streak_start, window_sec,
              std::string("[") + cfg_.ip +
              "] camera unreachable after " +
              std::to_string(consecutive_failures) +
              " consecutive failures -- pausing before retry"
              " (last error: " + std::string(sub_or.status().message()) + ")");
          continue;
        }
        LOG(ERROR) << '[' << cfg_.ip << "] error: "
                   << sub_or.status().message()
                   << ", reconnecting in " << cfg_.retry_interval_sec << "s"
                   << (max_failures > 0
                         ? " (" + std::to_string(consecutive_failures) +
                           "/" + std::to_string(max_failures) + ")"
                         : "");
        sleep_interruptible(cfg_.retry_interval_sec);
        continue;
      }

      const std::string& sub_url = *sub_or;
      if (sub_url.empty()) {
        if (consecutive_failures == 0)
          streak_start = std::chrono::steady_clock::now();
        ++consecutive_failures;
        if (max_failures > 0 && consecutive_failures >= max_failures) {
          pause_and_reset(&consecutive_failures, streak_start, window_sec,
              std::string("[") + cfg_.ip +
              "] failed to get subscription URL after " +
              std::to_string(consecutive_failures) +
              " consecutive attempts -- pausing before retry");
          continue;
        }
        LOG_FIRST_N(ERROR, 1) << '[' << cfg_.ip
                             << "] failed to get subscription URL"
                             << ", retrying in " << cfg_.retry_interval_sec
                             << "s"
                             << (max_failures > 0
                                   ? " (" + std::to_string(consecutive_failures) +
                                     "/" + std::to_string(max_failures) + ")"
                                   : "");
        sleep_interruptible(cfg_.retry_interval_sec);
        continue;
      }

      // Successful subscription -- reset the failure counter.
      consecutive_failures = 0;
      LOG(INFO) << '[' << cfg_.ip << "] subscription -> " << sub_url;

      auto renew_at =
        std::chrono::steady_clock::now() + std::chrono::seconds(90);

      bool inner_ok = true;
      while (running_ && inner_ok) {
        if (std::chrono::steady_clock::now() >= renew_at) {
          absl::Status rs = renew(sub_url);
          if (!rs.ok())
            LOG(WARNING) << '[' << cfg_.ip << "] renew error: " << rs.message();
          renew_at = std::chrono::steady_clock::now()
                     + std::chrono::seconds(90);
        }
        absl::Status ps = pull(sub_url, sv.alarm_url);
        if (!ps.ok()) {
          if (consecutive_failures == 0)
            streak_start = std::chrono::steady_clock::now();
          ++consecutive_failures;
          if (max_failures > 0 && consecutive_failures >= max_failures) {
            pause_and_reset(&consecutive_failures, streak_start, window_sec,
                std::string("[") + cfg_.ip +
                "] camera unreachable after " +
                std::to_string(consecutive_failures) +
                " consecutive failures -- pausing before retry"
                " (last error: " + std::string(ps.message()) + ")");
          } else {
            LOG_FIRST_N(ERROR, 1) << '[' << cfg_.ip << "] error: "
                                  << ps.message()
                                  << ", reconnecting in "
                                  << cfg_.retry_interval_sec << "s"
                                  << (max_failures > 0
                                        ? " (" +
                                          std::to_string(consecutive_failures) +
                                          "/" + std::to_string(max_failures) +
                                          ")"
                                        : "");
            sleep_interruptible(cfg_.retry_interval_sec);
          }
          inner_ok = false;
        }
      }
    }
    LOG(INFO) << '[' << cfg_.ip << "] stopped";
  }

 private:
  void sleep_interruptible(int secs) {
    for (int i = 0; i < secs && running_; ++i)
      std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  // Log msg, sleep for the remainder of the failure window, then reset *failures.
  void pause_and_reset(int* failures,
                       const std::chrono::steady_clock::time_point& streak_start,
                       int window_sec,
                       const std::string& msg) {
    using Clk = std::chrono::steady_clock;
    auto deadline  = streak_start + std::chrono::seconds(window_sec);
    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                         deadline - Clk::now());
    int pause_sec  = remaining.count() > 0
                     ? static_cast<int>(remaining.count()) : 0;
    LOG(WARNING) << msg << " (retry in " << pause_sec << "s)";
    sleep_interruptible(pause_sec);
    *failures = 0;
  }

  std::string event_url() const {
    return "http://" + cfg_.ip + "/onvif/event_service";
  }

  std::string device_url() const {
    return "http://" + cfg_.ip + "/onvif/device_service";
  }

  // Calls GetServices (with capabilities) on the device management endpoint to
  // discover the canonical event service XAddr and, if advertised, an alarm
  // service base URL.
  //
  // event_url: falls back to /onvif/event_service on any error.
  // alarm_url: empty when no service with "alarm" in its namespace is found
  //            (camera not managed by UniFi Protect or does not support alarms).
  DiscoveredServices discover_services() {
    static const char* ACTION =
      "http://www.onvif.org/ver10/device/wsdl/DeviceManagement/GetServicesRequest";

    const std::string body =
      "    <tds:GetServices>\n"
      "      <tds:IncludeCapability>true</tds:IncludeCapability>\n"
      "    </tds:GetServices>\n";

    DiscoveredServices result;
    result.event_url = event_url();  // default fallback

    auto soap = build_soap(cfg_.user, cfg_.password, body, device_url(), ACTION);
    auto resp_or = soap_post_r(device_url(), soap, ACTION, 10);
    if (!resp_or.ok()) {
      LOG(INFO) << '[' << cfg_.ip << "] GetServices: "
                << resp_or.status().message() << " -- using defaults";
      return result;
    }
    if (resp_or->status_code != 200) {
      LOG_FIRST_N(INFO, 1) << '[' << cfg_.ip << "] GetServices HTTP "
                           << resp_or->status_code << " -- using defaults";
      return result;
    }

    auto doc_or = XmlDoc::Create(resp_or->body);
    if (!doc_or.ok()) {
      LOG(INFO) << '[' << cfg_.ip << "] GetServices parse error -- using defaults";
      return result;
    }

    // Walk every tds:Service and extract event/alarm XAddrs by namespace.
    auto services = doc_or->xpath("//tds:Service");
    if (services && services->nodesetval) {
      for (int i = 0; i < services->nodesetval->nodeNr; ++i) {
        xmlNodePtr node = services->nodesetval->nodeTab[i];
        const std::string ns =
            XmlDoc::trim(doc_or->text("tds:Namespace", node));
        const std::string xaddr =
            XmlDoc::trim(doc_or->text("tds:XAddr", node));
        if (xaddr.empty()) continue;

        if (ns.find("events") != std::string::npos && result.event_url == event_url()) {
          LOG(INFO) << '[' << cfg_.ip << "] event service URL: " << xaddr;
          result.event_url = xaddr;
        } else if (ns.find("alarm") != std::string::npos) {
          LOG(INFO) << '[' << cfg_.ip << "] alarm service URL: " << xaddr;
          result.alarm_url = xaddr;
        }
      }
    }

    if (result.event_url == event_url())
      LOG(INFO) << '[' << cfg_.ip
                << "] events service not in GetServices -- using default";
    if (result.alarm_url.empty())
      LOG(INFO) << '[' << cfg_.ip
                << "] alarm service not in GetServices -- skipping alarms";

    return result;
  }

  // soap_post wrapper: records the exchange if raw recording is enabled.
  absl::StatusOr<HttpResponse> soap_post_r(const std::string& url,
                                            const std::string& body,
                                            const std::string& action,
                                            int timeout_sec) {
    auto resp_or = soap_post(url, body, action, timeout_sec);
    if (!resp_or.ok()) return resp_or;
    if (raw_)
      raw_->record(cfg_.ip, url, action, body,
                   resp_or->status_code, resp_or->body);
    return resp_or;
  }

  // CreatePullPointSubscription -> subscription URL (empty string on HTTP error)
  absl::StatusOr<std::string> create_subscription(const std::string& ev_url) {
    static const char* ACTION =
      "http://www.onvif.org/ver10/events/wsdl/EventPortType/"
      "CreatePullPointSubscriptionRequest";

    const std::string body =
      "    <tev:CreatePullPointSubscription>\n"
      "      <tev:InitialTerminationTime>PT120S</tev:InitialTerminationTime>\n"
      "    </tev:CreatePullPointSubscription>\n";

    auto soap = build_soap(cfg_.user, cfg_.password, body, ev_url, ACTION);
    auto resp_or = soap_post_r(ev_url, soap, ACTION, 20);
    if (!resp_or.ok()) return resp_or.status();

    const HttpResponse& resp = *resp_or;
    if (resp.status_code != 200) {
      LOG_FIRST_N(INFO, 1) << '[' << cfg_.ip
                           << "] CreatePullPointSubscription HTTP "
                           << resp.status_code << ": "
                           << resp.body.substr(0, 300);
      return std::string{};
    }

    auto doc_or = XmlDoc::Create(resp.body);
    if (!doc_or.ok()) {
      LOG(ERROR) << '[' << cfg_.ip << "] parse sub URL: "
                 << doc_or.status().message();
      return std::string{};
    }
    return XmlDoc::trim(
      doc_or->text("//*[local-name()='SubscriptionReference']"
                   "/*[local-name()='Address']"));
  }

  absl::Status renew(const std::string& sub_url) {
    static const char* ACTION =
      "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/RenewRequest";

    const std::string body =
      "    <wsnt:Renew>\n"
      "      <wsnt:TerminationTime>PT120S</wsnt:TerminationTime>\n"
      "    </wsnt:Renew>\n";

    auto soap = build_soap(cfg_.user, cfg_.password, body, sub_url, ACTION);
    auto resp_or = soap_post_r(sub_url, soap, ACTION, 15);
    if (!resp_or.ok()) return resp_or.status();
    if (resp_or->status_code == 200)
      LOG(INFO) << '[' << cfg_.ip << "] subscription renewed";
    else
      LOG_FIRST_N(WARNING, 1) << '[' << cfg_.ip << "] renew HTTP "
                              << resp_or->status_code;
    return absl::OkStatus();
  }

  absl::Status pull(const std::string& sub_url, const std::string& alarm_url) {
    static const char* ACTION =
      "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/"
      "PullMessagesRequest";

    const std::string body =
      "    <tev:PullMessages>\n"
      "      <tev:MessageLimit>100</tev:MessageLimit>\n"
      "      <tev:Timeout>PT5S</tev:Timeout>\n"
      "    </tev:PullMessages>\n";

    auto soap = build_soap(cfg_.user, cfg_.password, body, sub_url, ACTION);
    // HTTP timeout > SOAP pull timeout (5 s) to allow for network headroom
    auto resp_or = soap_post_r(sub_url, soap, ACTION, 20);
    if (!resp_or.ok()) return resp_or.status();

    const HttpResponse& resp = *resp_or;
    if (resp.status_code != 200)
      return absl::InternalError(
        "PullMessages HTTP " + std::to_string(resp.status_code) +
        ": " + resp.body.substr(0, 300));

    auto events = parse_notifications(resp.body);
    if (!events.empty())
      LOG(INFO) << '[' << cfg_.ip << "] received " << events.size() << " event(s)";

    for (auto& n : events) {
      LOG(INFO) << '[' << cfg_.ip << "]   topic=" << n.topic
                << " op=" << n.property_op << " t=" << n.event_time;
      cb_(OnvifEvent{
        cfg_.ip, cfg_.user,
        n.topic, n.event_time, n.property_op,
        n.source, n.data, n.bbox, alarm_url});
    }
    return absl::OkStatus();
  }

  std::vector<Notification> parse_notifications(const std::string& xml) {
    std::vector<Notification> out;
    auto doc_or = XmlDoc::Create(xml);
    if (!doc_or.ok()) return out;
    XmlDoc& doc = *doc_or;

    auto notifs = doc.xpath("//wsnt:NotificationMessage");
    if (!notifs || !notifs->nodesetval) return out;

    for (int i = 0; i < notifs->nodesetval->nodeNr; ++i) {
      xmlNodePtr node = notifs->nodesetval->nodeTab[i];
      Notification n;

      n.topic = XmlDoc::trim(doc.text("wsnt:Topic", node));

      auto msg_obj = doc.xpath(".//tt:Message", node);
      if (!msg_obj || !msg_obj->nodesetval ||
          msg_obj->nodesetval->nodeNr == 0) {
        out.push_back(n);
        continue;
      }
      xmlNodePtr msg_node = msg_obj->nodesetval->nodeTab[0];

      auto prop = [&](const char* attr) -> std::string {
        xmlChar* v = xmlGetProp(msg_node, BAD_CAST attr);
        if (!v) return {};
        std::string s(reinterpret_cast<char*>(v));
        xmlFree(v);
        return s;
      };
      n.event_time  = prop("UtcTime");
      n.property_op = prop("PropertyOperation");

      auto fill_items = [&](const std::string& xpath_expr,
                            std::map<std::string, std::string>& target) {
        auto obj = doc.xpath(xpath_expr, msg_node);
        if (!obj || !obj->nodesetval) return;
        for (int j = 0; j < obj->nodesetval->nodeNr; ++j) {
          xmlNodePtr it    = obj->nodesetval->nodeTab[j];
          xmlChar*   name  = xmlGetProp(it, BAD_CAST "Name");
          xmlChar*   value = xmlGetProp(it, BAD_CAST "Value");
          if (name && value)
            target[reinterpret_cast<char*>(name)] =
              reinterpret_cast<char*>(value);
          if (name)  xmlFree(name);
          if (value) xmlFree(value);
        }
      };
      fill_items(".//tt:Source/tt:SimpleItem", n.source);
      fill_items(".//tt:Data/tt:SimpleItem",   n.data);

      // Extract optional tt:BoundingBox from analytics events.
      // ONVIF uses [-1,1] coordinate space; convert to normalised [0,1].
      {
        auto bb_obj = doc.xpath(".//tt:BoundingBox", msg_node);
        if (bb_obj && bb_obj->nodesetval && bb_obj->nodesetval->nodeNr > 0) {
          xmlNodePtr bb_node = bb_obj->nodesetval->nodeTab[0];
          auto get_attr = [&](const char* attr) -> std::optional<float> {
            xmlChar* v = xmlGetProp(bb_node, BAD_CAST attr);
            if (!v) return std::nullopt;
            float val = std::strtof(reinterpret_cast<char*>(v), nullptr);
            xmlFree(v);
            return val;
          };
          auto left   = get_attr("left");
          auto top    = get_attr("top");
          auto right  = get_attr("right");
          auto bottom = get_attr("bottom");
          if (left && top && right && bottom) {
            jpeg_crop::BoundingBox bb;
            bb.x = (*left   + 1.0f) / 2.0f;
            bb.y = (*top    + 1.0f) / 2.0f;
            bb.w = (*right  - *left)  / 2.0f;
            bb.h = (*bottom - *top)   / 2.0f;
            if (bb.w > 0.0f && bb.h > 0.0f)
              n.bbox = bb;
          }
        }
      }

      out.push_back(std::move(n));
    }
    return out;
  }

  const CameraConfig&       cfg_;
  const EventCallback&      cb_;
  const std::atomic<bool>&  running_;
  RawRecorder*              raw_;      // nullable; not owned
};

}  // anonymous namespace

// ============================================================
// OnvifListener public methods
// ============================================================
OnvifListener::OnvifListener()  = default;
OnvifListener::~OnvifListener() = default;

void OnvifListener::add_camera(const CameraConfig& cfg) {
  cameras_.push_back(cfg);
}

void OnvifListener::enable_raw_recording(const std::string& path) {
  raw_path_ = path;
}

void OnvifListener::run(EventCallback cb) {
  running_ = true;

  // Create raw recorder if a path was configured (lives for the duration of run())
  std::unique_ptr<RawRecorder> raw_recorder;
  if (!raw_path_.empty()) {
    auto raw_or = RawRecorder::Create(raw_path_);
    if (!raw_or.ok()) {
      std::cerr << "[onvif] raw recording disabled: "
                << raw_or.status().message() << '\n';
    } else {
      raw_recorder = std::move(*raw_or);
    }
  }
  RawRecorder* raw = raw_recorder.get();  // nullptr when disabled

  // Build workers (heap-allocated so their address is stable across moves)
  std::vector<std::unique_ptr<CameraWorker>> workers;
  workers.reserve(cameras_.size());
  for (const auto& cam : cameras_)
    workers.push_back(
      std::make_unique<CameraWorker>(cam, cb, running_, raw));

  // Launch one thread per camera
  std::vector<std::thread> threads;
  threads.reserve(workers.size());
  for (auto& w : workers) {
    CameraWorker* ptr = w.get();
    threads.emplace_back([ptr] { ptr->run(); });
  }

  for (auto& t : threads)
    t.join();
}

void OnvifListener::stop() {
  running_ = false;
}

}  // namespace onvif
