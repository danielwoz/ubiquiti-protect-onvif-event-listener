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

#include "util.hpp"

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <string>

namespace onvif {
namespace util {

uint64_t now_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string utc_now_iso8601() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
  struct tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  char out[48];
  std::snprintf(out, sizeof(out), "%s.%03dZ", buf,
                static_cast<int>(ms.count()));
  return out;
}

std::string utc_now_iso8601_ms() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
  struct tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  std::ostringstream oss;
  oss << buf << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
  return oss.str();
}

std::string generate_uuid() {
  static std::random_device rd;
  thread_local std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;

  uint64_t hi = dis(gen);
  uint64_t lo = dis(gen);

  hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
  lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

  char buf[37];
  std::snprintf(buf, sizeof(buf),
    "%08x-%04x-%04x-%04x-%012" PRIx64,
    static_cast<unsigned>(hi >> 32),
    static_cast<unsigned>((hi >> 16) & 0xFFFF),
    static_cast<unsigned>(hi & 0xFFFF),
    static_cast<unsigned>((lo >> 48) & 0xFFFF),
    static_cast<uint64_t>(lo & 0x0000FFFFFFFFFFFFull));
  return buf;
}

std::string generate_24hex_id() {
  static std::random_device rd;
  thread_local std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;
  uint64_t a = dis(gen);
  uint64_t b = dis(gen);
  char buf[25];
  std::snprintf(buf, sizeof(buf), "%016" PRIx64 "%08" PRIx64,
                static_cast<uint64_t>(a),
                static_cast<uint64_t>(b & 0xFFFFFFFFull));
  return std::string(buf, 24);
}

std::string make_msr_thumbnail_id(const std::string& mac, uint64_t ts_ms) {
  return mac + "-" + std::to_string(ts_ms);
}

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

std::string json_obj(const std::map<std::string, std::string>& m) {
  std::string out = "{";
  bool first = true;
  for (auto& [k, v] : m) {
    if (!first) out += ',';
    out += json_str(k) + ':' + json_str(v);
    first = false;
  }
  out += '}';
  return out;
}

}  // namespace util
}  // namespace onvif
