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
#include <set>
#include <string>
#include <vector>

#include "absl/synchronization/mutex.h"
#include "protect_user_id_provider.hpp"

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
 * Thumbnail strategy
 * ------------------
 * Protect's push path resolves thumbnails via buildEventFromSource, which looks
 * up the camera by its DB UUID (not MAC).  On startup refresh_alarms() fetches
 * GET /api/cameras to build a MAC → DB-UUID map and registers each automation
 * with the in-memory UOS store via
 *   POST /internal/automationManager/external/change
 * so that subsequent calls to
 *   POST /internal/automationManager/external/actions/notify
 * succeed and produce push notifications with thumbnails.
 *
 * The in-memory registration is lost when Protect restarts.  notify() detects
 * HTTP 500 ("Alarm not found") and re-registers before retrying once.
 *
 * Usage
 * -----
 *   ProtectUserIdProvider provider(user_id, cache_path);
 *   AlarmNotifier notifier("http://localhost:7080", &provider, db_connstr);
 *   notifier.refresh_alarms();
 *
 *   // On each detection event:
 *   notifier.notify("person", "FC5F49CA68D4", event_id, ts_ms);
 *
 * Thread-safe: notify() may be called concurrently from multiple camera threads.
 *
 * 401 self-heal: if a Protect API call returns 401, we ask the shared
 * ProtectUserIdProvider to re-query unifi-core for a fresh user_id and retry
 * the request once.  Refresh is rate-limited inside the provider.
 */
class AlarmNotifier {
 public:
  AlarmNotifier(std::string protect_url,
                ProtectUserIdProvider* user_id_provider,
                std::string db_connstr = "");

  /// Fetch the current automation list from Protect API and cache it.
  /// Also fetches the camera list to populate the MAC → DB-UUID map and
  /// registers each enabled automation with the UOS in-memory store.
  void refresh_alarms();

  /// Route notifications through Protect's UOS external automation
  /// manager (default true).  That path is the only one that can attach a
  /// thumbnail, but it requires Protect's Global Alarm Manager to be
  /// enabled.  When it is not -- or when this is false -- notifications
  /// fall back to the legacy /api/automations/<id>/run endpoint, which
  /// fires the automation but cannot carry a thumbnail.
  void set_notify_via_uos(bool enabled);

  /// One-shot authenticated GET against Protect's local API, returning the
  /// response body ("" on any failure).  Shares the notifier's curl setup
  /// (X-UserId bypass header, 5 s timeout) so callers outside the alarm
  /// path -- notably the featureFlags drift check -- don't need their own
  /// HTTP plumbing.
  static std::string protect_api_get(const std::string& url,
                                     const std::string& user_id);

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

  // Grant the white-box test access to the private static parser and the
  // AutomationEntry layout. Defined in test/test_alarm_notifier.cpp.
  friend struct AlarmNotifierTest;

  std::string protect_url_;
  ProtectUserIdProvider* user_id_provider_;
  std::string db_connstr_;
  bool notify_via_uos_{true};
  // Set once the UOS endpoint reports it isn't there, so we stop
  // attempting it on every single detection and just use the legacy path.
  bool uos_unavailable_{false};
  absl::Mutex mu_;
  std::vector<AutomationEntry> automations_;  // protected by mu_
  std::map<std::string, uint64_t> last_fired_;   // automation_id → ms, protected by mu_
  std::map<std::string, std::string> mac_to_camera_id_;  // MAC → camera DB UUID, protected by mu_
  std::chrono::steady_clock::time_point last_refresh_{};

  // Does this HTTP status from /actions/notify mean the UOS automation
  // manager has lost our registration?  Its store is in-memory only, so a
  // Protect restart evicts it and the endpoint answers 500 "Alarm not
  // found" -- re-registering and retrying recovers that.  Any other code
  // is either success or a failure re-registration cannot fix (401 is
  // auth, 4xx is a malformed request), so we must not retry-storm on it.
  // Pulled out as a static purely so the rule is testable without a live
  // Protect.
  static bool should_re_register(long http_code);  // NOLINT(runtime/int)

  // Does this status from /actions/notify mean the UOS external
  // automation manager isn't available at all -- i.e. Protect's Global
  // Alarm Manager is switched off, so the endpoint doesn't exist?  That
  // is not recoverable by retrying, but it IS recoverable by using the
  // legacy /api/automations/<id>/run path, which works regardless.
  static bool uos_unavailable(long http_code);  // NOLINT(runtime/int)

  static std::vector<AutomationEntry> parse_automations(
      const std::string& json);
  static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata);

  // Build the JSON payload for POST /internal/automationManager/external/actions/notify.
  // alarm_id     -- automation UUID (the "alarm" in UOS terminology)
  // event_key    -- EventKeys enum string: "person", "vehicle", "animal", etc.
  // camera_db_id -- camera UUID from GET /api/cameras (not MAC); passed as
  //                 sourceEvent.cameraId so buildEventFromSource can resolve
  //                 the live event row and attach the thumbnail to the push
  // event_id     -- UUID of the just-inserted events row (real thumbnail anchor)
  // ts_ms        -- event start ms-since-epoch
  // user_id      -- UCore user UUID for the receivers array (schema requirement)
  static std::string build_notify_payload(const std::string& alarm_id,
                                          const std::string& event_key,
                                          const std::string& camera_db_id,
                                          const std::string& event_id,
                                          uint64_t           ts_ms,
                                          const std::string& user_id);

  std::string http_get(const std::string& url);
  // POST helper; returns the HTTP status code (0 on network error).
  // Handles 401 → user_id refresh → retry automatically.  Requests to
  // Protect's /internal/automationManager/ routes are sent without user
  // credentials: from Protect 7.3 those routes answer 404 to any request
  // carrying X-UserId and accept only credential-less calls from the
  // console itself.  @p response_body, if given, receives the body.
  long http_post(const std::string& url,   // NOLINT(runtime/int)
                 const std::string& body,
                 std::string* response_body = nullptr);
  // Inner request workers; return the HTTP status code.  perform_get
  // also writes the response body into `*body`.  Empty body and non-200
  // codes are surfaced to the caller, which decides whether to retry
  // after a user_id refresh.
  // NOLINTNEXTLINE(runtime/int)
  static long perform_get(const std::string& url,
                          const std::string& user_id,
                          std::string* body);
  // An empty @p user_id omits the X-UserId header.
  long perform_post(const std::string& url, const std::string& user_id,  // NOLINT(runtime/int)
                    const std::string& body,
                    std::string* response_body = nullptr);

  // Register one automation with the in-memory UOS automation manager via
  //   POST /internal/automationManager/external/change  (type "created")
  // so that subsequent /actions/notify calls can find it.
  // Must be called once per automation on startup and again whenever
  // /actions/notify returns HTTP 500 (in-memory state lost after restart).
  // Returns the HTTP status (2xx on success, 0 on network error).
  long register_automation(const AutomationEntry& entry);  // NOLINT(runtime/int)
  // Request goes to an internal route that rejects user credentials.
  static bool is_internal_automation_url(const std::string& url);

  void record_history(const AutomationEntry& automation,
                      const std::string& obj_type,
                      const std::string& camera_mac,
                      const std::string& event_id,
                      uint64_t ts_ms);
};

}  // namespace onvif
