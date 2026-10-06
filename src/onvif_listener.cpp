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
#include <zstd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/synchronization/mutex.h"

#include "contention_profiler.hpp"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "listener_status.hpp"
#include "util.hpp"

namespace onvif {

int next_backoff_sec(int current_sec, int base_sec, int max_sec) {
  if (base_sec < 1)       base_sec = 1;
  if (max_sec  < base_sec) max_sec = base_sec;
  if (current_sec < base_sec) return base_sec;
  if (current_sec >= max_sec) return max_sec;
  // Doubling can't overflow here: current_sec < max_sec <= INT_MAX/2 in any
  // sane config, but clamp through int64 so a hand-edited max can't wrap.
  const int64_t doubled = static_cast<int64_t>(current_sec) * 2;
  return doubled >= max_sec ? max_sec : static_cast<int>(doubled);
}

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


// -------------------------------------------------------
// (RawSink is defined in onvif_listener.hpp / implemented further below.)

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
  const std::string& wsa_action = "",
  const std::string& ref_params = "") {
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
  // WS-Addressing ReferenceParameters: forwarded verbatim so cameras like Axis
  // that multiplex all ONVIF operations through a single endpoint can route the
  // request to the correct subscription.
  if (!ref_params.empty())
    s << ref_params << "\n";

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
    // Use recovery mode to handle malformed XML (e.g. undeclared namespace
    // prefixes like "tad:" in some cameras' GetServices responses) without
    // printing errors to stderr.
    xd.doc = xmlReadMemory(xml.c_str(), static_cast<int>(xml.size()),
                           nullptr, nullptr,
                           XML_PARSE_RECOVER | XML_PARSE_NOERROR |
                           XML_PARSE_NOWARNING);
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

  // Serialize the inner XML (children) of the first node matched by expr.
  // Returns an empty string if the expression matches nothing or the node
  // has no children.  Used to capture ReferenceParameters verbatim.
  std::string inner_xml(const std::string& expr) const {
    auto obj = xpath(expr);
    if (!obj || obj->type != XPATH_NODESET ||
        !obj->nodesetval || obj->nodesetval->nodeNr == 0) return {};
    xmlNodePtr node = obj->nodesetval->nodeTab[0];
    if (!node->children) return {};
    xmlBufferPtr buf = xmlBufferCreate();
    if (!buf) return {};
    for (xmlNodePtr child = node->children; child; child = child->next)
      xmlNodeDump(buf, doc, child, 0, 0);
    std::string result(reinterpret_cast<const char*>(xmlBufferContent(buf)));
    xmlBufferFree(buf);
    return result;
  }
};

// Extract the path (and optional query) component from a URL string.
// "http://192.168.1.1:80/onvif/events?x=1" -> "/onvif/events?x=1"
// Returns "/" when no path is found.
static std::string url_path(const std::string& url) {
  const auto scheme_end = url.find("://");
  const auto start = (scheme_end == std::string::npos) ? 0 : scheme_end + 3;
  const auto slash = url.find('/', start);
  return (slash == std::string::npos) ? "/" : url.substr(slash);
}

// -------------------------------------------------------
// Pair of URLs discovered via GetServices
// -------------------------------------------------------
struct DiscoveredServices {
  std::string event_url;   // events service XAddr (fallback: /onvif/event_service)
  std::string alarm_url;   // alarm service XAddr; empty = no alarm service found
};

// -------------------------------------------------------
// Result of CreatePullPointSubscription
// -------------------------------------------------------
struct Subscription {
  std::string url;        // SubscriptionReference/Address
  std::string ref_params;  // inner XML of SubscriptionReference/ReferenceParameters;
                          // empty when the camera provides none.
                          // Must be forwarded verbatim in the SOAP header of
                          // subsequent PullMessages and Renew calls so that
                          // cameras using WS-Addressing routing (e.g. Axis)
                          // can identify the subscription.
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
  using PublishHealthFn = std::function<void(const CameraHealth&)>;

  CameraWorker(const CameraConfig& cfg,
               const EventCallback& cb,
               const std::atomic<bool>& running,
               PublishHealthFn publish_health = {})
    : publish_health_(std::move(publish_health)),
      cfg_(cfg), cb_(cb), running_(running) {}

  void run() {
    LOG(INFO) << '[' << cfg_.ip << "] started";
    const int max_failures = cfg_.max_consecutive_failures;  // 0 = unlimited
    int consecutive_failures = 0;
    int backoff_sec = next_backoff_sec(0, cfg_.retry_backoff_base_sec,
                                       cfg_.retry_backoff_max_sec);

    while (running_) {
      // Discover event and alarm service URLs via GetServices (IncludeCapability=true).
      // Re-runs on every outer loop iteration so a camera restart is handled cleanly.
      const DiscoveredServices sv = discover_services();

      auto sub_or = create_subscription(sv.event_url);
      if (!sub_or.ok()) {
        ++consecutive_failures;
        if (max_failures > 0 && consecutive_failures >= max_failures) {
          pause_and_reset(&consecutive_failures, &backoff_sec,
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

      const Subscription& sub = *sub_or;
      if (sub.url.empty()) {
        ++consecutive_failures;
        if (max_failures > 0 && consecutive_failures >= max_failures) {
          pause_and_reset(&consecutive_failures, &backoff_sec,
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

      // Successful subscription -- reset the failure counter and the backoff.
      consecutive_failures = 0;
      backoff_sec = next_backoff_sec(0, cfg_.retry_backoff_base_sec,
                                     cfg_.retry_backoff_max_sec);
      LOG(INFO) << '[' << cfg_.ip << "] subscription -> " << sub.url;
      subscribed_at_ms_.store(util::now_ms());
      last_renew_ms_.store(util::now_ms());

      auto renew_at =
        std::chrono::steady_clock::now() + std::chrono::seconds(90);
      auto heartbeat_at =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);

      bool inner_ok = true;
      while (running_ && inner_ok) {
        if (std::chrono::steady_clock::now() >= renew_at) {
          absl::Status rs = renew(sub.url, sub.ref_params);
          if (!rs.ok())
            LOG(WARNING) << '[' << cfg_.ip << "] renew error: " << rs.message();
          else
            last_renew_ms_.store(util::now_ms());
          renew_at = std::chrono::steady_clock::now()
                     + std::chrono::seconds(90);
        }
        if (std::chrono::steady_clock::now() >= heartbeat_at) {
          const uint64_t now = util::now_ms();
          const uint64_t le  = last_event_ms_.load();
          const uint64_t lr  = last_renew_ms_.load();
          LOG(INFO) << '[' << cfg_.ip << "] alive: events_recv="
                    << events_received_total_.load()
                    << " renew_age=" << (lr ? (now - lr) / 1000 : 0) << "s"
                    << " last_event="
                    << (le ? std::to_string((now - le) / 1000) + "s"
                           : std::string("never"));
          heartbeat_at = std::chrono::steady_clock::now()
                       + std::chrono::seconds(60);
        }
        if (!running_) break;
        const uint64_t events_before = events_received_total_.load();
        auto pull_started = std::chrono::steady_clock::now();
        absl::Status ps = pull(sub.url, sub.ref_params, sv.alarm_url);
        auto pull_dur = std::chrono::steady_clock::now() - pull_started;
        const bool got_events =
            events_received_total_.load() > events_before;

        // Post-pull rate limit.  ONVIF PullMessages carries an upper-bound
        // timeout (we request PT5S), NOT a lower bound -- a camera whose
        // event queue is empty is spec-compliant to return an empty
        // PullMessagesResponse immediately.  Many real cameras do
        // exactly this (Amcrest, some Reolinks).  We used to interpret
        // fast empty responses as "camera is rate-limiting us" and back
        // off exponentially up to 1 h, which silently delayed events on
        // any spec-compliant chatty firmware.
        //
        // New rule: only for empty responses that came back before our
        // requested timeout ("short side"), sleep 3x the pull duration
        // before the next request.  This throttles busy-loops on
        // fast-responding cameras without penalising cameras that hold
        // the long-poll open (they cross the 4 s threshold and get
        // polled again immediately when their turn ends).  Capped so a
        // very slow-but-still-empty response can't stall us for
        // minutes.
        constexpr auto kLongPollHonored = std::chrono::milliseconds(4000);
        constexpr auto kMaxIdleSleep    = std::chrono::milliseconds(30000);
        if (ps.ok() && !got_events && pull_dur < kLongPollHonored) {
          auto sleep_dur = pull_dur * 3;
          if (sleep_dur > kMaxIdleSleep) sleep_dur = kMaxIdleSleep;
          sleep_interruptible_ms(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  sleep_dur).count());
        }
        // Publish health after every pull tick so the admin UI sees
        // backoff changes on its next refresh.
        publish_current_health();
        if (!ps.ok()) {
          ++consecutive_failures;
          if (max_failures > 0 && consecutive_failures >= max_failures) {
            pause_and_reset(&consecutive_failures, &backoff_sec,
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
      // Inner loop exited (subscription dropped); reflect that in health.
      // Release the subscription so the camera doesn't hold it until its
      // termination time lapses.  Covers both the stop() path and a
      // resubscribe after an error, which is where the leaks accumulate.
      unsubscribe(sub.url, sub.ref_params);
      subscribed_at_ms_.store(0);
    }
    LOG(INFO) << '[' << cfg_.ip << "] stopped";
    finished_ = true;
  }

  bool finished() const { return finished_.load(); }

  CameraHealth health() const {
    CameraHealth h;
    h.ip                    = cfg_.ip;
    h.events_received_total = events_received_total_.load();
    h.last_event_ms         = last_event_ms_.load();
    h.last_renew_ms         = last_renew_ms_.load();
    h.subscribed_at_ms      = subscribed_at_ms_.load();
    h.subscribed            = subscribed_at_ms_.load() != 0;
    return h;
  }

 private:
  std::atomic<bool>     finished_{false};
  std::atomic<uint64_t> events_received_total_{0};
  std::atomic<uint64_t> last_event_ms_{0};
  std::atomic<uint64_t> last_renew_ms_{0};
  std::atomic<uint64_t> subscribed_at_ms_{0};
  PublishHealthFn       publish_health_;

  void publish_current_health() {
    if (publish_health_) publish_health_(health());
  }

  void sleep_interruptible(int secs) {
    for (int i = 0; i < secs && running_; ++i)
      std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  // Sleep for @p ms milliseconds in 250 ms slices so a stop() during a
  // long backoff returns promptly.
  void sleep_interruptible_ms(uint64_t ms) {
    constexpr uint64_t kSliceMs = 250;
    for (uint64_t elapsed = 0; elapsed < ms && running_; elapsed += kSliceMs) {
      const uint64_t remaining = ms - elapsed;
      const uint64_t slice = remaining < kSliceMs ? remaining : kSliceMs;
      std::this_thread::sleep_for(std::chrono::milliseconds(slice));
    }
  }

  // Log msg, sleep for the current backoff, then double it (clamped) and
  // reset *failures so the next attempt starts a fresh streak.  The caller
  // resets *backoff_sec to the base on a successful subscription.
  void pause_and_reset(int* failures, int* backoff_sec, const std::string& msg) {
    LOG(WARNING) << msg << " (retry in " << *backoff_sec << "s)";
    sleep_interruptible(*backoff_sec);
    *backoff_sec = next_backoff_sec(*backoff_sec,
                                    cfg_.retry_backoff_base_sec,
                                    cfg_.retry_backoff_max_sec);
    *failures = 0;
  }

  std::string event_url() const {
    return cfg_.http_base() + "/onvif/event_service";
  }

  std::string device_url() const {
    return cfg_.http_base() + "/onvif/device_service";
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
          result.event_url = cfg_.http_base() + url_path(xaddr);
          LOG(INFO) << '[' << cfg_.ip << "] event service URL: " << result.event_url;
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
    RawSink::instance().record(cfg_.ip, url, action, body,
                               resp_or->status_code, resp_or->body);
    return resp_or;
  }

  // CreatePullPointSubscription -> Subscription{url, ref_params}
  // url is empty string on HTTP error; ref_params may be empty when the camera
  // does not include WS-Addressing ReferenceParameters.
  absl::StatusOr<Subscription> create_subscription(const std::string& ev_url) {
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
      // Event-subscription auth is gated on a dedicated ONVIF user with
      // Administrator privileges on most Hikvision firmware (and several
      // OEM rebadges).  GetServices works with the camera's web-UI admin,
      // but CreatePullPointSubscription rejects the same credentials with
      // ter:NotAuthorized.  Both Operator-level and User-level ONVIF
      // accounts produce the identical fault -- only Administrator works.
      // Confirmed against a Hikvision DS-2CD2387G2P-LSU/SL in issue #20.
      if (resp.body.find("NotAuthorized") != std::string::npos) {
        // Surface this in the admin Camera Health card so the user
        // doesn't have to dig through the journal to find the hint
        // below.  Cleared on first successful subscription event.
        mark_camera_needs_onvif_admin(cfg_.ip);
        LOG_FIRST_N(ERROR, 1)
            << '[' << cfg_.ip << "] PullPoint subscription rejected with "
            << "ter:NotAuthorized.\n"
            << "  The credentials are valid for the camera's web UI but "
            << "not for ONVIF event subscription.\n"
            << "  This is a separate user on most Hikvision firmware "
            << "(and OEM rebadges).  To fix:\n"
            << "    1. Open the camera's web UI ->\n"
            << "       Configuration -> Network -> Advanced Settings ->\n"
            << "       Integration Protocol -> ONVIF\n"
            << "    2. Tick \"Enable ONVIF\" and add a user with the\n"
            << "       \"Administrator\" level (Operator and User both\n"
            << "       produce this exact same fault).\n"
            << "    3. In UniFi Protect, edit the camera and replace the\n"
            << "       credentials with that ONVIF user's username and\n"
            << "       password (not the web-UI admin's).\n"
            << "  Reference: "
            << "https://github.com/danielwoz/ubiquiti-protect-onvif-event-listener/issues/20";
      }
      return Subscription{};
    }

    auto doc_or = XmlDoc::Create(resp.body);
    if (!doc_or.ok()) {
      LOG(ERROR) << '[' << cfg_.ip << "] parse sub URL: "
                 << doc_or.status().message();
      return Subscription{};
    }

    Subscription sub;
    sub.url = XmlDoc::trim(
      doc_or->text("//*[local-name()='SubscriptionReference']"
                   "/*[local-name()='Address']"));
    sub.ref_params = doc_or->inner_xml(
      "//*[local-name()='SubscriptionReference']"
      "/*[local-name()='ReferenceParameters']");
    if (!sub.ref_params.empty())
      LOG(INFO) << '[' << cfg_.ip << "] subscription has ReferenceParameters";
    // Successful subscription -- whatever auth we used here works.
    // Clear any stale "needs ONVIF Administrator" badge so a camera
    // that was once flagged but is now healthy stops nagging the user.
    clear_camera_needs_onvif_admin(cfg_.ip);
    return sub;
  }

  absl::Status renew(const std::string& sub_url, const std::string& ref_params) {
    static const char* ACTION =
      "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/RenewRequest";

    const std::string body =
      "    <wsnt:Renew>\n"
      "      <wsnt:TerminationTime>PT120S</wsnt:TerminationTime>\n"
      "    </wsnt:Renew>\n";

    auto soap = build_soap(cfg_.user, cfg_.password, body, sub_url, ACTION,
                           ref_params);
    auto resp_or = soap_post_r(sub_url, soap, ACTION, 15);
    if (!resp_or.ok()) return resp_or.status();
    if (resp_or->status_code == 200)
      LOG(INFO) << '[' << cfg_.ip << "] subscription renewed";
    else
      LOG_FIRST_N(WARNING, 1) << '[' << cfg_.ip << "] renew HTTP "
                              << resp_or->status_code;
    return absl::OkStatus();
  }

  // Politely tear the PullPoint subscription down.  Without this we just
  // stop renewing and the camera holds the subscription until its
  // termination time lapses -- so every restart leaks one, and a burst of
  // restarts (the admin page's Save & Restart, or re-adding a camera)
  // stacks them up.  Cameras and ONVIF proxies cap concurrent
  // subscribers, and once that cap is hit new subscriptions are refused
  // and events stop arriving entirely (issue #47).
  //
  // Best-effort by design: this runs on the shutdown path, so a short
  // timeout and a swallowed failure are preferable to delaying exit.  If
  // it doesn't land, behaviour is exactly what it was before.
  void unsubscribe(const std::string& sub_url, const std::string& ref_params) {
    if (sub_url.empty()) return;
    static const char* ACTION =
      "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/"
      "UnsubscribeRequest";

    const std::string body =
      "    <wsnt:Unsubscribe/>\n";

    auto soap = build_soap(cfg_.user, cfg_.password, body, sub_url, ACTION,
                           ref_params);
    auto resp_or = soap_post_r(sub_url, soap, ACTION, 5);
    if (!resp_or.ok()) {
      LOG(INFO) << '[' << cfg_.ip << "] unsubscribe failed (non-fatal): "
                << resp_or.status().message();
      return;
    }
    if (resp_or->status_code == 200)
      LOG(INFO) << '[' << cfg_.ip << "] subscription released";
    else
      LOG(INFO) << '[' << cfg_.ip << "] unsubscribe HTTP "
                << resp_or->status_code << " (non-fatal)";
  }

  absl::Status pull(const std::string& sub_url, const std::string& ref_params,
                    const std::string& alarm_url) {
    static const char* ACTION =
      "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/"
      "PullMessagesRequest";

    const std::string body =
      "    <tev:PullMessages>\n"
      "      <tev:MessageLimit>100</tev:MessageLimit>\n"
      "      <tev:Timeout>PT5S</tev:Timeout>\n"
      "    </tev:PullMessages>\n";

    auto soap = build_soap(cfg_.user, cfg_.password, body, sub_url, ACTION,
                           ref_params);
    // HTTP timeout > SOAP pull timeout (5 s) to allow for network headroom
    auto resp_or = soap_post_r(sub_url, soap, ACTION, 20);
    if (!resp_or.ok()) return resp_or.status();

    const HttpResponse& resp = *resp_or;
    if (resp.status_code != 200)
      return absl::InternalError(
        "PullMessages HTTP " + std::to_string(resp.status_code) +
        ": " + resp.body.substr(0, 300));

    auto events = parse_notifications(resp.body);
    if (!events.empty()) {
      LOG(INFO) << '[' << cfg_.ip << "] received " << events.size() << " event(s)";
      events_received_total_.fetch_add(events.size());
      last_event_ms_.store(util::now_ms());
    }

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
};

}  // anonymous namespace

// ============================================================
// OnvifListener public methods
// ============================================================
OnvifListener::OnvifListener() {
  ContentionProfiler::instance().register_mutex(
      &pending_mutex_, "onvif_listener.pending");
}
OnvifListener::~OnvifListener() = default;

void OnvifListener::add_camera(const CameraConfig& cfg) {
  cameras_.push_back(cfg);
}

void OnvifListener::add_camera_live(const CameraConfig& cfg) {
  absl::MutexLock lock(&pending_mutex_);
  pending_cameras_.push_back(cfg);
}

void OnvifListener::enable_raw_recording(const std::string& path) {
  RawSink::instance().enable_disk(path);
}

void OnvifListener::run(EventCallback cb) {
  running_ = true;
  // stop() sets stop_requested_ before clearing running_, so either this
  // check sees it or stop()'s own store lands after ours.
  if (stop_requested_) running_ = false;

  // Build workers (heap-allocated so their address is stable across moves)
  std::vector<std::unique_ptr<CameraWorker>> workers;
  workers.reserve(cameras_.size());
  for (const auto& cam : cameras_)
    workers.push_back(
      std::make_unique<CameraWorker>(cam, cb, running_,
          [this](const CameraHealth& h){ this->publish_health(h); }));

  // Launch one thread per camera
  std::vector<std::thread> threads;
  threads.reserve(workers.size());
  for (auto& w : workers) {
    CameraWorker* ptr = w.get();
    threads.emplace_back([ptr] { ptr->run(); });
  }

  // Block until stop() is called or every worker finishes on its own
  // (e.g. max_consecutive_failures reached).  When there are no workers
  // (no third-party cameras configured in Protect) keep running until
  // stop() so the service stays alive for the motion poller, log server,
  // admin page, etc. instead of exiting silently.  Also drain any cameras
  // hot-added via add_camera_live() since the last tick.
  while (running_) {
    // Drain pending hot-added cameras.
    {
      std::vector<CameraConfig> hot_adds;
      {
        absl::MutexLock lock(&pending_mutex_);
        hot_adds.swap(pending_cameras_);
      }
      for (auto& cfg : hot_adds) {
        cameras_.push_back(cfg);
        workers.push_back(
            std::make_unique<CameraWorker>(cfg, cb, running_,
                [this](const CameraHealth& h){ this->publish_health(h); }));
        CameraWorker* ptr = workers.back().get();
        threads.emplace_back([ptr] { ptr->run(); });
      }
    }

    if (!workers.empty()) {
      bool all_done = true;
      for (auto& w : workers) {
        if (!w->finished()) {
          all_done = false;
          break;
        }
      }
      if (all_done) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  // Signal all workers to stop (idempotent if already false).
  running_ = false;

  // Give threads a deadline to finish after the stop signal.  If a camera
  // thread is stuck in a blocking curl call we detach it rather than
  // blocking the entire process forever.
  constexpr int kJoinTimeoutSec = 30;
  auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(kJoinTimeoutSec);
  for (size_t i = 0; i < threads.size(); ++i) {
    if (workers[i]->finished()) {
      threads[i].join();
      continue;
    }
    bool joined = false;
    while (std::chrono::steady_clock::now() < deadline) {
      if (workers[i]->finished()) {
        threads[i].join();
        joined = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!joined) {
      LOG(ERROR) << '[' << cameras_[i].ip
                 << "] thread did not stop within " << kJoinTimeoutSec
                 << "s -- detaching";
      threads[i].detach();
    }
  }
}

void OnvifListener::stop() {
  stop_requested_ = true;
  running_ = false;
}

// ============================================================
// RawSink — process-global capture of recent SOAP exchanges
// ============================================================
RawSink::RawSink() {
  ContentionProfiler::instance().register_mutex(&mu_, "raw_sink");
}

RawSink& RawSink::instance() {
  static RawSink kInstance;
  return kInstance;
}

void RawSink::enable_disk(const std::string& path) {
  absl::MutexLock lk(&mu_);
  if (disk_.is_open()) disk_.close();
  if (!path.empty()) {
    disk_.open(path, std::ios::app);
    if (!disk_.is_open()) {
      std::cerr << "[onvif] raw recording to disk disabled: cannot open "
                << path << '\n';
    }
  }
}

void RawSink::record(const std::string& camera_ip,
                     const std::string& url,
                     const std::string& soap_action,
                     const std::string& request,
                     int64_t            response_status,
                     const std::string& response) {
  // Build the JSONL line uncompressed first — it is what we tee to disk
  // (when --raw_log is set) and what we feed to zstd for the ring entry.
  std::string line;
  line.reserve(request.size() + response.size() + 256);
  line += '{';
  line += util::json_str("timestamp")       + ':'
       +  util::json_str(util::utc_now_iso8601_ms()) + ',';
  line += util::json_str("camera_ip")       + ':'
       +  util::json_str(camera_ip) + ',';
  line += util::json_str("url")             + ':'
       +  util::json_str(url) + ',';
  line += util::json_str("soap_action")     + ':'
       +  util::json_str(soap_action) + ',';
  line += util::json_str("request")         + ':'
       +  util::json_str(request) + ',';
  line += util::json_str("response_status") + ':'
       +  std::to_string(response_status) + ',';
  line += util::json_str("response")        + ':'
       +  util::json_str(response);
  line += "}\n";

  // Compress for the in-memory ring.  ZSTD_compressBound is generous
  // (~roughly +13 bytes on top of input) — covers the worst case where
  // zstd cannot compress at all (e.g. a tiny line) and falls back to a
  // raw block.  Done outside the lock so concurrent records don't
  // serialise on each other through the compressor.
  std::string compressed;
  compressed.resize(ZSTD_compressBound(line.size()));
  const size_t cz = ZSTD_compress(compressed.data(), compressed.size(),
                                  line.data(),       line.size(),
                                  kZstdLevel);
  if (ZSTD_isError(cz)) {
    // Compression failed (should not happen with a fresh stack buffer of
    // adequate size, but we still want bounded memory if it does).
    // Fall back to storing the raw line so the data is preserved.
    compressed = line;
  } else {
    compressed.resize(cz);
  }

  absl::MutexLock lk(&mu_);
  if (disk_.is_open()) {
    disk_ << line;
    disk_.flush();
  }
  ring_bytes_ += compressed.size();
  ring_.push_back(std::move(compressed));
  while (ring_.size() > kMaxEntries ||
         (ring_bytes_ > kMaxBytes && !ring_.empty())) {
    ring_bytes_ -= ring_.front().size();
    ring_.pop_front();
  }
}

std::string RawSink::snapshot() const {
  // Copy the compressed ring under the lock so we can decompress without
  // holding the recorder lock open across (potentially expensive) zstd
  // calls.  The cost is one extra std::string copy per entry.
  std::vector<std::string> copy;
  {
    absl::MutexLock lk(&mu_);
    copy.reserve(ring_.size());
    for (const auto& e : ring_) copy.push_back(e);
  }

  std::string out;
  // Heuristic preallocate: assume ~5x compression ratio.
  size_t total_compressed = 0;
  for (const auto& e : copy) total_compressed += e.size();
  out.reserve(total_compressed * 5);

  for (const auto& e : copy) {
    // ZSTD_getFrameContentSize tells us the exact decompressed size from
    // the frame header (we wrote whole single-shot frames in record()).
    const unsigned long long sz =  // NOLINT(runtime/int)
        ZSTD_getFrameContentSize(e.data(), e.size());
    if (sz == ZSTD_CONTENTSIZE_ERROR ||
        sz == ZSTD_CONTENTSIZE_UNKNOWN) {
      // Not a zstd frame — must be a raw line preserved by the
      // compression-failure fallback in record().  Append verbatim.
      out.append(e);
      continue;
    }
    const size_t before = out.size();
    out.resize(before + static_cast<size_t>(sz));
    const size_t dz = ZSTD_decompress(out.data() + before, sz,
                                      e.data(),            e.size());
    if (ZSTD_isError(dz)) {
      // Decompression failed on this entry — drop the partial growth
      // and skip it rather than letting one bad frame poison the dump.
      out.resize(before);
      continue;
    }
    out.resize(before + dz);
  }
  return out;
}

size_t RawSink::entry_count() const {
  absl::MutexLock lk(&mu_);
  return ring_.size();
}

size_t RawSink::compressed_bytes() const {
  absl::MutexLock lk(&mu_);
  return ring_bytes_;
}

// ---------------------------------------------------------------
// OnvifListener: per-camera health surface
// ---------------------------------------------------------------

void OnvifListener::publish_health(const CameraHealth& h) {
  absl::MutexLock lock(&healths_mutex_);
  healths_[h.ip] = h;
}

std::vector<CameraHealth> OnvifListener::healths() const {
  absl::MutexLock lock(&healths_mutex_);
  std::vector<CameraHealth> out;
  out.reserve(healths_.size());
  for (const auto& [ip, h] : healths_) out.push_back(h);
  return out;
}

}  // namespace onvif
