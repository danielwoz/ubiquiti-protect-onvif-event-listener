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

#include "notification_fragments.hpp"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace onvif {
namespace pullpoint {
namespace {

// Namespace URIs declared for these prefixes when a camera uses them
// without declaring them.
struct Conventional {
  const char* prefix;
  const char* uri;
};
constexpr Conventional kConventional[] = {
    {"wsnt", "http://docs.oasis-open.org/wsn/b-2"},
    {"tt", "http://www.onvif.org/ver10/schema"},
    {"tns1", "http://www.onvif.org/ver10/topics"},
    {"tev", "http://www.onvif.org/ver10/events/wsdl"},
};

// Namespace URI, followed by the prefix, declared for any other element or
// attribute prefix that was never declared, so the fragment is still
// namespace-well-formed.  Each prefix gets its own URI, so attributes such
// as p:z and q:z on one element stay distinct.
constexpr char kUndeclaredPrefixNs[] = "urn:onvif-recorder:undeclared-prefix:";

struct DocFree {
  void operator()(xmlDoc* d) const { xmlFreeDoc(d); }
};
using DocPtr = std::unique_ptr<xmlDoc, DocFree>;

// Parse @p xml, or nullptr when it is not XML.  A document with a DOCTYPE
// is refused: SOAP messages carry no DTD, and refusing one keeps entity
// references out of the copies.
DocPtr parse(const std::string& xml) {
  // Recover from sloppy camera XML, never touch the network, stay quiet,
  // and leave entity references unexpanded.
  constexpr int kOptions =
      XML_PARSE_RECOVER | XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING;
  if (xml.empty() || xml.size() > static_cast<size_t>(INT_MAX)) return nullptr;
  DocPtr doc(xmlReadMemory(xml.data(), static_cast<int>(xml.size()), nullptr,
                           nullptr, kOptions));
  if (!doc || doc->intSubset || doc->extSubset ||
      !xmlDocGetRootElement(doc.get()))
    return nullptr;
  return doc;
}

const char* str(const xmlChar* s) { return reinterpret_cast<const char*>(s); }

// Local part of an element or attribute name.  Names whose prefix was not
// declared keep the prefix in the name ("tad:Foo"), so it is cut off here.
const char* local_name(const xmlChar* name) {
  if (!name) return "";
  const char* colon = std::strrchr(str(name), ':');
  return colon ? colon + 1 : str(name);
}

bool is_element(const xmlNode* n, const char* local) {
  return n->type == XML_ELEMENT_NODE &&
         std::strcmp(local_name(n->name), local) == 0;
}

// Every element named @p local under (and including) the siblings starting
// at @p n, in document order.
void find_all(xmlNode* n, const char* local, std::vector<xmlNode*>* out) {
  for (; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) continue;
    if (is_element(n, local)) out->push_back(n);
    find_all(n->children, local, out);
  }
}

xmlNode* first_child(xmlNode* parent, const char* local) {
  for (xmlNode* c = parent->children; c; c = c->next)
    if (is_element(c, local)) return c;
  return nullptr;
}

// Attribute named @p local in any namespace.
xmlAttr* find_attr(xmlNode* n, const char* local) {
  for (xmlAttr* a = n->properties; a; a = a->next)
    if (std::strcmp(local_name(a->name), local) == 0) return a;
  return nullptr;
}

std::string attr_value(const xmlAttr* a) {
  std::string v;
  if (!a) return v;
  for (const xmlNode* t = a->children; t; t = t->next)
    if (t->content) v += str(t->content);
  return v;
}

std::string text_of(xmlNode* n) {
  xmlChar* c = xmlNodeGetContent(n);
  std::string s = c ? str(c) : "";
  if (c) xmlFree(c);
  return s;
}

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// First Message element (any namespace) under @p n, in document order,
// that carries a PropertyOperation attribute.
xmlNode* find_property_message(xmlNode* n) {
  for (; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) continue;
    if (is_element(n, "Message") && find_attr(n, "PropertyOperation")) return n;
    if (xmlNode* found = find_property_message(n->children)) return found;
  }
  return nullptr;
}

// "name=value" for each SimpleItem directly under @p items, sorted and
// joined with ",".  "\", ",", "|" and "=" inside names and values are
// escaped with "\" so different item sets never join to the same text.
std::string simple_items(xmlNode* items) {
  if (!items) return {};
  auto escape = [](const std::string& s) {
    std::string out;
    for (char c : s) {
      if (c == '\\' || c == ',' || c == '|' || c == '=') out += '\\';
      out += c;
    }
    return out;
  };
  std::vector<std::string> pairs;
  for (xmlNode* c = items->children; c; c = c->next) {
    if (!is_element(c, "SimpleItem")) continue;
    pairs.push_back(escape(attr_value(find_attr(c, "Name"))) + "=" +
                    escape(attr_value(find_attr(c, "Value"))));
  }
  std::sort(pairs.begin(), pairs.end());
  std::string out;
  for (const auto& p : pairs) {
    if (!out.empty()) out += ',';
    out += p;
  }
  return out;
}

bool is_name_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool is_name_char(char c) {
  return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

bool is_token_boundary(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' ||
         c == '|' || c == '(' || c == '[' || c == ',';
}

// Namespaces in scope at @p node that have a prefix: the nearest
// declaration of each prefix.
std::vector<xmlNs*> prefixes_in_scope(xmlNode* node) {
  std::vector<xmlNs*> out;
  for (xmlNode* n = node; n && n->type == XML_ELEMENT_NODE; n = n->parent) {
    for (xmlNs* ns = n->nsDef; ns; ns = ns->next) {
      if (!ns->prefix) continue;
      const bool shadowed = std::any_of(out.begin(), out.end(), [&](xmlNs* o) {
        return xmlStrEqual(o->prefix, ns->prefix);
      });
      if (!shadowed) out.push_back(ns);
    }
  }
  return out;
}

// Calls @p fn with each QName prefix in @p text: the "tns1" and "tt" of
// "tns1:RuleEngine/tt:CellMotionDetector", or the "tt" of a
// "tt:ReferenceToken" attribute value.  A token starts at the beginning of
// the text or after whitespace or one of "/|([,".
template <typename Fn>
void for_each_qname_prefix(const char* text, const Fn& fn) {
  if (!text) return;
  bool boundary = true;
  for (size_t i = 0; text[i];) {
    if (boundary && is_name_start(text[i])) {
      size_t j = i + 1;
      while (is_name_char(text[j])) ++j;
      if (text[j] == ':' && is_name_start(text[j + 1])) fn(std::string(text + i, j - i));
      i = j;
      boundary = false;
      continue;
    }
    boundary = is_token_boundary(text[i]);
    ++i;
  }
}

// Calls @p fn with each QName prefix in the attribute values and text
// under element @p e, including its own attributes.
template <typename Fn>
void for_each_text_prefix(const xmlNode* e, const Fn& fn) {
  for (const xmlAttr* a = e->properties; a; a = a->next) {
    for (const xmlNode* t = a->children; t; t = t->next)
      for_each_qname_prefix(str(t->content), fn);
  }
  for (const xmlNode* c = e->children; c; c = c->next) {
    if (c->type == XML_ELEMENT_NODE)
      for_each_text_prefix(c, fn);
    else if (c->type == XML_TEXT_NODE || c->type == XML_CDATA_SECTION_NODE)
      for_each_qname_prefix(str(c->content), fn);
  }
}

const char* conventional_uri(const std::string& prefix) {
  for (const auto& c : kConventional)
    if (prefix == c.prefix) return c.uri;
  return nullptr;
}

// Prefix of a "prefix:local" name that carries no namespace, or empty.
std::string undeclared_prefix(const xmlChar* name, const xmlNs* ns) {
  if (ns || !name) return {};
  const char* colon = std::strchr(str(name), ':');
  return colon ? std::string(str(name), colon - str(name)) : std::string();
}

// Declares on @p root every element or attribute prefix under @p n that
// has no declaration in scope: its conventional URI, else
// kUndeclaredPrefixNs followed by the prefix.
void declare_undeclared(xmlNode* root, xmlNode* n) {
  for (; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) continue;
    std::vector<std::string> prefixes{undeclared_prefix(n->name, n->ns)};
    for (xmlAttr* a = n->properties; a; a = a->next)
      prefixes.push_back(undeclared_prefix(a->name, a->ns));
    for (const auto& p : prefixes) {
      if (p.empty() || p == "xml" || p == "xmlns") continue;
      if (xmlSearchNs(root->doc, n, BAD_CAST p.c_str())) continue;
      const char* uri = conventional_uri(p);
      const std::string own = std::string(kUndeclaredPrefixNs) + p;
      xmlNewNs(root, BAD_CAST(uri ? uri : own.c_str()), BAD_CAST p.c_str());
    }
    declare_undeclared(root, n->children);
  }
}

// Removes the entity references in the element content under @p n.  With
// no DTD (parse() refuses one) each names an undeclared entity, which a
// strict parser rejects.  Some libxml2 versions keep them when recovering,
// others drop them; removing them gives the same fragment with either.
void remove_entity_refs(xmlNode* n) {
  for (xmlNode* c = n->children; c;) {
    xmlNode* next = c->next;
    if (c->type == XML_ENTITY_REF_NODE) {
      xmlUnlinkNode(c);
      xmlFreeNode(c);
    } else if (c->type == XML_ELEMENT_NODE) {
      remove_entity_refs(c);
    }
    c = next;
  }
}

// Removes every ElementItem element under @p n.  Returns how many.
size_t remove_element_items(xmlNode* n) {
  std::vector<xmlNode*> items;
  std::vector<xmlNode*> stack{n};
  while (!stack.empty()) {
    xmlNode* cur = stack.back();
    stack.pop_back();
    for (xmlNode* c = cur->children; c; c = c->next) {
      if (c->type != XML_ELEMENT_NODE) continue;
      if (is_element(c, "ElementItem"))
        items.push_back(c);  // its own subtree goes with it
      else
        stack.push_back(c);
    }
  }
  for (xmlNode* item : items) {
    xmlUnlinkNode(item);
    xmlFreeNode(item);
  }
  return items.size();
}

std::string dump(xmlDoc* doc, xmlNode* node) {
  std::string out;
  xmlBufferPtr buf = xmlBufferCreate();
  if (!buf) return out;
  if (xmlNodeDump(buf, doc, node, 0, 0) >= 0)
    out.assign(str(xmlBufferContent(buf)), xmlBufferLength(buf));
  xmlBufferFree(buf);
  return out;
}

bool contains_any(const std::string& s, std::initializer_list<const char*> subs) {
  for (const char* sub : subs)
    if (s.find(sub) != std::string::npos) return true;
  return false;
}

// Builds the fragment for @p msg.  Returns false when it is too big even
// without ElementItems (or cannot be serialised); @p stripped is set when
// ElementItems were removed.
bool build_fragment(xmlNode* msg, Fragment* f, bool* stripped) {
  *stripped = false;
  if (xmlNode* topic = first_child(msg, "Topic")) f->topic = trim(text_of(topic));
  if (xmlNode* m = find_property_message(msg->children)) {
    f->op = attr_value(find_attr(m, "PropertyOperation"));
    if (!f->op.empty())
      f->key = f->topic + "|" + simple_items(first_child(m, "Source")) + "|" +
               simple_items(first_child(m, "Key"));
  }
  f->protect_relevant = contains_any(
      f->topic, {"MotionAlarm", "CellMotionDetector", "MotionRegionDetector",
                 "DigitalInput", "AlarmIn"});

  // The copy declares on its root the element and attribute namespaces
  // the subtree inherits from outside it.  Prefixes used only inside text
  // and attribute values are declared here from the namespaces in scope at
  // @p msg, or with their conventional URI when the camera never declared
  // them.
  DocPtr out(xmlNewDoc(BAD_CAST "1.0"));
  if (!out) return false;
  xmlNode* copy = xmlDocCopyNode(msg, out.get(), 1);
  if (!copy) return false;
  xmlDocSetRootElement(out.get(), copy);
  remove_entity_refs(copy);

  const std::vector<xmlNs*> scope = prefixes_in_scope(msg);
  for_each_text_prefix(msg, [&](const std::string& prefix) {
    const xmlChar* p = BAD_CAST prefix.c_str();
    if (xmlSearchNs(out.get(), copy, p)) return;
    for (xmlNs* ns : scope) {
      if (xmlStrEqual(ns->prefix, p)) {
        xmlNewNs(copy, ns->href, ns->prefix);
        return;
      }
    }
    if (const char* uri = conventional_uri(prefix)) xmlNewNs(copy, BAD_CAST uri, p);
  });
  declare_undeclared(copy, copy);

  f->xml = dump(out.get(), copy);
  if (f->xml.size() <= kMaxFragmentBytes) return !f->xml.empty();
  if (remove_element_items(copy) == 0) return false;
  f->xml = dump(out.get(), copy);
  *stripped = true;
  return !f->xml.empty() && f->xml.size() <= kMaxFragmentBytes;
}

}  // namespace

ExtractResult extract_fragments(const std::string& pull_response_xml) {
  ExtractResult r;
  DocPtr doc = parse(pull_response_xml);
  if (!doc) return r;
  r.parsed = true;

  std::vector<xmlNode*> msgs;
  find_all(xmlDocGetRootElement(doc.get()), "NotificationMessage", &msgs);
  r.fragments.reserve(msgs.size());
  for (xmlNode* msg : msgs) {
    Fragment f;
    bool stripped = false;
    if (!build_fragment(msg, &f, &stripped)) {
      ++r.oversize_dropped;
      continue;
    }
    if (stripped) ++r.stripped;
    r.fragments.push_back(std::move(f));
  }
  return r;
}

std::string as_initialized(const std::string& fragment_xml) {
  DocPtr doc = parse(fragment_xml);
  if (!doc) return fragment_xml;
  xmlNode* root = xmlDocGetRootElement(doc.get());
  xmlNode* msg = find_property_message(root->children);
  if (!msg) return fragment_xml;
  xmlAttr* op = find_attr(msg, "PropertyOperation");
  if (attr_value(op) == "Initialized") return fragment_xml;
  if (!xmlSetNsProp(msg, op->ns, op->name, BAD_CAST "Initialized"))
    return fragment_xml;
  std::string out = dump(doc.get(), root);
  return out.empty() ? fragment_xml : out;
}

}  // namespace pullpoint
}  // namespace onvif
