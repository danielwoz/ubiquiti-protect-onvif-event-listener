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

#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "util.hpp"

namespace unifi {

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
  using onvif::util::json_str;
  line += json_str("ts") + ':' + json_str(onvif::util::utc_now_iso8601())
       + ',';
  line += json_str("camera_id") + ':' + json_str(camera_id) + ',';
  line += json_str("col")       + ':' + json_str(column)    + ',';
  line += json_str("old")       + ':' + json_str(old_value) + ',';
  line += json_str("new")       + ':' + json_str(new_value);
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
