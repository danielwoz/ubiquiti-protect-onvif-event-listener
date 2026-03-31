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

#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace onvif {

/**
 * AlarmNotifier
 *
 * Notifies the UOS external automation manager (uos-agent, port 11010) when a
 * human or vehicle detection event is recorded, enabling UniFi Protect security
 * alarms to fire for ONVIF cameras.
 *
 * Usage
 * -----
 *   AlarmNotifier notifier;       // or: AlarmNotifier notifier(custom_url);
 *   notifier.refresh_alarms();    // load initial alarm list from UOS
 *
 *   // On each detection event:
 *   notifier.notify("person", camera_mac, event_id, ts_ms);
 *
 * The current alarm list is automatically refreshed every 5 minutes from
 * notify() so new alarms configured in Protect are picked up without restart.
 *
 * Network errors (UOS not available, HTTP failures) are logged at ERROR level
 * and ignored — alarm notification is best-effort and does not affect
 * event recording.
 *
 * Thread-safe: notify() may be called concurrently from multiple camera threads.
 */
class AlarmNotifier {
 public:
  explicit AlarmNotifier(std::string uos_base_url = "http://localhost:11010");

  /// Update the UOS base URL (e.g. after discovering it from ONVIF GetServices).
  /// Clears the cached alarm list so the next notify() fetches from the new URL.
  /// Thread-safe; safe to call concurrently with notify().
  void set_base_url(const std::string& url);

  /// Fetch the current alarm list from UOS and cache it.
  /// Safe to call before the listener starts. Silently ignores errors.
  void refresh_alarms();

  /// Post a detection event to UOS for every alarm that has a matching trigger.
  ///   obj_type   -- "person", "vehicle", "animal", or "package"
  ///   camera_mac -- uppercase no-colon MAC, e.g. "FC5F49CA68D4"
  ///   event_id   -- UUID of the inserted events row
  ///   ts_ms      -- event start timestamp (ms since Unix epoch)
  void notify(const std::string& obj_type,
              const std::string& camera_mac,
              const std::string& event_id,
              uint64_t ts_ms);

 private:
  struct AlarmEntry {
    std::string id;
    std::set<std::string> trigger_types;  // "person", "vehicle", "animal", "package"
  };

  std::string uos_base_url_;
  std::mutex mu_;
  std::vector<AlarmEntry> alarms_;  // protected by mu_
  std::chrono::steady_clock::time_point last_refresh_{};

  static std::vector<AlarmEntry> parse_alarms(const std::string& json);
  static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata);
  std::string http_get(const std::string& url);
  void http_post(const std::string& url, const std::string& body);
};

}  // namespace onvif
