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
#include <fcntl.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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

bool is_empty_notification(const OnvifEvent& ev) {
  return ev.topic.empty() && ev.source.empty() && ev.data.empty();
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

namespace detail {

// Requests another thread makes of one CameraWorker (nudge, resubscribe).
// Shared by the worker and the listener's registry, so it stays valid for
// a worker that outlives run().
struct WorkerControl {
  explicit WorkerControl(std::string camera_ip) : ip(std::move(camera_ip)) {}

  const std::string       ip;
  std::mutex              mu;
  std::condition_variable cv;
  bool wake{false};           // end the current (or next) interruptible sleep
  bool reset_backoff{false};  // the next pause uses the base backoff
  bool resubscribe{false};    // drop the subscription and create a new one
};

}  // namespace detail

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

// Short description of a failed SOAP exchange for logs and the state tap:
// the fault's code values and reason ("SOAP-ENV:Sender
// ter:BlockedThreadUpperLimit: text") when @p body is a SOAP fault, else
// the first 300 bytes of @p body.  Fault envelopes often spend their first
// few kilobytes on namespace declarations, so a plain prefix would hide
// the fault code.
std::string failure_detail(const std::string& body) {
  constexpr size_t kMaxDetail = 300;
  auto doc_or = XmlDoc::Create(body);
  if (doc_or.ok()) {
    std::string codes;
    std::set<std::string> seen;
    auto values = doc_or->xpath(
        "//*[local-name()='Fault']//*[local-name()='Value' or "
        "local-name()='faultcode']");
    if (values && values->type == XPATH_NODESET && values->nodesetval) {
      for (int i = 0; i < values->nodesetval->nodeNr; ++i) {
        xmlChar* c = xmlNodeGetContent(values->nodesetval->nodeTab[i]);
        if (!c) continue;
        const std::string v = XmlDoc::trim(reinterpret_cast<char*>(c));
        xmlFree(c);
        if (v.empty() || !seen.insert(v).second) continue;
        if (!codes.empty()) codes += ' ';
        codes += v;
      }
    }
    const std::string reason = XmlDoc::trim(doc_or->text(
        "//*[local-name()='Fault']/*[local-name()='Reason']"
        "/*[local-name()='Text'] | "
        "//*[local-name()='Fault']/*[local-name()='faultstring']"));
    if (!codes.empty() || !reason.empty()) {
      std::string out = codes;
      if (!reason.empty()) out += (out.empty() ? "" : ": ") + reason;
      return out.substr(0, kMaxDetail);
    }
  }
  return body.substr(0, kMaxDetail);
}

// Upper case without separators: "f0:00:00:fd:ae:d9" -> "F00000FDAED9".
std::string normalize_mac(const std::string& mac) {
  std::string out;
  for (char c : mac) {
    if (c == ':' || c == '-' || c == ' ') continue;
    out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

// -------------------------------------------------------
// Subscriptions kept across a restart (event_proxy cameras)
// -------------------------------------------------------
struct SavedSubscription {
  std::string mac;
  std::string ip;
  std::string url;
  std::string ref_params;
  std::string alarm_url;
  int64_t     expires_ms{0};  // Unix ms when the camera drops it
};

// Subscriptions handed over by workers that stopped while healthy.
struct ResumeCollector {
  std::mutex                     mu;
  bool                           closed{false};  // later hand-overs are refused
  std::vector<SavedSubscription> entries;
};

// A saved subscription belongs to @p cam when both carry a MAC and the MACs
// agree, or, when either has none, when the ips agree.
bool saved_matches(const SavedSubscription& s, const CameraConfig& cam) {
  const std::string a = normalize_mac(s.mac);
  const std::string b = normalize_mac(cam.mac);
  if (!a.empty() && !b.empty()) return a == b;
  return !s.ip.empty() && s.ip == cam.ip;
}

std::string saved_subscriptions_json(
    const std::vector<SavedSubscription>& subs) {
  std::string out = "{\"subscriptions\":[";
  for (size_t i = 0; i < subs.size(); ++i) {
    const SavedSubscription& s = subs[i];
    if (i) out += ',';
    out += "{\"mac\":" + util::json_str(s.mac) +
           ",\"ip\":" + util::json_str(s.ip) +
           ",\"url\":" + util::json_str(s.url) +
           ",\"ref_params\":" + util::json_str(s.ref_params) +
           ",\"alarm_url\":" + util::json_str(s.alarm_url) +
           ",\"expires_ms\":" + std::to_string(s.expires_ms) + "}";
  }
  out += "]}\n";
  return out;
}

// Reader for the file saved_subscriptions_json() writes: an object whose
// "subscriptions" member is an array of flat objects.  Members it does not
// know are skipped whatever their type.
class SavedSubscriptionsReader {
 public:
  explicit SavedSubscriptionsReader(const std::string& text) : s_(text) {}

  bool read(std::vector<SavedSubscription>* out) {
    ws();
    if (!eat('{')) return false;
    ws();
    if (eat('}')) return true;
    for (;;) {
      std::string key;
      ws();
      if (!read_string(&key)) return false;
      ws();
      if (!eat(':')) return false;
      ws();
      if (key == "subscriptions") {
        if (!read_entries(out)) return false;
      } else if (!skip_value(0)) {
        return false;
      }
      ws();
      if (eat(',')) continue;
      return eat('}');
    }
  }

 private:
  bool read_entries(std::vector<SavedSubscription>* out) {
    if (!eat('[')) return false;
    ws();
    if (eat(']')) return true;
    for (;;) {
      SavedSubscription e;
      ws();
      if (!read_entry(&e)) return false;
      if (!e.url.empty()) out->push_back(std::move(e));
      ws();
      if (eat(',')) continue;
      return eat(']');
    }
  }

  bool read_entry(SavedSubscription* e) {
    if (!eat('{')) return false;
    ws();
    if (eat('}')) return true;
    for (;;) {
      std::string key;
      ws();
      if (!read_string(&key)) return false;
      ws();
      if (!eat(':')) return false;
      ws();
      std::string* field = key == "mac"        ? &e->mac
                         : key == "ip"         ? &e->ip
                         : key == "url"        ? &e->url
                         : key == "ref_params" ? &e->ref_params
                         : key == "alarm_url"  ? &e->alarm_url
                                               : nullptr;
      if (field) {
        if (!read_string(field)) return false;
      } else if (key == "expires_ms") {
        if (!read_integer(&e->expires_ms)) return false;
      } else if (!skip_value(0)) {
        return false;
      }
      ws();
      if (eat(',')) continue;
      return eat('}');
    }
  }

  bool read_string(std::string* out) {
    if (!eat('"')) return false;
    out->clear();
    while (p_ < s_.size()) {
      const char c = s_[p_++];
      if (c == '"') return true;
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (p_ >= s_.size()) return false;
      const char esc = s_[p_++];
      switch (esc) {
        case '"': case '\\': case '/': out->push_back(esc); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          if (p_ + 4 > s_.size()) return false;
          unsigned cp = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = s_[p_++];
            cp <<= 4;
            if (h >= '0' && h <= '9')      cp |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
            else
              return false;
          }
          append_utf8(cp, out);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  bool read_integer(int64_t* v) {
    const size_t start = p_;
    if (p_ < s_.size() && s_[p_] == '-') ++p_;
    while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_])))
      ++p_;
    if (p_ == start) return false;
    *v = std::strtoll(s_.substr(start, p_ - start).c_str(), nullptr, 10);
    return true;
  }

  bool skip_value(int depth) {
    if (depth > 32 || p_ >= s_.size()) return false;
    const char c = s_[p_];
    if (c == '"') {
      std::string ignored;
      return read_string(&ignored);
    }
    if (c == '{' || c == '[') {
      const char close = c == '{' ? '}' : ']';
      ++p_;
      ws();
      if (eat(close)) return true;
      for (;;) {
        ws();
        if (c == '{') {
          std::string key;
          if (!read_string(&key)) return false;
          ws();
          if (!eat(':')) return false;
          ws();
        }
        if (!skip_value(depth + 1)) return false;
        ws();
        if (eat(',')) continue;
        return eat(close);
      }
    }
    const size_t start = p_;
    while (p_ < s_.size() &&
           (std::isalnum(static_cast<unsigned char>(s_[p_])) ||
            s_[p_] == '-' || s_[p_] == '+' || s_[p_] == '.'))
      ++p_;
    return p_ > start;
  }

  static void append_utf8(unsigned cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  void ws() {
    while (p_ < s_.size() &&
           std::isspace(static_cast<unsigned char>(s_[p_])))
      ++p_;
  }
  bool eat(char c) {
    if (p_ < s_.size() && s_[p_] == c) {
      ++p_;
      return true;
    }
    return false;
  }

  const std::string& s_;
  size_t             p_{0};
};

// Whole file into @p out.  False when it cannot be opened.
bool read_whole_file(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

// Replace @p path with @p data (mode 0600) through a temporary file that
// is fsync'ed before the rename, so a crash leaves the old file or the new
// one, never a partial one.
bool write_file_atomic(const std::string& path, const std::string& data) {
  const std::string tmp = path + ".tmp";
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                      0600);
  if (fd < 0) return false;
  bool ok = fchmod(fd, 0600) == 0;
  size_t off = 0;
  while (ok && off < data.size()) {
    const ssize_t n = write(fd, data.data() + off, data.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      ok = false;
    } else {
      off += static_cast<size_t>(n);
    }
  }
  ok = ok && fsync(fd) == 0;
  ok = close(fd) == 0 && ok;
  ok = ok && std::rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok) {
    unlink(tmp.c_str());
    return false;
  }
  // Persist the rename itself (best effort).
  const auto slash = path.rfind('/');
  const std::string dir = slash == std::string::npos ? "."
                        : slash == 0 ? "/" : path.substr(0, slash);
  const int dfd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    fsync(dfd);
    close(dfd);
  }
  return true;
}

// Load and delete the subscriptions file the previous run() wrote.  A
// missing or unreadable file yields nothing.
std::vector<SavedSubscription> take_saved_subscriptions(
    const std::string& path) {
  std::vector<SavedSubscription> subs;
  std::string text;
  if (!read_whole_file(path, &text)) return subs;
  if (!SavedSubscriptionsReader(text).read(&subs)) {
    LOG(WARNING) << "[onvif] ignoring unreadable saved subscriptions file "
                 << path;
    subs.clear();
  } else {
    LOG(INFO) << "[onvif] loaded " << subs.size()
              << " saved subscription(s) from " << path;
  }
  if (std::remove(path.c_str()) != 0 && errno != ENOENT)
    LOG(WARNING) << "[onvif] cannot delete " << path << ": "
                 << std::strerror(errno);
  return subs;
}

// Write @p subs to @p path, or remove @p path when there are none.
// Returns false when @p subs could not be written.
bool store_saved_subscriptions(const std::string& path,
                               const std::vector<SavedSubscription>& subs) {
  if (subs.empty()) {
    if (std::remove(path.c_str()) != 0 && errno != ENOENT)
      LOG(WARNING) << "[onvif] cannot delete " << path << ": "
                   << std::strerror(errno);
    return true;
  }
  if (!write_file_atomic(path, saved_subscriptions_json(subs))) {
    LOG(WARNING) << "[onvif] cannot save " << subs.size()
                 << " subscription(s) to " << path << ": "
                 << std::strerror(errno);
    return false;
  }
  LOG(INFO) << "[onvif] saved " << subs.size() << " subscription(s) to "
            << path;
  return true;
}

// Unsubscribe @p url with @p cam's credentials.  Returns the HTTP status,
// or an error when the request did not complete.
absl::StatusOr<int64_t> send_unsubscribe(const CameraConfig& cam,
                                         const std::string& url,
                                         const std::string& ref_params,
                                         int timeout_sec) {
  static const char* ACTION =
    "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/"
    "UnsubscribeRequest";
  const std::string body = "    <wsnt:Unsubscribe/>\n";
  const std::string soap = build_soap(cam.user, cam.password, body, url,
                                      ACTION, ref_params);
  auto resp_or = soap_post(url, soap, ACTION, timeout_sec);
  if (!resp_or.ok()) return resp_or.status();
  RawSink::instance().record(cam.ip, url, ACTION, soap,
                             resp_or->status_code, resp_or->body);
  return resp_or->status_code;
}

// Unsubscribe @p s, best effort, with the credentials of the camera in
// @p cams it belongs to (a MAC match wins over an ip one), or none.
void release_saved_subscription(const SavedSubscription& s,
                                const std::vector<CameraConfig>& cams) {
  const CameraConfig* cam = nullptr;
  for (const auto& c : cams) {
    if (!saved_matches(s, c)) continue;
    if (!cam || (!normalize_mac(c.mac).empty() &&
                 !normalize_mac(s.mac).empty()))
      cam = &c;
  }
  CameraConfig creds;
  if (cam) creds = *cam;
  if (creds.ip.empty()) creds.ip = s.ip;
  auto status_or = send_unsubscribe(creds, s.url, s.ref_params, 5);
  LOG(INFO) << '[' << creds.ip << "] released saved subscription " << s.url
            << ": "
            << (status_or.ok() ? "HTTP " + std::to_string(*status_or)
                               : std::string(status_or.status().message()));
}

// -------------------------------------------------------
// AsyncDelivery -- one camera's events to the callback from a thread of
// its own, so a slow callback (snapshot fetch, detection, DB) never delays
// the camera's next pull.  Order is kept; when kMaxQueued events are
// waiting the oldest is dropped.
// -------------------------------------------------------
class AsyncDelivery {
 public:
  static constexpr size_t kMaxQueued = 1000;

  AsyncDelivery(std::string ip, EventCallback cb)
    : ip_(std::move(ip)), cb_(std::move(cb)), thread_([this] { loop(); }) {}

  ~AsyncDelivery() { finish(std::chrono::milliseconds(0)); }

  AsyncDelivery(const AsyncDelivery&)            = delete;
  AsyncDelivery& operator=(const AsyncDelivery&) = delete;

  void push(OnvifEvent ev) {
    uint64_t dropped = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (q_.size() >= kMaxQueued) {
        q_.pop_front();
        ++dropped_;
        const auto now = std::chrono::steady_clock::now();
        if (!warned_ || now - last_warn_ >= std::chrono::minutes(1)) {
          warned_    = true;
          last_warn_ = now;
          dropped    = dropped_;
          dropped_   = 0;
        }
      }
      q_.push_back(std::move(ev));
    }
    cv_.notify_one();
    if (dropped) {
      LOG(WARNING) << '[' << ip_ << "] event queue full (" << kMaxQueued
                   << "): dropped the " << dropped << " oldest event(s)";
    }
  }

  // Deliver what is still queued for at most @p drain, then stop the
  // delivery thread and wait for it.  Idempotent.
  void finish(std::chrono::milliseconds drain) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!stopping_) {
        stopping_       = true;
        drain_deadline_ = std::chrono::steady_clock::now() + drain;
      }
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
  }

 private:
  void loop() {
    for (;;) {
      OnvifEvent ev;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return stopping_ || !q_.empty(); });
        if (q_.empty()) return;
        if (stopping_ &&
            std::chrono::steady_clock::now() >= drain_deadline_) {
          LOG(WARNING) << '[' << ip_ << "] discarding " << q_.size()
                       << " undelivered event(s) at shutdown";
          q_.clear();
          return;
        }
        ev = std::move(q_.front());
        q_.pop_front();
      }
      cb_(ev);
    }
  }

  const std::string                     ip_;
  const EventCallback                   cb_;
  std::mutex                            mu_;
  std::condition_variable               cv_;
  std::deque<OnvifEvent>                q_;
  bool                                  stopping_{false};
  std::chrono::steady_clock::time_point drain_deadline_{};
  uint64_t                              dropped_{0};  // since the last warning
  bool                                  warned_{false};
  std::chrono::steady_clock::time_point last_warn_{};
  std::thread                           thread_;  // last: starts in the ctor
};

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
// What a CameraWorker shares with its listener besides the camera and the
// event callback.
struct WorkerHooks {
  std::function<void(const CameraHealth&)> publish_health;
  OnvifListener::RawResponseTap            raw_tap;
  OnvifListener::UpstreamStateTap          state_tap;
  OnvifListener::HeartbeatSuffixFn         heartbeat_suffix;
  std::shared_ptr<detail::WorkerControl>   control;
  // Set when the listener keeps event_proxy subscriptions across restarts.
  std::shared_ptr<ResumeCollector>         resume_out;
  // Subscription this camera held when the previous run() stopped.
  std::optional<SavedSubscription>         resume_in;
};

class CameraWorker {
 public:
  // cfg, cb and hooks are copied: a worker can outlive both the caller's
  // camera list (hot-added cameras, cameras_ reallocation) and run() itself
  // (a thread detached at shutdown).
  CameraWorker(CameraConfig cfg,
               EventCallback cb,
               const std::atomic<bool>& running,
               WorkerHooks hooks = {})
    : hooks_(std::move(hooks)),
      cfg_(std::move(cfg)), cb_(std::move(cb)), running_(running) {
    if (!hooks_.control)
      hooks_.control = std::make_shared<detail::WorkerControl>(cfg_.ip);
  }

  void run() {
    LOG(INFO) << '[' << cfg_.ip << "] started";
    if (cfg_.event_proxy)
      async_ = std::make_unique<AsyncDelivery>(cfg_.ip, cb_);
    const int max_failures = cfg_.max_consecutive_failures;  // 0 = unlimited
    int consecutive_failures = 0;
    int backoff_sec = base_backoff_sec();
    std::optional<SavedSubscription> saved = std::move(hooks_.resume_in);
    hooks_.resume_in.reset();

    while (running_) {
      Subscription sub;
      std::string  alarm_url;
      bool resumed = false;
      if (saved) {
        // A subscription taken over from here on satisfies any pending
        // resubscribe() request.
        take_resubscribe();
        resumed = try_resume(*saved, &sub, &alarm_url);
        saved.reset();
        if (!resumed && !running_) break;
      }

      if (!resumed) {
        // Discover event and alarm service URLs via GetServices
        // (IncludeCapability=true).  Re-runs on every outer loop iteration
        // so a camera restart is handled cleanly.
        const DiscoveredServices sv = discover_services();
        alarm_url = sv.alarm_url;

        // A subscription created from here on satisfies any pending
        // resubscribe() request.
        take_resubscribe();
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

        sub = *sub_or;
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
      }

      // Successful subscription -- reset the failure counter and the backoff.
      consecutive_failures = 0;
      backoff_sec = base_backoff_sec();
      if (resumed) {
        emit(UpstreamEvent::kResumed, 200, sub.url);
        // The resumed subscription carries on from the old stream, so ask
        // the camera for its current state, as after a new subscription.
        set_synchronization_point(sub.url, sub.ref_params);
      } else {
        LOG(INFO) << '[' << cfg_.ip << "] subscription -> " << sub.url;
        emit(UpstreamEvent::kSubscribed, 200, sub.url);
        // A shared camera's current state, as Initialized messages, is
        // what Protect's new subscribers are given first.  Some cameras
        // send it on a new subscription only when asked.
        if (cfg_.event_proxy) set_synchronization_point(sub.url, sub.ref_params);
      }
      subscribed_at_ms_.store(util::now_ms());
      last_renew_ms_.store(util::now_ms());

      auto renew_at =
        std::chrono::steady_clock::now() + renew_interval();
      auto heartbeat_at =
        std::chrono::steady_clock::now() + heartbeat_interval();

      // Why the inner loop ended.
      enum class End { kStopped, kResubscribe, kRenewFailed, kPullFailed };
      End end = End::kStopped;
      while (running_) {
        if (take_resubscribe()) {
          LOG(INFO) << '[' << cfg_.ip << "] resubscribing on request";
          end = End::kResubscribe;
          break;
        }
        if (std::chrono::steady_clock::now() >= renew_at) {
          int         renew_status = 0;
          std::string renew_detail;
          absl::Status rs = renew(sub.url, sub.ref_params, 15,
                                  &renew_status, &renew_detail);
          if (rs.ok()) {
            last_renew_ms_.store(util::now_ms());
          } else {
            warn_renew_failed(rs);
            // A subscription the camera no longer renews can keep answering
            // pulls with empty 200s; for a shared camera that would starve
            // Protect without any error, so start over.
            if (cfg_.event_proxy) {
              emit(UpstreamEvent::kRenewFailed, renew_status, renew_detail);
              end = End::kRenewFailed;
              break;
            }
          }
          renew_at = std::chrono::steady_clock::now() + renew_interval();
        }
        if (std::chrono::steady_clock::now() >= heartbeat_at) {
          const uint64_t now = util::now_ms();
          const uint64_t le  = last_event_ms_.load();
          const uint64_t lr  = last_renew_ms_.load();
          const uint64_t empty = empty_received_total_.load();
          LOG(INFO) << '[' << cfg_.ip << "] alive: events_recv="
                    << events_received_total_.load()
                    << (empty ? " empty=" + std::to_string(empty) : std::string())
                    << " renew_age=" << (lr ? (now - lr) / 1000 : 0) << "s"
                    << " last_event="
                    << (le ? std::to_string((now - le) / 1000) + "s"
                           : std::string("never"))
                    << heartbeat_suffix();
          heartbeat_at = std::chrono::steady_clock::now()
                       + heartbeat_interval();
        }
        if (!running_) break;
        const uint64_t events_before = events_received_total_.load();
        auto pull_started = std::chrono::steady_clock::now();
        int pull_status = 0;
        absl::Status ps = pull(sub.url, sub.ref_params, alarm_url,
                               &pull_status);
        auto pull_dur = std::chrono::steady_clock::now() - pull_started;
        const bool got_events =
            events_received_total_.load() > events_before;
        if (ps.ok())
          emit(UpstreamEvent::kPullOk, pull_status, "");
        else
          emit(UpstreamEvent::kPullFailed, pull_status,
               std::string(ps.message()));

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
        // minutes; a shared camera's cap is short because Protect sees
        // its events only as fast as we pull them.
        constexpr auto kLongPollHonored = std::chrono::milliseconds(4000);
        const auto max_idle_sleep = cfg_.event_proxy
                                      ? std::chrono::milliseconds(1000)
                                      : std::chrono::milliseconds(30000);
        if (ps.ok() && !got_events && pull_dur < kLongPollHonored) {
          auto sleep_dur = pull_dur * 3;
          if (sleep_dur > max_idle_sleep) sleep_dur = max_idle_sleep;
          sleep_interruptible_ms(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  sleep_dur).count(), /*idle=*/true);
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
            log_limited(&pull_warn_, kPullWarnInterval,
                        std::string("pull error: ") + std::string(ps.message()) +
                            " -- resubscribing in " +
                            std::to_string(cfg_.retry_interval_sec) + "s" +
                            (max_failures > 0
                               ? " (" + std::to_string(consecutive_failures) +
                                 "/" + std::to_string(max_failures) + ")"
                               : ""));
            sleep_interruptible(cfg_.retry_interval_sec);
          }
          end = End::kPullFailed;
          break;
        }
      }
      // Inner loop exited (subscription dropped); reflect that in health.
      // Release the subscription so the camera doesn't hold it until its
      // termination time lapses.  Covers both the stop() path and a
      // resubscribe after an error, which is where the leaks accumulate.
      // A shared camera stopped while healthy keeps its subscription
      // instead, for the next run() to take over.
      const bool kept = end == End::kStopped && keep_for_restart(sub, alarm_url);
      if (!kept) unsubscribe(sub.url, sub.ref_params);
      subscribed_at_ms_.store(0);
      emit(UpstreamEvent::kUnsubscribed, 0,
           kept                       ? "kept for restart"
           : end == End::kResubscribe ? "resubscribe requested"
           : end == End::kRenewFailed ? "renew failed"
           : end == End::kPullFailed  ? "pull failed"
                                      : "stopped");
    }
    // Never tried (stop() came first): leave it for the next run().
    if (saved) hand_over(*saved);
    // Delivery drains for at most 5 s and is joined before finished_ is
    // set, so run() never destroys a worker whose delivery thread runs.
    if (async_) async_->finish(std::chrono::seconds(5));
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
  // A saved subscription is taken over only with at least this long left.
  static constexpr int64_t kResumeMinLeftMs = 15000;
  // Termination time asked for when a subscription is kept for restart.
  static constexpr int64_t kKeepForRestartMs = 120000;

  std::atomic<bool>     finished_{false};
  std::atomic<uint64_t> events_received_total_{0};
  std::atomic<uint64_t> empty_received_total_{0};  // of events_received_total_
  std::atomic<uint64_t> last_event_ms_{0};
  std::atomic<uint64_t> last_renew_ms_{0};
  std::atomic<uint64_t> subscribed_at_ms_{0};
  WorkerHooks           hooks_;
  std::unique_ptr<AsyncDelivery> async_;  // event_proxy cameras only

  // A warning logged at most once per interval; see log_limited().
  struct LimitedWarning {
    std::chrono::steady_clock::time_point last{};
    bool     warned{false};
    uint64_t since{0};  // occurrences logged at INFO since the warning
  };
  static constexpr std::chrono::minutes kRenewWarnInterval{10};
  static constexpr std::chrono::hours   kPullWarnInterval{1};
  LimitedWarning        renew_warn_;
  LimitedWarning        pull_warn_;

  void publish_current_health() {
    if (hooks_.publish_health) hooks_.publish_health(health());
  }

  // True when emit() reaches a state tap.
  bool state_tapped() const {
    return cfg_.event_proxy && hooks_.state_tap != nullptr;
  }

  void emit(UpstreamEvent ev, int http_status, const std::string& detail) {
    if (state_tapped()) hooks_.state_tap(cfg_, ev, http_status, detail);
  }

  std::string heartbeat_suffix() const {
    if (cfg_.event_proxy && hooks_.heartbeat_suffix)
      return hooks_.heartbeat_suffix(cfg_);
    return {};
  }

  std::chrono::seconds renew_interval() const {
    return std::chrono::seconds(cfg_.renew_interval_sec > 0
                                    ? cfg_.renew_interval_sec : 90);
  }

  std::chrono::seconds heartbeat_interval() const {
    return std::chrono::seconds(cfg_.heartbeat_interval_sec > 0
                                    ? cfg_.heartbeat_interval_sec : 60);
  }

  int base_backoff_sec() const {
    return next_backoff_sec(0, cfg_.retry_backoff_base_sec,
                            cfg_.retry_backoff_max_sec);
  }

  // Log "[ip] <msg>" at WARNING when @p w has not warned within @p every,
  // else at INFO, so every occurrence is in the log but the warnings stay
  // few.
  void log_limited(LimitedWarning* w, std::chrono::steady_clock::duration every,
                   const std::string& msg) {
    const auto now = std::chrono::steady_clock::now();
    if (w->warned && now - w->last < every) {
      ++w->since;
      LOG(INFO) << '[' << cfg_.ip << "] " << msg;
      return;
    }
    LOG(WARNING) << '[' << cfg_.ip << "] " << msg
                 << (w->since
                       ? " (" + std::to_string(w->since) +
                         " more since the last warning)"
                       : "");
    w->warned = true;
    w->last   = now;
    w->since  = 0;
  }

  void warn_renew_failed(const absl::Status& st) {
    log_limited(&renew_warn_, kRenewWarnInterval,
                "renew error: " + std::string(st.message()) +
                    (cfg_.event_proxy ? " -- resubscribing" : ""));
  }

  bool take_resubscribe() {
    detail::WorkerControl& c = *hooks_.control;
    std::lock_guard<std::mutex> lk(c.mu);
    const bool requested = c.resubscribe;
    c.resubscribe = false;
    return requested;
  }

  bool take_reset_backoff() {
    detail::WorkerControl& c = *hooks_.control;
    std::lock_guard<std::mutex> lk(c.mu);
    const bool requested = c.reset_backoff;
    c.reset_backoff = false;
    return requested;
  }

  void sleep_interruptible(int secs) {
    if (secs > 0) sleep_interruptible_ms(static_cast<uint64_t>(secs) * 1000);
  }
  // Sleep for @p ms milliseconds, waking every 250 ms so a stop() during a
  // long backoff returns promptly.  A nudge() ends the sleep at once, and
  // so does a resubscribe() when @p idle (the subscription is live).
  void sleep_interruptible_ms(uint64_t ms, bool idle = false) {
    constexpr auto kSlice = std::chrono::milliseconds(250);
    detail::WorkerControl& c = *hooks_.control;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    std::unique_lock<std::mutex> lk(c.mu);
    while (running_ && !c.wake && !(idle && c.resubscribe)) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) break;
      const auto left = deadline - now;
      c.cv.wait_for(lk, left < kSlice ? left : kSlice);
    }
    c.wake = false;
  }

  // Log msg, sleep for the current backoff, then double it (clamped) and
  // reset *failures so the next attempt starts a fresh streak.  The caller
  // resets *backoff_sec to the base on a successful subscription; a
  // nudge() resets it too.
  void pause_and_reset(int* failures, int* backoff_sec, const std::string& msg) {
    if (take_reset_backoff()) *backoff_sec = base_backoff_sec();
    LOG(WARNING) << msg << " (retry in " << *backoff_sec << "s)";
    sleep_interruptible(*backoff_sec);
    *backoff_sec = take_reset_backoff()
                     ? base_backoff_sec()
                     : next_backoff_sec(*backoff_sec,
                                        cfg_.retry_backoff_base_sec,
                                        cfg_.retry_backoff_max_sec);
    *failures = 0;
  }

  // Give @p saved to the listener to write out at the end of run().  False
  // when the listener keeps nothing (no resume path, or run() already
  // wrote the file because this worker was detached).
  bool hand_over(const SavedSubscription& saved) {
    if (!hooks_.resume_out) return false;
    std::lock_guard<std::mutex> lk(hooks_.resume_out->mu);
    if (hooks_.resume_out->closed) return false;
    hooks_.resume_out->entries.push_back(saved);
    return true;
  }

  // Renew a healthy shared subscription so it outlives the restart, and
  // hand it over instead of unsubscribing.  False when it must be
  // released as usual.
  bool keep_for_restart(const Subscription& sub, const std::string& alarm_url) {
    if (!cfg_.event_proxy || !hooks_.resume_out || sub.url.empty())
      return false;
    const int64_t asked_at = static_cast<int64_t>(util::now_ms());
    int         status = 0;
    std::string detail;
    absl::Status rs = renew(sub.url, sub.ref_params, 5, &status, &detail);
    if (!rs.ok()) {
      LOG(INFO) << '[' << cfg_.ip << "] cannot keep subscription for restart ("
                << rs.message() << ")";
      return false;
    }
    SavedSubscription saved;
    saved.mac        = cfg_.mac;
    saved.ip         = cfg_.ip;
    saved.url        = sub.url;
    saved.ref_params = sub.ref_params;
    saved.alarm_url  = alarm_url;
    saved.expires_ms = asked_at + kKeepForRestartMs;
    if (!hand_over(saved)) return false;
    LOG(INFO) << '[' << cfg_.ip << "] subscription kept for restart: "
              << sub.url;
    return true;
  }

  // Take over the subscription saved at the last stop: renew it and, when
  // the camera accepts, use it as if just created.  Otherwise release it
  // (best effort) and return false so the caller subscribes afresh.
  bool try_resume(const SavedSubscription& saved, Subscription* sub,
                  std::string* alarm_url) {
    if (!cfg_.event_proxy) {
      LOG(INFO) << '[' << cfg_.ip
                << "] releasing subscription kept at the last stop";
      unsubscribe(saved.url, saved.ref_params);
      return false;
    }
    const int64_t left_ms =
        saved.expires_ms - static_cast<int64_t>(util::now_ms());
    if (left_ms < kResumeMinLeftMs) {
      LOG(INFO) << '[' << cfg_.ip << "] saved subscription "
                << (left_ms > 0 ? "expires in " + std::to_string(left_ms / 1000) +
                                      "s"
                                : std::string("has expired"))
                << " -- subscribing afresh";
      unsubscribe(saved.url, saved.ref_params);
      return false;
    }
    int         status = 0;
    std::string detail;
    absl::Status rs = renew(saved.url, saved.ref_params, 10, &status, &detail);
    if (!rs.ok()) {
      LOG(INFO) << '[' << cfg_.ip << "] cannot resume saved subscription ("
                << rs.message() << ") -- subscribing afresh";
      unsubscribe(saved.url, saved.ref_params);
      return false;
    }
    sub->url        = saved.url;
    sub->ref_params = saved.ref_params;
    *alarm_url      = saved.alarm_url;
    LOG(INFO) << '[' << cfg_.ip << "] resumed subscription " << saved.url;
    return true;
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
  // does not include WS-Addressing ReferenceParameters.  Every failed attempt
  // is reported as kCreateFailed.
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
    if (!resp_or.ok()) {
      emit(UpstreamEvent::kCreateFailed, 0,
           std::string(resp_or.status().message()));
      return resp_or.status();
    }

    const HttpResponse& resp = *resp_or;
    if (resp.status_code != 200) {
      if (state_tapped())
        emit(UpstreamEvent::kCreateFailed, static_cast<int>(resp.status_code),
             failure_detail(resp.body));
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
      emit(UpstreamEvent::kCreateFailed, 200, "no SubscriptionReference");
      return Subscription{};
    }

    Subscription sub;
    sub.url = XmlDoc::trim(
      doc_or->text("//*[local-name()='SubscriptionReference']"
                   "/*[local-name()='Address']"));
    sub.ref_params = doc_or->inner_xml(
      "//*[local-name()='SubscriptionReference']"
      "/*[local-name()='ReferenceParameters']");
    if (sub.url.empty())
      emit(UpstreamEvent::kCreateFailed, 200, "no SubscriptionReference");
    if (!sub.ref_params.empty())
      LOG(INFO) << '[' << cfg_.ip << "] subscription has ReferenceParameters";
    // Successful subscription -- whatever auth we used here works.
    // Clear any stale "needs ONVIF Administrator" badge so a camera
    // that was once flagged but is now healthy stops nagging the user.
    clear_camera_needs_onvif_admin(cfg_.ip);
    return sub;
  }

  // Renew for another 120 s.  Any answer but HTTP 200 is an error; the
  // HTTP status (0 for a transport error) and a short description of the
  // failure go to @p http_status and @p detail.  The caller logs failures.
  absl::Status renew(const std::string& sub_url, const std::string& ref_params,
                     int timeout_sec, int* http_status, std::string* detail) {
    static const char* ACTION =
      "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/RenewRequest";

    const std::string body =
      "    <wsnt:Renew>\n"
      "      <wsnt:TerminationTime>PT120S</wsnt:TerminationTime>\n"
      "    </wsnt:Renew>\n";

    auto soap = build_soap(cfg_.user, cfg_.password, body, sub_url, ACTION,
                           ref_params);
    auto resp_or = soap_post_r(sub_url, soap, ACTION, timeout_sec);
    if (!resp_or.ok()) {
      *http_status = 0;
      *detail = std::string(resp_or.status().message());
      return resp_or.status();
    }
    *http_status = static_cast<int>(resp_or->status_code);
    if (resp_or->status_code != 200) {
      *detail = failure_detail(resp_or->body);
      return absl::InternalError("Renew HTTP " +
                                 std::to_string(resp_or->status_code) + ": " +
                                 *detail);
    }
    detail->clear();
    LOG(INFO) << '[' << cfg_.ip << "] subscription renewed";
    return absl::OkStatus();
  }

  // SetSynchronizationPoint: the camera queues its current property state
  // as Initialized messages for the next pull.  Best effort: a camera that
  // refuses sends each property again only when it next changes.
  void set_synchronization_point(const std::string& sub_url,
                                 const std::string& ref_params) {
    static const char* ACTION =
      "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/"
      "SetSynchronizationPointRequest";

    const std::string body = "    <tev:SetSynchronizationPoint/>\n";

    auto soap = build_soap(cfg_.user, cfg_.password, body, sub_url, ACTION,
                           ref_params);
    auto resp_or = soap_post_r(sub_url, soap, ACTION, 5);
    if (!resp_or.ok()) {
      LOG(INFO) << '[' << cfg_.ip << "] SetSynchronizationPoint failed: "
                << resp_or.status().message();
      return;
    }
    if (resp_or->status_code != 200) {
      LOG(INFO) << '[' << cfg_.ip << "] SetSynchronizationPoint HTTP "
                << resp_or->status_code << ": "
                << failure_detail(resp_or->body);
      return;
    }
    LOG(INFO) << '[' << cfg_.ip << "] synchronization point set";
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
    auto status_or = send_unsubscribe(cfg_, sub_url, ref_params, 5);
    if (!status_or.ok()) {
      LOG(INFO) << '[' << cfg_.ip << "] unsubscribe failed (non-fatal): "
                << status_or.status().message();
      return;
    }
    if (*status_or == 200)
      LOG(INFO) << '[' << cfg_.ip << "] subscription released";
    else
      LOG(INFO) << '[' << cfg_.ip << "] unsubscribe HTTP "
                << *status_or << " (non-fatal)";
  }

  // One PullMessages round.  @p http_status receives the HTTP status (0 for
  // a transport error).
  absl::Status pull(const std::string& sub_url, const std::string& ref_params,
                    const std::string& alarm_url, int* http_status) {
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
    if (!resp_or.ok()) {
      *http_status = 0;
      return resp_or.status();
    }

    const HttpResponse& resp = *resp_or;
    *http_status = static_cast<int>(resp.status_code);
    if (resp.status_code != 200)
      return absl::InternalError(
        "PullMessages HTTP " + std::to_string(resp.status_code) +
        ": " + failure_detail(resp.body));

    if (cfg_.event_proxy && hooks_.raw_tap) hooks_.raw_tap(cfg_, resp.body);

    std::vector<OnvifEvent> events;
    for (auto& n : parse_notifications(resp.body)) {
      events.push_back(OnvifEvent{
        cfg_.ip, cfg_.user,
        std::move(n.topic), std::move(n.event_time), std::move(n.property_op),
        std::move(n.source), std::move(n.data), n.bbox, alarm_url});
    }
    if (events.empty()) return absl::OkStatus();
    events_received_total_.fetch_add(events.size());
    last_event_ms_.store(util::now_ms());

    // Empty notifications are delivered like any other but only counted
    // in the "alive:" line, not logged one by one.
    const size_t empty = static_cast<size_t>(
        std::count_if(events.begin(), events.end(), is_empty_notification));
    empty_received_total_.fetch_add(empty);
    if (events.size() > empty) {
      LOG(INFO) << '[' << cfg_.ip << "] received " << events.size() - empty
                << " event(s)";
    }

    for (auto& ev : events) {
      if (!is_empty_notification(ev)) {
        LOG(INFO) << '[' << cfg_.ip << "]   topic=" << ev.topic
                  << " op=" << ev.property_op << " t=" << ev.event_time;
      }
      if (async_)
        async_->push(std::move(ev));
      else
        cb_(ev);
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

  const CameraConfig        cfg_;
  const EventCallback       cb_;
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

void OnvifListener::set_raw_response_tap(RawResponseTap tap) {
  raw_tap_ = std::move(tap);
}

void OnvifListener::set_upstream_state_tap(UpstreamStateTap tap) {
  state_tap_ = std::move(tap);
}

void OnvifListener::set_heartbeat_suffix_fn(HeartbeatSuffixFn fn) {
  heartbeat_suffix_fn_ = std::move(fn);
}

void OnvifListener::set_resume_path(const std::string& path,
                                    std::vector<CameraConfig> credentials) {
  resume_path_  = path;
  resume_creds_ = std::move(credentials);
}

std::vector<std::shared_ptr<detail::WorkerControl>>
OnvifListener::controls_for(const std::string& ip) {
  std::vector<std::shared_ptr<detail::WorkerControl>> out;
  absl::MutexLock lock(&controls_mutex_);
  for (const auto& c : controls_)
    if (c->ip == ip) out.push_back(c);
  return out;
}

void OnvifListener::nudge(const std::string& ip) {
  for (const auto& c : controls_for(ip)) {
    {
      std::lock_guard<std::mutex> lk(c->mu);
      c->wake          = true;
      c->reset_backoff = true;
    }
    c->cv.notify_all();
  }
}

void OnvifListener::resubscribe(const std::string& ip) {
  for (const auto& c : controls_for(ip)) {
    {
      std::lock_guard<std::mutex> lk(c->mu);
      c->resubscribe = true;
    }
    c->cv.notify_all();
  }
}

void OnvifListener::run(EventCallback cb) {
  running_ = true;
  // stop() sets stop_requested_ before clearing running_, so either this
  // check sees it or stop()'s own store lands after ours.
  if (stop_requested_) running_ = false;

  // Subscriptions the previous run() kept for event_proxy cameras.  Each
  // goes to the first camera that matches it.
  std::vector<SavedSubscription> saved;
  std::shared_ptr<ResumeCollector> resume_out;
  if (!resume_path_.empty()) {
    saved = take_saved_subscriptions(resume_path_);
    resume_out = std::make_shared<ResumeCollector>();
  }
  auto claim_saved = [&saved](const CameraConfig& cam)
      -> std::optional<SavedSubscription> {
    // A MAC match wins over an ip match.
    for (int pass = 0; pass < 2; ++pass) {
      for (auto it = saved.begin(); it != saved.end(); ++it) {
        const bool by_mac = !normalize_mac(it->mac).empty() &&
                            !normalize_mac(cam.mac).empty();
        if ((pass == 0) != by_mac || !saved_matches(*it, cam)) continue;
        SavedSubscription s = std::move(*it);
        saved.erase(it);
        return s;
      }
    }
    return std::nullopt;
  };

  auto make_worker = [&](const CameraConfig& cam) {
    WorkerHooks h;
    h.publish_health   = [this](const CameraHealth& ch) {
      this->publish_health(ch);
    };
    h.raw_tap          = raw_tap_;
    h.state_tap        = state_tap_;
    h.heartbeat_suffix = heartbeat_suffix_fn_;
    h.control          = std::make_shared<detail::WorkerControl>(cam.ip);
    h.resume_out       = resume_out;
    h.resume_in        = claim_saved(cam);
    {
      absl::MutexLock lock(&controls_mutex_);
      controls_.push_back(h.control);
    }
    return std::make_unique<CameraWorker>(cam, cb, running_, std::move(h));
  };

  // Build workers (heap-allocated so their address is stable across moves)
  std::vector<std::unique_ptr<CameraWorker>> workers;
  workers.reserve(cameras_.size());
  for (const auto& cam : cameras_)
    workers.push_back(make_worker(cam));

  // Launch one thread per camera
  std::vector<std::thread> threads;
  threads.reserve(workers.size());
  for (auto& w : workers) {
    CameraWorker* ptr = w.get();
    threads.emplace_back([ptr] { ptr->run(); });
  }

  // A saved subscription no camera took (its camera was excluded, removed
  // or paired with an AI Port since) would hold a slot on the camera until
  // it expires; release it now.
  if (!saved.empty()) {
    LOG(INFO) << "[onvif] " << saved.size()
              << " saved subscription(s) matched no camera";
    std::vector<CameraConfig> creds = resume_creds_;
    creds.insert(creds.end(), cameras_.begin(), cameras_.end());
    for (const SavedSubscription& s : saved)
      release_saved_subscription(s, creds);
    saved.clear();
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
        workers.push_back(make_worker(cfg));
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
      // The detached thread still runs on this worker, so it is leaked
      // rather than destroyed when run() returns.
      static_cast<void>(workers[i].release());
    }
  }

  {
    absl::MutexLock lock(&controls_mutex_);
    controls_.clear();
  }

  // Save what the joined workers kept.  Closing the collector first makes
  // a detached worker that finishes later release its subscription itself.
  if (resume_out) {
    std::vector<SavedSubscription> keep;
    {
      std::lock_guard<std::mutex> lk(resume_out->mu);
      resume_out->closed = true;
      keep.swap(resume_out->entries);
    }
    // Kept subscriptions that cannot be saved are released instead.
    if (!store_saved_subscriptions(resume_path_, keep)) {
      std::vector<CameraConfig> creds = cameras_;
      creds.insert(creds.end(), resume_creds_.begin(), resume_creds_.end());
      for (const SavedSubscription& s : keep)
        release_saved_subscription(s, creds);
    }
  }
}

void OnvifListener::stop() {
  stop_requested_ = true;
  running_ = false;
}

absl::Status OnvifListener::release_saved_subscriptions(
    const std::string& path, const std::vector<CameraConfig>& cams) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    if (errno == ENOENT) return absl::OkStatus();
    return absl::InternalError("cannot read " + path + ": " +
                               std::strerror(errno));
  }
  std::string text;
  if (!read_whole_file(path, &text))
    return absl::InternalError("cannot read " + path);
  std::vector<SavedSubscription> subs;
  const bool parsed = SavedSubscriptionsReader(text).read(&subs);
  for (const SavedSubscription& s : subs) release_saved_subscription(s, cams);
  if (std::remove(path.c_str()) != 0 && errno != ENOENT)
    return absl::InternalError("cannot delete " + path + ": " +
                               std::strerror(errno));
  if (!parsed)
    return absl::InvalidArgumentError("unreadable saved subscriptions file " +
                                      path);
  return absl::OkStatus();
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
