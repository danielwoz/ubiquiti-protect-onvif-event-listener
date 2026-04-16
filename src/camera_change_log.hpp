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

#pragma once

#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace unifi {

/// Thread-safe JSON Lines change log that records every cameras-table
/// modification made by this tool.  Each line is a JSON object:
///
///   {"ts":"2026-04-08T12:00:00.000Z","camera_id":"abc123",
///    "col":"featureFlags.smartDetectTypes","old":"[]","new":"[...]"}
///
/// Used by rollback_camera_changes() to undo mutations on demand.
class CameraChangeLog {
 public:
  /// Open (or create) the log file at @p path for append.
  static absl::StatusOr<std::unique_ptr<CameraChangeLog>> Create(
      const std::string& path);

  /// Append one change record.  Thread-safe; flushes after each write.
  void record(const std::string& camera_id,
              const std::string& column,
              const std::string& old_value,
              const std::string& new_value);

  /// A single change record read back from the log file.
  struct ChangeRecord {
    std::string timestamp;
    std::string camera_id;
    std::string column;
    std::string old_value;
    std::string new_value;
  };

  /// Read every record from @p path.  Returns an empty vector if the file
  /// does not exist or cannot be opened.
  static std::vector<ChangeRecord> read_all(const std::string& path);

 private:
  explicit CameraChangeLog(const std::string& path);

  std::ofstream file_;
  std::mutex    mu_;
};

}  // namespace unifi
