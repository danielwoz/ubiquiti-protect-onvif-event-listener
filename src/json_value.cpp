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

#include "json_value.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace onvif {
namespace json {

const Value* Value::member(const std::string& name) const {
  if (type != Type::kObject) return nullptr;
  for (const auto& m : members) {
    if (m.first == name) return &m.second;
  }
  return nullptr;
}

const Value* Value::find(std::initializer_list<const char*> path) const {
  const Value* v = this;
  for (const char* name : path) {
    v = v->member(name);
    if (v == nullptr) return nullptr;
  }
  return v;
}

namespace {

class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text) {}

  bool parse(Value* out) {
    if (!parse_value(out, 0)) return false;
    skip_ws();
    return pos_ == s_.size();
  }

 private:
  static constexpr int kMaxDepth = 64;

  void skip_ws() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' ||
                                s_[pos_] == '\n' || s_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool consume(char ch) {
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == ch) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool literal(const char* word) {
    const size_t n = std::strlen(word);
    if (s_.compare(pos_, n, word) != 0) return false;
    pos_ += n;
    return true;
  }

  bool parse_value(Value* v, int depth) {
    if (depth >= kMaxDepth) return false;
    skip_ws();
    if (pos_ >= s_.size()) return false;
    const char ch = s_[pos_];
    if (ch == '{') return parse_object(v, depth);
    if (ch == '[') return parse_array(v, depth);
    if (ch == '"') {
      v->type = Value::Type::kString;
      return parse_string(&v->text);
    }
    if (literal("true")) {
      v->type = Value::Type::kBool;
      v->boolean = true;
      return true;
    }
    if (literal("false")) {
      v->type = Value::Type::kBool;
      return true;
    }
    if (literal("null")) return true;
    return parse_number(v);
  }

  bool parse_object(Value* v, int depth) {
    v->type = Value::Type::kObject;
    ++pos_;
    if (consume('}')) return true;
    do {
      skip_ws();
      std::string name;
      if (!parse_string(&name) || !consume(':')) return false;
      Value child;
      if (!parse_value(&child, depth + 1)) return false;
      v->members.emplace_back(std::move(name), std::move(child));
    } while (consume(','));
    return consume('}');
  }

  bool parse_array(Value* v, int depth) {
    v->type = Value::Type::kArray;
    ++pos_;
    if (consume(']')) return true;
    do {
      Value child;
      if (!parse_value(&child, depth + 1)) return false;
      v->items.push_back(std::move(child));
    } while (consume(','));
    return consume(']');
  }

  static void append_utf8(uint32_t cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  // Four hex digits after "\u".
  bool parse_hex4(uint32_t* cp) {
    if (pos_ + 4 > s_.size()) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char h = s_[pos_++];
      v <<= 4;
      if (h >= '0' && h <= '9') {
        v |= static_cast<uint32_t>(h - '0');
      } else if (h >= 'a' && h <= 'f') {
        v |= static_cast<uint32_t>(h - 'a' + 10);
      } else if (h >= 'A' && h <= 'F') {
        v |= static_cast<uint32_t>(h - 'A' + 10);
      } else {
        return false;
      }
    }
    *cp = v;
    return true;
  }

  bool parse_string(std::string* out) {
    if (pos_ >= s_.size() || s_[pos_] != '"') return false;
    ++pos_;
    while (pos_ < s_.size()) {
      const char ch = s_[pos_++];
      if (ch == '"') return true;
      if (static_cast<unsigned char>(ch) < 0x20) return false;
      if (ch != '\\') {
        out->push_back(ch);
        continue;
      }
      if (pos_ >= s_.size()) return false;
      const char esc = s_[pos_++];
      switch (esc) {
        case '"': case '\\': case '/': out->push_back(esc); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!parse_hex4(&cp)) return false;
          // A surrogate pair encodes one code point above U+FFFF.
          if (cp >= 0xD800 && cp < 0xDC00 && s_.compare(pos_, 2, "\\u") == 0) {
            const size_t saved = pos_;
            pos_ += 2;
            uint32_t lo = 0;
            if (parse_hex4(&lo) && lo >= 0xDC00 && lo < 0xE000) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              pos_ = saved;
            }
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

  bool parse_number(Value* v) {
    const size_t start = pos_;
    while (pos_ < s_.size() &&
           (std::isdigit(static_cast<unsigned char>(s_[pos_])) ||
            s_[pos_] == '-' || s_[pos_] == '+' || s_[pos_] == '.' ||
            s_[pos_] == 'e' || s_[pos_] == 'E')) {
      ++pos_;
    }
    if (pos_ == start) return false;
    const std::string token = s_.substr(start, pos_ - start);
    char* end = nullptr;
    v->number = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size()) return false;
    v->type = Value::Type::kNumber;
    return true;
  }

  const std::string& s_;
  size_t pos_ = 0;
};

}  // namespace

bool parse(const std::string& text, Value* out) {
  *out = Value();
  return Parser(text).parse(out);
}

}  // namespace json
}  // namespace onvif
