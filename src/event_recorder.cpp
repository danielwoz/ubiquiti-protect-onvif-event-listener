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

#include "event_recorder.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "util.hpp"

namespace onvif {

absl::StatusOr<std::unique_ptr<EventRecorder>> EventRecorder::Create(
    const std::string& path) {
  auto r = std::unique_ptr<EventRecorder>(new EventRecorder(path));
  if (!r->file_.is_open())
    return absl::InternalError("Cannot open: " + path);
  LOG(INFO) << "[recorder] event log -> " << path;
  return r;
}

EventRecorder::EventRecorder(const std::string& path) {
  file_.open(path, std::ios::app);
}

void EventRecorder::write(const OnvifEvent& ev) {
  using onvif::util::json_obj;
  using onvif::util::json_str;
  std::string line;
  line += '{';
  line += json_str("recorded_at") + ':' +
          json_str(onvif::util::utc_now_iso8601_ms()) + ',';
  line += json_str("camera_ip")   + ':' + json_str(ev.camera_ip)   + ',';
  line += json_str("camera_user") + ':' + json_str(ev.camera_user) + ',';
  line += json_str("event_time")  + ':' + json_str(ev.event_time)  + ',';
  line += json_str("topic")       + ':' + json_str(ev.topic)       + ',';
  line += json_str("property_op") + ':' + json_str(ev.property_op) + ',';
  line += json_str("source")      + ':' + json_obj(ev.source)      + ',';
  line += json_str("data")        + ':' + json_obj(ev.data);
  line += "}\n";

  std::lock_guard<std::mutex> lk(mu_);
  file_ << line;
  file_.flush();
}

}  // namespace onvif
