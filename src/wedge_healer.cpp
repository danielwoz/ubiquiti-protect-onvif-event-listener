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

#include "wedge_healer.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ctime>
#include <cstdlib>
#include <iterator>
#include <mutex>  // NOLINT(build/c++11)
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "json_value.hpp"
#include "pg_stats.hpp"

namespace onvif {

namespace {

constexpr const char* kRestartCmdDbWedge = "systemctl restart msp.service msr.service unifi-protect.service";  // NOLINT(whitespace/line_length)

// MSR wedge is scoped to the msr binary only: it is the least
// disruptive service to bounce and playback stays functional.
constexpr const char* kRestartCmdMsrWedge = "systemctl restart msr.service";

// featureFlags drift is a Protect-process problem only -- msp/msr hold no
// camera capability state, so bouncing them would add downtime for
// nothing.  Field-confirmed on issue #34: restarting unifi-protect alone
// clears the stale in-memory flags in ~90 s.
constexpr const char* kRestartCmdFlagDrift = "systemctl restart unifi-protect.service";  // NOLINT(whitespace/line_length)

// Patched service.js is read only when the Protect process starts.
constexpr const char* kRestartCmdPatchReload = "systemctl restart unifi-protect.service";  // NOLINT(whitespace/line_length)

// Protect decides whether to record each camera when it starts.
constexpr const char* kRestartCmdRecordingStuck = "systemctl restart unifi-protect.service";  // NOLINT(whitespace/line_length)

const char* reason_str(WedgeHealer::Reason r) {
  switch (r) {
    case WedgeHealer::Reason::kDbWedge:        return "DB";
    case WedgeHealer::Reason::kMsrWedge:       return "MSR";
    case WedgeHealer::Reason::kFlagDrift:      return "featureFlags-drift";
    case WedgeHealer::Reason::kPatchReload:    return "patch-reload";
    case WedgeHealer::Reason::kRecordingStuck: return "recording-stuck";
    default:                                   return "none";
  }
}

int default_exec(const std::string& cmd) {
  // std::system inherits the parent's stdio; that's fine here -- the
  // command is a systemctl invocation which prints little on success.
  return std::system(cmd.c_str());
}

}  // namespace


namespace {
int64_t now_epoch_sec() {
  return static_cast<int64_t>(std::time(nullptr));
}
}  // namespace

void WedgeHealer::set_state_path(const std::string& path) {
  std::lock_guard<std::mutex> lk(history_mu_);
  state_path_ = path;
  load_state();
}

void WedgeHealer::load_state() {
  restart_times_.clear();
  if (state_path_.empty()) return;
  std::ifstream f(state_path_);
  if (!f.is_open()) return;   // first run
  const int64_t cutoff = now_epoch_sec() - kDaySec;
  int64_t t = 0;
  while (f >> t) {
    // Ignore anything outside the window, and anything in the future --
    // a clock step backwards must not lock the healer out indefinitely.
    if (t > cutoff && t <= now_epoch_sec()) restart_times_.push_back(t);
  }
  std::sort(restart_times_.begin(), restart_times_.end());
  if (!restart_times_.empty()) {
    LOG(INFO) << "[healer] restored " << restart_times_.size()
              << " restart(s) from the last 24 h from " << state_path_;
  }
}

void WedgeHealer::persist_state() {
  if (state_path_.empty()) return;
  const std::string tmp = state_path_ + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f.is_open()) {
      LOG(WARNING) << "[healer] cannot write " << tmp
                   << "; restart cap will not survive a restart";
      return;
    }
    for (int64_t t : restart_times_) f << t << "\n";
  }
  if (std::rename(tmp.c_str(), state_path_.c_str()) != 0) {
    LOG(WARNING) << "[healer] cannot rename " << tmp << " -> " << state_path_;
  }
}

int WedgeHealer::restarts_in_window_locked() const {
  const int64_t cutoff = now_epoch_sec() - kDaySec;
  int n = 0;
  for (int64_t t : restart_times_) if (t > cutoff) ++n;
  return n;
}

int64_t WedgeHealer::secs_since_last_restart_locked() const {
  if (restart_times_.empty()) return -1;
  return now_epoch_sec() - restart_times_.back();
}

WedgeHealer::WedgeHealer()
  : exec_(default_exec),
    boot_(std::chrono::steady_clock::now()) {}  // NOLINT(whitespace/indent_namespace)

WedgeHealer::~WedgeHealer() { stop(); }

void WedgeHealer::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void WedgeHealer::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

std::vector<WedgeHealer::RestartRecord> WedgeHealer::recent_restarts() const {
  std::lock_guard<std::mutex> lk(history_mu_);
  return history_;
}

WedgeHealer::Reason WedgeHealer::decide_wedge(
    int64_t db_ms_since_success,
    int64_t db_ms_since_timeout,
    const MsrSnapshot& msr) const {
  const int64_t win_ms = kWindowSec * 1000LL;

  // DB wedge: timeout observed within the window AND no successful
  // query completed within the window.  Both -1 means "no data yet",
  // which we treat as not wedged (during startup / warmup).
  if (db_ms_since_timeout >= 0 && db_ms_since_timeout < win_ms) {
    const bool no_recent_success =
        db_ms_since_success < 0 || db_ms_since_success >= win_ms;
    if (no_recent_success) return Reason::kDbWedge;
  }

  // MSR wedge: total_fail advanced but total_ok did not, and the last
  // OK is older than the window.  The gap between last_msr_seen_ and
  // the current snapshot tells us "activity happened this window."
  //
  // We only consider it wedged if there have been failures in the
  // window (ms_since_fail < win_ms) *and* no successes in that time
  // (ms_since_ok < 0 or >= win_ms).
  if (msr.ms_since_fail >= 0 && msr.ms_since_fail < win_ms) {
    const bool no_recent_ok =
        msr.ms_since_ok < 0 || msr.ms_since_ok >= win_ms;
    if (no_recent_ok) return Reason::kMsrWedge;
  }

  return Reason::kNone;
}

WedgeHealer::FireResult WedgeHealer::fire_restart(Reason r) {
  const auto now = std::chrono::steady_clock::now();
  const int64_t warmup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - boot_).count();
  if (warmup_ms < kWarmupSec * 1000LL) {
    LOG(WARNING) << "[healer] " << reason_str(r)
                 << " trigger detected but suppressed (warmup, "
                 << warmup_ms / 1000 << "s < " << kWarmupSec << "s)";
    return FireResult::kWaitAndRetry;
  }
  {
    std::lock_guard<std::mutex> lk(history_mu_);
    const int64_t since = secs_since_last_restart_locked();
    if (since >= 0 && since < kCooldownSec) {
      LOG(WARNING) << "[healer] " << reason_str(r)
                   << " trigger detected but suppressed (cooldown, "
                   << since << "s < " << kCooldownSec << "s)";
      return FireResult::kWaitAndRetry;
    }
    const int in_window = restarts_in_window_locked();
    if (in_window >= kMaxPerDay) {
      LOG(WARNING) << "[healer] " << reason_str(r)
                   << " trigger detected but suppressed (24 h cap reached: "
                   << in_window << "/" << kMaxPerDay
                   << ", counted across restarts of this service)";
      return FireResult::kGaveUp;
    }
  }

  const char* cmd_c = kRestartCmdMsrWedge;
  if (r == Reason::kDbWedge)             cmd_c = kRestartCmdDbWedge;
  else if (r == Reason::kFlagDrift)      cmd_c = kRestartCmdFlagDrift;
  else if (r == Reason::kPatchReload)    cmd_c = kRestartCmdPatchReload;
  else if (r == Reason::kRecordingStuck) cmd_c = kRestartCmdRecordingStuck;
  const std::string cmd = cmd_c;
  if (r == Reason::kFlagDrift) {
    LOG(ERROR) << "[healer] " << reason_str(r)
               << ": Protect still reports stale featureFlags "
               << kFlagDriftGraceSec
               << "s after we wrote them -- running: " << cmd;
  } else if (r == Reason::kPatchReload) {
    LOG(ERROR) << "[healer] " << reason_str(r)
               << ": Protect started before service.js was patched and "
                  "only loads it at start-up -- running: " << cmd;
  } else if (r == Reason::kRecordingStuck) {
    LOG(ERROR) << "[healer] " << reason_str(r)
               << ": Protect has not started recording a third-party camera "
                  "it would now record, and decides that only at start-up "
                  "-- running: " << cmd;
  } else {
    LOG(ERROR) << "[healer] " << reason_str(r)
               << " wedged for >" << kWindowSec
               << "s -- running: " << cmd;
  }
  const int rc = exec_ ? exec_(cmd) : -1;
  if (rc != 0) {
    LOG(ERROR) << "[healer] restart exit code " << rc
               << " (command: " << cmd << ")";
  } else {
    LOG(WARNING) << "[healer] Protect services restarted successfully";
  }
  restart_count_.fetch_add(1);
  {
    std::lock_guard<std::mutex> lk(history_mu_);
    restart_times_.push_back(now_epoch_sec());
    const int64_t cutoff = now_epoch_sec() - kDaySec;
    restart_times_.erase(
        std::remove_if(restart_times_.begin(), restart_times_.end(),
                       [cutoff](int64_t t) { return t <= cutoff; }),
        restart_times_.end());
    persist_state();
    history_.push_back(RestartRecord{r, warmup_ms, cmd, rc});
    if (history_.size() > static_cast<size_t>(kMaxPerDay))
      history_.erase(history_.begin());
  }
  return FireResult::kRan;
}

void WedgeHealer::arm_flag_drift_check(
    const std::vector<std::string>& camera_ids) {
  if (camera_ids.empty()) return;
  std::lock_guard<std::mutex> lk(drift_mu_);
  // Merge rather than replace: a second write landing inside the grace
  // window must not drop the cameras the first write was waiting on.
  for (const auto& id : camera_ids) {
    if (std::find(drift_camera_ids_.begin(), drift_camera_ids_.end(), id) ==
        drift_camera_ids_.end()) {
      drift_camera_ids_.push_back(id);
    }
  }
  // Restart the grace clock so the check runs a full window after the
  // most recent write, giving Protect the best chance to catch up on
  // its own before we conclude it never will.
  drift_armed_at_ = std::chrono::steady_clock::now();
}

WedgeHealer::Reason WedgeHealer::maybe_check_flag_drift(bool force) {
  std::vector<std::string> ids;
  {
    std::lock_guard<std::mutex> lk(drift_mu_);
    if (drift_armed_at_ == std::chrono::steady_clock::time_point{})
      return Reason::kNone;  // nothing armed
    if (!force) {
      const int64_t waited_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - drift_armed_at_).count();
      if (waited_ms < kFlagDriftGraceSec * 1000LL) return Reason::kNone;
    }
    ids.swap(drift_camera_ids_);
    drift_armed_at_ = {};  // one-shot: disarm regardless of outcome
  }
  if (ids.empty() || !flag_drift_) return Reason::kNone;

  const std::vector<std::string> drifted = flag_drift_(ids);
  if (drifted.empty()) {
    LOG(INFO) << "[healer] featureFlags verified in Protect for "
              << ids.size() << " camera(s); no drift";
    return Reason::kNone;
  }
  LOG(WARNING) << "[healer] " << drifted.size() << " of " << ids.size()
               << " camera(s) still show stale featureFlags in Protect "
               << "after " << kFlagDriftGraceSec << "s";
  const FireResult fr = fire_restart(Reason::kFlagDrift);
  if (fr == FireResult::kWaitAndRetry) {
    // A safeguard declined this one (most often warmup -- the common
    // path is an admin-UI toggle, which restarts *us*, so our own
    // uptime is near zero exactly when the drift appears).  Re-arm with
    // the cameras that are still drifting so the restart happens as
    // soon as it is permitted instead of being lost.
    // Merge, don't assign: arm_flag_drift_check() may have added cameras
    // while flag_drift_() was running with drift_mu_ released, and
    // clobbering them would break the merge invariant that function
    // documents.
    std::lock_guard<std::mutex> lk(drift_mu_);
    for (const auto& id : drifted) {
      if (std::find(drift_camera_ids_.begin(), drift_camera_ids_.end(), id) ==
          drift_camera_ids_.end()) {
        drift_camera_ids_.push_back(id);
      }
    }
    drift_armed_at_ = std::chrono::steady_clock::now();
  } else if (fr == FireResult::kGaveUp) {
    // The daily cap lifts only as earlier restarts age out of the 24 h
    // window, so re-arming would repeat a Postgres query plus one HTTP GET
    // per camera every kFlagDriftGraceSec for hours with no progress.  Stay
    // disarmed until something arms us again.
    LOG(WARNING) << "[healer] featureFlags drift persists but the restart "
                    "cap is reached; not re-arming. Restart onvif-recorder "
                    "or restart unifi-protect manually to clear it.";
  }
  return Reason::kFlagDrift;
}

WedgeHealer::Reason WedgeHealer::tick_for_testing(
    int64_t db_ms_since_success,
    int64_t db_ms_since_timeout,
    const MsrSnapshot& msr) {
  const Reason r = decide_wedge(db_ms_since_success,
                                 db_ms_since_timeout, msr);
  if (r != Reason::kNone) fire_restart(r);
  return r;
}

WedgeHealer::Reason WedgeHealer::check_flag_drift_for_testing() {
  return maybe_check_flag_drift(/*force=*/true);
}

WedgeHealer::Reason WedgeHealer::maybe_check_patch_reload(bool force) {
  if (!patch_reload_) return Reason::kNone;
  const auto now = std::chrono::steady_clock::now();
  if (now < patch_resume_at_) return Reason::kNone;
  const bool checked_before =
      patch_checked_at_ != std::chrono::steady_clock::time_point();
  if (!force && checked_before &&
      now - patch_checked_at_ < std::chrono::seconds(kPatchReloadCheckSec)) {
    return Reason::kNone;
  }
  patch_checked_at_ = now;
  if (!patch_reload_()) {
    patch_stale_streak_ = 0;
    return Reason::kNone;
  }
  if (++patch_stale_streak_ < 2) return Reason::kNone;

  const FireResult fr = fire_restart(Reason::kPatchReload);
  if (fr == FireResult::kRan) {
    patch_stale_streak_ = 0;
  } else if (fr == FireResult::kGaveUp) {
    // Pause instead of retrying every minute against a cap that lifts
    // only as earlier restarts age out of the 24 h window.
    patch_stale_streak_ = 0;
    patch_resume_at_ = now + std::chrono::seconds(kPatchReloadBackoffSec);
    LOG(WARNING) << "[healer] Protect is still running the service.js it "
                    "loaded before onvif-recorder patched it, but the "
                    "restart cap is reached; checking again in "
                 << kPatchReloadBackoffSec / 60
                 << " min. Restart unifi-protect to load the patch now.";
  }
  return Reason::kPatchReload;
}

WedgeHealer::Reason WedgeHealer::check_patch_reload_for_testing() {
  return maybe_check_patch_reload(/*force=*/true);
}

WedgeHealer::Reason WedgeHealer::maybe_check_recording_stuck(bool force) {
  if (!recording_stuck_) return Reason::kNone;
  const auto now = std::chrono::steady_clock::now();
  if (now < stuck_resume_at_) return Reason::kNone;
  const bool checked_before =
      stuck_checked_at_ != std::chrono::steady_clock::time_point();
  if (!force && checked_before &&
      now - stuck_checked_at_ < std::chrono::seconds(kRecordingStuckCheckSec)) {
    return Reason::kNone;
  }
  stuck_checked_at_ = now;

  std::vector<std::string> stuck = recording_stuck_();
  std::sort(stuck.begin(), stuck.end());
  stuck.erase(std::unique(stuck.begin(), stuck.end()), stuck.end());
  // Only a camera reported on two checks in a row counts, so one that
  // Protect is about to start recording anyway is not acted on.
  std::vector<std::string> repeated;
  std::set_intersection(stuck.begin(), stuck.end(), stuck_prev_.begin(),
                        stuck_prev_.end(), std::back_inserter(repeated));
  stuck_prev_ = std::move(stuck);
  if (repeated.empty()) return Reason::kNone;

  std::string ids;
  for (const auto& id : repeated) {
    if (!ids.empty()) ids += ", ";
    ids += id;
  }
  LOG(WARNING) << "[healer] Protect has still not started recording "
               << repeated.size() << " camera(s) it would record: " << ids;
  const FireResult fr = fire_restart(Reason::kRecordingStuck);
  if (fr == FireResult::kRan) {
    stuck_prev_.clear();
  } else if (fr == FireResult::kGaveUp) {
    // Pause instead of retrying every minute against a cap that lifts
    // only as earlier restarts age out of the 24 h window.
    stuck_prev_.clear();
    stuck_resume_at_ = now + std::chrono::seconds(kRecordingStuckBackoffSec);
    LOG(WARNING) << "[healer] Protect is still not recording " << ids
                 << " but the restart cap is reached; checking again in "
                 << kRecordingStuckBackoffSec / 60
                 << " min. Switch the camera's recording mode to Always and "
                    "back, or restart unifi-protect, to start it now.";
  }
  return Reason::kRecordingStuck;
}

WedgeHealer::Reason WedgeHealer::check_recording_stuck_for_testing() {
  return maybe_check_recording_stuck(/*force=*/true);
}

void WedgeHealer::run() {
  // Sleep in 1-second slices so stop() is prompt.  Tick cadence: 10 s.
  constexpr int kTickSec = 10;
  int tick_ctr = 0;
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!running_.load()) break;
    if (++tick_ctr < kTickSec) continue;
    tick_ctr = 0;

    const MsrSnapshot msr =
        msr_snapshot_ ? msr_snapshot_() : MsrSnapshot{};
    const int64_t db_ms_success = pg::MsSinceLastSuccess();
    const int64_t db_ms_timeout = pg::MsSinceLastTimeout();
    const Reason r = decide_wedge(db_ms_success, db_ms_timeout, msr);
    if (r != Reason::kNone) {
      fire_restart(r);
      continue;  // one action per tick
    }
    if (maybe_check_flag_drift(/*force=*/false) != Reason::kNone) continue;
    if (maybe_check_patch_reload(/*force=*/false) != Reason::kNone) continue;
    maybe_check_recording_stuck(/*force=*/false);
  }
}

namespace {

// Position just past the first occurrence of "key": that is a real object
// key (not inside a string value), or npos.
size_t find_json_key_value(const std::string& body, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  bool in_string = false;
  for (size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (in_string) {
      if (c == '\\') {
        ++i;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c != '"') continue;
    if (body.compare(i, needle.size(), needle) == 0) {
      size_t j = i + needle.size();
      while (j < body.size() &&
             std::isspace(static_cast<unsigned char>(body[j]))) ++j;
      if (j < body.size() && body[j] == ':') {
        ++j;
        while (j < body.size() &&
               std::isspace(static_cast<unsigned char>(body[j]))) ++j;
        return j;
      }
    }
    in_string = true;
  }
  return std::string::npos;
}

constexpr char kLicensePlate[] = "\"licensePlate\"";

// Whether the smartDetectTypes array starting at @p sdt lists licensePlate.
bool array_lists_license_plate(const std::string& body, size_t sdt) {
  return body.find(kLicensePlate, sdt) < body.find(']', sdt);
}

}  // namespace

std::string describe_protect_flag_drift(
    const std::string& api_body, const std::string& db_smart_detect_types) {
  std::string out;
  const size_t sdt = find_json_key_value(api_body, "smartDetectTypes");
  if (sdt != std::string::npos && api_body[sdt] == '[') {
    size_t j = sdt + 1;
    while (j < api_body.size() &&
           std::isspace(static_cast<unsigned char>(api_body[j]))) ++j;
    if (j < api_body.size() && api_body[j] == ']')
      out = "smartDetectTypes=[]";

    if (!db_smart_detect_types.empty()) {
      const bool protect_has = array_lists_license_plate(api_body, sdt);
      const bool db_has =
          db_smart_detect_types.find(kLicensePlate) != std::string::npos;
      if (protect_has && !db_has) {
        out = "smartDetectTypes has licensePlate";
      } else if (!protect_has && db_has && out.empty()) {
        out = "smartDetectTypes lacks licensePlate";
      }
    }
  }
  const size_t hsd = find_json_key_value(api_body, "hasSmartDetect");
  if (hsd != std::string::npos && api_body.compare(hsd, 5, "false") == 0) {
    if (!out.empty()) out += ", ";
    out += "hasSmartDetect=false";
  }
  return out;
}

bool protect_holds_cleared_license_plate(
    const std::string& api_body, const std::string& db_smart_detect_types) {
  const size_t sdt = find_json_key_value(api_body, "smartDetectTypes");
  return sdt != std::string::npos && api_body[sdt] == '[' &&
         array_lists_license_plate(api_body, sdt) &&
         db_smart_detect_types.find(kLicensePlate) == std::string::npos;
}

namespace {

bool member_true(const json::Value& obj, const char* name) {
  const json::Value* v = obj.member(name);
  return v != nullptr && v->is_true();
}

ProtectRecordingState recording_state_of(const json::Value& cam) {
  ProtectRecordingState st;
  if (cam.type != json::Value::Type::kObject) return st;
  st.valid = true;
  const json::Value* state = cam.member("state");
  st.connected = state != nullptr && state->is_string("CONNECTED");
  st.recording_started = member_true(cam, "hasRecordingStarted");
  st.paired_with_ai_port = member_true(cam, "isPairedWithAiPort");

  const json::Value* started = cam.member("hasRecordingStarted");
  if (started == nullptr || !started->is_false()) return st;
  if (!member_true(cam, "isThirdPartyCamera") ||
      !member_true(cam, "isAdopted") || !st.connected) {
    return st;
  }
  const json::Value* mode = cam.find({"recordingSettings", "mode"});
  if (mode == nullptr || mode->type != json::Value::Type::kString ||
      mode->text == "never") {
    return st;
  }
  // With geofencing "allAway" Protect stops recording while someone is
  // home, which the camera body does not show.
  const json::Value* geo = cam.find({"recordingSettings", "geofencing"});
  if (geo != nullptr && geo->is_string("allAway")) return st;
  if (member_true(cam, "isRecordingsPaused")) return st;
  // Protect starts no recording for a camera without channels.
  const json::Value* channels = cam.member("channels");
  if (channels == nullptr || channels->type != json::Value::Type::kArray ||
      channels->items.empty()) {
    return st;
  }
  const json::Value* flags = cam.member("featureFlags");
  const bool recordable =
      mode->text == "always" || mode->text == "schedule" ||
      (flags != nullptr && (member_true(*flags, "hasMotionDetection") ||
                            member_true(*flags, "hasSmartDetect"))) ||
      st.paired_with_ai_port;
  st.stuck = recordable;
  return st;
}

}  // namespace

ProtectRecordingState read_protect_recording_state(
    const std::string& api_body) {
  json::Value cam;
  if (!json::parse(api_body, &cam)) return {};
  return recording_state_of(cam);
}

bool protect_lists_recording_camera(
    const std::string& api_body, const std::vector<std::string>& ignore_ids) {
  json::Value cams;
  if (!json::parse(api_body, &cams) ||
      cams.type != json::Value::Type::kArray) {
    return false;
  }
  for (const auto& cam : cams.items) {
    const json::Value* id = cam.member("id");
    if (id == nullptr || id->type != json::Value::Type::kString) continue;
    if (std::find(ignore_ids.begin(), ignore_ids.end(), id->text) !=
        ignore_ids.end()) {
      continue;
    }
    const ProtectRecordingState st = recording_state_of(cam);
    if (st.connected && st.recording_started) return true;
  }
  return false;
}

}  // namespace onvif
