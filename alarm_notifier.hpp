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
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace onvif {

/**
 * AlarmNotifier
 *
 * Triggers UniFi Protect automations (e.g. "Make Sound for detection") when a
 * smart detection event is recorded.  Uses the local Protect API on port 7080
 * with X-UserId auth bypass from localhost.
 *
 * Records automation history in the Protect DB so hits appear in the Protect UI
 * and respects the "ignore repeated" cooldown setting.
 *
 * Usage
 * -----
 *   AlarmNotifier notifier("http://localhost:7080", "user-uuid", db_connstr);
 *   notifier.refresh_alarms();
 *
 *   // On each detection event:
 *   notifier.notify("person", "FC5F49CA68D4", event_id, ts_ms);
 *
 * Thread-safe: notify() may be called concurrently from multiple camera threads.
 */
class AlarmNotifier {
 public:
  AlarmNotifier(std::string protect_url,
                std::string user_id,
                std::string db_connstr = "");

  /// Fetch the current automation list from Protect API and cache it.
  void refresh_alarms();

  /// Trigger matching Protect automations for a detection event.
  ///   obj_type   -- "person", "vehicle", "animal", or "package"
  ///   camera_mac -- uppercase no-colon MAC, e.g. "FC5F49CA68D4"
  ///   event_id   -- UUID of the inserted events row
  ///   ts_ms      -- event start timestamp (ms since Unix epoch)
  void notify(const std::string& obj_type,
              const std::string& camera_mac,
              const std::string& event_id,
              uint64_t ts_ms);

 private:
  struct AutomationEntry {
    std::string id;
    std::string name;
    std::set<std::string> trigger_types;   // "person", "vehicle", etc.
    std::set<std::string> source_devices;  // camera MACs (empty = all cameras)
    bool enabled = false;
    bool cooldown_enabled = false;
    uint64_t cooldown_ms = 0;
  };

  std::string protect_url_;
  std::string user_id_;
  std::string db_connstr_;
  std::mutex mu_;
  std::vector<AutomationEntry> automations_;  // protected by mu_
  std::map<std::string, uint64_t> last_fired_;  // automation_id → ms, protected by mu_
  std::chrono::steady_clock::time_point last_refresh_{};

  static std::vector<AutomationEntry> parse_automations(
      const std::string& json);
  static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata);
  std::string http_get(const std::string& url);
  void http_post(const std::string& url, const std::string& body);
  void record_history(const AutomationEntry& automation,
                      const std::string& obj_type,
                      const std::string& camera_mac,
                      const std::string& event_id,
                      uint64_t ts_ms);
};

}  // namespace onvif
