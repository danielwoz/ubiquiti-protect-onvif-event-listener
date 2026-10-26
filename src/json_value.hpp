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

#ifndef SRC_JSON_VALUE_HPP_
#define SRC_JSON_VALUE_HPP_

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace onvif {
namespace json {

// One parsed JSON value.  Object members keep their document order.
struct Value {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool boolean = false;
  double number = 0;
  std::string text;
  std::vector<Value> items;
  std::vector<std::pair<std::string, Value>> members;

  // The member called @p name, or nullptr when there is none or this is not
  // an object.  The first one wins when a name repeats.
  const Value* member(const std::string& name) const;

  // The value at @p path, following object members from this one, e.g.
  // find({"featureFlags", "hasSmartDetect"}); nullptr when any step is
  // missing.
  const Value* find(std::initializer_list<const char*> path) const;

  bool is_true() const { return type == Type::kBool && boolean; }
  bool is_false() const { return type == Type::kBool && !boolean; }
  bool is_string(const char* s) const {
    return type == Type::kString && text == s;
  }
};

// Parse @p text as a single JSON document into @p out.  Returns false on
// malformed input, trailing data, or nesting deeper than 64 levels.
bool parse(const std::string& text, Value* out);

}  // namespace json
}  // namespace onvif

#endif  // SRC_JSON_VALUE_HPP_
