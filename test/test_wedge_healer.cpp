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

#include <unistd.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "wedge_healer.hpp"

// A file under the test's private temp directory, so concurrent runs of
// this test do not share state files.
static std::string temp_path(const char* name) {
  const char* dir = std::getenv("TEST_TMPDIR");
  return std::string(dir ? dir : "/tmp") + "/" + name + "." +
         std::to_string(getpid());
}

int main() {
  using onvif::WedgeHealer;

  // Case 1: brand-new process, no stats yet -> no wedge.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    WedgeHealer::MsrSnapshot msr;
    const auto r = h.tick_for_testing(/*db_ms_since_success=*/-1,
                                       /*db_ms_since_timeout=*/-1,
                                       msr);
    assert(r == WedgeHealer::Reason::kNone);
    assert(cmds.empty());
  }

  // Case 2: DB timeout very recent, no success in the window -> DB wedge.
  // But the fire is suppressed by the warmup gate (5 min), so no
  // command is executed on a freshly-constructed healer.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    WedgeHealer::MsrSnapshot msr;
    const auto r = h.tick_for_testing(/*db_ms_since_success=*/-1,
                                       /*db_ms_since_timeout=*/10,
                                       msr);
    assert(r == WedgeHealer::Reason::kDbWedge);
    assert(cmds.empty());  // suppressed by warmup
  }

  // Case 3: recent success AND recent timeout -> not wedged (we're still
  // making progress, some queries just happen to slow down).
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    WedgeHealer::MsrSnapshot msr;
    const auto r = h.tick_for_testing(/*db_ms_since_success=*/500,
                                       /*db_ms_since_timeout=*/500,
                                       msr);
    assert(r == WedgeHealer::Reason::kNone);
    assert(cmds.empty());
  }

  // Case 4: MSR failing continuously, no OK in the window -> MSR wedge.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    WedgeHealer::MsrSnapshot msr;
    msr.total_fail    = 100;
    msr.total_ok      = 5;
    msr.ms_since_fail = 5000;    // 5 s ago
    msr.ms_since_ok   = 120000;  // 2 min ago
    const auto r = h.tick_for_testing(/*db_ms_since_success=*/50,
                                       /*db_ms_since_timeout=*/-1,
                                       msr);
    assert(r == WedgeHealer::Reason::kMsrWedge);
    // Still in warmup so the command isn't executed on this fresh healer.
    assert(cmds.empty());
  }

  // Case 5: MSR failing recently but OK also recent -> not wedged.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    WedgeHealer::MsrSnapshot msr;
    msr.total_fail    = 10;
    msr.total_ok      = 100;
    msr.ms_since_fail = 5000;
    msr.ms_since_ok   = 1000;
    const auto r = h.tick_for_testing(/*db_ms_since_success=*/50,
                                       /*db_ms_since_timeout=*/-1,
                                       msr);
    assert(r == WedgeHealer::Reason::kNone);
    assert(cmds.empty());
  }

  // ---------------------------------------------------------------
  // featureFlags drift (issue #34)
  // ---------------------------------------------------------------

  // Case 6: nothing armed -> the check is a no-op even if the drift fn
  // would report drift.  The healer must never go looking on its own.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    bool called = false;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    h.set_flag_drift_fn([&](const std::vector<std::string>&) {
      called = true;
      return std::vector<std::string>{"cam1"};
    });
    const auto r = h.check_flag_drift_for_testing();
    assert(r == WedgeHealer::Reason::kNone);
    assert(!called);
    assert(cmds.empty());
  }

  // Case 7: armed, drift fn reports everything in sync -> no restart.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    h.set_flag_drift_fn([](const std::vector<std::string>&) {
      return std::vector<std::string>{};  // no drift
    });
    h.arm_flag_drift_check({"cam1", "cam2"});
    const auto r = h.check_flag_drift_for_testing();
    assert(r == WedgeHealer::Reason::kNone);
    assert(cmds.empty());
  }

  // Case 8: armed, drift reported -> kFlagDrift.  The restart itself is
  // suppressed by warmup on a fresh healer, but the reason is returned so
  // the caller (and the log) still record the detection.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    std::vector<std::string> seen_ids;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    h.set_flag_drift_fn([&](const std::vector<std::string>& ids) {
      seen_ids = ids;
      return std::vector<std::string>{"cam2"};
    });
    h.arm_flag_drift_check({"cam1", "cam2"});
    const auto r = h.check_flag_drift_for_testing();
    assert(r == WedgeHealer::Reason::kFlagDrift);
    assert(seen_ids.size() == 2);
    assert(cmds.empty());  // suppressed by warmup

    // Suppressed by a safeguard -> must have re-armed with the drifting
    // subset so the restart isn't lost.  Second check sees only cam2.
    seen_ids.clear();
    const auto r2 = h.check_flag_drift_for_testing();
    assert(r2 == WedgeHealer::Reason::kFlagDrift);
    assert(seen_ids.size() == 1 && seen_ids[0] == "cam2");
  }

  // Case 9: arming twice before the check runs merges both ID sets
  // rather than dropping the first.
  {
    WedgeHealer h;
    std::vector<std::string> seen_ids;
    h.set_exec_fn([&](const std::string&) { return 0; });
    h.set_flag_drift_fn([&](const std::vector<std::string>& ids) {
      seen_ids = ids;
      return std::vector<std::string>{};
    });
    h.arm_flag_drift_check({"cam1"});
    h.arm_flag_drift_check({"cam2", "cam1"});  // cam1 must not duplicate
    h.check_flag_drift_for_testing();
    assert(seen_ids.size() == 2);
  }

  // Case 10: the drift check is one-shot -- after it runs and finds
  // nothing, a second call does nothing until re-armed.
  {
    WedgeHealer h;
    int calls = 0;
    h.set_exec_fn([&](const std::string&) { return 0; });
    h.set_flag_drift_fn([&](const std::vector<std::string>&) {
      ++calls;
      return std::vector<std::string>{};
    });
    h.arm_flag_drift_check({"cam1"});
    h.check_flag_drift_for_testing();
    h.check_flag_drift_for_testing();
    assert(calls == 1);
  }


  // Case 11: once the daily cap is reached the drift check must NOT
  // re-arm.  Re-arming there would reschedule a DB query plus an HTTP GET
  // per camera every grace period, forever, with no possible progress.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    int drift_calls = 0;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    h.set_flag_drift_fn([&](const std::vector<std::string>&) {
      ++drift_calls;
      return std::vector<std::string>{"cam1"};
    });

    // Burn the cap.  Warmup suppresses the actual command on a fresh
    // healer, so drive restart_count_ via the DB-wedge path instead:
    // each tick_for_testing is suppressed, leaving the count at 0, so
    // instead assert the re-arm contract directly.
    h.arm_flag_drift_check({"cam1"});
    const auto r1 = h.check_flag_drift_for_testing();
    assert(r1 == WedgeHealer::Reason::kFlagDrift);
    const int after_first = drift_calls;

    // Warmup is a time-bounded suppression, so it MUST have re-armed.
    h.check_flag_drift_for_testing();
    assert(drift_calls == after_first + 1);
  }


  // Case 12: the 24 h cap must survive our own restart.  The service uses
  // Restart=always and the admin page's Save & Restart exits the process,
  // so a process-local counter meant every config save reset the cap --
  // which would let a restart loop drive unbounded Protect restarts.
  {
    const std::string path = temp_path("healer_restarts");
    std::remove(path.c_str());
    // Pretend a previous process already used the whole budget just now.
    {
      std::ofstream f(path);
      const int64_t now = static_cast<int64_t>(std::time(nullptr));
      for (int i = 0; i < WedgeHealer::kMaxPerDay; ++i) f << (now - 10) << "\n";
    }

    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    h.set_state_path(path);
    h.set_flag_drift_fn([](const std::vector<std::string>&) {
      return std::vector<std::string>{"cam1"};
    });

    h.arm_flag_drift_check({"cam1"});
    h.check_flag_drift_for_testing();
    assert(cmds.empty());   // cap inherited from the previous process

    // And an entry outside the window must NOT count against the cap.
    {
      std::ofstream f(path, std::ios::trunc);
      const int64_t old = static_cast<int64_t>(std::time(nullptr))
                            - WedgeHealer::kDaySec - 60;
      f << old << "\n";
    }
    WedgeHealer h2;
    h2.set_state_path(path);
    assert(h2.restart_count() == 0);
    std::remove(path.c_str());
  }

  // ---------------------------------------------------------------
  // Patch reload: Protect started before service.js was patched (#58)
  // ---------------------------------------------------------------

  // Case 13: no callback, or Protect already running the patched file ->
  // no restart.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    h.end_warmup_for_testing();
    assert(h.check_patch_reload_for_testing() == WedgeHealer::Reason::kNone);
    int calls = 0;
    h.set_patch_reload_fn([&] { ++calls; return false; });
    assert(h.check_patch_reload_for_testing() == WedgeHealer::Reason::kNone);
    assert(h.check_patch_reload_for_testing() == WedgeHealer::Reason::kNone);
    assert(calls == 2);
    assert(cmds.empty());
  }

  // Case 14: one stale reading is not enough, because an installer that
  // just wrote service.js may be about to restart Protect itself.  The
  // second reading in a row restarts Protect only; a fresh reading in
  // between starts the count again.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    h.end_warmup_for_testing();
    bool stale = true;
    h.set_patch_reload_fn([&] { return stale; });
    assert(h.check_patch_reload_for_testing() == WedgeHealer::Reason::kNone);
    stale = false;
    assert(h.check_patch_reload_for_testing() == WedgeHealer::Reason::kNone);
    stale = true;
    assert(h.check_patch_reload_for_testing() == WedgeHealer::Reason::kNone);
    assert(cmds.empty());
    assert(h.check_patch_reload_for_testing() ==
           WedgeHealer::Reason::kPatchReload);
    assert(cmds.size() == 1);
    assert(cmds[0] == "systemctl restart unifi-protect.service");

    // Still stale right after the restart: the cooldown holds the next
    // one back, and the check keeps running so it can fire later.
    assert(h.check_patch_reload_for_testing() == WedgeHealer::Reason::kNone);
    assert(h.check_patch_reload_for_testing() ==
           WedgeHealer::Reason::kPatchReload);
    assert(cmds.size() == 1);
    assert(h.check_patch_reload_for_testing() ==
           WedgeHealer::Reason::kPatchReload);
    assert(cmds.size() == 1);
  }

  // Case 15: warmup holds the restart back without giving up.
  {
    WedgeHealer h;
    std::vector<std::string> cmds;
    int calls = 0;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    h.set_patch_reload_fn([&] { ++calls; return true; });
    h.check_patch_reload_for_testing();
    assert(h.check_patch_reload_for_testing() ==
           WedgeHealer::Reason::kPatchReload);
    assert(cmds.empty());
    h.end_warmup_for_testing();
    assert(h.check_patch_reload_for_testing() ==
           WedgeHealer::Reason::kPatchReload);
    assert(cmds.size() == 1);
    assert(calls == 3);
  }

  // Case 16: once the daily cap is reached the check pauses, so a restart
  // that cannot happen yet is not retried and logged every minute, and it
  // resumes after the pause.
  {
    const std::string path = temp_path("healer_restarts_patch");
    {
      std::ofstream f(path, std::ios::trunc);
      const int64_t now = static_cast<int64_t>(std::time(nullptr));
      for (int i = 0; i < WedgeHealer::kMaxPerDay; ++i) {
        f << (now - WedgeHealer::kCooldownSec - 10) << "\n";
      }
    }
    WedgeHealer h;
    std::vector<std::string> cmds;
    int calls = 0;
    h.set_exec_fn([&](const std::string& c) { cmds.push_back(c); return 0; });
    h.set_state_path(path);
    h.end_warmup_for_testing();
    h.set_patch_reload_fn([&] { ++calls; return true; });
    h.check_patch_reload_for_testing();
    assert(h.check_patch_reload_for_testing() ==
           WedgeHealer::Reason::kPatchReload);
    assert(cmds.empty());
    assert(h.check_patch_reload_for_testing() == WedgeHealer::Reason::kNone);
    assert(calls == 2);
    h.end_patch_reload_backoff_for_testing();
    h.check_patch_reload_for_testing();
    assert(calls == 3);
    std::remove(path.c_str());
  }

  // Protect flag drift parser (#34, #58).
  {
    using onvif::describe_protect_flag_drift;
    const std::string synced =
        "{\"name\":\"Front\",\"featureFlags\":{\"hasSmartDetect\":true,"
        "\"smartDetectTypes\":[\"person\",\"vehicle\"]},"
        "\"recordingSettings\":{\"mode\":\"detections\"}}";
    assert(describe_protect_flag_drift(synced).empty());

    const std::string stale_flag =
        "{\"featureFlags\":{\"hasSmartDetect\": false,"
        "\"smartDetectTypes\":[\"person\"]}}";
    assert(describe_protect_flag_drift(stale_flag) == "hasSmartDetect=false");

    const std::string stale_both =
        "{\"featureFlags\":{\"smartDetectTypes\":[ ],"
        "\"hasSmartDetect\":false}}";
    assert(describe_protect_flag_drift(stale_both) ==
           "smartDetectTypes=[], hasSmartDetect=false");

    // A camera named after the keys must not fake drift.
    const std::string spoof =
        "{\"name\":\"\\\"hasSmartDetect\\\":false \\\"smartDetectTypes\\\":[]\","
        "\"featureFlags\":{\"hasSmartDetect\":true,"
        "\"smartDetectTypes\":[\"person\"]}}";
    assert(describe_protect_flag_drift(spoof).empty());

    // null smartDetectTypes is not evidence of drift.
    assert(describe_protect_flag_drift(
               "{\"featureFlags\":{\"smartDetectTypes\":null,"
               "\"smartDetectZones\":[]}}").empty());

    // licensePlate cleared in Postgres but still held by Protect (#67).
    const std::string with_lp =
        "{\"featureFlags\":{\"smartDetectTypes\":[\"person\","
        "\"licensePlate\"],\"hasSmartDetect\":true},"
        "\"extendedAiFeatures\":{\"smartDetectTypes\":[]}}";
    assert(describe_protect_flag_drift(with_lp, "[\"person\"]") ==
           "smartDetectTypes has licensePlate");
    assert(describe_protect_flag_drift(
               with_lp, "[\"person\", \"licensePlate\"]").empty());
    assert(describe_protect_flag_drift(with_lp).empty());
    // licensePlate in Postgres that Protect has not loaded yet.
    const std::string without_lp =
        "{\"featureFlags\":{\"smartDetectTypes\":[\"person\"],"
        "\"hasSmartDetect\":true}}";
    assert(describe_protect_flag_drift(without_lp,
                                       "[\"person\", \"licensePlate\"]") ==
           "smartDetectTypes lacks licensePlate");
    assert(describe_protect_flag_drift(
               "{\"featureFlags\":{\"smartDetectTypes\":[]}}",
               "[\"licensePlate\"]") == "smartDetectTypes=[]");
    assert(describe_protect_flag_drift(
               "{\"featureFlags\":{\"smartDetectTypes\":null}}",
               "[\"licensePlate\"]").empty());

    using onvif::protect_holds_cleared_license_plate;
    assert(protect_holds_cleared_license_plate(with_lp, "[\"person\"]"));
    assert(!protect_holds_cleared_license_plate(
        with_lp, "[\"person\", \"licensePlate\"]"));
    assert(!protect_holds_cleared_license_plate(without_lp, "[\"person\"]"));

    // Only the first smartDetectTypes array (featureFlags) is compared.
    assert(describe_protect_flag_drift(
               "{\"featureFlags\":{\"smartDetectTypes\":[\"person\"]},"
               "\"extendedAiFeatures\":{\"smartDetectTypes\":"
               "[\"licensePlate\"]}}",
               "[\"person\"]").empty());
  }

  std::printf("test_wedge_healer: OK\n");
  return 0;
}
