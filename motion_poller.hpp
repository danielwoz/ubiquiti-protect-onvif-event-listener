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

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "absl/status/statusor.h"

namespace object_detect { class ObjectDetector; }

namespace onvif {

class AlarmNotifier;

/// Polls the UniFi Protect `events` table for `motion` events from first-party
/// cameras that lack native smart detection, runs NanoDet-M on the existing
/// Protect thumbnail, and inserts smart detection records when a relevant
/// subject is found.
///
/// Runs in its own background thread with its own PostgreSQL connection.
/// Thread-safe: start() and stop() may be called from any thread.
class MotionPoller {
 public:
  /// Create the poller and open a PostgreSQL connection.
  static absl::StatusOr<std::unique_ptr<MotionPoller>> Create(
      const std::string& db_connstr);

  ~MotionPoller();

  MotionPoller(const MotionPoller&)            = delete;
  MotionPoller& operator=(const MotionPoller&) = delete;

  /// Camera IDs to watch for motion events.  Must be called before start().
  void set_camera_ids(const std::vector<std::string>& ids);

  /// Camera MAC addresses indexed by ID (for alarm notification).
  void set_camera_macs(const std::map<std::string, std::string>& id_to_mac);

  /// Set the NanoDet-M detector.  Must outlive the poller.  Required.
  void set_detector(const object_detect::ObjectDetector* detector);

  /// Set the alarm notifier (optional).  Must outlive the poller.
  void set_alarm_notifier(AlarmNotifier* notifier);

  /// Base directory for UBV thumbnail files (native Protect path convention).
  /// When set, thumbnails are written alongside Protect's own UBV files.
  void set_ubv_dir(const std::string& dir);

  /// Interval in seconds between poll cycles.  Default: 10.
  void set_poll_interval(int sec);

  /// Coalescing window (seconds).  If a smart detection event already exists
  /// near a motion event, the motion event is skipped.  Default: 30.
  void set_coalesce_window(uint32_t sec);

  /// Launch the background poll thread.  Non-blocking.
  void start();

  /// Request stop and join the thread.  Safe to call from a signal handler
  /// (only sets an atomic flag; the join happens from the destructor or
  /// an explicit stop() call from the main thread).
  void stop();

 private:
  MotionPoller() = default;

  void poll_loop();
  void init_high_water_marks();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace onvif
