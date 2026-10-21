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

#include "pullpoint_soap.hpp"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <time.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pullpoint_hub.hpp"
#include "pullpoint_server.hpp"

namespace onvif {
namespace pullpoint {
namespace soap {

namespace {

using std::chrono::milliseconds;
using std::chrono::system_clock;

// Longest duration accepted, about 317 years; longer values saturate.
constexpr double kMaxDurationMs = 1e13;
// More integer digits than this in one duration component is refused.
constexpr size_t kMaxDurationDigits = 15;

// Namespaces declared on every response envelope.
constexpr const char* kEnvelopeNamespaces[][2] = {
    {"env", "http://www.w3.org/2003/05/soap-envelope"},
    {"wsa", "http://www.w3.org/2005/08/addressing"},
    {"tev", "http://www.onvif.org/ver10/events/wsdl"},
    {"wsnt", "http://docs.oasis-open.org/wsn/b-2"},
    {"tt", "http://www.onvif.org/ver10/schema"},
    {"ter", "http://www.onvif.org/ver10/error"},
};

// wsa:Action of each response.
constexpr char kActionFault[] = "http://www.w3.org/2005/08/addressing/soap/fault";
constexpr char kEventPort[] = "http://www.onvif.org/ver10/events/wsdl/EventPortType/";
constexpr char kPullPointPort[] = "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/";
constexpr char kSubscriptionManager[] = "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/";

std::string trim(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

bool is_lower_hex(char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }

int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

// Exactly @p n decimal digits at @p pos.
bool read_digits(const std::string& s, size_t pos, size_t n, int* out) {
  if (pos + n > s.size()) return false;
  int v = 0;
  for (size_t i = pos; i < pos + n; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    v = v * 10 + (s[i] - '0');
  }
  *out = v;
  return true;
}

bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int days_in_month(int y, int m) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && is_leap(y) ? 29 : kDays[m - 1];
}

// Days from 1970-01-01 to the given proleptic Gregorian date.
int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Local name of an element.
bool named(const xmlNode* n, const char* local) {
  return n != nullptr && n->type == XML_ELEMENT_NODE && n->name != nullptr &&
         std::strcmp(reinterpret_cast<const char*>(n->name), local) == 0;
}

const xmlNode* first_element(const xmlNode* parent) {
  for (const xmlNode* c = parent->children; c != nullptr; c = c->next) {
    if (c->type == XML_ELEMENT_NODE) return c;
  }
  return nullptr;
}

std::string node_text(const xmlNode* n) {
  xmlChar* content = xmlNodeGetContent(n);
  if (content == nullptr) return std::string();
  std::string out(reinterpret_cast<const char*>(content));
  xmlFree(content);
  return trim(out);
}

bool contains_doctype(const std::string& body) {
  static const char kNeedle[] = "<!doctype";
  const size_t n = sizeof(kNeedle) - 1;
  auto it = std::search(body.begin(), body.end(), kNeedle, kNeedle + n, [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == b;
  });
  return it != body.end();
}

struct DocFree {
  void operator()(xmlDoc* d) const { xmlFreeDoc(d); }
};

std::string time_element(const char* name, system_clock::time_point t) {
  return std::string("<") + name + '>' + format_date_time(t) + "</" + name + '>';
}

}  // namespace

// ---------------------------------------------------------------------
// Time values
// ---------------------------------------------------------------------

std::optional<milliseconds> parse_duration(const std::string& text) {
  const std::string s = trim(text);
  size_t i = 0;
  bool negative = false;
  if (i < s.size() && s[i] == '-') {
    negative = true;
    ++i;
  }
  if (i >= s.size() || s[i] != 'P') return std::nullopt;
  ++i;
  static const char kDateUnits[] = "YMWD";
  static const double kDateMs[] = {365.0 * 86400e3, 30.0 * 86400e3, 7.0 * 86400e3, 86400e3};
  static const char kTimeUnits[] = "HMS";
  static const double kTimeMs[] = {3600e3, 60e3, 1e3};
  size_t next_date = 0;
  size_t next_time = 0;
  bool in_time = false;
  int components = 0;
  int time_components = 0;
  double total = 0;
  while (i < s.size()) {
    if (s[i] == 'T') {
      if (in_time) return std::nullopt;
      in_time = true;
      ++i;
      continue;
    }
    const size_t start = i;
    double value = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
      value = value * 10 + (s[i] - '0');
      ++i;
    }
    if (i == start || i - start > kMaxDurationDigits) return std::nullopt;
    bool fraction = false;
    if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
      ++i;
      const size_t frac_start = i;
      double scale = 0.1;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        value += (s[i] - '0') * scale;
        scale /= 10;
        ++i;
      }
      if (i == frac_start) return std::nullopt;
      fraction = true;
    }
    if (i >= s.size()) return std::nullopt;
    const char unit = s[i++];
    double unit_ms = 0;
    if (!in_time) {
      const char* p = std::strchr(kDateUnits + next_date, unit);
      if (unit == '\0' || p == nullptr || fraction) return std::nullopt;
      const size_t idx = static_cast<size_t>(p - kDateUnits);
      unit_ms = kDateMs[idx];
      next_date = idx + 1;
    } else {
      const char* p = std::strchr(kTimeUnits + next_time, unit);
      if (unit == '\0' || p == nullptr) return std::nullopt;
      const size_t idx = static_cast<size_t>(p - kTimeUnits);
      if (fraction && unit != 'S') return std::nullopt;
      unit_ms = kTimeMs[idx];
      next_time = idx + 1;
      ++time_components;
    }
    total += value * unit_ms;
    ++components;
  }
  if (components == 0 || (in_time && time_components == 0)) return std::nullopt;
  total = std::min(total, kMaxDurationMs);
  const int64_t ms = static_cast<int64_t>(std::llround(total));
  return milliseconds(negative ? -ms : ms);
}

std::optional<system_clock::time_point> parse_date_time(const std::string& text) {
  const std::string s = trim(text);
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  // YYYY-MM-DDThh:mm:ss
  if (!read_digits(s, 0, 4, &year) || s.size() < 19 || s[4] != '-' ||
      !read_digits(s, 5, 2, &month) || s[7] != '-' || !read_digits(s, 8, 2, &day) ||
      (s[10] != 'T' && s[10] != 't') || !read_digits(s, 11, 2, &hour) || s[13] != ':' ||
      !read_digits(s, 14, 2, &minute) || s[16] != ':' || !read_digits(s, 17, 2, &second)) {
    return std::nullopt;
  }
  if (year < 1900 || year > 2200 || month < 1 || month > 12 || day < 1 ||
      day > days_in_month(year, month) || hour > 24 || minute > 59 || second > 59) {
    return std::nullopt;
  }
  size_t i = 19;
  int64_t frac_ms = 0;
  if (i < s.size() && s[i] == '.') {
    ++i;
    const size_t start = i;
    int64_t scale = 100;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
      frac_ms += (s[i] - '0') * scale;
      scale /= 10;
      ++i;
    }
    if (i == start) return std::nullopt;
  }
  if (hour == 24 && (minute != 0 || second != 0 || frac_ms != 0)) return std::nullopt;
  int64_t offset_s = 0;
  if (i < s.size()) {
    if (s[i] == 'Z' || s[i] == 'z') {
      ++i;
    } else if (s[i] == '+' || s[i] == '-') {
      int oh = 0;
      int om = 0;
      if (!read_digits(s, i + 1, 2, &oh) || i + 3 >= s.size() || s[i + 3] != ':' ||
          !read_digits(s, i + 4, 2, &om) || oh > 14 || om > 59) {
        return std::nullopt;
      }
      offset_s = (oh * 3600 + om * 60) * (s[i] == '-' ? -1 : 1);
      i += 6;
    }
  }
  if (i != s.size()) return std::nullopt;
  const int64_t secs = days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 +
                       second - offset_s;
  const milliseconds since_epoch(secs * 1000 + frac_ms);
  return system_clock::time_point(
      std::chrono::duration_cast<system_clock::duration>(since_epoch));
}

std::optional<milliseconds> parse_time_offset(const std::string& text,
                                              system_clock::time_point now) {
  const std::string s = trim(text);
  if (!s.empty() && (s[0] == 'P' || s[0] == '-')) return parse_duration(s);
  const auto t = parse_date_time(s);
  if (!t) return std::nullopt;
  return std::chrono::duration_cast<milliseconds>(*t - now);
}

std::string format_date_time(system_clock::time_point t) {
  const time_t secs = static_cast<time_t>(
      std::chrono::floor<std::chrono::seconds>(t.time_since_epoch()).count());
  struct tm tm {};
  gmtime_r(&secs, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

// ---------------------------------------------------------------------
// Request paths
// ---------------------------------------------------------------------

bool percent_decode(const std::string& in, std::string* out) {
  std::string r;
  r.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    char ch = in[i];
    if (ch == '%') {
      if (i + 2 >= in.size()) return false;
      const int hi = hex_value(in[i + 1]);
      const int lo = hex_value(in[i + 2]);
      if (hi < 0 || lo < 0) return false;
      ch = static_cast<char>(hi * 16 + lo);
      i += 2;
    }
    if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f) return false;
    r.push_back(ch);
  }
  *out = std::move(r);
  return true;
}

Route parse_route(const std::string& path) {
  Route r;
  const std::string prefix = PullPointServer::kPathPrefix;
  if (path.compare(0, prefix.size(), prefix) != 0) {
    r.error = "path is not under " + prefix;
    return r;
  }
  const size_t host_end = path.find('/', prefix.size());
  if (host_end == std::string::npos) {
    r.error = "path names no camera port";
    return r;
  }
  size_t port_end = path.find('/', host_end + 1);
  if (port_end == std::string::npos) port_end = path.size();
  std::string host;
  std::string port;
  if (!percent_decode(path.substr(prefix.size(), host_end - prefix.size()), &host) ||
      !percent_decode(path.substr(host_end + 1, port_end - host_end - 1), &port)) {
    r.error = "malformed camera host or port in path";
    return r;
  }
  if (host.empty()) {
    r.error = "path names no camera host";
    return r;
  }
  r.key = camera_key(host, port);
  r.base = path.substr(0, port_end);
  const std::string rest = path.substr(port_end);
  static const char kSub[] = "/s/";
  constexpr size_t kSubLen = sizeof(kSub) - 1;
  constexpr size_t kIdLen = 32;
  if (rest == "/status") {
    r.kind = Route::Kind::kStatus;
  } else if (rest.size() == kSubLen + kIdLen && rest.compare(0, kSubLen, kSub) == 0 &&
             std::all_of(rest.begin() + kSubLen, rest.end(), is_lower_hex)) {
    r.kind = Route::Kind::kSubscription;
    r.id = rest.substr(kSubLen);
  } else {
    r.kind = Route::Kind::kCreate;
    r.suffix = rest;
  }
  return r;
}

// ---------------------------------------------------------------------
// Request bodies
// ---------------------------------------------------------------------

const std::string* Request::param(const char* name) const {
  for (const auto& p : params) {
    if (p.first == name) return &p.second;
  }
  return nullptr;
}

bool parse_request(const std::string& body, Request* req, std::string* error) {
  *req = Request();
  if (body.empty()) {
    *error = "empty request";
    return false;
  }
  if (contains_doctype(body)) {
    *error = "DOCTYPE is not allowed";
    return false;
  }
  if (body.size() > static_cast<size_t>(INT_MAX)) {
    *error = "request too large";
    return false;
  }
  // No XML_PARSE_NOENT (entities stay unexpanded) and no XML_PARSE_DTDLOAD.
  constexpr int kOptions =
      XML_PARSE_RECOVER | XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING;
  std::unique_ptr<xmlDoc, DocFree> doc(
      xmlReadMemory(body.data(), static_cast<int>(body.size()), nullptr, nullptr, kOptions));
  if (!doc) {
    *error = "request is not XML";
    return false;
  }
  if (doc->intSubset != nullptr || doc->extSubset != nullptr) {
    *error = "DOCTYPE is not allowed";
    return false;
  }
  const xmlNode* root = xmlDocGetRootElement(doc.get());
  if (!named(root, "Envelope")) {
    *error = "request is not a SOAP envelope";
    return false;
  }
  const xmlNode* soap_body = nullptr;
  for (const xmlNode* c = root->children; c != nullptr; c = c->next) {
    if (named(c, "Body")) {
      soap_body = c;
      break;
    }
  }
  if (soap_body == nullptr) {
    *error = "SOAP envelope has no Body";
    return false;
  }
  const xmlNode* op = first_element(soap_body);
  if (op == nullptr) {
    *error = "SOAP Body is empty";
    return false;
  }
  req->op = reinterpret_cast<const char*>(op->name);
  for (const xmlNode* c = op->children; c != nullptr; c = c->next) {
    if (c->type != XML_ELEMENT_NODE) continue;
    req->params.emplace_back(reinterpret_cast<const char*>(c->name), node_text(c));
  }
  return true;
}

// ---------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------

std::string xml_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    switch (ch) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(ch);
    }
  }
  return out;
}

std::string envelope(const std::string& action, const std::string& body) {
  std::string out;
  out.reserve(action.size() + body.size() + 512);
  out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?><env:Envelope";
  for (const auto& ns : kEnvelopeNamespaces) {
    out += " xmlns:";
    out += ns[0];
    out += "=\"";
    out += ns[1];
    out += '"';
  }
  out += "><env:Header><wsa:Action>";
  out += action;
  out += "</wsa:Action></env:Header><env:Body>";
  out += body;
  out += "</env:Body></env:Envelope>";
  return out;
}

std::string fault(FaultKind kind, const std::string& reason) {
  std::string subcode;
  switch (kind) {
    case FaultKind::kActionNotSupported:
      subcode = "<env:Subcode><env:Value>ter:ActionNotSupported</env:Value></env:Subcode>";
      break;
    case FaultKind::kInvalidArgVal:
      subcode = "<env:Subcode><env:Value>ter:InvalidArgVal</env:Value></env:Subcode>";
      break;
    case FaultKind::kResourceUnknown:
      subcode =
          "<env:Subcode><env:Value>ter:InvalidArgVal</env:Value>"
          "<env:Subcode><env:Value>ter:ResourceUnknown</env:Value></env:Subcode>"
          "</env:Subcode>";
      break;
  }
  return envelope(kActionFault,
                  "<env:Fault><env:Code><env:Value>env:Receiver</env:Value>" + subcode +
                      "</env:Code><env:Reason><env:Text xml:lang=\"en\">onvif-recorder: " +
                      xml_escape(reason) + "</env:Text></env:Reason></env:Fault>");
}

std::string create_response(const std::string& address, system_clock::time_point current,
                            system_clock::time_point termination) {
  std::string body = "<tev:CreatePullPointSubscriptionResponse><tev:SubscriptionReference>";
  body += "<wsa:Address>" + xml_escape(address) + "</wsa:Address></tev:SubscriptionReference>";
  body += time_element("wsnt:CurrentTime", current);
  body += time_element("wsnt:TerminationTime", termination);
  body += "</tev:CreatePullPointSubscriptionResponse>";
  return envelope(std::string(kEventPort) + "CreatePullPointSubscriptionResponse", body);
}

std::string capabilities_response() {
  return envelope(std::string(kEventPort) + "GetServiceCapabilitiesResponse",
                  "<tev:GetServiceCapabilitiesResponse>"
                  "<tev:Capabilities WSPullPointSupport=\"true\"/>"
                  "</tev:GetServiceCapabilitiesResponse>");
}

std::string pull_response(system_clock::time_point current, system_clock::time_point termination,
                          const std::vector<std::shared_ptr<const std::string>>& messages) {
  size_t bytes = 0;
  for (const auto& m : messages) bytes += m ? m->size() : 0;
  std::string body;
  body.reserve(bytes + 160);
  body += "<tev:PullMessagesResponse>";
  body += time_element("tev:CurrentTime", current);
  body += time_element("tev:TerminationTime", termination);
  for (const auto& m : messages) {
    if (m) body += *m;
  }
  body += "</tev:PullMessagesResponse>";
  return envelope(std::string(kPullPointPort) + "PullMessagesResponse", body);
}

std::string renew_response(system_clock::time_point current,
                           system_clock::time_point termination) {
  std::string body = "<wsnt:RenewResponse>";
  body += time_element("wsnt:TerminationTime", termination);
  body += time_element("wsnt:CurrentTime", current);
  body += "</wsnt:RenewResponse>";
  return envelope(std::string(kSubscriptionManager) + "RenewResponse", body);
}

std::string unsubscribe_response() {
  return envelope(std::string(kSubscriptionManager) + "UnsubscribeResponse",
                  "<wsnt:UnsubscribeResponse/>");
}

std::string sync_point_response() {
  return envelope(std::string(kPullPointPort) + "SetSynchronizationPointResponse",
                  "<tev:SetSynchronizationPointResponse/>");
}

}  // namespace soap
}  // namespace pullpoint
}  // namespace onvif
