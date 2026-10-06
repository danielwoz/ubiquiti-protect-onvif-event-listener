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
#include <mutex>  // NOLINT(build/c++11)
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <utility>
#include <vector>

namespace onvif {

// Background thread that watches for wedged Protect state and issues a
// targeted `systemctl restart` when it fires.  There are two independent
// wedge signals:
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
  };

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

  /// Persist restart timestamps to @p path so the cooldown and the
  /// 24-hour cap survive our own restarts.  Without this both are
  /// process-local: the service uses Restart=always and the admin page's
  /// Save & Restart deliberately exits the process, so every config save
  /// silently reset the "4 per 24 h" limit to zero -- meaning a restart
  /// loop elsewhere could drive unbounded Protect restarts.  Warmup stays
  /// process-local by design; it is about OUR uptime.
  /// Must be called before start().  Empty path disables persistence.
  void set_state_path(const std::string& path);

  // Arm a one-shot featureFlags drift check for @p camera_ids.  Call this
  // immediately after enable_smart_detect() writes flags into Postgres.
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

 private:
  void run();
  Reason decide_wedge(int64_t db_ms_since_success,
                      int64_t db_ms_since_timeout,
                      const MsrSnapshot& msr) const;
  // Runs the armed drift check if its grace period has elapsed (or
  // @p force).  Disarms afterwards either way.  Returns kFlagDrift if
  // drift was found and a restart was requested.
  Reason maybe_check_flag_drift(bool force);
  // Why a restart attempt did or didn't run.  The distinction matters for
  // the flag-drift path: a time-bounded suppression is worth waiting out
  // and retrying, but the daily cap never lifts within a process, so
  // re-arming against it would spin forever.
  enum class FireResult {
    kRan,          // command executed
    kWaitAndRetry, // warmup or cooldown -- try again later
    kGaveUp,       // daily cap reached; retrying cannot help
  };
  FireResult fire_restart(Reason r);

  MsrSnapshotFn                     msr_snapshot_;
  ExecFn                            exec_;
  FlagDriftFn                       flag_drift_;
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
/// to "never" record (#58).  Returns a short description of the drift, or
/// an empty string if none was found.
///
/// Keys are only honoured outside JSON string values, so a camera named
/// after a key cannot fake drift.
std::string describe_protect_flag_drift(const std::string& api_body);

}  // namespace onvif

#endif  // SRC_WEDGE_HEALER_HPP_
