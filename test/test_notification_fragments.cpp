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

// Tests for notification_fragments: every NotificationMessage of the
// recorded PullMessagesResponses (the JSONL captures passed as arguments)
// must come out as a standalone, namespace-well-formed element that is the
// same tree as the original, plus synthetic namespace and size cases.

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "notification_fragments.hpp"

using onvif::pullpoint::as_initialized;
using onvif::pullpoint::extract_fragments;
using onvif::pullpoint::ExtractResult;
using onvif::pullpoint::Fragment;
using onvif::pullpoint::kMaxFragmentBytes;

// ============================================================
// Tiny test framework
// ============================================================
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& msg, const char* file, int line) {
  if (cond) {
    ++g_pass;
  } else {
    std::cerr << "  FAIL [" << file << ":" << line << "] " << msg << "\n";
    ++g_fail;
  }
}

#define CHECK(cond, msg) check((cond), (msg), __FILE__, __LINE__)

static bool run_test(const std::string& name, const std::function<void()>& fn) {
  int before = g_fail;
  std::cout << "[ RUN ] " << name << "\n";
  fn();
  bool ok = (g_fail == before);
  std::cout << (ok ? "[  OK ] " : "[FAIL ] ") << name << "\n\n";
  return ok;
}

// ============================================================
// JSON Lines captures
//
// One flat JSON object per line; the fields used here are
// "soap_action" (string), "response_status" (number) and
// "response" (string).
// ============================================================
struct Exchange {
  std::string action;  // last path segment of soap_action
  int status = 0;
  std::string response;
};

static void append_utf8(unsigned cp, std::string* out) {
  if (cp < 0x80) {
    *out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    *out += static_cast<char>(0xC0 | (cp >> 6));
    *out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    *out += static_cast<char>(0xE0 | (cp >> 12));
    *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    *out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    *out += static_cast<char>(0xF0 | (cp >> 18));
    *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    *out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

static bool hex4(const std::string& s, size_t pos, unsigned* v) {
  if (pos + 4 > s.size()) return false;
  *v = 0;
  for (size_t i = pos; i < pos + 4; ++i) {
    const char c = s[i];
    unsigned d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return false;
    *v = *v * 16 + d;
  }
  return true;
}

// Decodes the JSON string that starts just after the opening quote at
// *pos and leaves *pos after the closing quote.
static std::string json_string(const std::string& s, size_t* pos) {
  std::string out;
  size_t p = *pos;
  while (p < s.size() && s[p] != '"') {
    if (s[p] != '\\' || p + 1 >= s.size()) {
      out += s[p++];
      continue;
    }
    const char e = s[p + 1];
    p += 2;
    switch (e) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'u': {
        unsigned cp = 0;
        if (!hex4(s, p, &cp)) break;
        p += 4;
        unsigned lo = 0;
        if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < s.size() && s[p] == '\\' &&
            s[p + 1] == 'u' && hex4(s, p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          p += 6;
        }
        append_utf8(cp, &out);
        break;
      }
      default: out += e; break;  // '"', '\\', '/'
    }
  }
  *pos = p < s.size() ? p + 1 : p;
  return out;
}

static Exchange parse_line(const std::string& line) {
  Exchange ex;
  size_t p = line.find('{');
  if (p == std::string::npos) return ex;
  ++p;
  while (p < line.size()) {
    while (p < line.size() && (line[p] == ',' || line[p] == ' ')) ++p;
    if (p >= line.size() || line[p] != '"') break;
    ++p;
    const std::string key = json_string(line, &p);
    while (p < line.size() && (line[p] == ':' || line[p] == ' ')) ++p;
    if (p < line.size() && line[p] == '"') {
      ++p;
      std::string val = json_string(line, &p);
      if (key == "soap_action") {
        const auto slash = val.rfind('/');
        ex.action = slash == std::string::npos ? val : val.substr(slash + 1);
      } else if (key == "response") {
        ex.response = std::move(val);
      }
    } else {
      std::string num;
      while (p < line.size() && line[p] != ',' && line[p] != '}') num += line[p++];
      if (key == "response_status") ex.status = std::atoi(num.c_str());
    }
  }
  return ex;
}

static std::vector<Exchange> read_capture(const std::string& path) {
  std::vector<Exchange> out;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line))
    if (!line.empty()) out.push_back(parse_line(line));
  return out;
}

// ============================================================
// libxml2 helpers (independent of the code under test)
// ============================================================
struct DocFree {
  void operator()(xmlDoc* d) const { xmlFreeDoc(d); }
};
using DocPtr = std::unique_ptr<xmlDoc, DocFree>;

static const char* str(const xmlChar* s) { return s ? reinterpret_cast<const char*>(s) : ""; }

// Parses camera XML the way the recorder does (recovering).
static DocPtr lenient_parse(const std::string& xml) {
  return DocPtr(xmlReadMemory(xml.data(), static_cast<int>(xml.size()), nullptr, nullptr,
                              XML_PARSE_RECOVER | XML_PARSE_NONET | XML_PARSE_NOERROR |
                              XML_PARSE_NOWARNING));
}

struct Strict {
  DocPtr doc;
  bool ns_ok = false;  // well-formed and namespace-well-formed
};

// Parses without recovery, as a strict consumer would.
static Strict strict_parse(const std::string& xml) {
  Strict s;
  xmlParserCtxtPtr ctxt = xmlNewParserCtxt();
  if (!ctxt) return s;
  s.doc.reset(xmlCtxtReadMemory(ctxt, xml.data(), static_cast<int>(xml.size()), nullptr,
                                nullptr,
                                XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING));
  s.ns_ok = s.doc && ctxt->wellFormed && ctxt->nsWellFormed;
  xmlFreeParserCtxt(ctxt);
  return s;
}

static const char* local(const xmlNode* n) {
  const char* name = str(n->name);
  const char* colon = std::strrchr(name, ':');
  return colon ? colon + 1 : name;
}

static void collect(xmlNode* n, const char* name, std::vector<xmlNode*>* out) {
  for (; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) continue;
    if (std::strcmp(local(n), name) == 0) out->push_back(n);
    collect(n->children, name, out);
  }
}

static std::vector<xmlNode*> notification_messages(xmlDoc* doc) {
  std::vector<xmlNode*> out;
  collect(xmlDocGetRootElement(doc), "NotificationMessage", &out);
  return out;
}

static xmlNode* child(xmlNode* n, const char* name) {
  for (xmlNode* c = n ? n->children : nullptr; c; c = c->next)
    if (c->type == XML_ELEMENT_NODE && std::strcmp(local(c), name) == 0) return c;
  return nullptr;
}

static std::string content(xmlNode* n) {
  xmlChar* c = n ? xmlNodeGetContent(n) : nullptr;
  std::string s = str(c);
  if (c) xmlFree(c);
  return s;
}

static std::string trimmed(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

static std::string attr(const xmlAttr* a) {
  std::string v;
  for (const xmlNode* t = a ? a->children : nullptr; t; t = t->next) v += str(t->content);
  return v;
}

// PropertyOperation of the first Message element that has one, or "".
static std::string property_op(xmlNode* n) {
  std::vector<xmlNode*> messages;
  collect(n->children, "Message", &messages);
  for (xmlNode* m : messages)
    for (const xmlAttr* a = m->properties; a; a = a->next)
      if (std::strcmp(str(a->name), "PropertyOperation") == 0) return attr(a);
  return {};
}

static const char* href(const xmlNs* ns) { return ns ? str(ns->href) : ""; }

// True when @p a and @p b are the same tree: names, namespace URIs,
// attributes (in order) and text.  @p why says where they differ.
static bool same_tree(const xmlNode* a, const xmlNode* b, std::string* why) {
  if (a->type != b->type) {
    *why = std::string("node type differs at ") + str(a->name);
    return false;
  }
  if (a->type == XML_TEXT_NODE || a->type == XML_CDATA_SECTION_NODE ||
      a->type == XML_COMMENT_NODE) {
    if (std::strcmp(str(a->content), str(b->content)) == 0) return true;
    *why = "text differs: \"" + std::string(str(a->content)).substr(0, 80) + "\"";
    return false;
  }
  if (a->type != XML_ELEMENT_NODE) return true;
  if (std::strcmp(str(a->name), str(b->name)) != 0 ||
      std::strcmp(href(a->ns), href(b->ns)) != 0) {
    *why = std::string("element ") + str(a->name) + " {" + href(a->ns) + "} became " +
           str(b->name) + " {" + href(b->ns) + "}";
    return false;
  }
  const xmlAttr* x = a->properties;
  const xmlAttr* y = b->properties;
  for (; x && y; x = x->next, y = y->next) {
    if (std::strcmp(str(x->name), str(y->name)) != 0 ||
        std::strcmp(href(x->ns), href(y->ns)) != 0 || attr(x) != attr(y)) {
      *why = std::string("attribute ") + str(x->name) + " of " + str(a->name) + " differs";
      return false;
    }
  }
  if (x || y) {
    *why = std::string("attribute count of ") + str(a->name) + " differs";
    return false;
  }
  const xmlNode* c = a->children;
  const xmlNode* d = b->children;
  for (; c && d; c = c->next, d = d->next)
    if (!same_tree(c, d, why)) return false;
  if (c || d) {
    *why = std::string("child count of ") + str(a->name) + " differs";
    return false;
  }
  return true;
}

// Prefixes of the "prefix:local" steps of a topic expression.
static std::vector<std::string> topic_prefixes(const std::string& topic) {
  std::vector<std::string> out;
  std::string token;
  auto flush = [&] {
    const auto colon = token.find(':');
    if (colon != std::string::npos && colon > 0 && colon + 1 < token.size() &&
        token[colon + 1] != '/')
      out.push_back(token.substr(0, colon));
    token.clear();
  };
  for (char c : topic) {
    if (std::strchr(" \t\r\n/|()[],", c)) flush();
    else token += c;
  }
  flush();
  return out;
}

// Namespace URI that @p prefix resolves to at @p node, or "" when it
// does not resolve.
static std::string resolve(xmlDoc* doc, xmlNode* node, const std::string& prefix) {
  return href(xmlSearchNs(doc, node, reinterpret_cast<const xmlChar*>(prefix.c_str())));
}

// Checks one fragment against the NotificationMessage it was copied from.
// Returns the number of Topic prefixes checked.
static size_t check_fragment(const Fragment& f, xmlDoc* src_doc, xmlNode* src,
                             const std::string& where) {
  Strict re = strict_parse(f.xml);
  CHECK(re.doc != nullptr, where + ": fragment does not parse: " + f.xml.substr(0, 200));
  if (!re.doc) return 0;
  CHECK(re.ns_ok, where + ": fragment not namespace-well-formed: " + f.xml.substr(0, 300));
  xmlNode* root = xmlDocGetRootElement(re.doc.get());
  std::string why;
  CHECK(same_tree(src, root, &why), where + ": fragment differs from source: " + why);

  xmlNode* src_topic = child(src, "Topic");
  CHECK(f.topic == trimmed(content(src_topic)), where + ": topic \"" + f.topic + "\"");
  CHECK(f.op == property_op(src), where + ": op \"" + f.op + "\"");
  CHECK(f.op.empty() == f.key.empty(), where + ": key present iff op: " + f.key);
  if (!f.op.empty())
    CHECK(f.key.compare(0, f.topic.size() + 1, f.topic + "|") == 0,
          where + ": key starts with topic: " + f.key);

  size_t checked = 0;
  xmlNode* topic = child(root, "Topic");
  for (const auto& p : topic_prefixes(f.topic)) {
    const std::string want = resolve(src_doc, src_topic, p);
    const std::string got = resolve(re.doc.get(), topic, p);
    CHECK(!got.empty(), where + ": topic prefix " + p + " unresolved in " + f.xml.substr(0, 300));
    CHECK(got == want, where + ": topic prefix " + p + " is {" + got + "}, source {" + want + "}");
    ++checked;
  }
  return checked;
}

// as_initialized() on a fragment: property messages come back as
// Initialized with the same identity, everything else unchanged.
static void check_initialized(const Fragment& f, const std::string& where) {
  const std::string init = as_initialized(f.xml);
  if (f.op.empty() || f.op == "Initialized") {
    CHECK(init == f.xml, where + ": as_initialized changed a " +
          (f.op.empty() ? std::string("non-property") : f.op) + " message");
    return;
  }
  CHECK(init != f.xml, where + ": as_initialized left " + f.op);
  Strict re = strict_parse(init);
  CHECK(re.ns_ok, where + ": initialized fragment not namespace-well-formed");
  const ExtractResult again = extract_fragments(init);
  CHECK(again.fragments.size() == 1, where + ": initialized fragment re-extracts");
  if (again.fragments.size() != 1) return;
  CHECK(again.fragments[0].op == "Initialized", where + ": op " + again.fragments[0].op);
  CHECK(again.fragments[0].key == f.key, where + ": key changed: " + again.fragments[0].key);
  CHECK(again.fragments[0].topic == f.topic, where + ": topic changed");
}

static bool same_fragments(const ExtractResult& a, const ExtractResult& b) {
  if (a.fragments.size() != b.fragments.size()) return false;
  for (size_t i = 0; i < a.fragments.size(); ++i) {
    const Fragment& x = a.fragments[i];
    const Fragment& y = b.fragments[i];
    if (x.xml != y.xml || x.topic != y.topic || x.op != y.op || x.key != y.key ||
        x.protect_relevant != y.protect_relevant)
      return false;
  }
  return true;
}

// ============================================================
// Captures
// ============================================================
struct CaptureTotals {
  size_t responses = 0;
  size_t messages = 0;
  size_t initialized = 0;
  size_t changed = 0;
  size_t relevant = 0;
  size_t multi_prefix_topics = 0;
  size_t element_items = 0;
  size_t topic_prefixes = 0;
};

static void test_capture(const std::string& path, CaptureTotals* totals) {
  const std::vector<Exchange> exchanges = read_capture(path);
  CHECK(!exchanges.empty(), "no exchanges read from " + path);
  const std::string name = path.substr(path.rfind('/') + 1);
  CaptureTotals t;
  for (size_t n = 0; n < exchanges.size(); ++n) {
    const Exchange& ex = exchanges[n];
    const ExtractResult r = extract_fragments(ex.response);
    if (ex.action != "PullMessagesRequest" || ex.status != 200) {
      CHECK(r.fragments.empty(), name + ": fragments from a " + ex.action + " response");
      continue;
    }
    const std::string where = name + " line " + std::to_string(n + 1);
    ++t.responses;
    DocPtr src = lenient_parse(ex.response);
    CHECK(src != nullptr && r.parsed, where + ": response not parsed");
    if (!src) continue;
    const std::vector<xmlNode*> msgs = notification_messages(src.get());
    CHECK(r.fragments.size() == msgs.size(),
          where + ": " + std::to_string(r.fragments.size()) + " fragments for " +
          std::to_string(msgs.size()) + " NotificationMessages");
    CHECK(r.oversize_dropped == 0 && r.stripped == 0, where + ": size limits hit");
    CHECK(same_fragments(r, extract_fragments(ex.response)), where + ": not deterministic");
    if (r.fragments.size() != msgs.size()) continue;

    for (size_t i = 0; i < msgs.size(); ++i) {
      const Fragment& f = r.fragments[i];
      const std::string at = where + " msg " + std::to_string(i);
      t.topic_prefixes += check_fragment(f, src.get(), msgs[i], at);
      check_initialized(f, at);
      ++t.messages;
      if (f.op == "Initialized") ++t.initialized;
      if (f.op == "Changed") ++t.changed;
      if (f.protect_relevant) ++t.relevant;
      if (topic_prefixes(f.topic).size() > 1) ++t.multi_prefix_topics;
      if (f.xml.find("ElementItem") != std::string::npos) ++t.element_items;
    }
  }
  std::cout << "  " << name << ": responses=" << t.responses << " messages=" << t.messages
            << " initialized=" << t.initialized << " changed=" << t.changed
            << " relevant=" << t.relevant << " multi_prefix=" << t.multi_prefix_topics
            << " element_items=" << t.element_items << " topic_prefixes=" << t.topic_prefixes
            << "\n";
  totals->responses += t.responses;
  totals->messages += t.messages;
  totals->initialized += t.initialized;
  totals->changed += t.changed;
  totals->relevant += t.relevant;
  totals->multi_prefix_topics += t.multi_prefix_topics;
  totals->element_items += t.element_items;
  totals->topic_prefixes += t.topic_prefixes;
}

// ============================================================
// Synthetic responses
// ============================================================
static const char kSoap[] = "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"";
static const char kTev[] = "xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\"";
static const char kWsnt[] = "xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\"";
static const char kTt[] = "xmlns:tt=\"http://www.onvif.org/ver10/schema\"";
static const char kTns1[] = "xmlns:tns1=\"http://www.onvif.org/ver10/topics\"";
static const char kSchemaUri[] = "http://www.onvif.org/ver10/schema";
static const char kTopicsUri[] = "http://www.onvif.org/ver10/topics";

// A PullMessagesResponse with @p env_ns declared on the Envelope and
// @p body_ns on the Body.
static std::string response(const std::string& env_ns, const std::string& body_ns,
                            const std::string& messages) {
  return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?><s:Envelope ") + kSoap + " " +
         env_ns + "><s:Body " + body_ns + "><tev:PullMessagesResponse " + kTev +
         "><tev:CurrentTime>2026-10-21T10:00:00Z</tev:CurrentTime>"
         "<tev:TerminationTime>2026-10-21T10:01:00Z</tev:TerminationTime>" +
         messages + "</tev:PullMessagesResponse></s:Body></s:Envelope>";
}

// A wsnt/tt NotificationMessage.
static std::string message(const std::string& topic, const std::string& op,
                           const std::string& source, const std::string& data,
                           const std::string& key = "") {
  std::string m = "<wsnt:NotificationMessage><wsnt:Topic Dialect=\"http://www.onvif.org/"
                  "ver10/tev/topicExpression/ConcreteSet\">" + topic +
                  "</wsnt:Topic><wsnt:Message><tt:Message UtcTime=\"2026-10-21T10:00:00Z\"";
  if (!op.empty()) m += " PropertyOperation=\"" + op + "\"";
  m += "><tt:Source>" + source + "</tt:Source>";
  if (!key.empty()) m += "<tt:Key>" + key + "</tt:Key>";
  m += "<tt:Data>" + data + "</tt:Data></tt:Message></wsnt:Message></wsnt:NotificationMessage>";
  return m;
}

static std::string item(const std::string& name, const std::string& value) {
  return "<tt:SimpleItem Name=\"" + name + "\" Value=\"" + value + "\"/>";
}

// Extracts @p xml and checks every fragment against its source.
static ExtractResult extract_checked(const std::string& xml, const std::string& where) {
  const ExtractResult r = extract_fragments(xml);
  CHECK(r.parsed, where + ": not parsed");
  DocPtr src = lenient_parse(xml);
  if (!src) return r;
  const std::vector<xmlNode*> msgs = notification_messages(src.get());
  CHECK(r.fragments.size() + r.oversize_dropped == msgs.size(), where + ": fragment count");
  if (r.oversize_dropped == 0 && r.stripped == 0 && r.fragments.size() == msgs.size()) {
    for (size_t i = 0; i < msgs.size(); ++i)
      check_fragment(r.fragments[i], src.get(), msgs[i], where + " msg " + std::to_string(i));
  }
  return r;
}

// Namespace URI of @p prefix on the root of fragment @p xml.
static std::string root_ns(const std::string& xml, const std::string& prefix) {
  Strict re = strict_parse(xml);
  if (!re.doc) return {};
  return resolve(re.doc.get(), xmlDocGetRootElement(re.doc.get()), prefix);
}

static void test_nonstandard_wsnt_uri() {
  const std::string xml = response(
      std::string("xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2/\" ") + kTt + " " + kTns1, "",
      message("tns1:VideoSource/MotionAlarm", "Changed", item("Source", "0"),
              item("State", "true")));
  const ExtractResult r = extract_checked(xml, "nonstandard wsnt");
  CHECK(r.fragments.size() == 1, "nonstandard wsnt: one fragment");
  if (r.fragments.empty()) return;
  CHECK(root_ns(r.fragments[0].xml, "wsnt") == "http://docs.oasis-open.org/wsn/b-2/",
        "nonstandard wsnt: URI kept");
  CHECK(r.fragments[0].protect_relevant, "nonstandard wsnt: MotionAlarm is relevant");
}

static void test_prefixes_only_on_envelope() {
  const std::string xml = response(
      std::string(kWsnt) + " " + kTt + " " + kTns1 + " xmlns:unused=\"urn:unused\"", "",
      message("tns1:RuleEngine/CellMotionDetector/Motion", "Initialized",
              item("VideoSourceConfigurationToken", "V") + item("Rule", "R"),
              item("IsMotion", "false")));
  const ExtractResult r = extract_checked(xml, "envelope only");
  CHECK(r.fragments.size() == 1, "envelope only: one fragment");
  if (r.fragments.empty()) return;
  const std::string& x = r.fragments[0].xml;
  CHECK(root_ns(x, "wsnt") == "http://docs.oasis-open.org/wsn/b-2", "envelope only: wsnt");
  CHECK(root_ns(x, "tt") == kSchemaUri, "envelope only: tt");
  CHECK(root_ns(x, "tns1") == kTopicsUri, "envelope only: tns1");
  CHECK(x.find("urn:unused") == std::string::npos, "envelope only: unused prefix declared");
  CHECK(x.find("soap-envelope") == std::string::npos, "envelope only: SOAP prefix declared");
  CHECK(x.find("<?xml") == std::string::npos, "envelope only: XML declaration in fragment");
}

static void test_prefix_collision() {
  // tns1 and tt are bound to wrong URIs on the Envelope and rebound on
  // the Body; inside the message, tt is rebound once more on Data.
  const std::string msg =
      "<wsnt:NotificationMessage><wsnt:Topic>tns1:VideoSource/MotionAlarm</wsnt:Topic>"
      "<wsnt:Message><tt:Message PropertyOperation=\"Changed\"><tt:Source>" +
      item("Source", "0") +
      "</tt:Source><tt:Data xmlns:tt=\"urn:inner\"><tt:SimpleItem Name=\"State\" "
      "Value=\"true\"/></tt:Data></tt:Message></wsnt:Message></wsnt:NotificationMessage>";
  const std::string xml = response(
      std::string(kWsnt) + " xmlns:tns1=\"urn:wrong-topics\" xmlns:tt=\"urn:wrong-schema\"",
      std::string(kTns1) + " " + kTt, msg);
  const ExtractResult r = extract_checked(xml, "collision");
  CHECK(r.fragments.size() == 1, "collision: one fragment");
  if (r.fragments.empty()) return;
  const std::string& x = r.fragments[0].xml;
  CHECK(root_ns(x, "tns1") == kTopicsUri, "collision: tns1 takes the nearer binding");
  CHECK(root_ns(x, "tt") == kSchemaUri, "collision: tt takes the nearer binding");
  CHECK(x.find("urn:wrong") == std::string::npos, "collision: outer binding leaked");
  CHECK(x.find("urn:inner") != std::string::npos, "collision: inner binding kept");
}

static void test_multiple_topic_prefixes() {
  // tt is used only inside the Topic text (the message elements use the
  // "onvif" prefix for the schema); tnsx only in a union expression.
  const std::string msg1 =
      "<wsnt:NotificationMessage><wsnt:Topic>tns1:RuleEngine/tt:CellMotionDetector"
      "</wsnt:Topic><wsnt:Message><onvif:Message PropertyOperation=\"Changed\"><onvif:Source>"
      "<onvif:SimpleItem Name=\"Source\" Value=\"0001\"/></onvif:Source><onvif:Data>"
      "<onvif:SimpleItem Name=\"IsMotion\" Value=\"true\"/></onvif:Data></onvif:Message>"
      "</wsnt:Message></wsnt:NotificationMessage>";
  const std::string msg2 =
      "<wsnt:NotificationMessage><wsnt:Topic> tns1:Device/Trigger/DigitalInput|"
      "tnsx:Vendor/Alarm </wsnt:Topic><wsnt:Message><onvif:Message/></wsnt:Message>"
      "</wsnt:NotificationMessage>";
  const std::string xml = response(
      std::string(kWsnt) + " " + kTt + " " + kTns1 + " xmlns:onvif=\"" + kSchemaUri + "\"",
      "xmlns:tnsx=\"urn:vendor-topics\"", msg1 + msg2);
  const ExtractResult r = extract_checked(xml, "multi prefix");
  CHECK(r.fragments.size() == 2, "multi prefix: two fragments");
  if (r.fragments.size() != 2) return;
  CHECK(root_ns(r.fragments[0].xml, "tt") == kSchemaUri, "multi prefix: tt declared");
  CHECK(root_ns(r.fragments[0].xml, "tns1") == kTopicsUri, "multi prefix: tns1 declared");
  CHECK(r.fragments[0].op == "Changed", "multi prefix: op from onvif:Message");
  CHECK(r.fragments[0].protect_relevant, "multi prefix: CellMotionDetector is relevant");
  CHECK(r.fragments[1].topic == "tns1:Device/Trigger/DigitalInput|tnsx:Vendor/Alarm",
        "multi prefix: topic trimmed: " + r.fragments[1].topic);
  CHECK(root_ns(r.fragments[1].xml, "tnsx") == "urn:vendor-topics", "multi prefix: tnsx");
  CHECK(r.fragments[1].xml.find("xmlns:tt=") == std::string::npos,
        "multi prefix: tt declared where unused");
  CHECK(r.fragments[1].op.empty() && r.fragments[1].key.empty(),
        "multi prefix: no op without PropertyOperation");
  CHECK(r.fragments[1].protect_relevant, "multi prefix: DigitalInput is relevant");
}

static void test_attribute_value_qnames() {
  const std::string msg =
      "<wsnt:NotificationMessage><wsnt:Topic>tns1:Monitoring/ProcessorUsage</wsnt:Topic>"
      "<wsnt:Message><tt:Message PropertyOperation=\"Changed\" xml:lang=\"en\"><tt:Source>"
      "<tt:SimpleItem Name=\"Token\" Value=\"0\" xsi:type=\"tt:ReferenceToken\"/></tt:Source>"
      "<tt:Data><tt:SimpleItem Name=\"Value\" Value=\"12:30\"/><tt:Ref Type=\"tvx:Thing\"/>"
      "</tt:Data></tt:Message></wsnt:Message></wsnt:NotificationMessage>";
  const std::string xml = response(
      std::string(kWsnt) + " " + kTt + " " + kTns1 +
      " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xmlns:tvx=\"urn:tvx\"",
      "", msg);
  const ExtractResult r = extract_checked(xml, "attribute qnames");
  CHECK(r.fragments.size() == 1, "attribute qnames: one fragment");
  if (r.fragments.empty()) return;
  const std::string& x = r.fragments[0].xml;
  CHECK(root_ns(x, "xsi") == "http://www.w3.org/2001/XMLSchema-instance",
        "attribute qnames: xsi");
  CHECK(root_ns(x, "tvx") == "urn:tvx", "attribute qnames: tvx from an attribute value");
  CHECK(!r.fragments[0].protect_relevant, "attribute qnames: ProcessorUsage not relevant");
}

static void test_default_namespace() {
  const std::string msg =
      "<NotificationMessage xmlns=\"http://docs.oasis-open.org/wsn/b-2\">"
      "<Topic>tns1:VideoSource/MotionAlarm</Topic><Message><tt:Message "
      "PropertyOperation=\"Changed\"><tt:Source>" + item("Source", "0") +
      "</tt:Source></tt:Message></Message></NotificationMessage>";
  const std::string xml = response(std::string(kTt) + " " + kTns1, "", msg);
  const ExtractResult r = extract_checked(xml, "default ns");
  CHECK(r.fragments.size() == 1, "default ns: one fragment");

  // Default namespace declared only on an ancestor.
  const std::string msg2 =
      "<NotificationMessage><Topic>tns1:Device/Trigger/AlarmIn</Topic><Message/>"
      "</NotificationMessage>";
  const std::string xml2 = response(std::string(kTns1), "xmlns=\"http://docs.oasis-open.org/"
                                    "wsn/b-2\"", msg2);
  const ExtractResult r2 = extract_checked(xml2, "inherited default ns");
  CHECK(r2.fragments.size() == 1, "inherited default ns: one fragment");
  if (r2.fragments.empty()) return;
  Strict re = strict_parse(r2.fragments[0].xml);
  CHECK(re.doc && std::strcmp(href(xmlDocGetRootElement(re.doc.get())->ns),
                              "http://docs.oasis-open.org/wsn/b-2") == 0,
        "inherited default ns: root keeps its namespace");
  CHECK(r2.fragments[0].protect_relevant, "inherited default ns: AlarmIn is relevant");
}

static void test_undeclared_prefixes() {
  // Prefixes the camera never declared: the fragment must still be
  // namespace-well-formed for a strict consumer.
  const std::string msg =
      "<wsnt:NotificationMessage><wsnt:Topic>tns1:VideoSource/MotionAlarm</wsnt:Topic>"
      "<wsnt:Message><tt:Message PropertyOperation=\"Changed\"><tt:Source>" +
      item("Source", "0") + "</tt:Source><tt:Data><tad:Extra tad:level=\"3\">x</tad:Extra>"
      "</tt:Data></tt:Message></wsnt:Message></wsnt:NotificationMessage>";
  const std::string xml = response(std::string(kWsnt) + " " + kTt + " " + kTns1, "", msg);
  const ExtractResult r = extract_fragments(xml);
  CHECK(r.fragments.size() == 1, "undeclared: one fragment");
  if (r.fragments.empty()) return;
  CHECK(strict_parse(r.fragments[0].xml).ns_ok,
        "undeclared: fragment not namespace-well-formed: " + r.fragments[0].xml);
  CHECK(r.fragments[0].op == "Changed", "undeclared: op");
  CHECK(!root_ns(r.fragments[0].xml, "tad").empty(), "undeclared: tad not declared");

  // No wsnt or tns1 declaration anywhere: the message is still found by
  // local name and the conventional URIs are declared.
  const std::string xml2 = response(std::string(kTt), "",
                                    message("tns1:VideoSource/MotionAlarm", "Changed",
                                            item("Source", "0"), item("State", "true")));
  const ExtractResult r2 = extract_fragments(xml2);
  CHECK(r2.fragments.size() == 1, "undeclared wsnt: one fragment");
  if (r2.fragments.empty()) return;
  const std::string& x = r2.fragments[0].xml;
  CHECK(strict_parse(x).ns_ok, "undeclared wsnt: fragment not namespace-well-formed: " + x);
  CHECK(r2.fragments[0].topic == "tns1:VideoSource/MotionAlarm", "undeclared wsnt: topic");
  CHECK(root_ns(x, "wsnt") == "http://docs.oasis-open.org/wsn/b-2", "undeclared wsnt: wsnt");
  CHECK(root_ns(x, "tns1") == kTopicsUri, "undeclared wsnt: tns1");
  CHECK(root_ns(x, "tt") == kSchemaUri, "undeclared wsnt: tt");
}

static void test_undeclared_attribute_prefixes() {
  // Two undeclared prefixes on attributes with the same local name must
  // not become the same expanded attribute name.
  const std::string msg =
      "<wsnt:NotificationMessage><wsnt:Topic>tns1:VideoSource/MotionAlarm</wsnt:Topic>"
      "<wsnt:Message><tt:Message PropertyOperation=\"Changed\" p:z=\"1\" q:z=\"2\" z=\"3\">"
      "<tt:Source/></tt:Message></wsnt:Message></wsnt:NotificationMessage>";
  const ExtractResult r =
      extract_fragments(response(std::string(kWsnt) + " " + kTt + " " + kTns1, "", msg));
  CHECK(r.fragments.size() == 1, "undeclared attributes: one fragment");
  if (r.fragments.empty()) return;
  const std::string& x = r.fragments[0].xml;
  CHECK(strict_parse(x).ns_ok, "undeclared attributes: not namespace-well-formed: " + x);
  CHECK(!root_ns(x, "p").empty() && root_ns(x, "p") != root_ns(x, "q"),
        "undeclared attributes: p and q share a namespace: " + x);
  CHECK(r.fragments[0].op == "Changed", "undeclared attributes: op");
}

static void test_entity_references() {
  // With no DTD every entity reference other than the predefined ones
  // names an undeclared entity: the fragment leaves it out.
  const std::string msg =
      "<wsnt:NotificationMessage><wsnt:Topic>tns1:VideoSource/Motion&nbsp;Alarm</wsnt:Topic>"
      "<wsnt:Message><tt:Message PropertyOperation=\"Changed\"><tt:Source>" +
      item("Source", "0") +
      "</tt:Source><tt:Data>a&bar;b<tt:X>&baz;</tt:X>c&#65;</tt:Data></tt:Message>"
      "</wsnt:Message></wsnt:NotificationMessage>";
  const ExtractResult r =
      extract_fragments(response(std::string(kWsnt) + " " + kTt + " " + kTns1, "", msg));
  CHECK(r.fragments.size() == 1, "entities: one fragment");
  if (r.fragments.empty()) return;
  const Fragment& f = r.fragments[0];
  CHECK(strict_parse(f.xml).ns_ok, "entities: fragment not well-formed: " + f.xml);
  CHECK(f.xml.find("&nbsp;") == std::string::npos && f.xml.find("&bar;") == std::string::npos &&
        f.xml.find("&baz;") == std::string::npos,
        "entities: undeclared entity reference kept: " + f.xml);
  CHECK(f.xml.find("<tt:Data>ab<tt:X/>cA</tt:Data>") != std::string::npos,
        "entities: text around the references lost: " + f.xml);
  CHECK(f.topic == "tns1:VideoSource/MotionAlarm", "entities: topic " + f.topic);
  CHECK(f.protect_relevant, "entities: MotionAlarm is relevant");
  const std::string init = as_initialized(f.xml);
  CHECK(init.find("PropertyOperation=\"Initialized\"") != std::string::npos &&
        strict_parse(init).ns_ok, "entities: as_initialized: " + init);
}

static void test_oversize_stripped() {
  const std::string image(kMaxFragmentBytes + 1000, 'A');
  const std::string data =
      item("State", "true") + "<tt:ElementItem Name=\"Image\"><tt:Img>" + image +
      "</tt:Img><tt:ElementItem Name=\"Nested\">x</tt:ElementItem></tt:ElementItem>"
      "<tt:ElementItem Name=\"Box\"><tt:Box/></tt:ElementItem>";
  const std::string xml = response(
      std::string(kWsnt) + " " + kTt + " " + kTns1, "",
      message("tns1:RuleEngine/MyRule/ObjectDetection", "Changed", item("Source", "0"), data) +
      message("tns1:VideoSource/MotionAlarm", "Changed", item("Source", "0"),
              item("State", "true")));
  const ExtractResult r = extract_fragments(xml);
  CHECK(r.fragments.size() == 2, "stripped: both fragments kept");
  CHECK(r.stripped == 1 && r.oversize_dropped == 0,
        "stripped: stripped=" + std::to_string(r.stripped) +
        " dropped=" + std::to_string(r.oversize_dropped));
  if (r.fragments.size() != 2) return;
  const std::string& x = r.fragments[0].xml;
  CHECK(x.size() <= kMaxFragmentBytes, "stripped: size " + std::to_string(x.size()));
  CHECK(x.find("ElementItem") == std::string::npos, "stripped: ElementItem left");
  CHECK(x.find("Name=\"State\"") != std::string::npos, "stripped: SimpleItem lost");
  CHECK(strict_parse(x).ns_ok, "stripped: not namespace-well-formed");
  CHECK(r.fragments[0].key == "tns1:RuleEngine/MyRule/ObjectDetection|Source=0|",
        "stripped: key " + r.fragments[0].key);
  CHECK(r.fragments[1].topic == "tns1:VideoSource/MotionAlarm", "stripped: order kept");
}

static void test_oversize_dropped() {
  const std::string big(kMaxFragmentBytes + 1000, 'B');
  const std::string xml = response(
      std::string(kWsnt) + " " + kTt + " " + kTns1, "",
      message("tns1:A/Big", "Changed", item("Source", "0"), item("Blob", big)) +
      message("tns1:A/BigWithItem", "Changed", item("Source", "0"),
              item("Blob", big) + "<tt:ElementItem Name=\"x\"><tt:X/></tt:ElementItem>") +
      message("tns1:A/Small", "Changed", item("Source", "0"), item("State", "1")));
  const ExtractResult r = extract_fragments(xml);
  CHECK(r.oversize_dropped == 2, "dropped: count " + std::to_string(r.oversize_dropped));
  CHECK(r.stripped == 0, "dropped: a dropped fragment counted as stripped");
  CHECK(r.fragments.size() == 1 && r.fragments[0].topic == "tns1:A/Small",
        "dropped: the small message survives");
}

static void test_op_and_key() {
  const std::string msgs =
      message("tns1:X/Y", "Changed", item("B", "2") + item("A", "1"), item("S", "1"),
              item("K", "1")) +
      message("tns1:X/Y", "Initialized", item("A", "1") + item("B", "2"), item("S", "0"),
              item("K", "1")) +
      message("tns1:X/Y", "Changed", item("A", "1") + item("B", "3"), item("S", "1"),
              item("K", "1")) +
      message("tns1:X/Y", "", item("A", "1"), item("S", "1")) +
      message("tns1:X/Y", "Deleted", item("A", "1,B=2"), "") +
      message("tns1:X/Y", "Deleted", item("A", "1") + item("B", "2"), "");
  const std::string xml = response(std::string(kWsnt) + " " + kTt + " " + kTns1, "", msgs);
  const ExtractResult r = extract_checked(xml, "op/key");
  CHECK(r.fragments.size() == 6, "op/key: six fragments");
  if (r.fragments.size() != 6) return;
  const auto& f = r.fragments;
  CHECK(f[0].key == "tns1:X/Y|A=1,B=2|K=1", "op/key: format " + f[0].key);
  CHECK(f[0].key == f[1].key, "op/key: Source order changes the key");
  CHECK(f[0].key != f[2].key, "op/key: Source value ignored");
  CHECK(f[1].op == "Initialized" && f[2].op == "Changed", "op/key: ops");
  CHECK(f[3].op.empty() && f[3].key.empty(), "op/key: event message has no key");
  CHECK(f[4].op == "Deleted", "op/key: Deleted");
  CHECK(f[4].key != f[5].key, "op/key: escaping " + f[4].key + " vs " + f[5].key);
}

static void test_as_initialized() {
  const std::string xml = response(
      std::string(kWsnt) + " " + kTt + " " + kTns1, "",
      message("tns1:VideoSource/MotionAlarm", "Changed", item("Source", "0"),
              item("State", "true")) +
      message("tns1:VideoSource/MotionAlarm", "Initialized", item("Source", "0"),
              item("State", "false")) +
      message("tns1:VideoSource/MotionAlarm", "", item("Source", "0"), item("State", "true")));
  const ExtractResult r = extract_fragments(xml);
  CHECK(r.fragments.size() == 3, "as_initialized: three fragments");
  if (r.fragments.size() != 3) return;

  const std::string init = as_initialized(r.fragments[0].xml);
  CHECK(init.find("PropertyOperation=\"Initialized\"") != std::string::npos,
        "as_initialized: not rewritten: " + init);
  CHECK(init.find("Changed") == std::string::npos, "as_initialized: Changed left");
  CHECK(init.find("Value=\"true\"") != std::string::npos, "as_initialized: data lost");
  CHECK(strict_parse(init).ns_ok, "as_initialized: not namespace-well-formed");
  std::string back = init;
  back.replace(back.find("Initialized"), 11, "Changed");
  CHECK(back == r.fragments[0].xml, "as_initialized: more than the operation changed");

  CHECK(as_initialized(r.fragments[1].xml) == r.fragments[1].xml,
        "as_initialized: Initialized message changed");
  CHECK(as_initialized(r.fragments[2].xml) == r.fragments[2].xml,
        "as_initialized: event message changed");
  CHECK(as_initialized("").empty(), "as_initialized: empty input");
  CHECK(as_initialized("not xml") == "not xml", "as_initialized: non-XML input");
}

static void test_not_pull_responses() {
  CHECK(!extract_fragments("").parsed, "empty input parsed");
  CHECK(!extract_fragments("not xml at all").parsed, "plain text parsed");
  const ExtractResult html = extract_fragments("<html><body>404</body></html>");
  CHECK(html.parsed && html.fragments.empty(), "HTML: parsed without fragments");
  const ExtractResult dtd = extract_fragments(
      "<!DOCTYPE x [<!ENTITY e \"tns1:Evil\">]><x><NotificationMessage><Topic>&e;</Topic>"
      "</NotificationMessage></x>");
  CHECK(!dtd.parsed && dtd.fragments.empty(), "DOCTYPE: refused");

  // A response cut off mid-message still yields a well-formed fragment.
  std::string cut = response(std::string(kWsnt) + " " + kTt + " " + kTns1, "",
                             message("tns1:VideoSource/MotionAlarm", "Changed",
                                     item("Source", "0"), item("State", "true")));
  cut.resize(cut.find("<tt:Data>"));
  const ExtractResult r = extract_fragments(cut);
  CHECK(r.parsed && r.fragments.size() == 1, "truncated: one fragment");
  if (r.fragments.size() == 1) {
    CHECK(strict_parse(r.fragments[0].xml).ns_ok, "truncated: not namespace-well-formed");
    CHECK(r.fragments[0].op == "Changed", "truncated: op");
  }
}

static void test_document_order() {
  const std::string msgs =
      message("tns1:Order/One", "", item("Source", "0"), "") +
      "<n:NotificationMessage xmlns:n=\"http://docs.oasis-open.org/wsn/b-2\"><n:Topic>"
      "tns1:Order/Two</n:Topic><n:Message/></n:NotificationMessage>" +
      message("tns1:Order/Three", "", item("Source", "0"), "");
  const std::string xml = response(std::string(kWsnt) + " " + kTt + " " + kTns1, "", msgs);
  const ExtractResult r = extract_checked(xml, "order");
  CHECK(r.fragments.size() == 3, "order: three fragments");
  if (r.fragments.size() != 3) return;
  CHECK(r.fragments[0].topic == "tns1:Order/One" && r.fragments[1].topic == "tns1:Order/Two" &&
        r.fragments[2].topic == "tns1:Order/Three", "order: document order");
}

// Camera threads extract concurrently; results must not depend on it.
static void test_concurrent_extraction() {
  std::string msgs;
  for (int i = 0; i < 8; ++i)
    msgs += message("tns1:RuleEngine/tt:CellMotionDetector/Motion", i % 2 ? "Changed" : "",
                    item("Rule", std::to_string(i)), item("IsMotion", "true"));
  const std::string xml = response(std::string(kWsnt) + " " + kTt + " " + kTns1, "", msgs);
  const ExtractResult want = extract_fragments(xml);
  CHECK(want.fragments.size() == 8, "concurrent: eight fragments");
  std::vector<int> mismatches(4, 0);
  std::vector<std::thread> threads;
  for (size_t t = 0; t < mismatches.size(); ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 200; ++i) {
        const ExtractResult got = extract_fragments(xml);
        if (!same_fragments(want, got)) ++mismatches[t];
        for (const Fragment& f : got.fragments)
          if (!f.op.empty() && as_initialized(f.xml).find("Initialized") == std::string::npos)
            ++mismatches[t];
      }
    });
  }
  for (auto& th : threads) th.join();
  for (size_t t = 0; t < mismatches.size(); ++t)
    CHECK(mismatches[t] == 0, "concurrent: thread " + std::to_string(t) + " saw " +
          std::to_string(mismatches[t]) + " mismatches");
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <capture.jsonl>...\n";
    return 1;
  }
  xmlInitParser();

  CaptureTotals totals;
  run_test("captures", [&] {
    for (int i = 1; i < argc; ++i) test_capture(argv[i], &totals);
    CHECK(totals.messages > 0, "captures: no NotificationMessages");
    CHECK(totals.initialized > 0, "captures: no Initialized messages");
    CHECK(totals.changed > 0, "captures: no Changed messages");
    CHECK(totals.relevant > 0, "captures: no Protect-relevant messages");
    CHECK(totals.multi_prefix_topics > 0, "captures: no topic with several prefixes");
    CHECK(totals.element_items > 0, "captures: no ElementItem payloads");
    CHECK(totals.topic_prefixes > 0, "captures: no topic prefixes checked");
  });
  run_test("nonstandard_wsnt_uri", test_nonstandard_wsnt_uri);
  run_test("prefixes_only_on_envelope", test_prefixes_only_on_envelope);
  run_test("prefix_collision", test_prefix_collision);
  run_test("multiple_topic_prefixes", test_multiple_topic_prefixes);
  run_test("attribute_value_qnames", test_attribute_value_qnames);
  run_test("default_namespace", test_default_namespace);
  run_test("undeclared_prefixes", test_undeclared_prefixes);
  run_test("undeclared_attribute_prefixes", test_undeclared_attribute_prefixes);
  run_test("entity_references", test_entity_references);
  run_test("oversize_stripped", test_oversize_stripped);
  run_test("oversize_dropped", test_oversize_dropped);
  run_test("op_and_key", test_op_and_key);
  run_test("as_initialized", test_as_initialized);
  run_test("not_pull_responses", test_not_pull_responses);
  run_test("document_order", test_document_order);
  run_test("concurrent_extraction", test_concurrent_extraction);

  xmlCleanupParser();

  std::cout << "================================================\n"
            << "Results: " << g_pass << " checks passed, "
            << g_fail    << " checks failed\n"
            << "================================================\n";
  return g_fail > 0 ? 1 : 0;
}
