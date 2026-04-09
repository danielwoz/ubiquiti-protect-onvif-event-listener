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

#include "camera_change_log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/statusor.h"

namespace unifi {

// ---------------------------------------------------------------------------
// JSON helpers (minimal, no external dependency)
// ---------------------------------------------------------------------------
static std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 4);
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
  return out;
}

static std::string utc_now_iso8601() {
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

// ---------------------------------------------------------------------------
// Minimal JSON string-value extractor for reading back log records.
//
// Extracts the value for a given key from a single-level JSON object with
// string values.  Handles escaped quotes within values.
// ---------------------------------------------------------------------------
static std::string json_read(const std::string& json, const std::string& key) {
  std::string needle = "\"" + key + "\":\"";
  auto pos = json.find(needle);
  if (pos == std::string::npos) return {};
  pos += needle.size();

  std::string val;
  while (pos < json.size() && json[pos] != '"') {
    if (json[pos] == '\\' && pos + 1 < json.size()) {
      ++pos;
      switch (json[pos]) {
        case '"':  val += '"';  break;
        case '\\': val += '\\'; break;
        case 'n':  val += '\n'; break;
        case 'r':  val += '\r'; break;
        case 't':  val += '\t'; break;
        default:   val += json[pos]; break;
      }
    } else {
      val += json[pos];
    }
    ++pos;
  }
  return val;
}

// ---------------------------------------------------------------------------
// CameraChangeLog implementation
// ---------------------------------------------------------------------------

CameraChangeLog::CameraChangeLog(const std::string& path) {
  file_.open(path, std::ios::app);
}

absl::StatusOr<std::unique_ptr<CameraChangeLog>> CameraChangeLog::Create(
    const std::string& path) {
  auto log = std::unique_ptr<CameraChangeLog>(new CameraChangeLog(path));
  if (!log->file_.is_open())
    return absl::InternalError("Cannot open change log: " + path);
  LOG(INFO) << "[change_log] -> " << path;
  return log;
}

void CameraChangeLog::record(const std::string& camera_id,
                              const std::string& column,
                              const std::string& old_value,
                              const std::string& new_value) {
  std::string line;
  line += '{';
  line += json_escape("ts")        + ':' + json_escape(utc_now_iso8601()) + ',';
  line += json_escape("camera_id") + ':' + json_escape(camera_id)        + ',';
  line += json_escape("col")       + ':' + json_escape(column)           + ',';
  line += json_escape("old")       + ':' + json_escape(old_value)        + ',';
  line += json_escape("new")       + ':' + json_escape(new_value);
  line += "}\n";

  std::lock_guard<std::mutex> lk(mu_);
  file_ << line;
  file_.flush();
}

std::vector<CameraChangeLog::ChangeRecord> CameraChangeLog::read_all(
    const std::string& path) {
  std::vector<ChangeRecord> records;
  std::ifstream f(path);
  if (!f.is_open()) return records;

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] != '{') continue;
    ChangeRecord r;
    r.timestamp  = json_read(line, "ts");
    r.camera_id  = json_read(line, "camera_id");
    r.column     = json_read(line, "col");
    r.old_value  = json_read(line, "old");
    r.new_value  = json_read(line, "new");
    if (!r.camera_id.empty() && !r.column.empty())
      records.push_back(std::move(r));
  }
  return records;
}

}  // namespace unifi
