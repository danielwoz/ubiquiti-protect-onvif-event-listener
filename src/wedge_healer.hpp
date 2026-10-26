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

#ifndef SRC_WEDGE_HEALER_HPP_
#define SRC_WEDGE_HEALER_HPP_

#include <atomic>
#include <chrono>  // NOLINT(build/c++11)
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>  // NOLINT(build/c++11)
#include <set>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <utility>
#include <vector>

namespace onvif {

// Background thread that watches for wedged Protect state and issues a
// targeted `systemctl restart` when it fires.  Besides the featureFlags
// drift, patch-reload and recording-stuck checks below, there are two
// independent wedge signals:
//
//   * DB wedge   -- pg_stats has seen at least one query timeout in the
//                    last kWindowSec and zero successful queries in the
//                    same window.  Restarts msp + msr + unifi-protect.
//
//   * MSR wedge  -- msr_client has been failing continuously for
//                    kWindowSec with no successes.  Restarts msr only
//                    (less disruptive than the DB path).
//
// Rate limits protect against restart storms: no restart in the first
// kWarmupSec after our own start, at most one restart per kCooldownSec,
// and a hard 24 h cap.
class WedgeHealer {
 public:
  static constexpr int kWindowSec   = 60;
  static constexpr int kWarmupSec   = 300;   // 5 min
  static constexpr int kCooldownSec = 300;   // 5 min
  static constexpr int kMaxPerDay   = 4;
  static constexpr int kDaySec      = 24 * 60 * 60;

  // Callback to fetch the current MSR success / failure snapshot from
  // detection_recorder.  Called on the monitor thread with no locks
  // held; must be cheap + thread-safe.
  //   msr_ok_since_boot   - total OK calls since process start
  //   msr_fail_since_boot - total failed calls since process start
  //   msr_ms_since_last_ok / _fail - milliseconds since the most recent
  //                                 OK / fail (-1 if none yet)
  struct MsrSnapshot {
    uint64_t total_ok       = 0;
    uint64_t total_fail     = 0;
    int64_t  ms_since_ok    = -1;
    int64_t  ms_since_fail  = -1;
  };
  using MsrSnapshotFn = std::function<MsrSnapshot()>;

  // Callback that actually runs a shell command (e.g. `systemctl
  // restart ...`).  Injectable so tests can capture the invocation
  // without actually restarting services.  Should return the shell
  // exit code.
  using ExecFn = std::function<int(const std::string&)>;

  // Grace period after we write featureFlags before we conclude Protect
  // is never going to re-read them.  Protect does pick some changes up on
  // its own; only drift that survives this window counts.
  static constexpr int kFlagDriftGraceSec = 60;

  // Reasons a restart fires; also used as the string identifier in the
  // ERROR log line so downstream grep-based dashboards can tag them.
  enum class Reason {
    kNone,
    kDbWedge,
    kMsrWedge,
    kFlagDrift,
    kPatchReload,
    kRecordingStuck,
  };

  // How often the patch-reload check runs.  A restart needs two positive
  // checks in a row, so a service.js written by an installer that restarts
  // Protect itself straight afterwards is not acted on.
  static constexpr int kPatchReloadCheckSec = 60;
  // Pause after the daily cap stops a patch-reload restart; the cap lifts
  // as earlier restarts age out of the 24 h window.
  static constexpr int kPatchReloadBackoffSec = 60 * 60;

  // Callback that reports whether Protect is still running code older than
  // a file we patched (service.js written after Protect started), so only a
  // restart loads the patch.  Called on the monitor thread.
  using PatchReloadFn = std::function<bool()>;

  // How often the recording-stuck check runs.  A restart needs the same
  // camera reported on two checks in a row.
  static constexpr int kRecordingStuckCheckSec = 60;
  // Pause after the daily cap stops a recording-stuck restart.
  static constexpr int kRecordingStuckBackoffSec = 60 * 60;

  // What the recording-stuck callback found among the cameras it checked.
  struct RecordingCheck {
    // Cameras Protect would record now but has not started recording
    // (see read_protect_recording_state()).
    std::vector<std::string> stuck;
    // Cameras whose recording Protect has started.
    std::vector<std::string> recording;
  };
  // Callback for the recording-stuck check.  Protect decides whether to
  // record a camera at start-up and when its recording settings change, so
  // a camera it skipped stays unrecorded until it restarts (#70).  Called
  // on the monitor thread.
  using RecordingStuckFn = std::function<RecordingCheck()>;

  // Callback that compares what we wrote into Postgres against what
  // Protect's API reports in memory, for the given camera IDs.  Returns
  // the subset whose featureFlags disagree (empty == Protect is in sync).
  // Injected from main.cpp so this module needs neither libpq nor curl,
  // and so tests can drive it without a live Protect.
  using FlagDriftFn =
      std::function<std::vector<std::string>(const std::vector<std::string>&)>;
  struct RestartRecord {
    Reason      reason;
    int64_t     ms_since_boot;   // when it fired
    std::string action;          // command line executed
    int         exit_code;
  };

  WedgeHealer();
  ~WedgeHealer();

  // Attach dependencies before start().
  void set_msr_snapshot_fn(MsrSnapshotFn fn) { msr_snapshot_ = std::move(fn); }
  void set_exec_fn(ExecFn fn) { exec_ = std::move(fn); }
  void set_flag_drift_fn(FlagDriftFn fn) { flag_drift_ = std::move(fn); }
  void set_patch_reload_fn(PatchReloadFn fn) {
    patch_reload_ = std::move(fn);
  }
  void set_recording_stuck_fn(RecordingStuckFn fn) {
    recording_stuck_ = std::move(fn);
  }

  /// Persist restart timestamps to @p path so the cooldown and the
  /// 24-hour cap survive our own restarts.  Without this both are
  /// process-local: the service uses Restart=always and the admin page's
  /// Save & Restart deliberately exits the process, so every config save
  /// silently reset the "4 per 24 h" limit to zero -- meaning a restart
  /// loop elsewhere could drive unbounded Protect restarts.  Warmup stays
  /// process-local by design; it is about OUR uptime.
  /// Must be called before start().  Empty path disables persistence.
  void set_state_path(const std::string& path);

  /// Keep in @p path the cameras a recording-stuck restart was issued for.
  /// Each camera gets one such restart until Protect is seen recording it,
  /// so a camera that a restart does not fix cannot use up the daily cap
  /// the other reasons share, also across restarts of this service (which
  /// restarts itself after Protect does).  Must be called before start().
  /// Empty path keeps the list in memory only.
  void set_recording_stuck_state_path(const std::string& path);

  // Arm a one-shot featureFlags drift check for @p camera_ids.  Call this
  // right after writing their featureFlags into Postgres.
  // kFlagDriftGraceSec later the healer runs flag_drift_fn once; if any
  // camera still disagrees, Protect is holding stale in-memory state
  // (issue #34) and only a `systemctl restart unifi-protect` clears it.
  //
  // Deliberately one-shot and caller-armed: the healer never goes looking
  // for drift on its own, so a restart can only ever follow a write we
  // made.  Re-arming before the pending check runs merges the ID sets.
  void arm_flag_drift_check(const std::vector<std::string>& camera_ids);

  // Spawn the monitor thread.  Idempotent; safe to call once.
  void start();

  // Signal + join.
  void stop();

  // Number of restarts we've issued this run.  Used by the admin
  // /api/healer_status endpoint.
  uint64_t restart_count() const { return restart_count_.load(); }

  // Snapshot of the most recent restart records (up to kMaxPerDay).
  // Copies under a mutex; safe from any thread.
  std::vector<RestartRecord> recent_restarts() const;

  // Exposed for unit tests: run one monitor tick against injected
  // snapshots.  Returns the reason if it fired a restart this tick.
  Reason tick_for_testing(int64_t db_ms_since_success,
                          int64_t db_ms_since_timeout,
                          const MsrSnapshot& msr);

  // Exposed for unit tests: run the pending flag-drift check immediately,
  // bypassing the grace-period wait.  Returns the reason if it fired.
  Reason check_flag_drift_for_testing();

  // Exposed for unit tests: run the patch-reload check immediately,
  // bypassing its interval.  Returns kPatchReload when it asked for a
  // restart, whether or not a safeguard then suppressed it.
  Reason check_patch_reload_for_testing();

  // Exposed for unit tests: run the recording-stuck check immediately,
  // bypassing its interval.  Returns kRecordingStuck when it asked for a
  // restart, whether or not a safeguard then suppressed it.
  Reason check_recording_stuck_for_testing();

  // Exposed for unit tests: behave as if the warmup period had passed.
  void end_warmup_for_testing() {
    boot_ = std::chrono::steady_clock::now() -
            std::chrono::seconds(kWarmupSec + 1);
  }

  // Exposed for unit tests: end a patch-reload pause after the daily cap.
  void end_patch_reload_backoff_for_testing() { patch_resume_at_ = {}; }

  // Exposed for unit tests: end a recording-stuck pause after the daily cap.
  void end_recording_stuck_backoff_for_testing() { stuck_resume_at_ = {}; }

 private:
  void run();
  Reason decide_wedge(int64_t db_ms_since_success,
                      int64_t db_ms_since_timeout,
                      const MsrSnapshot& msr) const;
  // Runs the armed drift check if its grace period has elapsed (or
  // @p force).  Disarms afterwards either way.  Returns kFlagDrift if
  // drift was found and a restart was requested.
  Reason maybe_check_flag_drift(bool force);
  // Runs the patch-reload check if kPatchReloadCheckSec has passed since
  // the last one (or @p force).  Returns kPatchReload if it requested a
  // restart.
  Reason maybe_check_patch_reload(bool force);
  // Runs the recording-stuck check if kRecordingStuckCheckSec has passed
  // since the last one (or @p force).  Returns kRecordingStuck if it
  // requested a restart.
  Reason maybe_check_recording_stuck(bool force);
  // Why a restart attempt did or didn't run.  The distinction matters for
  // the flag-drift, patch-reload and recording-stuck paths: warmup and
  // cooldown lift within minutes and are worth retrying, while the daily
  // cap lifts only as earlier restarts age out of the 24 h window.
  enum class FireResult {
    kRan,          // command executed
    kWaitAndRetry, // warmup or cooldown -- try again later
    kGaveUp,       // daily cap reached; retrying soon cannot help
  };
  FireResult fire_restart(Reason r);

  MsrSnapshotFn                     msr_snapshot_;
  ExecFn                            exec_;
  FlagDriftFn                       flag_drift_;
  // Patch-reload check state; only touched by the monitor thread.
  PatchReloadFn                     patch_reload_;
  std::chrono::steady_clock::time_point patch_checked_at_{};
  int                               patch_stale_streak_ = 0;
  std::chrono::steady_clock::time_point patch_resume_at_{};
  // Recording-stuck check state; only touched by the monitor thread.
  // stuck_prev_ holds the sorted IDs the previous check reported.
  RecordingStuckFn                  recording_stuck_;
  std::chrono::steady_clock::time_point stuck_checked_at_{};
  std::vector<std::string>          stuck_prev_;
  std::chrono::steady_clock::time_point stuck_resume_at_{};
  // Cameras a recording-stuck restart was issued for, with its wall-clock
  // time (epoch seconds).  A camera leaves the map once Protect is seen
  // recording it.  stuck_warned_ holds the ones already logged as still
  // not recording in this run.
  std::map<std::string, int64_t>    stuck_restarted_;
  std::set<std::string>             stuck_warned_;
  std::string                       stuck_state_path_;
  void persist_stuck_state() const;
  // Pending one-shot drift check, guarded by drift_mu_.  armed_at_ is
  // default-constructed when nothing is pending.
  mutable std::mutex                drift_mu_;
  std::vector<std::string>          drift_camera_ids_;
  std::chrono::steady_clock::time_point drift_armed_at_{};
  std::atomic<bool>                 running_{false};
  std::thread                       thread_;
  std::atomic<uint64_t>             restart_count_{0};
  std::chrono::steady_clock::time_point boot_{};
  // Wall-clock (epoch seconds) of restarts we have issued, newest last,
  // pruned to the last kDaySec.  Wall-clock rather than steady_clock
  // because these outlive the process.  Guarded by history_mu_.
  std::string                       state_path_;
  std::vector<int64_t>              restart_times_;
  void load_state();
  void persist_state();
  // Restarts within the last 24 h, and seconds since the most recent
  // (-1 if none).  Caller must hold history_mu_.
  int  restarts_in_window_locked() const;
  int64_t secs_since_last_restart_locked() const;
  // History under a small mutex; recorded newest-last then trimmed.
  mutable std::mutex                history_mu_;
  std::vector<RestartRecord>        history_;
  // MSR snapshot at last successful monitor tick, used to detect that
  // OK counter advanced (== healthy) in the current window.
  MsrSnapshot                       last_msr_seen_{};
  std::chrono::steady_clock::time_point last_msr_ok_advance_{};
};

/// Inspect a Protect `GET /api/cameras/<id>` body for the in-memory state
/// that makes Protect ignore our featureFlags writes: an empty
/// `smartDetectTypes` array, or `hasSmartDetect` false.  The latter makes
/// Protect force a third-party camera in Events/Adaptive recording mode
/// to "never" record (#58).  When @p db_smart_detect_types (the Postgres
/// value) is given, a "licensePlate" entry on one side only also counts.
/// Returns a short description of the drift, or an empty string if none
/// was found.
///
/// Keys are only honoured outside JSON string values, so a camera named
/// after a key cannot fake drift.
std::string describe_protect_flag_drift(
    const std::string& api_body,
    const std::string& db_smart_detect_types = "");

/// True if Protect still lists "licensePlate" in a camera's
/// `smartDetectTypes` while @p db_smart_detect_types (the Postgres value)
/// no longer does.
bool protect_holds_cleared_license_plate(
    const std::string& api_body, const std::string& db_smart_detect_types);

/// A camera's recording state as Protect reports it in its
/// `GET /api/cameras/<id>` body.
struct ProtectRecordingState {
  bool valid = false;              // the body is a JSON object
  bool connected = false;          // state "CONNECTED"
  bool recording_started = false;  // hasRecordingStarted
  bool paired_with_ai_port = false;
  // Protect would record this third-party camera now but has not started
  // recording it.  Protect decides whether to record a camera at start-up
  // and when its recording mode changes.  A third-party camera in a mode
  // other than always, never or schedule counts as "never record" unless
  // featureFlags.hasMotionDetection, featureFlags.hasSmartDetect or
  // isPairedWithAiPort is true, and Protect does not re-check when one of
  // those becomes true later, so the camera stays unrecorded until Protect
  // restarts (#70).  Set when the camera is third-party, adopted,
  // provisioned, CONNECTED, has channels, is not paused or held back by
  // geofencing, its recordingSettings.mode is not "never", Protect would
  // record it now (mode always or schedule, or one of the three flags
  // true), and hasRecordingStarted is false.
  bool stuck = false;
};
ProtectRecordingState read_protect_recording_state(const std::string& api_body);

/// The recording states of the cameras the recording-stuck check reads,
/// from their `GET /api/cameras/<id>` bodies.
struct ProtectRecordingSummary {
  // Stuck cameras (see ProtectRecordingState::stuck), leaving out those
  // paired with an AI Port: Protect manages their recording.
  std::vector<std::string> stuck;
  // Cameras whose recording Protect has started.
  std::vector<std::string> recording;
  // A connected camera other than the stuck ones is recording.
  bool other_recording = false;
};
/// Summarise @p bodies, each a camera ID and its `GET /api/cameras/<id>`
/// body.
ProtectRecordingSummary summarize_protect_recording(
    const std::vector<std::pair<std::string, std::string>>& bodies);

/// Whether Protect's `GET /api/cameras` body lists a connected camera whose
/// recording has started, other than those in @p ignore_ids.
bool protect_lists_recording_camera(const std::string& api_body,
                                    const std::vector<std::string>& ignore_ids);

}  // namespace onvif

#endif  // SRC_WEDGE_HEALER_HPP_
