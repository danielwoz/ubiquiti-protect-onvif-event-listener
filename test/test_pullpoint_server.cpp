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
 * test_pullpoint_server.cpp
 *
 * PullPointServer, the loopback ONVIF PullPoint service Protect's
 * subscription requests are routed to: ISO-8601 time values, request
 * paths and bodies, and the HTTP/SOAP behaviour against a real
 * PullPointHub with a libcurl client (and raw sockets for HTTP/1.0).
 * Every response is checked to be a SOAP 1.2 envelope, never a 401 and
 * never an empty 200; faults are HTTP 500.
 */

#include <arpa/inet.h>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "notification_fragments.hpp"
#include "pullpoint_hub.hpp"
#include "pullpoint_server.hpp"
#include "pullpoint_soap.hpp"
#include "upstream_event.hpp"

using onvif::UpstreamEvent;
using onvif::pullpoint::HubCamera;
using onvif::pullpoint::PullPointHub;
using onvif::pullpoint::PullPointServer;
using std::chrono::milliseconds;
using std::chrono::steady_clock;
using std::chrono::system_clock;
namespace soap = onvif::pullpoint::soap;

namespace {

std::atomic<int> g_pass{0};
std::atomic<int> g_fail{0};

void check(bool cond, const std::string& label) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << label << '\n';
  }
}

const char kIp[] = "192.168.1.109";
const char kMac[] = "F00000FDAED9";
const char kUnsharedIp[] = "10.0.0.7:8000";
const char kNamedIp[] = "cam-one.local:8000";
const char kPrefix[] = "/onvif-recorder/pullpoint/";
const char kCamPath[] = "/onvif-recorder/pullpoint/192.168.1.109/80";
const char kSoap12[] = "http://www.w3.org/2003/05/soap-envelope";

int64_t ms_of(steady_clock::duration d) {
  return std::chrono::duration_cast<milliseconds>(d).count();
}

// ---------------------------------------------------------------------
// Requests as Protect sends them: SOAP 1.2, a WS-Security header with
// mustUnderstand, default-namespace operation elements, no SOAPAction.
// ---------------------------------------------------------------------
std::string protect_envelope(const std::string& body) {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
         "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Header>"
         "<Security s:mustUnderstand=\"1\" xmlns=\"http://docs.oasis-open.org/wss/2004/01/"
         "oasis-200401-wss-wssecurity-secext-1.0.xsd\"><UsernameToken><Username>admin</Username>"
         "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-"
         "token-profile-1.0#PasswordDigest\">c2VjcmV0</Password><Nonce>bm9uY2U=</Nonce>"
         "<Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-"
         "utility-1.0.xsd\">2026-10-21T20:00:00.000Z</Created></UsernameToken></Security>"
         "</s:Header><s:Body>" + body + "</s:Body></s:Envelope>";
}

std::string create_body(const std::string& itt) {
  std::string inner;
  if (!itt.empty()) inner = "<InitialTerminationTime>" + itt + "</InitialTerminationTime>";
  return protect_envelope(
      "<CreatePullPointSubscription xmlns=\"http://www.onvif.org/ver10/events/wsdl\">" + inner +
      "</CreatePullPointSubscription>");
}

std::string pull_body(const std::string& timeout, int limit) {
  return protect_envelope("<PullMessages xmlns=\"http://www.onvif.org/ver10/events/wsdl\">"
                          "<Timeout>" + timeout + "</Timeout><MessageLimit>" +
                          std::to_string(limit) + "</MessageLimit></PullMessages>");
}

std::string renew_body(const std::string& ttl) {
  return protect_envelope("<Renew xmlns=\"http://docs.oasis-open.org/wsn/b-2\">"
                          "<TerminationTime>" + ttl + "</TerminationTime></Renew>");
}

std::string unsubscribe_body() {
  return protect_envelope("<Unsubscribe xmlns=\"http://docs.oasis-open.org/wsn/b-2\"/>");
}

std::string sync_body() {
  return protect_envelope(
      "<SetSynchronizationPoint xmlns=\"http://www.onvif.org/ver10/events/wsdl\"/>");
}

std::string capabilities_body() {
  return protect_envelope(
      "<GetServiceCapabilities xmlns=\"http://www.onvif.org/ver10/events/wsdl\"/>");
}

// ---------------------------------------------------------------------
// The camera's own PullMessagesResponse, fed to the hub.
// ---------------------------------------------------------------------
std::string notification(const std::string& topic, const std::string& value,
                         const std::string& op, const std::string& padding = "") {
  return "<wsnt:NotificationMessage><wsnt:Topic Dialect=\"http://www.onvif.org/ver10/tev/"
         "topicExpression/ConcreteSet\">" + topic + "</wsnt:Topic><wsnt:Message>"
         "<tt:Message UtcTime=\"2018-01-01T00:00:00Z\"" +
         (op.empty() ? std::string() : " PropertyOperation=\"" + op + "\"") +
         "><tt:Source><tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"VSC0\"/>"
         "</tt:Source><tt:Data><tt:SimpleItem Name=\"State\" Value=\"" + value + "\"/>" +
         padding + "</tt:Data></tt:Message></wsnt:Message></wsnt:NotificationMessage>";
}

std::string motion(const std::string& value, const std::string& op = "Changed") {
  return notification("tns1:VideoSource/MotionAlarm", value, op);
}

std::string upstream_response(const std::vector<std::string>& notes) {
  std::string out =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><SOAP-ENV:Envelope"
      " xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\""
      " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
      " xmlns:tns1=\"http://www.onvif.org/ver10/topics\""
      " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
      " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\"><SOAP-ENV:Body>"
      "<tev:PullMessagesResponse><tev:CurrentTime>2018-01-01T00:00:00Z</tev:CurrentTime>"
      "<tev:TerminationTime>2018-01-01T00:01:00Z</tev:TerminationTime>";
  for (const auto& n : notes) out += n;
  out += "</tev:PullMessagesResponse></SOAP-ENV:Body></SOAP-ENV:Envelope>";
  return out;
}

// Fragments the hub forwards for @p response, in order.
std::vector<std::string> fragments_of(const std::string& response) {
  std::vector<std::string> out;
  for (const auto& f : onvif::pullpoint::extract_fragments(response).fragments) {
    out.push_back(f.xml);
  }
  return out;
}

// ---------------------------------------------------------------------
// XML inspection
// ---------------------------------------------------------------------
struct DocFree {
  void operator()(xmlDoc* d) const { xmlFreeDoc(d); }
};
using DocPtr = std::unique_ptr<xmlDoc, DocFree>;

DocPtr parse_strict(const std::string& xml) {
  return DocPtr(xmlReadMemory(xml.data(), static_cast<int>(xml.size()), nullptr, nullptr,
                              XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING));
}

const xmlNode* find_element(const xmlNode* n, const char* local) {
  for (; n != nullptr; n = n->next) {
    if (n->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char*>(n->name),
                                                   local) == 0) {
      return n;
    }
    if (const xmlNode* c = find_element(n->children, local)) return c;
  }
  return nullptr;
}

int count_elements(const xmlNode* n, const char* local) {
  int count = 0;
  for (; n != nullptr; n = n->next) {
    if (n->type == XML_ELEMENT_NODE &&
        std::strcmp(reinterpret_cast<const char*>(n->name), local) == 0) {
      ++count;
    }
    count += count_elements(n->children, local);
  }
  return count;
}

// Text of the first element called @p local, or "" when there is none.
std::string text_of(const std::string& xml, const char* local) {
  DocPtr doc = parse_strict(xml);
  if (!doc) return "";
  const xmlNode* n = find_element(xmlDocGetRootElement(doc.get()), local);
  if (n == nullptr) return "";
  xmlChar* c = xmlNodeGetContent(n);
  std::string out = c != nullptr ? reinterpret_cast<const char*>(c) : "";
  xmlFree(c);
  return out;
}

int count_of(const std::string& xml, const char* local) {
  DocPtr doc = parse_strict(xml);
  return doc ? count_elements(xmlDocGetRootElement(doc.get()), local) : -1;
}

std::string attr_of(const std::string& xml, const char* local, const char* attr) {
  DocPtr doc = parse_strict(xml);
  if (!doc) return "";
  const xmlNode* n = find_element(xmlDocGetRootElement(doc.get()), local);
  if (n == nullptr) return "";
  xmlChar* v = xmlGetProp(n, reinterpret_cast<const xmlChar*>(attr));
  std::string out = v != nullptr ? reinterpret_cast<const char*>(v) : "";
  xmlFree(v);
  return out;
}

// Seconds from the response's CurrentTime to its TerminationTime.
int64_t lifetime_s(const std::string& xml) {
  const auto cur = soap::parse_date_time(text_of(xml, "CurrentTime"));
  const auto term = soap::parse_date_time(text_of(xml, "TerminationTime"));
  if (!cur || !term) return -1000000;
  return std::chrono::duration_cast<std::chrono::seconds>(*term - *cur).count();
}

// ---------------------------------------------------------------------
// HTTP client
// ---------------------------------------------------------------------
struct Http {
  bool transport_ok = false;
  std::string error;
  long status = 0;  // NOLINT(runtime/int)
  std::string body;
  std::map<std::string, std::string> headers;  // lower-case names
};

size_t on_body(char* p, size_t size, size_t n, void* ud) {
  static_cast<std::string*>(ud)->append(p, size * n);
  return size * n;
}

size_t on_header(char* p, size_t size, size_t n, void* ud) {
  const std::string line(p, size * n);
  const size_t colon = line.find(':');
  if (colon != std::string::npos) {
    std::string name = line.substr(0, colon);
    for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string value = line.substr(colon + 1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
      value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
      value.pop_back();
    }
    (*static_cast<std::map<std::string, std::string>*>(ud))[name] = value;
  }
  return size * n;
}

Http request(uint16_t port, const std::string& method, const std::string& path,
             const std::string& body, const std::vector<std::string>& extra_headers = {}) {
  Http r;
  CURL* c = curl_easy_init();
  const std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/soap+xml; charset=utf-8");
  headers = curl_slist_append(headers, "Expect:");
  for (const auto& h : extra_headers) headers = curl_slist_append(headers, h.c_str());
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_PATH_AS_IS, 1L);  // NOLINT(runtime/int)
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);  // NOLINT(runtime/int)
  curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 10000L);  // NOLINT(runtime/int)
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &on_body);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, &on_header);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, &r.headers);
  if (method == "POST") {
    curl_easy_setopt(c, CURLOPT_POST, 1L);  // NOLINT(runtime/int)
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
  } else if (method == "GET") {
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);  // NOLINT(runtime/int)
  } else if (method == "HEAD") {
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);  // NOLINT(runtime/int)
  } else {
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
  }
  const CURLcode rc = curl_easy_perform(c);
  r.transport_ok = rc == CURLE_OK;
  if (!r.transport_ok) r.error = curl_easy_strerror(rc);
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(c);
  return r;
}

std::string header(const Http& r, const char* name) {
  const auto it = r.headers.find(name);
  return it == r.headers.end() ? std::string() : it->second;
}

std::atomic<int> g_responses{0};
std::atomic<int> g_unauthorized{0};
std::atomic<int> g_empty_ok{0};

// Checks every response must pass: a SOAP 1.2 envelope with a Body, a
// Fault exactly when the status is not 200, faults on HTTP 500 (405 for
// a wrong method), never 401, never an empty 200, and the header rules.
void check_response(const Http& r, const std::string& label, bool head = false) {
  check(r.transport_ok, label + ": transport (" + r.error + ")");
  if (!r.transport_ok) return;
  ++g_responses;
  if (r.status == 401) ++g_unauthorized;
  if (r.status == 200 && r.body.empty()) ++g_empty_ok;
  check(r.status != 401, label + ": never 401");
  check(r.status == 200 || r.status == 500 || r.status == 405,
        label + ": status 200, 405 or 500 (got " + std::to_string(r.status) + ")");
  check(header(r, "content-type") == "application/soap+xml; charset=utf-8",
        label + ": Content-Type");
  std::string conn = header(r, "connection");
  for (char& ch : conn) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  check(conn.find("close") != std::string::npos, label + ": Connection: close");
  check(r.headers.count("content-length") == 1, label + ": Content-Length present");
  check(r.headers.count("transfer-encoding") == 0, label + ": not chunked");
  if (head) return;
  check(header(r, "content-length") == std::to_string(r.body.size()),
        label + ": Content-Length matches the body");
  check(!r.body.empty(), label + ": body not empty");
  check(r.body.size() <= 512 * 1024, label + ": body within 512 KiB");
  check(r.body.find("NotAuthorized") == std::string::npos, label + ": no NotAuthorized");
  DocPtr doc = parse_strict(r.body);
  check(doc != nullptr, label + ": body is well-formed XML");
  if (!doc) return;
  const xmlNode* root = xmlDocGetRootElement(doc.get());
  check(root != nullptr && std::strcmp(reinterpret_cast<const char*>(root->name),
                                       "Envelope") == 0,
        label + ": root is Envelope");
  if (root == nullptr) return;
  check(root->ns != nullptr &&
            std::strcmp(reinterpret_cast<const char*>(root->ns->href), kSoap12) == 0,
        label + ": SOAP 1.2 namespace");
  const xmlNode* body = find_element(root->children, "Body");
  check(body != nullptr, label + ": has a Body");
  const bool has_fault = find_element(root->children, "Fault") != nullptr;
  if (r.status == 200) {
    check(!has_fault, label + ": 200 carries no Fault");
  } else {
    check(has_fault, label + ": error status carries a Fault");
    check(text_of(r.body, "Text").rfind("onvif-recorder: ", 0) == 0,
          label + ": fault reason starts with onvif-recorder:");
  }
}

Http call(uint16_t port, const std::string& path, const std::string& body,
          const std::string& label, const std::vector<std::string>& extra_headers = {}) {
  Http r = request(port, "POST", path, body, extra_headers);
  check_response(r, label);
  return r;
}

bool is_fault(const Http& r, const char* subcode) {
  return r.status == 500 && text_of(r.body, "Value").find("env:Receiver") == 0 &&
         r.body.find(subcode) != std::string::npos;
}

// ---------------------------------------------------------------------
// A hub with three cameras and a server on a free port.
// ---------------------------------------------------------------------
struct Fixture {
  explicit Fixture(bool ready = true) : hub(PullPointHub::Options{}), server(&hub) {
    hub.register_camera(HubCamera{kIp, kMac, true});
    hub.register_camera(HubCamera{kUnsharedIp, "", false});
    hub.register_camera(HubCamera{kNamedIp, "", true});
    if (ready) hub.set_ready();
    started = server.start(0);
    port = server.port();
  }

  void up(const std::string& ip = kIp) {
    hub.on_upstream_event(ip, UpstreamEvent::kSubscribed, 200, "");
  }

  // Create a subscription for the 109 camera and return its path, or ""
  // when declined.
  std::string subscribe(const std::string& itt = "PT60S") {
    Http r = call(port, std::string(kCamPath) + "/onvif/event_service", create_body(itt),
                  "subscribe");
    if (r.status != 200) return "";
    const std::string address = text_of(r.body, "Address");
    const std::string origin = "http://127.0.0.1:" + std::to_string(port);
    return address.rfind(origin, 0) == 0 ? address.substr(origin.size()) : "";
  }

  PullPointHub hub;
  PullPointServer server;
  bool started = false;
  uint16_t port = 0;
};

// ---------------------------------------------------------------------
// Time values
// ---------------------------------------------------------------------
void test_durations() {
  struct Case {
    const char* text;
    int64_t ms;
  };
  const Case good[] = {
      {"PT60S", 60000},      {"PT1M", 60000},          {"P0DT0H1M0S", 60000},
      {"PT1.5S", 1500},      {"PT0.25S", 250},         {"PT1,5S", 1500},
      {"PT2S", 2000},        {" PT5S\n", 5000},        {"PT0S", 0},
      {"P1D", 86400000},     {"PT1H", 3600000},        {"PT1H30M", 5400000},
      {"P1W", 604800000},    {"P1M", 2592000000LL},    {"P1Y", 31536000000LL},
      {"P1DT1S", 86401000},  {"-PT5S", -5000},         {"PT600S", 600000},
  };
  for (const auto& c : good) {
    const auto d = soap::parse_duration(c.text);
    check(d.has_value() && d->count() == c.ms,
          std::string("duration '") + c.text + "' = " + std::to_string(c.ms));
  }
  const char* bad[] = {"",      "P",      "PT",     "60",      "PT5",    "PTS",
                       "P1H",   "PT1S1M", "PT1.S",  "PT.5S",   "P1.5D",  "PT1M1M",
                       "xPT5S", "PT5Sx",  "P1DT",   "PT-5S",   "T5S",    "P1S"};
  for (const char* b : bad) {
    check(!soap::parse_duration(b).has_value(), std::string("duration '") + b + "' refused");
  }
}

void test_date_times() {
  const auto at = [](int64_t s, int64_t ms = 0) {
    return system_clock::time_point(std::chrono::duration_cast<system_clock::duration>(
        milliseconds(s * 1000 + ms)));
  };
  struct Case {
    const char* text;
    system_clock::time_point t;
  };
  const Case good[] = {
      {"1970-01-01T00:00:00Z", at(0)},
      {"2026-10-21T20:00:00Z", at(1792612800)},
      {"2026-10-21T20:00:00.5Z", at(1792612800, 500)},
      {"2026-10-21T20:00:00.123456Z", at(1792612800, 123)},
      {"2026-10-21T22:00:00+02:00", at(1792612800)},
      {"2026-10-21T15:00:00-05:00", at(1792612800)},
      {"2026-10-21T20:00:00", at(1792612800)},
      {"2026-10-21t20:00:00z", at(1792612800)},
      {"2024-02-29T00:00:00Z", at(1709164800)},
      {"2026-10-20T24:00:00Z", at(1792540800)},
      {"1900-01-01T00:00:00Z", at(-2208988800LL)},
  };
  for (const auto& c : good) {
    const auto t = soap::parse_date_time(c.text);
    check(t.has_value() && *t == c.t, std::string("dateTime '") + c.text + "'");
  }
  const char* bad[] = {"",
                       "garbage",
                       "2026-02-29T00:00:00Z",
                       "2026-13-01T00:00:00Z",
                       "2026-00-01T00:00:00Z",
                       "2026-10-32T00:00:00Z",
                       "2026-10-21 20:00:00Z",
                       "2026-10-21T20:00Z",
                       "2026-10-21T20:60:00Z",
                       "2026-10-21T24:00:01Z",
                       "2026-10-21T20:00:00+2:00",
                       "2026-10-21T20:00:00Zjunk",
                       "2026-10-21T20:00:00.Z",
                       "1899-12-31T23:59:59Z",
                       "2201-01-01T00:00:00Z",
                       "26-10-21T20:00:00Z"};
  for (const char* b : bad) {
    check(!soap::parse_date_time(b).has_value(), std::string("dateTime '") + b + "' refused");
  }

  const system_clock::time_point now = at(1792612800);
  auto off = soap::parse_time_offset("2026-10-21T20:02:00Z", now);
  check(off && off->count() == 120000, "offset of a dateTime two minutes ahead");
  off = soap::parse_time_offset("2026-10-21T19:59:00Z", now);
  check(off && off->count() == -60000, "offset of a dateTime in the past is negative");
  off = soap::parse_time_offset("PT60S", now);
  check(off && off->count() == 60000, "offset of a duration");
  check(!soap::parse_time_offset("junk", now), "offset of junk refused");
  check(!soap::parse_time_offset("", now), "offset of nothing refused");

  check(soap::format_date_time(at(0)) == "1970-01-01T00:00:00Z", "format the epoch");
  check(soap::format_date_time(at(1792612800, 900)) == "2026-10-21T20:00:00Z",
        "format keeps whole seconds");
}

// ---------------------------------------------------------------------
// Paths and bodies
// ---------------------------------------------------------------------
void test_routes() {
  using Kind = soap::Route::Kind;
  const std::string id = "0123456789abcdef0123456789abcdef";

  soap::Route r = soap::parse_route(std::string(kCamPath) + "/onvif/event_service");
  check(r.kind == Kind::kCreate, "route: event service path is a Create");
  check(r.key == "192.168.1.109", "route: port 80 dropped from the key");
  check(r.base == kCamPath, "route: base");
  check(r.suffix == "/onvif/event_service", "route: suffix");

  r = soap::parse_route(std::string(kCamPath) + "/status");
  check(r.kind == Kind::kStatus, "route: status probe");

  r = soap::parse_route(std::string(kCamPath) + "/s/" + id);
  check(r.kind == Kind::kSubscription && r.id == id, "route: pull point");

  std::string upper = id;
  for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  r = soap::parse_route(std::string(kCamPath) + "/s/" + upper);
  check(r.kind == Kind::kCreate, "route: an upper-case id is not a pull point");
  r = soap::parse_route(std::string(kCamPath) + "/s/" + id + "/");
  check(r.kind == Kind::kCreate, "route: a trailing slash is not a pull point");

  r = soap::parse_route(std::string(kPrefix) + "Cam%2DOne.LOCAL./8000/onvif/Events");
  check(r.kind == Kind::kCreate && r.key == "cam-one.local:8000",
        "route: decoded, lower-cased, trailing dot stripped");
  check(r.base == std::string(kPrefix) + "Cam%2DOne.LOCAL./8000", "route: base kept encoded");

  r = soap::parse_route(std::string(kPrefix) + "10.0.0.7/");
  check(r.kind == Kind::kCreate && r.key == "10.0.0.7" && r.suffix.empty(),
        "route: empty port and suffix");
  r = soap::parse_route(std::string(kPrefix) + "10.0.0.7/0/x");
  check(r.key == "10.0.0.7", "route: port 0 dropped from the key");
  r = soap::parse_route(std::string(kPrefix) + "10.0.0.7/8000/x");
  check(r.key == "10.0.0.7:8000", "route: other ports kept");

  const std::string bad[] = {
      std::string(kPrefix) + "10.0.0.7",      std::string(kPrefix),
      std::string(kPrefix) + "/80/x",         "/onvif/event_service",
      "/onvif-recorder/pullpoint",            std::string(kPrefix) + "%zz/80/x",
      std::string(kPrefix) + "a%0Ab/80/x",    std::string(kPrefix) + "a%2/80/x",
      std::string(kPrefix) + "host/8%00/x",
  };
  for (const auto& b : bad) {
    r = soap::parse_route(b);
    check(r.kind == Kind::kInvalid && !r.error.empty(), "route: '" + b + "' refused");
  }

  std::string out;
  check(soap::percent_decode("a%2Fb%20c", &out) && out == "a/b c", "percent decode");
  check(!soap::percent_decode("%4", &out), "percent decode: short escape refused");
}

void test_request_bodies() {
  soap::Request req;
  std::string err;
  check(soap::parse_request(create_body("PT60S"), &req, &err) &&
            req.op == "CreatePullPointSubscription",
        "body: Protect's Create");
  check(req.param("InitialTerminationTime") != nullptr &&
            *req.param("InitialTerminationTime") == "PT60S",
        "body: InitialTerminationTime read");
  check(req.param("Filter") == nullptr, "body: no Filter");

  const std::string prefixed =
      "<env:Envelope xmlns:env=\"http://www.w3.org/2003/05/soap-envelope\""
      " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\"><env:Header><wsa:Action"
      " xmlns:wsa=\"http://www.w3.org/2005/08/addressing\" env:mustUnderstand=\"1\">x"
      "</wsa:Action></env:Header><env:Body>\n  <!-- c --> <tev:PullMessages>"
      "<tev:Timeout> PT2S </tev:Timeout><tev:MessageLimit>10</tev:MessageLimit>"
      "</tev:PullMessages></env:Body></env:Envelope>";
  check(soap::parse_request(prefixed, &req, &err) && req.op == "PullMessages",
        "body: prefixed operation after whitespace and a comment");
  check(req.params.size() == 2 && *req.param("Timeout") == "PT2S" &&
            *req.param("MessageLimit") == "10",
        "body: parameters trimmed");

  const std::string soap11 =
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
      "<Unsubscribe xmlns=\"http://docs.oasis-open.org/wsn/b-2\"/></s:Body></s:Envelope>";
  check(soap::parse_request(soap11, &req, &err) && req.op == "Unsubscribe",
        "body: dispatch by local name only");

  struct Bad {
    std::string body;
    const char* why;
  };
  const Bad bad[] = {
      {"", "empty"},
      {"not xml at all", "not XML"},
      {"<?xml version=\"1.0\"?><!DOCTYPE x [<!ENTITY a \"b\">]>" + create_body(""), "DOCTYPE"},
      {"<!doctype html><html/>", "lower-case doctype"},
      {"<Envelope/>", "no Body"},
      {"<s:Envelope xmlns:s=\"x\"><s:Body/></s:Envelope>", "empty Body"},
      {"<s:Envelope xmlns:s=\"x\"><s:Body> text </s:Body></s:Envelope>", "Body with only text"},
      {"<Root><Body><Op/></Body></Root>", "not an Envelope"},
  };
  for (const auto& b : bad) {
    err.clear();
    check(!soap::parse_request(b.body, &req, &err) && !err.empty(),
          std::string("body refused: ") + b.why);
  }
}

// ---------------------------------------------------------------------
// HTTP behaviour
// ---------------------------------------------------------------------
void test_create() {
  Fixture f;
  check(f.started && f.port != 0 && f.server.running(), "server started on a free port");
  f.up();

  const std::string path = std::string(kCamPath) + "/onvif/event_service";
  struct Case {
    std::string itt;
    int64_t lo;
    int64_t hi;
  };
  const std::string later = soap::format_date_time(system_clock::now() + std::chrono::seconds(120));
  const Case cases[] = {
      {"PT60S", 59, 61},       {"PT1M", 59, 61},  {"P0DT0H1M0S", 59, 61},
      {"PT90.5S", 90, 92},     {later, 118, 121}, {"PT5S", 29, 31},
      {"PT3600S", 599, 601},   {"", 59, 61},      {"not a time", 59, 61},
  };
  for (const auto& c : cases) {
    const std::string label = "create '" + c.itt + "'";
    Http r = call(f.port, path, create_body(c.itt), label);
    check(r.status == 200, label + ": accepted");
    check(text_of(r.body, "Action").find("CreatePullPointSubscriptionResponse") !=
              std::string::npos,
          label + ": wsa:Action");
    const int64_t life = lifetime_s(r.body);
    check(life >= c.lo && life <= c.hi,
          label + ": lifetime " + std::to_string(life) + " s in [" + std::to_string(c.lo) +
              ", " + std::to_string(c.hi) + "]");
    const std::string address = text_of(r.body, "Address");
    const std::string expect = "http://127.0.0.1:" + std::to_string(f.port) + kCamPath + "/s/";
    check(address.rfind(expect, 0) == 0 && address.size() == expect.size() + 32,
          label + ": Address " + address);
    check(count_of(r.body, "ReferenceParameters") == 0, label + ": no ReferenceParameters");
  }

  // The camera part of the Address is echoed exactly as Protect encoded it.
  f.up(kNamedIp);
  Http r = call(f.port, std::string(kPrefix) + "Cam%2DOne.LOCAL/8000/onvif/Events",
                create_body("PT60S"), "create for an encoded host");
  check(r.status == 200, "encoded host: accepted");
  check(text_of(r.body, "Address").find(std::string(kPrefix) + "Cam%2DOne.LOCAL/8000/s/") !=
            std::string::npos,
        "encoded host: Address keeps the encoding");

  // Dispatch reads the body only: a misleading SOAPAction or action
  // parameter changes nothing.
  r = call(f.port, path, create_body("PT60S"), "create with a misleading SOAPAction",
           {"SOAPAction: \"http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/"
            "RenewRequest\""});
  check(r.status == 200 && count_of(r.body, "CreatePullPointSubscriptionResponse") == 1,
        "misleading SOAPAction ignored");
  r = call(f.port, path, create_body("PT60S"), "create with an action parameter",
           {"Content-Type: application/soap+xml; charset=utf-8; action=\"Unsubscribe\""});
  check(r.status == 200 && count_of(r.body, "CreatePullPointSubscriptionResponse") == 1,
        "Content-Type action ignored");
}

void test_declines() {
  {
    Fixture f(/*ready=*/false);
    f.up();
    Http r = call(f.port, std::string(kCamPath) + "/onvif/event_service", create_body("PT60S"),
                  "create while starting");
    check(is_fault(r, "ter:ActionNotSupported") && r.body.find("starting") != std::string::npos,
          "declined while starting");
  }
  Fixture f;
  f.up();
  f.up(kUnsharedIp);
  Http r = call(f.port, std::string(kPrefix) + "10.0.0.7/8000/onvif/event_service",
                create_body("PT60S"), "create for an unshared camera");
  check(is_fault(r, "ter:ActionNotSupported") && r.body.find("not shared") != std::string::npos,
        "declined: not shared");
  r = call(f.port, std::string(kPrefix) + "10.9.9.9/80/onvif/event_service",
           create_body("PT60S"), "create for an unknown camera");
  check(is_fault(r, "ter:ActionNotSupported") &&
            r.body.find("unknown camera") != std::string::npos,
        "declined: unknown camera");

  const std::string filtered = protect_envelope(
      "<CreatePullPointSubscription xmlns=\"http://www.onvif.org/ver10/events/wsdl\">"
      "<Filter><TopicExpression xmlns=\"http://docs.oasis-open.org/wsn/b-2\" Dialect=\"x\">"
      "tns1:VideoSource</TopicExpression></Filter>"
      "<InitialTerminationTime>PT60S</InitialTerminationTime></CreatePullPointSubscription>");
  r = call(f.port, std::string(kCamPath) + "/onvif/event_service", filtered, "filtered create");
  check(is_fault(r, "ter:ActionNotSupported"), "declined: Filter");
  const std::string policy = protect_envelope(
      "<CreatePullPointSubscription xmlns=\"http://www.onvif.org/ver10/events/wsdl\">"
      "<InitialTerminationTime>PT60S</InitialTerminationTime>"
      "<SubscriptionPolicy><ChangedOnly>true</ChangedOnly></SubscriptionPolicy>"
      "</CreatePullPointSubscription>");
  r = call(f.port, std::string(kCamPath) + "/onvif/event_service", policy, "policy create");
  check(is_fault(r, "ter:ActionNotSupported"), "declined: SubscriptionPolicy");

  // Operations sent to the wrong kind of path.
  r = call(f.port, std::string(kCamPath) + "/onvif/event_service", capabilities_body(),
           "capabilities on the create path");
  check(is_fault(r, "ter:ActionNotSupported"), "GetServiceCapabilities only on /status");
  r = call(f.port, std::string(kCamPath) + "/status", create_body("PT60S"),
           "create on the status path");
  check(is_fault(r, "ter:ActionNotSupported"), "Create not on /status");
  r = call(f.port, std::string(kCamPath) + "/onvif/event_service",
           protect_envelope("<GetEventProperties xmlns=\"http://www.onvif.org/ver10/events/"
                            "wsdl\"/>"),
           "GetEventProperties");
  check(is_fault(r, "ter:ActionNotSupported"), "GetEventProperties not supported");
}

void test_status_probe() {
  Fixture f;
  const std::string path = std::string(kCamPath) + "/status";
  Http r = call(f.port, path, capabilities_body(), "status before the camera is up");
  check(is_fault(r, "ter:ActionNotSupported"), "status no while the camera is not up");
  f.up();
  r = call(f.port, path, capabilities_body(), "status while up");
  check(r.status == 200 && attr_of(r.body, "Capabilities", "WSPullPointSupport") == "true",
        "status yes while up");
  check(text_of(r.body, "Action").find("GetServiceCapabilitiesResponse") != std::string::npos,
        "status: wsa:Action");
  f.up(kUnsharedIp);
  r = call(f.port, std::string(kPrefix) + "10.0.0.7/8000/status", capabilities_body(),
           "status for an unshared camera");
  check(is_fault(r, "ter:ActionNotSupported"), "status no for an unshared camera");
}

void test_pull_order_and_snapshot() {
  Fixture f;
  f.up();
  // The camera's current state before Protect subscribes.
  const std::string state = upstream_response({motion("true")});
  f.hub.on_upstream_response(kIp, state);
  const std::string sub = f.subscribe();
  check(!sub.empty(), "subscribed");

  Http r = call(f.port, sub, pull_body("PT2S", 10), "first pull");
  check(r.status == 200 && count_of(r.body, "NotificationMessage") == 1,
        "first pull returns the snapshot");
  const std::string initialized = onvif::pullpoint::as_initialized(fragments_of(state)[0]);
  check(r.body.find(initialized) != std::string::npos, "snapshot is the Initialized fragment");
  check(text_of(r.body, "Action").find("PullMessagesResponse") != std::string::npos,
        "pull: wsa:Action");
  check(lifetime_s(r.body) >= 55, "pull: TerminationTime ahead");

  const std::string burst = upstream_response(
      {motion("false"), notification("tns1:Device/Trigger/DigitalInput", "true", "Changed"),
       motion("true")});
  const std::vector<std::string> frags = fragments_of(burst);
  check(frags.size() == 3, "three fragments upstream");
  f.hub.on_upstream_response(kIp, burst);
  r = call(f.port, sub, pull_body("PT2S", 2), "pull with MessageLimit 2");
  check(count_of(r.body, "NotificationMessage") == 2, "MessageLimit respected");
  const size_t a = r.body.find(frags[0]);
  const size_t b = r.body.find(frags[1]);
  check(a != std::string::npos && b != std::string::npos && a < b,
        "fragments verbatim and in order");
  r = call(f.port, sub, pull_body("PT2S", 10), "pull the rest");
  check(count_of(r.body, "NotificationMessage") == 1 &&
            r.body.find(frags[2]) != std::string::npos,
        "remaining fragment delivered");

  // MessageLimit outside [1, 100] is clamped; an unreadable one is ignored.
  f.hub.on_upstream_response(kIp, burst);
  r = call(f.port, sub, pull_body("PT0S", 0), "pull with MessageLimit 0");
  check(count_of(r.body, "NotificationMessage") == 1, "MessageLimit 0 acts as 1");
  r = call(f.port, sub,
           protect_envelope("<PullMessages xmlns=\"http://www.onvif.org/ver10/events/wsdl\">"
                            "<Timeout>PT0S</Timeout><MessageLimit>lots</MessageLimit>"
                            "</PullMessages>"),
           "pull with an unreadable MessageLimit");
  check(count_of(r.body, "NotificationMessage") == 2, "unreadable MessageLimit ignored");

  // SetSynchronizationPoint queues the current state again.
  r = call(f.port, sub, sync_body(), "SetSynchronizationPoint");
  check(r.status == 200 && count_of(r.body, "SetSynchronizationPointResponse") == 1,
        "SetSynchronizationPoint answered");
  r = call(f.port, sub, pull_body("PT0S", 100), "pull after SetSynchronizationPoint");
  check(count_of(r.body, "NotificationMessage") >= 1 &&
            r.body.find("PropertyOperation=\"Initialized\"") != std::string::npos,
        "state queued again as Initialized");
}

void test_response_cap() {
  Fixture f;
  f.up();
  const std::string sub = f.subscribe();
  const std::string pad = "<tt:SimpleItem Name=\"Pad\" Value=\"" + std::string(50000, 'x') +
                          "\"/>";
  std::vector<std::string> notes;
  for (int i = 0; i < 12; ++i) {
    notes.push_back(notification("tns1:Device/Trigger/Relay", std::to_string(i), "", pad));
  }
  f.hub.on_upstream_response(kIp, upstream_response(notes));
  size_t delivered = 0;
  for (int i = 0; i < 4; ++i) {
    Http r = call(f.port, sub, pull_body("PT0S", 100), "pull of large messages");
    check(r.body.size() <= 512 * 1024, "large pull within 512 KiB");
    delivered += static_cast<size_t>(std::max(0, count_of(r.body, "NotificationMessage")));
  }
  check(delivered >= 2, "large messages delivered across pulls");
}

void test_held_pull() {
  Fixture f;
  f.up();
  const std::string sub = f.subscribe();

  // A message published while a pull is held is answered at once.
  Http held;
  steady_clock::time_point answered;
  std::thread t([&] {
    held = request(f.port, "POST", sub, pull_body("PT2S", 10));
    answered = steady_clock::now();
  });
  std::this_thread::sleep_for(milliseconds(400));
  const steady_clock::time_point published = steady_clock::now();
  f.hub.on_upstream_response(kIp, upstream_response({motion("true")}));
  t.join();
  check_response(held, "held pull");
  const int64_t latency = ms_of(answered - published);
  check(latency >= 0 && latency < 100,
        "held pull answered " + std::to_string(latency) + " ms after the publish");
  check(count_of(held.body, "NotificationMessage") == 1, "held pull carries the message");

  // With nothing published it returns empty at its Timeout.
  steady_clock::time_point start = steady_clock::now();
  Http r = call(f.port, sub, pull_body("PT1S", 10), "pull until its timeout");
  int64_t took = ms_of(steady_clock::now() - start);
  check(r.status == 200 && count_of(r.body, "NotificationMessage") == 0,
        "timed-out pull is an empty 200");
  check(took >= 800 && took <= 1200, "PT1S pull took " + std::to_string(took) + " ms");

  // Long Timeouts are held for at most 1 s.
  start = steady_clock::now();
  r = call(f.port, sub, pull_body("PT10S", 10), "pull with a long timeout");
  took = ms_of(steady_clock::now() - start);
  check(r.status == 200 && count_of(r.body, "NotificationMessage") == 0, "long pull empty");
  check(took >= 800 && took <= 1200, "PT10S pull held " + std::to_string(took) + " ms");
}

void test_preempt() {
  Fixture f;
  f.up();
  const std::string sub = f.subscribe();
  Http first;
  Http second;
  steady_clock::time_point first_done;
  steady_clock::time_point second_start;
  const steady_clock::time_point first_start = steady_clock::now();
  std::thread a([&] {
    first = request(f.port, "POST", sub, pull_body("PT2S", 10));
    first_done = steady_clock::now();
  });
  std::this_thread::sleep_for(milliseconds(300));
  second_start = steady_clock::now();
  std::thread b([&] { second = request(f.port, "POST", sub, pull_body("PT1S", 10)); });
  a.join();
  b.join();
  check_response(first, "pre-empted pull");
  check_response(second, "pre-empting pull");
  check(first.status == 200 && count_of(first.body, "NotificationMessage") == 0,
        "pre-empted pull is an empty 200");
  const int64_t after = ms_of(first_done - second_start);
  check(after < 200, "pre-empted " + std::to_string(after) + " ms after the second pull");
  check(ms_of(first_done - first_start) < 800, "pre-empted pull did not wait out its hold");
  check(second.status == 200, "second pull answered");
}

void test_renew_unsubscribe() {
  Fixture f;
  f.up();
  const std::string sub = f.subscribe();

  Http r = call(f.port, sub, renew_body("PT120S"), "renew PT120S");
  check(r.status == 200 && count_of(r.body, "RenewResponse") == 1, "renew answered");
  const int64_t life = lifetime_s(r.body);
  check(life >= 119 && life <= 121, "renew lifetime " + std::to_string(life) + " s");
  check(text_of(r.body, "Action").find("RenewResponse") != std::string::npos, "renew: Action");
  const std::string later = soap::format_date_time(system_clock::now() + std::chrono::seconds(90));
  r = call(f.port, sub, renew_body(later), "renew to a dateTime");
  check(r.status == 200 && lifetime_s(r.body) >= 88 && lifetime_s(r.body) <= 91,
        "renew to an absolute dateTime");

  // A pull point Protect never created here (a recorder restart) is adopted.
  const std::string other = std::string(kCamPath) + "/s/0123456789abcdef0123456789abcdef";
  r = call(f.port, other, pull_body("PT0S", 10), "pull on an unknown id");
  check(r.status == 200, "unknown id adopted");

  r = call(f.port, sub,
           protect_envelope("<GetCurrentMessage xmlns=\"http://docs.oasis-open.org/wsn/b-2\"/>"),
           "unsupported operation on a pull point");
  check(is_fault(r, "ter:ActionNotSupported"), "unsupported operation faults");

  r = call(f.port, sub, unsubscribe_body(), "unsubscribe");
  check(r.status == 200 && count_of(r.body, "UnsubscribeResponse") == 1, "unsubscribe 200");
  r = call(f.port, sub, unsubscribe_body(), "unsubscribe again");
  check(r.status == 200, "unsubscribe is always 200");
  r = call(f.port, sub, pull_body("PT0S", 10), "pull after unsubscribe");
  check(is_fault(r, "ter:ResourceUnknown"), "pull after unsubscribe: ResourceUnknown");
  r = call(f.port, sub, renew_body("PT60S"), "renew after unsubscribe");
  check(is_fault(r, "ter:ActionNotSupported"), "renew after unsubscribe faults");
  r = call(f.port, sub, sync_body(), "sync after unsubscribe");
  check(is_fault(r, "ter:ResourceUnknown"), "sync after unsubscribe: ResourceUnknown");

  // A camera that stops being shared turns its pull points to draining:
  // pulls keep working and Renew faults so Protect moves to the camera.
  const std::string draining = f.subscribe();
  f.hub.register_camera(HubCamera{kIp, kMac, false});
  r = call(f.port, draining, pull_body("PT0S", 10), "pull while draining");
  check(r.status == 200, "draining pull answered");
  r = call(f.port, draining, renew_body("PT60S"), "renew while draining");
  check(is_fault(r, "ter:ActionNotSupported") && r.body.find("not shared") != std::string::npos,
        "draining renew faults with the reason");
}

void test_bad_requests() {
  Fixture f;
  f.up();
  const std::string create_path = std::string(kCamPath) + "/onvif/event_service";

  // A body over 64 KiB, and one just under it.
  std::string big = create_body("PT60S");
  big.insert(big.find("<s:Body>"), "<!--" + std::string(64 * 1024, 'x') + "-->");
  Http r = call(f.port, create_path, big, "oversize body");
  check(is_fault(r, "ter:InvalidArgVal"), "oversize body faults");
  std::string fits = create_body("PT60S");
  fits.insert(fits.find("<s:Body>"),
              "<!--" + std::string(64 * 1024 - fits.size() - 7, 'x') + "-->");
  check(fits.size() == 64 * 1024, "body of exactly 64 KiB built");
  r = call(f.port, create_path, fits, "64 KiB body");
  check(r.status == 200, "64 KiB body accepted");

  r = call(f.port, create_path,
           "<?xml version=\"1.0\"?><!DOCTYPE s:Envelope [<!ENTITY x \"PT60S\">]>" +
               create_body("&x;"),
           "DOCTYPE body");
  check(is_fault(r, "ter:InvalidArgVal") && r.body.find("DOCTYPE") != std::string::npos,
        "DOCTYPE faults");
  r = call(f.port, create_path, "hello", "non-XML body");
  check(is_fault(r, "ter:InvalidArgVal"), "non-XML body faults");
  r = call(f.port, create_path, "", "empty body");
  check(is_fault(r, "ter:InvalidArgVal"), "empty body faults");
  r = call(f.port, create_path, "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
                                "<s:Body/></s:Envelope>",
           "empty SOAP Body");
  check(is_fault(r, "ter:InvalidArgVal"), "empty SOAP Body faults");

  const std::string bad_paths[] = {"/", "/onvif/event_service", "/onvif-recorder/pullpoint/",
                                   "/onvif-recorder/pullpoint/192.168.1.109",
                                   "/onvif-recorder/pullpoint/%zz/80/x"};
  for (const auto& p : bad_paths) {
    r = call(f.port, p, create_body("PT60S"), "bad path " + p);
    check(is_fault(r, "ter:InvalidArgVal"), "bad path faults: " + p);
  }

  // An operation name echoed in a fault reason keeps the fault
  // well-formed even when it is long and made of multi-byte characters.
  std::string wide;
  for (int i = 0; i < 150; ++i) wide += "\xC3\xA9";
  const std::string wide_op = protect_envelope("<a" + wide + " xmlns=\"urn:x\"/>");
  r = call(f.port, std::string(kCamPath) + "/status", wide_op, "long multi-byte operation");
  check(is_fault(r, "ter:ActionNotSupported"), "long multi-byte operation faults");
  r = call(f.port, create_path, wide_op, "long multi-byte operation on the create path");
  check(is_fault(r, "ter:ActionNotSupported"), "long multi-byte operation faults on create");

  r = request(f.port, "GET", create_path, "");
  check_response(r, "GET");
  check(r.status == 405 && header(r, "allow") == "POST" &&
            count_of(r.body, "Fault") == 1,
        "GET is 405 with a SOAP fault");
  r = request(f.port, "PUT", create_path, create_body("PT60S"));
  check_response(r, "PUT");
  check(r.status == 405, "PUT is 405");
  r = request(f.port, "HEAD", create_path, "");
  check_response(r, "HEAD", /*head=*/true);
  check(r.status == 405, "HEAD is 405");
}

// ---------------------------------------------------------------------
// Raw sockets
// ---------------------------------------------------------------------
struct RawReply {
  std::string head;  // status line and headers
  std::string body;
  bool closed = false;  // the server closed the connection
};

RawReply raw_exchange(uint16_t port, const std::string& request) {
  RawReply out;
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return out;
  }
  timeval tv{};
  tv.tv_sec = 5;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t n = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) break;
    sent += static_cast<size_t>(n);
  }
  std::string all;
  char buf[4096];
  for (;;) {
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n == 0) {
      out.closed = true;
      break;
    }
    if (n < 0) break;
    all.append(buf, static_cast<size_t>(n));
  }
  close(fd);
  const size_t split = all.find("\r\n\r\n");
  if (split == std::string::npos) {
    out.head = all;
  } else {
    out.head = all.substr(0, split + 2);
    out.body = all.substr(split + 4);
  }
  return out;
}

// Value of header @p name in @p head, or nullopt.
std::optional<std::string> raw_header(const std::string& head, const std::string& name) {
  std::string lower = head;
  for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  const size_t at = lower.find("\r\n" + name + ":");
  if (at == std::string::npos) return std::nullopt;
  const size_t start = at + 2 + name.size() + 1;
  const size_t end = lower.find("\r\n", start);
  std::string v = lower.substr(start, end - start);
  while (!v.empty() && v.front() == ' ') v.erase(v.begin());
  return v;
}

void test_raw_http() {
  Fixture f;
  f.up();
  const std::string body = capabilities_body();
  const std::string path = std::string(kCamPath) + "/status";
  const std::string versions[] = {
      "POST " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\n",
      "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n",
  };
  for (const auto& start : versions) {
    const std::string label = start.substr(start.find("HTTP/"), 8);
    const RawReply reply = raw_exchange(
        f.port, start + "Content-Type: application/soap+xml; charset=utf-8\r\n"
                        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    check(reply.head.rfind("HTTP/1.", 0) == 0 && reply.head.find(" 200 ") != std::string::npos,
          label + ": 200");
    const auto conn = raw_header(reply.head, "connection");
    check(conn && conn->find("close") != std::string::npos, label + ": Connection: close");
    const auto length = raw_header(reply.head, "content-length");
    check(length && *length == std::to_string(reply.body.size()),
          label + ": Content-Length matches");
    check(!raw_header(reply.head, "transfer-encoding"), label + ": not chunked");
    check(reply.closed, label + ": server closed the connection");
    check(count_of(reply.body, "GetServiceCapabilitiesResponse") == 1, label + ": SOAP body");
  }

  // Protect's raw-socket transport: HTTP/1.0 with its header order, read
  // until the server closes and split at the first blank line.
  const RawReply protect_raw = raw_exchange(
      f.port, "POST " + path + " HTTP/1.0\r\nHost: 127.0.0.1:" + std::to_string(f.port) +
                  "\r\nContent-Length: " + std::to_string(body.size()) +
                  "\r\nContent-Type: application/soap+xml; charset=utf-8\r\nConnection: close"
                  "\r\n\r\n" + body);
  const auto raw_length = raw_header(protect_raw.head, "content-length");
  check(protect_raw.closed && protect_raw.head.rfind("HTTP/1.", 0) == 0 &&
            protect_raw.head.find(" 200 ") != std::string::npos &&
            raw_length && *raw_length == std::to_string(protect_raw.body.size()) &&
            count_of(protect_raw.body, "GetServiceCapabilitiesResponse") == 1,
        "Protect's raw-socket request answered in full");

  // A method that is not UTF-8 is echoed in the 405 fault without
  // breaking its XML.
  const RawReply odd_method = raw_exchange(
      f.port, "P\xFFST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
  check(odd_method.head.find(" 405 ") != std::string::npos &&
            parse_strict(odd_method.body) != nullptr && count_of(odd_method.body, "Fault") == 1,
        "non-UTF-8 method: well-formed 405 fault");

  // An absolute-form request target is routed by its path.
  const RawReply absolute = raw_exchange(
      f.port, "POST http://127.0.0.1:" + std::to_string(f.port) + path +
                  " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                  std::to_string(body.size()) + "\r\n\r\n" + body);
  check(absolute.head.find(" 200 ") != std::string::npos &&
            count_of(absolute.body, "GetServiceCapabilitiesResponse") == 1,
        "absolute-form target routed by its path");

  // A fault over HTTP/1.0 follows the same rules.
  const std::string bad = "junk";
  const RawReply reply = raw_exchange(
      f.port, "POST " + path + " HTTP/1.0\r\nContent-Length: " + std::to_string(bad.size()) +
                  "\r\n\r\n" + bad);
  check(reply.head.find(" 500 ") != std::string::npos, "HTTP/1.0 fault: 500");
  const auto length = raw_header(reply.head, "content-length");
  check(length && *length == std::to_string(reply.body.size()) && !reply.body.empty(),
        "HTTP/1.0 fault: Content-Length");
  check(count_of(reply.body, "Fault") == 1, "HTTP/1.0 fault: SOAP Fault");
}

// ---------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------
void test_stop_releases_pulls() {
  Fixture f;
  f.up();
  const std::string sub = f.subscribe();
  const std::string sub2 = f.subscribe();
  Http held[2];
  steady_clock::time_point done[2];
  std::thread a([&] {
    held[0] = request(f.port, "POST", sub, pull_body("PT2S", 10));
    done[0] = steady_clock::now();
  });
  std::thread b([&] {
    held[1] = request(f.port, "POST", sub2, pull_body("PT2S", 10));
    done[1] = steady_clock::now();
  });
  std::this_thread::sleep_for(milliseconds(400));
  const steady_clock::time_point stop_at = steady_clock::now();
  f.server.stop();
  const int64_t stop_took = ms_of(steady_clock::now() - stop_at);
  a.join();
  b.join();
  for (int i = 0; i < 2; ++i) {
    const std::string label = "pull held across stop #" + std::to_string(i);
    check_response(held[i], label);
    check(held[i].status == 200 && count_of(held[i].body, "PullMessagesResponse") == 1 &&
              count_of(held[i].body, "NotificationMessage") == 0,
          label + ": empty 200");
    const int64_t released = ms_of(done[i] - stop_at);
    check(released < 300, label + ": released " + std::to_string(released) + " ms after stop");
  }
  check(stop_took < 1000, "stop() took " + std::to_string(stop_took) + " ms");
  check(!f.server.running(), "not running after stop");
  const Http after = request(f.port, "POST", sub, pull_body("PT0S", 10));
  check(!after.transport_ok, "connection refused after stop");
  f.server.stop();
  check(!f.server.running(), "second stop is harmless");
}

void test_bind_retry() {
  Fixture first;
  PullPointHub hub{PullPointHub::Options{}};
  PullPointServer second(&hub);
  const steady_clock::time_point start = steady_clock::now();
  const bool ok = second.start(first.port, 2);
  const int64_t took = ms_of(steady_clock::now() - start);
  check(!ok && !second.running(), "start fails while the port is taken");
  check(took >= 450 && took < 2000, "bind retried 500 ms later (" + std::to_string(took) + ")");
  check(second.start(0), "start on a free port afterwards");
  check(second.running() && second.port() != 0 && second.port() != first.port,
        "running on its own port");
  second.stop();
}

}  // namespace

int main() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  xmlInitParser();

  test_durations();
  test_date_times();
  test_routes();
  test_request_bodies();
  test_create();
  test_declines();
  test_status_probe();
  test_pull_order_and_snapshot();
  test_response_cap();
  test_held_pull();
  test_preempt();
  test_renew_unsubscribe();
  test_bad_requests();
  test_raw_http();
  test_stop_releases_pulls();
  test_bind_retry();

  check(g_responses.load() > 50, "responses checked: " + std::to_string(g_responses.load()));
  check(g_unauthorized.load() == 0, "no 401 in any response");
  check(g_empty_ok.load() == 0, "no empty 200 in any response");

  curl_global_cleanup();
  std::cout << "passed: " << g_pass.load() << '\n';
  std::cout << "failed: " << g_fail.load() << '\n';
  return g_fail.load() == 0 ? 0 : 1;
}
