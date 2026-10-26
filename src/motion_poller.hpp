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
#include "pg_util.hpp"

namespace object_detect { class ObjectDetector; }

namespace onvif {

namespace motion_poller_internal {

/// Map a detection class label ("person", "vehicle", "animal", "package",
/// other) to the JSON array written into events.smartDetectTypes.
/// Anything not in the explicit set falls back to ["person"].  Exposed for
/// testing.
std::string smart_detect_types_json(const std::string& det_type);

/// Build the smartDetectRaws.payload JSON for a single detection at
/// @p ts_ms with object class @p obj_type.  Exposed for testing.
std::string build_sdr_payload(uint64_t ts_ms, const std::string& obj_type,
                              int confidence);

/// Build the smartDetectObjects.attributes JSON to mirror what native
/// Protect smart-detect events write (mostly nulls + objectType +
/// confidence + zone).  Without this richer shape, downstream consumers
/// such as the iOS app's Find Anything filter skip the event.  Exposed
/// for testing.
std::string build_sdo_attributes(const std::string& obj_type,
                                  int confidence);

/// Build the smartDetectTracks.payload JSON for a single detection
/// covering [@p start_ms, @p end_ms] with object class @p obj_type.
/// Native Protect events write one row in this table per detection;
/// without it, the Find Anything filter does not surface our events.
/// Exposed for testing.
std::string build_sdt_payload(uint64_t start_ms,
                               uint64_t end_ms,
                               const std::string& obj_type,
                               int confidence);

/// One per-frame detection candidate for the time-travel sampling pass.
/// Carries only what `select_best_candidate_index` needs: the security
/// class label and the model's confidence scaled to [0, 100].  The caller
/// keeps a parallel array of (jpeg, bbox) so it can recover the original
/// frame data for the chosen index.
struct Candidate {
  std::string obj_type;     // "person" / "vehicle" / "animal" / "package"
  int         confidence_pct;
};

/// Pick the best detection from a time-travel sample window, ignoring any
/// class that was already present in the pre-event baseline frame.
///
/// @p baseline_classes  obj_types found in the (start - 5000ms) baseline.
///                       A parked car will be {"vehicle"}, an empty driveway
///                       {}, etc.
/// @p event_candidates  flattened list of detections from every event-window
///                       frame (start+0, start+1s, ..., start+(N-1)s).
///
/// Returns the index of the highest-confidence candidate whose obj_type is
/// NOT in @p baseline_classes, or -1 if every candidate's class also appears
/// in the baseline (the "always-a-car" no-op case).
///
/// Tie-break: equal confidences -> lowest index (earliest frame).
/// Exposed for testing.
int select_best_candidate_index(
    const std::vector<std::string>& baseline_classes,
    const std::vector<Candidate>& event_candidates);

/// Push mode: wait for a NOTIFY on @p conn or for @p timeout_ms, whichever
/// comes first, checking @p running every second.  Returns at once when a
/// notification is already queued, or when @p conn is down -- the poll
/// loop's wait_for_connection() then paces the reconnect, rather than the
/// wait sitting out the whole timeout on a closed socket.  Notifications
/// are drained; the next poll query is driven by the high-water marks.
/// Exposed for testing.
void wait_for_notify_or_timeout(PGconn* conn, int timeout_ms,
                                const std::atomic<bool>& running);

/// Block until @p conn is usable, retrying through @p gate's back-off and
/// sleeping between attempts in slices of at most a second.  Returns true
/// once connected, false as soon as @p running is cleared.  The poll loop
/// calls this before every cycle so a database outage is waited out
/// instead of spinning through failing queries.  Exposed for testing.
bool wait_for_connection(PGconn* conn, onvif::pg::ReconnectGate* gate,
                         const std::atomic<bool>& running);

/// Push mode: issue LISTEN for the motion channel when @p conn is a
/// different server session from the one that last listened (*@p
/// listen_pid, updated on success).  A reset connection -- from the
/// reconnect gate, or from pg_util after a failed query -- starts a
/// session without the LISTEN, which would leave only the 60 s safety-net
/// poll.  Returns false only when the LISTEN itself is refused; a
/// connection that is down or drops meanwhile is retried on the next call.
/// Exposed for testing.
bool ensure_listening(PGconn* conn, int* listen_pid);

}  // namespace motion_poller_internal

class AlarmNotifier;
class MsrClient;
class ProtectUserIdProvider;

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

  /// When @p enabled (default true), the poller installs a Postgres AFTER
  /// INSERT trigger on the `events` table that fires
  /// `pg_notify('onvif_recorder_motion', NEW.id)` for `type='motion'` rows,
  /// and switches from a fixed poll_interval sleep to a socket wait that
  /// wakes on the first NOTIFY.  This drops steady-state DB query rate
  /// from (cameras x poll_cycles/min) to roughly the motion event rate.
  /// A slow fallback poll still runs on a 60-second safety-net timeout so
  /// notifications lost mid-flight don't cause missed events.
  /// If the trigger install fails (e.g. Protect's DB doesn't yet have the
  /// `events` table on a fresh install), the poller falls back to the
  /// classic interval-based poll for that run.
  void set_push_mode(bool enabled);

  /// On startup, pull the high-water mark back to (now - @p days) so the
  /// next poll cycles re-scan the last N days of motion events and
  /// re-classify any that didn't produce a smartDetectZone last time
  /// (because of an old code path or NanoDet failure).  Events with an
  /// existing overlapping smartDetectZone are excluded by the same
  /// NOT-EXISTS filter the live path uses, so this never duplicates.
  /// Default 0 disables the feature.
  void set_backfill_lookback_days(int days);

  /// When @p on is true and NanoDet-M doesn't find a security-relevant
  /// subject in either the wide snapshot or the cropped thumbnail, motion_poller
  /// still writes a smartDetectZone event tagged with @p fallback_object_type
  /// (defaulting to "person" when empty) and confidence=0.  This is what the
  /// "always smart-detect for opt'd-in first-party cameras" mode boils down to
  /// at the row level: the camera saw motion, the human user wants to see it
  /// surfaced through Protect's smart-detect filter, and we don't make them
  /// configure every detail.
  /// Default: on=true, fallback="person".
  void set_always_smart_detect(bool on, const std::string& fallback_object_type);

  /// Coalescing window (seconds).  If a smart detection event already exists
  /// near a motion event, the motion event is skipped.  Default: 30.
  void set_coalesce_window(uint32_t sec);

  /// Configure time-travel snapshot sampling for first-party motion events.
  ///
  /// When @p global_secs > 0, motion_poller fetches frames from Protect's
  /// `/api/cameras/<id>/snapshot?ts=<ms>` endpoint at 1-second offsets
  /// starting at the real motion-start moment, for N seconds where
  ///   N = min(per_camera[id] when present otherwise global_secs,
  ///           max(1, ceil((end - start)/1000))).
  /// Each frame is run through NanoDet-M and the highest-confidence
  /// security-relevant detection across all frames is used.
  ///
  /// A single baseline frame is also fetched at (start - 5000ms);
  /// any class detected in that baseline is treated as "already in the
  /// scene" (e.g. a parked car) and suppressed from the event sample.
  /// If every event-frame detection's class also appears in the baseline,
  /// the poller behaves as if NanoDet returned nothing (and falls through
  /// to the always-smart-detect fallback when that is enabled).
  ///
  /// Set @p global_secs <= 0 (or leave unset) to disable the time-travel
  /// path entirely; the poller then keeps using the legacy single-snapshot
  /// + cropped-thumbnail flow.  Per-camera overrides are matched by camera
  /// ID (24-char hex from the cameras table), not IP.
  void set_video_sample_secs(int global_secs,
                              const std::map<std::string, int>& per_camera);

  /// When enabled, thumbnail IDs use the MSR "{MAC}-{timestamp_ms}" format
  /// matching native Protect.  Must be called before start().
  void set_use_msr_thumbnail_ids(bool use_msr);

  /// MSR client for forwarding thumbnail JPEGs to MSR's
  /// `RecordingAPI.StoreSnapshots` -- MSR persists the JPEG as a native UBV
  /// file owned by ms:unifi-streaming, and the returned thumbnailId is
  /// served by the msp media server.  When set (and the camera has a MAC),
  /// the poller forwards the cropped JPEG to MSR and uses the returned id;
  /// without an MsrClient, falls back to a 24-char-hex DB-stored thumbnail.
  /// MSR-stored thumbnails are required for Protect 7.1+'s detection-search
  /// to surface our events in the Find Anything filter (which expects the
  /// MSR-format ids that first-party native AI events produce).
  void set_msr_client(MsrClient* msr);

  /// Local Protect API base URL (e.g. "http://localhost:7080").  Used to
  /// fetch MSR-format thumbnails (length != 24) that are stored as
  /// native UBV files served by the msp media server, rather than
  /// rows in the thumbnails Postgres table.  Empty url, or a null
  /// provider, disables the fallback (DB-only lookup).
  ///
  /// On observed 401 from Protect API, the provider is asked to
  /// refresh the user_id (rate-limited) and the request is retried
  /// once with the new value.
  void set_protect_api(const std::string& url,
                       ProtectUserIdProvider* user_id_provider);

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

  // Log the per-camera event-type counts in the last hour.  @p label
  // is the prefix shown in the log line (e.g. "startup" / "periodic").
  void log_event_type_breakdown(const char* label);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace onvif
