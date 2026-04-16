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

#ifndef SRC_EVENT_RECORDER_HPP_
#define SRC_EVENT_RECORDER_HPP_

#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include "absl/status/statusor.h"
#include "onvif_listener.hpp"

namespace onvif {

// ---------------------------------------------------------------
// EventRecorder -- thread-safe JSON Lines writer for parsed ONVIF events.
//
// Opens @p path in append mode; each call to write() emits one line with the
// recorded_at timestamp, camera identity, topic, and source/data key-value
// pairs from the OnvifEvent. Multiple threads may call write() concurrently.
// ---------------------------------------------------------------
class EventRecorder {
 public:
  static absl::StatusOr<std::unique_ptr<EventRecorder>> Create(
      const std::string& path);

  void write(const OnvifEvent& ev);

 private:
  explicit EventRecorder(const std::string& path);

  std::ofstream file_;
  std::mutex    mu_;
};

}  // namespace onvif

#endif  // SRC_EVENT_RECORDER_HPP_
