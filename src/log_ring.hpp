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

#include <cstddef>
#include <mutex>
#include <string>

#include "absl/log/log_sink.h"

namespace onvif {

/// Thread-safe 1 MiB in-memory ring buffer that captures absl log messages
/// at all severity levels, regardless of the current minimum log level.
///
/// Install with absl::AddLogSink() at startup.  The buffer is a single
/// contiguous byte array; when it fills up the oldest messages are overwritten.
class LogRing : public absl::LogSink {
 public:
  static constexpr size_t kCapacity = 1u << 20;  // 1 MiB

  LogRing();

  /// absl::LogSink interface.  Called for every log message at every level.
  void Send(const absl::LogEntry& entry) override;

  /// Return the current ring buffer contents as a string, oldest-first.
  std::string dump() const;

 private:
  mutable std::mutex mu_;
  char buf_[kCapacity];
  size_t head_{0};   // next write position
  bool wrapped_{false};
};

}  // namespace onvif
