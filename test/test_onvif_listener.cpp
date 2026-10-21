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

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "onvif_listener.hpp"
#include "camera_emulators.hpp"

// ============================================================
// Tiny test framework
// ============================================================
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& msg, const char* file, int line) {
  if (cond) {
    ++g_pass;
  } else {
    std::cerr << "  FAIL [" << file << ":" << line << "] " << msg << "\n";
    ++g_fail;
  }
}

#define CHECK(cond, msg) check((cond), (msg), __FILE__, __LINE__)

static bool run_test(const std::string& name, const std::function<void()>& fn) {
  int before = g_fail;
  std::cout << "[ RUN ] " << name << "\n";
  const auto t0 = std::chrono::steady_clock::now();
  fn();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
  bool ok = (g_fail == before);
  std::cout << (ok ? "[  OK ] " : "[FAIL ] ") << name << " (" << ms
            << " ms)\n\n";
  return ok;
}

// ============================================================
// Event collector: run OnvifListener until a predicate is satisfied or timeout
// ============================================================
struct CollectedEvents {
  std::vector<onvif::OnvifEvent> events;
  bool timed_out{false};
};

// Collect events until pred(events) returns true or timeout expires.
// pred is called (under the mutex) each time a new event arrives.
template<typename Pred>
static CollectedEvents collect_until(
  std::vector<onvif::CameraConfig> configs,
  Pred pred,
  std::chrono::seconds timeout) {
  onvif::OnvifListener listener;
  for (const auto& cfg : configs)
    listener.add_camera(cfg);

  std::mutex mu;
  std::condition_variable cv;
  std::vector<onvif::OnvifEvent> evs;

  std::thread t([&] {
    listener.run([&](const onvif::OnvifEvent& ev) {
      std::lock_guard<std::mutex> lk(mu);
      evs.push_back(ev);
      if (pred(evs))
        cv.notify_one();
    });
  });

  bool ok;
  {
    std::unique_lock<std::mutex> lk(mu);
    ok = cv.wait_for(lk, timeout, [&] { return pred(evs); });
  }

  listener.stop();
  t.join();

  CollectedEvents result;
  {
    std::lock_guard<std::mutex> lk(mu);
    result.events = std::move(evs);
  }
  result.timed_out = !ok;
  return result;
}

// Convenience: collect until at least N events have arrived.
static CollectedEvents collect(
  std::vector<onvif::CameraConfig> configs,
  std::size_t n,
  std::chrono::seconds timeout) {
  return collect_until(
    std::move(configs),
    [n](const std::vector<onvif::OnvifEvent>& evs) { return evs.size() >= n; },
    timeout);
}

// ============================================================
// Test: Hikvision-compatible -- subscribes on first attempt, receives events
// ============================================================
static void test_hikvision_basic(const std::string& jsonl) {
  HikvisionCompatibleEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "eW6iO01l";
  cfg.retry_interval_sec = 1;

  auto r = collect({cfg}, 5, std::chrono::seconds(30));

  CHECK(!r.timed_out, "timed out waiting for events");
  CHECK(r.events.size() >= 5, "expected >= 5 events");

  for (const auto& ev : r.events) {
    CHECK(!ev.topic.empty(),       "event topic must not be empty");
    CHECK(!ev.property_op.empty(), "event property_op must not be empty");
    CHECK(ev.property_op == "Initialized" ||
          ev.property_op == "Changed"     ||
          ev.property_op == "Deleted",
          "unexpected property_op: " + ev.property_op);
    CHECK(ev.camera_ip == emu.local_address(),
          "camera_ip mismatch: expected " + emu.local_address() +
          " got " + ev.camera_ip);
  }

  // First PullMessages response carries Initialized state notifications
  if (!r.events.empty()) {
    CHECK(r.events.front().property_op == "Initialized",
          "first event should be Initialized, got: " +
          r.events.front().property_op);
  }
}

// ============================================================
// Test: Hikvision-compatible -- Changed events appear after Initialized batch
// ============================================================
static void test_hikvision_changed_events(const std::string& jsonl) {
  HikvisionCompatibleEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "eW6iO01l";
  cfg.retry_interval_sec = 1;

  // Collect enough to get past the initial Initialized batch
  auto r = collect({cfg}, 20, std::chrono::seconds(30));

  CHECK(!r.timed_out, "timed out waiting for events");
  CHECK(r.events.size() >= 20, "expected >= 20 events");

  bool saw_changed = false;
  for (const auto& ev : r.events) {
    if (ev.property_op == "Changed") {
      saw_changed = true;
      CHECK(ev.topic.find("RuleEngine") != std::string::npos ||
            ev.topic.find("VideoAnalytics") != std::string::npos,
            "Changed event topic unexpected: " + ev.topic);
    }
  }
  CHECK(saw_changed, "expected at least one Changed event");
}

// ============================================================
// Test: Dahua DH-SD4A425DB-HNY -- retries subscription before succeeding
// ============================================================
static void test_dahua_retries(const std::string& jsonl) {
  DahuaSD4A425DBEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "danielwoz";
  cfg.password           = "eW6iO01l";
  cfg.retry_interval_sec = 1;  // retries x 1 s before subscribe succeeds

  auto r = collect({cfg}, 3, std::chrono::seconds(60));

  CHECK(!r.timed_out, "timed out waiting for events after subscription retries");
  CHECK(r.events.size() >= 3, "expected >= 3 events after retries");

  for (const auto& ev : r.events) {
    // Some PullMessages responses carry no notification elements; the
    // listener still invokes the callback with an empty-field event.
    // Skip those and only validate events that carry actual data.
    if (ev.topic.empty()) continue;

    CHECK(ev.property_op == "Initialized" || ev.property_op == "Changed",
          "unexpected property_op: " + ev.property_op);
    CHECK(ev.camera_ip == emu.local_address(),
          "camera_ip mismatch: " + ev.camera_ip);
  }
}

// ============================================================
// Test: Dahua DH-SD4A425DB-HNY -- events have motion-related topics
// ============================================================
static void test_dahua_topics(const std::string& jsonl) {
  DahuaSD4A425DBEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "danielwoz";
  cfg.password           = "eW6iO01l";
  cfg.retry_interval_sec = 1;

  auto r = collect({cfg}, 5, std::chrono::seconds(60));

  CHECK(!r.timed_out, "timed out");
  CHECK(r.events.size() >= 5, "expected >= 5 events");

  for (const auto& ev : r.events) {
    if (ev.topic.empty()) continue;  // skip empty-payload PullMessages responses

    // Dahua produces motion/alarm events under RuleEngine and VideoSource
    CHECK(ev.topic.find("RuleEngine")    != std::string::npos ||
          ev.topic.find("VideoSource")    != std::string::npos ||
          ev.topic.find("VideoAnalytics") != std::string::npos,
          "unexpected topic: " + ev.topic);
  }
}

// ============================================================
// Test: CellMotionCamera -- subscribes immediately, receives motion events
// ============================================================
static void test_cell_motion_basic(const std::string& jsonl) {
  CellMotionCameraEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "password";
  cfg.retry_interval_sec = 1;

  auto r = collect({cfg}, 5, std::chrono::seconds(30));

  CHECK(!r.timed_out, "timed out waiting for events");
  CHECK(r.events.size() >= 5, "expected >= 5 events");

  for (const auto& ev : r.events) {
    CHECK(!ev.topic.empty(), "event topic must not be empty");
    CHECK(ev.property_op == "Initialized" || ev.property_op == "Changed",
          "unexpected property_op: " + ev.property_op);
    CHECK(ev.camera_ip == emu.local_address(),
          "camera_ip mismatch: " + ev.camera_ip);
  }
}

// ============================================================
// Test: CellMotionCamera -- Changed events carry IsMotion data
// ============================================================
static void test_cell_motion_events(const std::string& jsonl) {
  CellMotionCameraEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "password";
  cfg.retry_interval_sec = 1;

  // Collect enough to cycle past the Initialized burst and see Changed events
  auto r = collect({cfg}, 30, std::chrono::seconds(60));

  CHECK(!r.timed_out, "timed out waiting for events");

  bool saw_cell_motion = false;
  bool saw_changed     = false;
  for (const auto& ev : r.events) {
    if (ev.topic == "tns1:RuleEngine/CellMotionDetector/Motion") {
      saw_cell_motion = true;
      if (ev.property_op == "Changed") {
        saw_changed = true;
        // IsMotion must be present in the data map
        CHECK(ev.data.count("IsMotion") > 0,
              "CellMotionDetector/Motion Changed event missing IsMotion data");
      }
    }
  }
  CHECK(saw_cell_motion, "expected CellMotionDetector/Motion events");
  CHECK(saw_changed,     "expected at least one Changed (motion) event");
}

// ============================================================
// Test: Hikvision PullPoint NotAuthorized -- GetServices succeeds but
// CreatePullPointSubscription returns HTTP 400 + ter:NotAuthorized.
// Reproduces the real camera reported in issue #20.
// Listener should give up gracefully after max_consecutive_failures.
// ============================================================
static void test_hikvision_pullpoint_notauthorized(const std::string& jsonl) {
  HikvisionPullPointNotAuthorizedEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                       = emu.local_address();
  cfg.user                     = "admin";
  cfg.password                 = "password";
  cfg.retry_interval_sec       = 0;
  cfg.max_consecutive_failures = 3;

  auto r = collect({cfg}, 1, std::chrono::seconds(10));

  CHECK(r.timed_out,
        "expected timeout (no events) when CreatePullPoint is NotAuthorized");
  CHECK(r.events.empty(),
        "expected zero events from NotAuthorized Hikvision");
}

// ============================================================
// Test: ThinginoCameraEmulator -- listener gives up after max failures
// ============================================================
static void test_thingino_graceful_stop(const std::string& jsonl) {
  ThinginoCameraEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                      = emu.local_address();
  cfg.user                    = "thingino";
  cfg.password                = "password";
  cfg.retry_interval_sec      = 0;  // no delay between retries in test
  cfg.max_consecutive_failures = 5;

  // The listener should give up after 5 consecutive 404s and emit no events.
  auto r = collect({cfg}, 1, std::chrono::seconds(10));

  CHECK(r.timed_out, "expected timeout (no events) from a camera that always 404s");
  CHECK(r.events.empty(), "expected zero events from Thingino camera");
}

// ============================================================
// Test: Html404CameraEmulator -- HTTP 200 with HTML body triggers XML
// parse failures; listener gives up after max_consecutive_failures
// ============================================================
static void test_html_404_graceful_stop(const std::string& jsonl) {
  Html404CameraEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                       = emu.local_address();
  cfg.user                     = "admin";
  cfg.password                 = "password";
  cfg.retry_interval_sec       = 0;
  cfg.max_consecutive_failures = 5;

  auto r = collect({cfg}, 1, std::chrono::seconds(10));

  CHECK(r.timed_out,    "expected timeout (no events) from HTML-404 camera");
  CHECK(r.events.empty(), "expected zero events from HTML-404 camera");
}

// ============================================================
// Test: CellMotionCamera with no motion events -- subscribes and renews
// but PullMessages always return empty; listener stays alive
// ============================================================
static void test_cell_motion_no_events(const std::string& jsonl) {
  CellMotionCameraEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "password";
  cfg.retry_interval_sec = 1;

  // Initialized events (not motion) are still delivered on first pull.
  auto r = collect({cfg}, 3, std::chrono::seconds(30));

  CHECK(!r.timed_out, "timed out waiting for Initialized events");
  CHECK(r.events.size() >= 3, "expected >= 3 Initialized events");

  for (const auto& ev : r.events) {
    if (ev.topic.empty()) continue;  // skip empty PullMessages callbacks
    CHECK(ev.property_op == "Initialized",
          "expected only Initialized events, got: " + ev.property_op);
  }
}

// ============================================================
// Test: UNVR camera -- cell-motion camera from UNVR system
// ============================================================
static void test_unvr_cell_motion(const std::string& jsonl) {
  CellMotionCameraEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "password";
  cfg.retry_interval_sec = 1;

  auto r = collect({cfg}, 5, std::chrono::seconds(30));

  CHECK(!r.timed_out, "timed out waiting for UNVR camera events");
  CHECK(r.events.size() >= 5, "expected >= 5 events from UNVR camera");

  for (const auto& ev : r.events) {
    CHECK(ev.property_op == "Initialized" || ev.property_op == "Changed",
          "unexpected property_op: " + ev.property_op);
    CHECK(ev.camera_ip == emu.local_address(), "camera_ip mismatch");
  }
}

// ============================================================
// Test: Axis-style WS-Addressing ReferenceParameters forwarding
//
// Uses AxisReferenceParamsEmulator which multiplexes all ONVIF operations
// through /onvif/services and identifies subscriptions via ReferenceParameters.
// The emulator returns HTTP 400 for PullMessages and Renew when the token is
// absent from the request body, so the test passes only if the listener
// correctly extracts and forwards the token in every subsequent call.
// ============================================================
static void test_axis_ref_params() {
  AxisReferenceParamsEmulator emu;
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "root";
  cfg.password           = "password";
  cfg.retry_interval_sec = 1;

  // Collect a few motion events to confirm that PullMessages succeeded
  // (which requires the ref_params token to have been forwarded).
  auto r = collect({cfg}, 3, std::chrono::seconds(30));

  CHECK(!r.timed_out, "axis: timed out waiting for events (possible missing "
        "ReferenceParameters forwarding)");
  CHECK(r.events.size() >= 3, "axis: expected >= 3 events");

  for (const auto& ev : r.events) {
    if (ev.topic.empty()) continue;
    CHECK(ev.topic.find("CellMotionDetector") != std::string::npos,
          "axis: unexpected topic: " + ev.topic);
    CHECK(ev.camera_ip == emu.local_address(),
          "axis: camera_ip mismatch: " + ev.camera_ip);
  }
}

// ============================================================
// Test: Reolink camera with malformed GetServices XML
//
// The Reolink RLC-811A returns GetServices XML containing an undeclared
// "tad:" namespace prefix in Capabilities elements.  Before the fix
// (xmlReadMemory with XML_PARSE_RECOVER), libxml2 would fail to parse
// the response, preventing event subscription.  This test verifies that
// the listener tolerates the malformed XML and still receives
// PeopleDetect Changed events.
// ============================================================
static void test_reolink_namespace(const std::string& jsonl) {
  ReolinkCameraEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "password";
  cfg.retry_interval_sec = 1;

  // The JSONL has Initialized + two Changed PullMessages (true/false).
  // Collect at least 2 events to prove the subscription worked.
  auto r = collect({cfg}, 2, std::chrono::seconds(30));

  CHECK(!r.timed_out, "reolink: timed out (namespace bug may have "
        "prevented subscription)");
  CHECK(r.events.size() >= 2, "reolink: expected >= 2 events, got: " +
        std::to_string(r.events.size()));

  bool saw_people_detect = false;
  for (const auto& ev : r.events) {
    CHECK(!ev.topic.empty(), "reolink: event topic must not be empty");
    CHECK(ev.camera_ip == emu.local_address(),
          "reolink: camera_ip mismatch: " + ev.camera_ip);
    if (ev.topic.find("PeopleDetect") != std::string::npos)
      saw_people_detect = true;
  }
  CHECK(saw_people_detect,
        "reolink: expected at least one PeopleDetect topic");
}

// ============================================================
// Test: Both cameras concurrently -- events arrive from both
// ============================================================
static void test_both_cameras(const std::string& hikvision_jsonl,
                               const std::string& dahua_jsonl) {
  HikvisionCompatibleEmulator emu_hik(hikvision_jsonl);
  DahuaSD4A425DBEmulator      emu_dah(dahua_jsonl);
  emu_hik.start();
  emu_dah.start();

  onvif::CameraConfig cfg_hik;
  cfg_hik.ip                 = emu_hik.local_address();
  cfg_hik.user               = "admin";
  cfg_hik.password           = "eW6iO01l";
  cfg_hik.retry_interval_sec = 1;

  onvif::CameraConfig cfg_dah;
  cfg_dah.ip                 = emu_dah.local_address();
  cfg_dah.user               = "danielwoz";
  cfg_dah.password           = "eW6iO01l";
  cfg_dah.retry_interval_sec = 1;

  // Hikvision delivers immediately; Dahua needs to work through its recorded
  // 400-error responses (retry_interval_sec=1).
  // Stop as soon as we have >=3 events from Hikvision AND >=1 from Dahua.
  const std::string addr_hik = emu_hik.local_address();
  const std::string addr_dah = emu_dah.local_address();

  auto r = collect_until(
    {cfg_hik, cfg_dah},
    [&](const std::vector<onvif::OnvifEvent>& evs) {
      int n_hik = 0, n_dah = 0;
      for (const auto& e : evs) {
        if (e.camera_ip == addr_hik) ++n_hik;
        if (e.camera_ip == addr_dah) ++n_dah;
      }
      return n_hik >= 3 && n_dah >= 1;
    },
    std::chrono::seconds(60));

  CHECK(!r.timed_out, "timed out waiting for events from both cameras");

  int from_hik = 0, from_dah = 0;
  for (const auto& ev : r.events) {
    if (ev.camera_ip == addr_hik) ++from_hik;
    if (ev.camera_ip == addr_dah) ++from_dah;
  }
  CHECK(from_hik >= 3,
        "expected >= 3 events from Hikvision-compatible, got: " +
        std::to_string(from_hik));
  CHECK(from_dah >= 1,
        "expected >= 1 events from Dahua DH-SD4A425DB-HNY, got: " +
        std::to_string(from_dah));
}

// ============================================================
// Test: hot-add cameras while run() is already executing.  One camera
// is registered before run() and two are hot-added, so the listener's
// camera list grows (and reallocates) under running workers; every
// camera must keep delivering events.
// ============================================================
static void test_hot_add(const std::string& jsonl) {
  constexpr int kCams = 3;
  std::vector<std::unique_ptr<HikvisionCompatibleEmulator>> emus;
  std::vector<onvif::CameraConfig> cfgs;
  for (int i = 0; i < kCams; ++i) {
    emus.push_back(std::make_unique<HikvisionCompatibleEmulator>(jsonl));
    emus.back()->start();
    onvif::CameraConfig cfg;
    cfg.ip                 = emus.back()->local_address();
    cfg.user               = "admin";
    cfg.password           = "eW6iO01l";
    cfg.retry_interval_sec = 1;
    cfgs.push_back(cfg);
  }

  onvif::OnvifListener listener;
  listener.add_camera(cfgs[0]);

  std::mutex mu;
  std::condition_variable cv;
  std::map<std::string, int> per_cam;
  auto all_seen = [&] {
    for (const auto& c : cfgs)
      if (per_cam[c.ip] < 3) return false;
    return true;
  };

  std::thread t([&] {
    listener.run([&](const onvif::OnvifEvent& ev) {
      std::lock_guard<std::mutex> lk(mu);
      ++per_cam[ev.camera_ip];
      if (all_seen()) cv.notify_one();
    });
  });

  // Give run() a beat to start the first worker before hot-adding.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  for (int i = 1; i < kCams; ++i) listener.add_camera_live(cfgs[i]);

  bool ok;
  {
    std::unique_lock<std::mutex> lk(mu);
    ok = cv.wait_for(lk, std::chrono::seconds(30), all_seen);
  }

  listener.stop();
  t.join();

  std::string counts;
  for (const auto& c : cfgs)
    counts += " " + c.ip + "=" + std::to_string(per_cam[c.ip]);
  CHECK(ok, "not every camera produced >= 3 events:" + counts);
}

// ============================================================
// Test: Dahua-family IVS camera -- line crossing carries the ONVIF
// ClassTypes object class, and the firmware-mangled CellMotionDetector
// topic still arrives intact for the recorder to normalise.
// ============================================================
static void test_dahua_ivs_line_crossing(const std::string& jsonl) {
  ReplayCameraEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "password";
  cfg.retry_interval_sec = 1;

  // Wait until a line-crossing notification shows up rather than a bare
  // event count -- the capture front-loads housekeeping topics.
  auto r = collect_until({cfg},
    [](const std::vector<onvif::OnvifEvent>& evs) {
      for (const auto& e : evs)
        if (e.topic == "tns1:RuleEngine/LineDetector/Crossed") return true;
      return false;
    }, std::chrono::seconds(30));

  CHECK(!r.timed_out, "timed out waiting for a LineDetector/Crossed event");

  bool saw_cross = false, saw_class = false, saw_mangled_cellmotion = false;
  for (const auto& ev : r.events) {
    if (ev.topic == "tns1:RuleEngine/LineDetector/Crossed") {
      saw_cross = true;
      // ObjectId is the one field ONVIF guarantees here.
      CHECK(ev.data.count("ObjectId") > 0,
            "line crossing should carry ObjectId");
      auto ct = ev.data.find("ClassTypes");
      if (ct != ev.data.end() && !ct->second.empty()) {
        saw_class = true;
        CHECK(ct->second == "Human" || ct->second == "Vehicle" ||
              ct->second == "Animal",
              "unexpected ClassTypes value: " + ct->second);
      }
    }
    if (ev.topic == "tns1:RuleEngine/tt:CellMotionDetector") {
      saw_mangled_cellmotion = true;
      CHECK(ev.data.count("IsMotion") > 0,
            "mangled CellMotionDetector topic should still carry IsMotion");
    }
  }
  CHECK(saw_cross, "expected at least one LineDetector/Crossed event");
  CHECK(saw_class,
        "expected at least one crossing carrying a ClassTypes object class");
  // Documents the firmware quirk this fixture exists to pin; not fatal if
  // the mangled topic falls outside the replayed window.
  if (!saw_mangled_cellmotion) {
    std::cout << "    (note: mangled CellMotionDetector topic not in window)\n";
  }
}

// ============================================================
// Test: Reolink AI doorbell -- the whole MyRuleDetector family arrives,
// covering all four smart-detect classes Protect models.
// ============================================================
static void test_reolink_ai_doorbell_classes(const std::string& jsonl) {
  ReplayCameraEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "password";
  cfg.retry_interval_sec = 1;

  auto r = collect({cfg}, 8, std::chrono::seconds(30));
  CHECK(!r.timed_out, "timed out waiting for doorbell events");

  std::set<std::string> topics;
  for (const auto& ev : r.events) {
    if (!ev.topic.empty()) topics.insert(ev.topic);
    // Every MyRuleDetector member shares the Source/State shape; that
    // uniformity is what lets one handler pattern cover all of them.
    if (ev.topic.rfind("tns1:RuleEngine/MyRuleDetector/", 0) == 0) {
      CHECK(ev.data.count("State") > 0,
            "MyRuleDetector topic should carry State: " + ev.topic);
    }
  }

  // The capture is what makes this fixture worth keeping: a single
  // device exercising person, vehicle, animal and package.
  const char* want[] = {
    "tns1:RuleEngine/MyRuleDetector/PeopleDetect",
    "tns1:RuleEngine/MyRuleDetector/VehicleDetect",
    "tns1:RuleEngine/MyRuleDetector/FaceDetect",
    "tns1:RuleEngine/MyRuleDetector/DogCatDetect",
    "tns1:RuleEngine/MyRuleDetector/Visitor",
    "tns1:RuleEngine/MyRuleDetector/Package",
  };
  int found = 0;
  for (const char* w : want) if (topics.count(w)) ++found;
  CHECK(found >= 4,
        "expected >= 4 of the 6 MyRuleDetector topics, got " +
            std::to_string(found));
}

// ============================================================
// Event sharing (CameraConfig::event_proxy)
// ============================================================

// Local name of the element whose start tag begins at xml[lt] ('<'), or
// empty for end tags, comments and processing instructions.
static std::string start_tag_local_name(const std::string& xml, size_t lt) {
  if (lt + 1 >= xml.size()) return {};
  const char c = xml[lt + 1];
  if (c == '/' || c == '?' || c == '!') return {};
  size_t end = lt + 1;
  while (end < xml.size() && xml[end] != '>' && xml[end] != '/' &&
         !std::isspace(static_cast<unsigned char>(xml[end])))
    ++end;
  std::string name = xml.substr(lt + 1, end - lt - 1);
  const auto colon = name.find(':');
  return colon == std::string::npos ? name : name.substr(colon + 1);
}

static int count_elements(const std::string& xml, const std::string& local) {
  int n = 0;
  for (size_t p = xml.find('<'); p != std::string::npos;
       p = xml.find('<', p + 1))
    if (start_tag_local_name(xml, p) == local) ++n;
  return n;
}

// Trimmed text of every element with local name "Topic", in order.
static std::vector<std::string> topics_in(const std::string& xml) {
  std::vector<std::string> out;
  for (size_t p = xml.find('<'); p != std::string::npos;
       p = xml.find('<', p + 1)) {
    if (start_tag_local_name(xml, p) != "Topic") continue;
    const size_t gt = xml.find('>', p);
    if (gt == std::string::npos) break;
    const size_t next = xml.find('<', gt);
    std::string t = xml.substr(gt + 1, next - gt - 1);
    const size_t b = t.find_first_not_of(" \t\r\n");
    t = b == std::string::npos
          ? std::string()
          : t.substr(b, t.find_last_not_of(" \t\r\n") - b + 1);
    out.push_back(t);
  }
  return out;
}

struct StateRecord {
  onvif::UpstreamEvent ev;
  int                  status;
  std::string          detail;
};

// What a listener reported through its taps and its event callback.
struct ProxyLog {
  std::mutex               mu;
  std::condition_variable  cv;
  int                      taps{0};
  int                      tapped_notifications{0};
  std::vector<std::string> tapped_topics;
  std::vector<std::string> event_topics;
  bool                     event_ahead_of_tap{false};
  std::vector<StateRecord> states;
  int                      heartbeat_calls{0};
  // Calls per camera ip.
  std::map<std::string, int> taps_by_ip;
  std::map<std::string, int> states_by_ip;
  std::map<std::string, int> heartbeats_by_ip;
  std::map<std::string, int> events_by_ip;

  template <typename Pred>
  bool wait(Pred pred, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, timeout, [&] { return pred(*this); });
  }

  // Callers hold mu.
  int count(onvif::UpstreamEvent ev) const {
    int n = 0;
    for (const auto& s : states)
      if (s.ev == ev) ++n;
    return n;
  }
  const StateRecord* first(onvif::UpstreamEvent ev) const {
    for (const auto& s : states)
      if (s.ev == ev) return &s;
    return nullptr;
  }
};

// Runs an OnvifListener on its own thread with every tap recording into a
// ProxyLog.  @p on_event runs in the event callback after the log update.
class ListenerRunner {
 public:
  ListenerRunner(const std::vector<onvif::CameraConfig>& cams, ProxyLog* log,
                 const std::string& resume_path = "",
                 std::function<void()> on_event = {})
    : log_(log) {
    for (const auto& c : cams) listener_.add_camera(c);
    if (!resume_path.empty()) listener_.set_resume_path(resume_path);
    listener_.set_raw_response_tap(
        [log](const onvif::CameraConfig& cam, const std::string& xml) {
          const auto topics = topics_in(xml);
          const int n = count_elements(xml, "NotificationMessage");
          std::lock_guard<std::mutex> lk(log->mu);
          ++log->taps;
          ++log->taps_by_ip[cam.ip];
          log->tapped_notifications += n;
          log->tapped_topics.insert(log->tapped_topics.end(), topics.begin(),
                                    topics.end());
          log->cv.notify_all();
        });
    listener_.set_upstream_state_tap(
        [log](const onvif::CameraConfig& cam, onvif::UpstreamEvent ev,
              int status, const std::string& detail) {
          std::lock_guard<std::mutex> lk(log->mu);
          log->states.push_back({ev, status, detail});
          ++log->states_by_ip[cam.ip];
          log->cv.notify_all();
        });
    listener_.set_heartbeat_suffix_fn([log](const onvif::CameraConfig& cam) {
      std::lock_guard<std::mutex> lk(log->mu);
      ++log->heartbeat_calls;
      ++log->heartbeats_by_ip[cam.ip];
      log->cv.notify_all();
      return std::string();
    });
    thread_ = std::thread([this, on_event] {
      listener_.run([this, on_event](const onvif::OnvifEvent& ev) {
        {
          std::lock_guard<std::mutex> lk(log_->mu);
          log_->event_topics.push_back(ev.topic);
          ++log_->events_by_ip[ev.camera_ip];
          if (static_cast<int>(log_->event_topics.size()) >
              log_->tapped_notifications)
            log_->event_ahead_of_tap = true;
          log_->cv.notify_all();
        }
        if (on_event) on_event();
      });
    });
  }
  ~ListenerRunner() { stop(); }

  void stop() {
    if (!thread_.joinable()) return;
    listener_.stop();
    thread_.join();
  }
  onvif::OnvifListener& listener() { return listener_; }

 private:
  ProxyLog*            log_;
  onvif::OnvifListener listener_;
  std::thread          thread_;
};

// Poll @p cond every 20 ms until it holds or @p timeout passes.
static bool wait_for_cond(const std::function<bool()>& cond,
                          std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return cond();
}

static onvif::CameraConfig shared_cfg(const std::string& ip) {
  onvif::CameraConfig cfg;
  cfg.ip                 = ip;
  cfg.user               = "admin";
  cfg.password           = "password";
  cfg.retry_interval_sec = 1;
  cfg.event_proxy        = true;
  return cfg;
}

static bool file_exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

static std::string read_file(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static void write_file(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::trunc);
  f << data;
}

static int64_t unix_ms_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

// Every response reaches the raw tap before its events reach the callback,
// and the callback sees the events in the order the camera sent them.
static void test_proxy_tap_before_callback(const std::string& jsonl) {
  HikvisionCompatibleEmulator emu(jsonl);
  emu.start();

  ProxyLog log;
  ListenerRunner run({shared_cfg(emu.local_address())}, &log);
  const bool ok = log.wait(
      [](ProxyLog& l) { return l.event_topics.size() >= 10; },
      std::chrono::seconds(20));
  run.stop();

  std::lock_guard<std::mutex> lk(log.mu);
  CHECK(ok, "timed out waiting for 10 events");
  CHECK(!log.event_ahead_of_tap,
        "an event reached the callback before its response reached the tap");
  CHECK(log.event_topics == log.tapped_topics,
        "callback topics must match the tapped responses, in order (" +
            std::to_string(log.event_topics.size()) + " vs " +
            std::to_string(log.tapped_topics.size()) + ")");
  CHECK(!log.states.empty() &&
            log.states.front().ev == onvif::UpstreamEvent::kSubscribed,
        "first state event must be kSubscribed");
  CHECK(log.count(onvif::UpstreamEvent::kPullOk) >= log.taps,
        "every tapped response must be reported as kPullOk");
  CHECK(!log.states.empty() &&
            log.states.back().ev == onvif::UpstreamEvent::kUnsubscribed,
        "last state event must be kUnsubscribed");
}

// A camera that is not shared never reaches the taps or the heartbeat
// suffix, while a shared camera in the same listener reaches all three.
static void test_proxy_taps_silent_when_not_shared(const std::string& jsonl) {
  HikvisionCompatibleEmulator plain_emu(jsonl);
  HikvisionCompatibleEmulator shared_emu(jsonl);
  plain_emu.start();
  shared_emu.start();

  onvif::CameraConfig plain = shared_cfg(plain_emu.local_address());
  plain.event_proxy            = false;
  plain.heartbeat_interval_sec = 1;
  onvif::CameraConfig shared = shared_cfg(shared_emu.local_address());
  shared.heartbeat_interval_sec = 1;
  ProxyLog log;
  ListenerRunner run({plain, shared}, &log);
  // Both cameras have pulled past their first heartbeat once the plain
  // camera has events and the shared one has asked for its suffix twice.
  const bool ok = log.wait([&](ProxyLog& l) {
    return l.events_by_ip[plain.ip] >= 5 && l.heartbeats_by_ip[shared.ip] >= 2;
  }, std::chrono::seconds(20));
  run.stop();

  std::lock_guard<std::mutex> lk(log.mu);
  CHECK(ok, "timed out waiting for events and heartbeats");
  CHECK(log.taps_by_ip.count(plain.ip) == 0,
        "raw tap must not fire for a camera that is not shared");
  CHECK(log.states_by_ip.count(plain.ip) == 0,
        "state tap must not fire for a camera that is not shared");
  CHECK(log.heartbeats_by_ip.count(plain.ip) == 0,
        "heartbeat suffix must not be asked for a camera that is not shared");
  CHECK(log.taps_by_ip[shared.ip] > 0 && log.states_by_ip[shared.ip] > 0,
        "both taps must fire for the shared camera");
}

// kCreateFailed carries the HTTP status and what went wrong.
static void test_proxy_create_failed_details(const std::string& dahua_jsonl,
                                             const std::string& html_jsonl) {
  {
    SlotLimitedCameraEmulator emu(dahua_jsonl, 0);
    emu.start();
    ProxyLog log;
    ListenerRunner run({shared_cfg(emu.local_address())}, &log);
    const bool ok = log.wait([](ProxyLog& l) {
      return l.count(onvif::UpstreamEvent::kCreateFailed) >= 1;
    }, std::chrono::seconds(10));
    run.stop();

    std::lock_guard<std::mutex> lk(log.mu);
    CHECK(ok, "slot fault: no kCreateFailed");
    const StateRecord* s = log.first(onvif::UpstreamEvent::kCreateFailed);
    CHECK(s && s->status == 400,
          "slot fault: status must be 400, got " +
              std::to_string(s ? s->status : -1));
    CHECK(s && s->detail.find("BlockedThreadUpperLimit") != std::string::npos,
          "slot fault: detail must name the fault, got: " +
              (s ? s->detail : std::string()));
    CHECK(log.count(onvif::UpstreamEvent::kSubscribed) == 0,
          "slot fault: must not report kSubscribed");
  }
  {
    Html404CameraEmulator emu(html_jsonl);
    emu.start();
    ProxyLog log;
    ListenerRunner run({shared_cfg(emu.local_address())}, &log);
    const bool ok = log.wait([](ProxyLog& l) {
      return l.count(onvif::UpstreamEvent::kCreateFailed) >= 1;
    }, std::chrono::seconds(10));
    run.stop();

    std::lock_guard<std::mutex> lk(log.mu);
    CHECK(ok, "HTML reply: no kCreateFailed");
    const StateRecord* s = log.first(onvif::UpstreamEvent::kCreateFailed);
    CHECK(s && s->status == 200 && s->detail == "no SubscriptionReference",
          "HTML reply: expected (200, no SubscriptionReference), got (" +
              std::to_string(s ? s->status : -1) + ", " +
              (s ? s->detail : std::string()) + ")");
  }
  {
    // Nothing listens on the port once the emulator has stopped.
    std::string addr;
    {
      SlotLimitedCameraEmulator emu(dahua_jsonl, 0);
      emu.start();
      addr = emu.local_address();
    }
    ProxyLog log;
    ListenerRunner run({shared_cfg(addr)}, &log);
    const bool ok = log.wait([](ProxyLog& l) {
      return l.count(onvif::UpstreamEvent::kCreateFailed) >= 1;
    }, std::chrono::seconds(10));
    run.stop();

    std::lock_guard<std::mutex> lk(log.mu);
    CHECK(ok, "transport error: no kCreateFailed");
    const StateRecord* s = log.first(onvif::UpstreamEvent::kCreateFailed);
    CHECK(s && s->status == 0 && !s->detail.empty(),
          "transport error: expected status 0 and a message");
  }
}

// A failed Renew starts a new subscription for a shared camera only; other
// cameras keep pulling on the old one as before.
static void test_renew_failure_resubscribes_shared_only(
    const std::string& jsonl) {
  for (const bool shared : {true, false}) {
    const std::string tag = shared ? "shared: " : "not shared: ";
    SlotLimitedCameraEmulator emu(jsonl, 1);
    emu.start();
    emu.set_fail_renews(true);

    onvif::CameraConfig cfg = shared_cfg(emu.local_address());
    cfg.event_proxy        = shared;
    cfg.renew_interval_sec = 1;
    ProxyLog log;
    ListenerRunner run({cfg}, &log);
    const bool renewed = wait_for_cond([&] { return emu.renews_total() >= 2; },
                                       std::chrono::seconds(10));
    const int pulls_before = emu.pulls_total();
    wait_for_cond([&] { return emu.pulls_total() > pulls_before; },
                  std::chrono::seconds(3));
    const int pulls_after = emu.pulls_total();
    run.stop();

    CHECK(renewed, tag + "timed out waiting for two renews");
    if (shared) {
      CHECK(emu.creates_total() >= 2,
            tag + "a failed renew must lead to a new Create, creates=" +
                std::to_string(emu.creates_total()));
      CHECK(emu.creates_refused() == 0,
            tag + "the old subscription must be released before the new Create");
      std::lock_guard<std::mutex> lk(log.mu);
      const StateRecord* s = log.first(onvif::UpstreamEvent::kRenewFailed);
      CHECK(s && s->status == 400 &&
                s->detail.find("renew refused") != std::string::npos,
            tag + "kRenewFailed must carry the status and the fault");
    } else {
      CHECK(emu.creates_total() == 1,
            tag + "a failed renew must not resubscribe, creates=" +
                std::to_string(emu.creates_total()));
      CHECK(pulls_after > pulls_before,
            tag + "pulling must go on after a failed renew");
    }
  }
}

// nudge() ends a long backoff at once.
static void test_nudge_wakes_backoff(const std::string& jsonl) {
  SlotLimitedCameraEmulator emu(jsonl, 0);
  emu.start();

  onvif::CameraConfig cfg = shared_cfg(emu.local_address());
  cfg.max_consecutive_failures = 1;
  cfg.retry_backoff_base_sec   = 300;
  cfg.retry_backoff_max_sec    = 300;
  ProxyLog log;
  ListenerRunner run({cfg}, &log);

  const bool first = wait_for_cond([&] { return emu.creates_total() >= 1; },
                                   std::chrono::seconds(10));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  CHECK(first && emu.creates_total() == 1,
        "expected one Create, then the 300 s pause");

  run.listener().nudge("192.0.2.1");  // unknown camera: ignored
  const auto t0 = std::chrono::steady_clock::now();
  run.listener().nudge(cfg.ip);
  const bool again = wait_for_cond([&] { return emu.creates_total() >= 2; },
                                   std::chrono::seconds(5));
  const auto woke_after = std::chrono::steady_clock::now() - t0;
  CHECK(again && woke_after < std::chrono::milliseconds(1500),
        "nudge must end the backoff within ~1 s, took " +
            std::to_string(std::chrono::duration_cast<
                std::chrono::milliseconds>(woke_after).count()) + " ms");

  const auto s0 = std::chrono::steady_clock::now();
  run.stop();
  CHECK(std::chrono::steady_clock::now() - s0 < std::chrono::seconds(2),
        "stop() must end a 300 s backoff promptly");
}

// resubscribe() replaces the subscription exactly once.
static void test_resubscribe_creates_once(const std::string& jsonl) {
  SlotLimitedCameraEmulator emu(jsonl, 1);
  emu.start();

  onvif::CameraConfig cfg = shared_cfg(emu.local_address());
  ProxyLog log;
  ListenerRunner run({cfg}, &log);
  const bool subscribed = log.wait([](ProxyLog& l) {
    return l.count(onvif::UpstreamEvent::kSubscribed) >= 1;
  }, std::chrono::seconds(10));
  CHECK(subscribed && emu.creates_total() == 1, "expected one subscription");

  run.listener().resubscribe(cfg.ip);
  const bool again = log.wait([](ProxyLog& l) {
    return l.count(onvif::UpstreamEvent::kSubscribed) >= 2;
  }, std::chrono::seconds(5));
  std::this_thread::sleep_for(std::chrono::seconds(1));
  CHECK(again, "resubscribe() must lead to a new subscription");
  CHECK(emu.creates_total() == 2,
        "resubscribe() must cause exactly one new Create, creates=" +
            std::to_string(emu.creates_total()));
  CHECK(emu.unsubscribes_total() == 1 && emu.creates_refused() == 0,
        "the old subscription must be released before the new Create");
  CHECK(emu.live_subscriptions() == 1, "one live subscription expected");
  run.stop();
  CHECK(emu.live_subscriptions() == 0,
        "stop() without a resume path must release the subscription");
}

// A slow callback never holds up the pull loop of a shared camera.
static void test_async_delivery(const std::string& jsonl) {
  HikvisionCompatibleEmulator emu(jsonl);
  emu.start();

  ProxyLog log;
  std::atomic<bool> blocked{false};
  std::atomic<bool> advanced{false};
  ListenerRunner run({shared_cfg(emu.local_address())}, &log, "", [&] {
    if (blocked.exchange(true)) return;
    // Hold the first callback until three more responses have come in.
    int taps0;
    {
      std::lock_guard<std::mutex> lk(log.mu);
      taps0 = log.taps;
    }
    advanced = log.wait([taps0](ProxyLog& l) { return l.taps >= taps0 + 3; },
                        std::chrono::seconds(5));
  });
  const bool ok = log.wait(
      [](ProxyLog& l) { return l.event_topics.size() >= 10; },
      std::chrono::seconds(20));
  run.stop();

  std::lock_guard<std::mutex> lk(log.mu);
  CHECK(ok, "timed out waiting for events");
  CHECK(advanced, "pulls must go on while the callback is blocked");
  CHECK(log.event_topics == log.tapped_topics,
        "events must be delivered in order and all be delivered at stop");
}

// A shared camera's subscription survives a restart of the listener.
static void test_resume_across_restart(const std::string& jsonl,
                                       const std::string& dir) {
  const std::string path = dir + "/resume.json";
  std::remove(path.c_str());
  SlotLimitedCameraEmulator emu(jsonl, 1);
  emu.start();

  onvif::CameraConfig cfg = shared_cfg(emu.local_address());
  cfg.mac = "F00000FDAED9";
  {
    ProxyLog log;
    ListenerRunner a({cfg}, &log, path);
    const bool ok = log.wait([](ProxyLog& l) {
      return l.count(onvif::UpstreamEvent::kSubscribed) >= 1 && l.taps >= 1;
    }, std::chrono::seconds(10));
    a.stop();
    CHECK(ok, "listener A did not subscribe");
  }
  struct stat st;
  CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
        "listener A must save its subscription with mode 0600");
  CHECK(read_file(path).find("/onvif/Events/pullSubmanager/1") !=
            std::string::npos,
        "saved file must name the subscription");
  CHECK(emu.unsubscribes_total() == 0,
        "listener A must not unsubscribe a kept subscription");
  CHECK(emu.live_subscriptions() == 1, "the kept subscription must be live");
  CHECK(emu.sync_points_total() == 0,
        "a new subscription must not send SetSynchronizationPoint");

  {
    onvif::CameraConfig cfg_b = cfg;
    cfg_b.mac = "f0:00:00:fd:ae:d9";
    cfg_b.renew_interval_sec = 1;
    const int renews_before = emu.renews_total();
    ProxyLog log;
    ListenerRunner b({cfg_b}, &log, path);
    const bool ok = log.wait([](ProxyLog& l) {
      return l.count(onvif::UpstreamEvent::kResumed) >= 1 &&
             l.tapped_notifications >= 1;
    }, std::chrono::seconds(10));
    CHECK(ok, "listener B did not resume and pull");
    CHECK(!file_exists(path), "the file must be deleted once loaded");
    // The resume renew, then two renews of the resumed subscription.
    const bool renewed = wait_for_cond(
        [&] { return emu.renews_total() >= renews_before + 3; },
        std::chrono::seconds(10));
    b.stop();
    CHECK(renewed, "listener B must keep renewing the resumed subscription");
    CHECK(emu.sync_points_total() == 1,
          "a resumed subscription must get exactly one SetSynchronizationPoint, "
          "got " + std::to_string(emu.sync_points_total()));
    std::lock_guard<std::mutex> lk(log.mu);
    CHECK(log.count(onvif::UpstreamEvent::kSubscribed) == 0,
          "listener B must not report kSubscribed");
  }
  CHECK(emu.creates_total() == 1, "listener B must resume without a Create");
  CHECK(emu.unsubscribes_total() == 0,
        "listener B must keep the subscription too");
  CHECK(file_exists(path), "listener B must save the subscription again");

  absl::Status st_rel =
      onvif::OnvifListener::release_saved_subscriptions(path, {cfg});
  CHECK(st_rel.ok(), "release_saved_subscriptions: " +
                         std::string(st_rel.message()));
  CHECK(emu.unsubscribes_total() == 1 && emu.live_subscriptions() == 0,
        "release_saved_subscriptions must unsubscribe");
  CHECK(!file_exists(path), "release_saved_subscriptions must delete the file");
  CHECK(onvif::OnvifListener::release_saved_subscriptions(path, {cfg}).ok(),
        "a missing file is OK");

  write_file(path, "not json");
  CHECK(!onvif::OnvifListener::release_saved_subscriptions(path, {cfg}).ok() &&
            !file_exists(path),
        "an unreadable file is reported and deleted");
}

// Saved subscriptions that cannot be taken over fall back to a Create.
static void test_resume_fallbacks(const std::string& jsonl,
                                  const std::string& dir) {
  const std::string path = dir + "/resume_fallbacks.json";
  std::remove(path.c_str());
  SlotLimitedCameraEmulator emu(jsonl, 1);
  emu.start();

  onvif::CameraConfig cfg = shared_cfg(emu.local_address());
  cfg.mac = "F00000FDAED9";
  auto subscribe_and_stop = [&](const onvif::CameraConfig& c, ProxyLog* log) {
    ListenerRunner run({c}, log, path);
    const bool ok = log->wait([](ProxyLog& l) {
      return l.count(onvif::UpstreamEvent::kSubscribed) >= 1;
    }, std::chrono::seconds(10));
    run.stop();
    return ok;
  };

  {
    ProxyLog log;
    CHECK(subscribe_and_stop(cfg, &log), "first listener did not subscribe");
  }
  CHECK(file_exists(path) && emu.live_subscriptions() == 1,
        "first listener must keep its subscription");

  // Expires in 5 s: released, then a fresh Create.
  {
    const std::string text = std::regex_replace(
        read_file(path), std::regex("\"expires_ms\":[0-9]+"),
        "\"expires_ms\":" + std::to_string(unix_ms_now() + 5000));
    write_file(path, text);
    ProxyLog log;
    CHECK(subscribe_and_stop(cfg, &log),
          "expiring entry: no fresh subscription");
    std::lock_guard<std::mutex> lk(log.mu);
    CHECK(log.count(onvif::UpstreamEvent::kResumed) == 0,
          "expiring entry must not be resumed");
    CHECK(emu.unsubscribes_total() == 1 && emu.creates_total() == 2 &&
              emu.creates_refused() == 0,
          "expiring entry must be released before the new Create");
  }

  // Unknown to the camera: the renew fails, so a fresh Create follows.
  CHECK(onvif::OnvifListener::release_saved_subscriptions(path, {cfg}).ok(),
        "release before the unknown-entry case");
  {
    const std::string url = "http://" + cfg.ip +
                            "/onvif/Events/pullSubmanager/999";
    write_file(path, "{\"subscriptions\":[{\"mac\":\"F00000FDAED9\","
                     "\"ip\":\"" + cfg.ip + "\",\"url\":\"" + url + "\","
                     "\"ref_params\":\"\",\"alarm_url\":\"\",\"expires_ms\":" +
                     std::to_string(unix_ms_now() + 60000) + "}]}");
    ProxyLog log;
    CHECK(subscribe_and_stop(cfg, &log), "unknown entry: no fresh subscription");
    std::lock_guard<std::mutex> lk(log.mu);
    CHECK(log.count(onvif::UpstreamEvent::kResumed) == 0,
          "unknown entry must not be resumed");
    CHECK(emu.creates_total() == 3 && emu.creates_refused() == 0,
          "unknown entry must lead to one fresh Create");
    CHECK(emu.sync_points_total() == 0,
          "SetSynchronizationPoint only after a resume");
  }

  // A camera that is no longer shared releases the saved subscription and
  // keeps none at its own stop.
  {
    CHECK(file_exists(path), "kept subscription expected before the last case");
    const int unsubs_before = emu.unsubscribes_total();
    onvif::CameraConfig plain = cfg;
    plain.event_proxy = false;
    ProxyLog log;
    ListenerRunner run({plain}, &log, path);
    const bool ok = wait_for_cond([&] { return emu.creates_total() >= 4; },
                                  std::chrono::seconds(10));
    run.stop();
    CHECK(ok, "camera that is not shared did not subscribe");
    CHECK(emu.creates_refused() == 0,
          "saved subscription must be released before the new Create");
    CHECK(emu.unsubscribes_total() == unsubs_before + 2 &&
              emu.live_subscriptions() == 0,
          "camera that is not shared must release both subscriptions");
    CHECK(!file_exists(path), "nothing kept, so no file");
  }

  // Without a MAC on either side the entry is matched by ip.
  {
    onvif::CameraConfig no_mac = cfg;
    no_mac.mac.clear();
    {
      ProxyLog log;
      CHECK(subscribe_and_stop(no_mac, &log), "no MAC: did not subscribe");
    }
    const int creates_before = emu.creates_total();
    ProxyLog log;
    ListenerRunner run({no_mac}, &log, path);
    const bool ok = log.wait([](ProxyLog& l) {
      return l.count(onvif::UpstreamEvent::kResumed) >= 1;
    }, std::chrono::seconds(10));
    run.stop();
    CHECK(ok && emu.creates_total() == creates_before,
          "no MAC: the entry must be resumed by ip without a Create");
    CHECK(emu.sync_points_total() == 1,
          "no MAC: the resumed subscription gets one SetSynchronizationPoint");
  }
  CHECK(onvif::OnvifListener::release_saved_subscriptions(path, {cfg}).ok() &&
            emu.live_subscriptions() == 0,
        "release after the ip case");
}

// A kept subscription is released when no camera takes it at the next start
// (its camera was excluded or removed meanwhile), or when it cannot be
// saved, so the camera's only slot is free again at once.
static void test_resume_unclaimed_released(const std::string& jsonl,
                                           const std::string& dir) {
  const std::string path = dir + "/resume_unclaimed.json";
  std::remove(path.c_str());
  SlotLimitedCameraEmulator emu(jsonl, 1);
  emu.start();

  onvif::CameraConfig cfg = shared_cfg(emu.local_address());
  cfg.mac = "F00000FDAED9";
  {
    ProxyLog log;
    ListenerRunner a({cfg}, &log, path);
    const bool ok = log.wait([](ProxyLog& l) {
      return l.count(onvif::UpstreamEvent::kSubscribed) >= 1;
    }, std::chrono::seconds(10));
    a.stop();
    CHECK(ok, "listener A did not subscribe");
  }
  CHECK(file_exists(path) && emu.live_subscriptions() == 1,
        "listener A must keep its subscription");

  // The camera is no longer one of the listener's cameras.
  {
    onvif::OnvifListener b;
    b.set_resume_path(path, {cfg});
    std::thread t([&] { b.run([](const onvif::OnvifEvent&) {}); });
    const bool released = wait_for_cond(
        [&] { return emu.unsubscribes_total() == 1; },
        std::chrono::seconds(10));
    b.stop();
    t.join();
    CHECK(released && emu.live_subscriptions() == 0,
          "a saved subscription no camera takes must be released at start");
    CHECK(!file_exists(path), "nothing kept, so no file");
  }

  // The state directory is gone by the time the kept subscription is saved.
  {
    const std::string gone_dir = dir + "/resume_gone";
    CHECK(mkdir(gone_dir.c_str(), 0700) == 0, "mkdir " + gone_dir);
    const std::string gone = gone_dir + "/resume.json";
    ProxyLog log;
    ListenerRunner c({cfg}, &log, gone);
    const bool ok = log.wait([](ProxyLog& l) {
      return l.count(onvif::UpstreamEvent::kSubscribed) >= 1;
    }, std::chrono::seconds(10));
    CHECK(rmdir(gone_dir.c_str()) == 0, "rmdir " + gone_dir);
    c.stop();
    CHECK(ok, "listener C did not subscribe");
    CHECK(emu.unsubscribes_total() == 2 && emu.live_subscriptions() == 0,
          "a kept subscription that cannot be saved must be released");
  }
}

// A per-test scratch directory under TEST_TMPDIR (or /tmp).
static std::string make_temp_dir() {
  const char* base = std::getenv("TEST_TMPDIR");
  std::string tmpl = std::string(base && *base ? base : "/tmp") +
                     "/onvif_listener_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  return mkdtemp(buf.data()) ? std::string(buf.data()) : std::string("/tmp");
}

// ============================================================
// main
// ============================================================

// Reconnect backoff.  The old scheme slept out the remainder of a 1 h
// "failure window" measured from the first failure in the streak, so three
// failures at a 10 s retry interval (~30 s) produced a ~3570 s sleep -- a
// brief blip cost an hour of blindness.  Observed on a live router as a
// nightly 57-75 min all-camera outage.  The replacement doubles from a base
// and clamps, so the worst case is bounded.
static void test_backoff_doubles_and_clamps() {
  const int base = 30, max = 300;

  // First pause uses the base regardless of how we got here.
  CHECK(onvif::next_backoff_sec(0, base, max) == base, "cold start -> base");
  CHECK(onvif::next_backoff_sec(-5, base, max) == base, "negative -> base");
  CHECK(onvif::next_backoff_sec(1, base, max) == base, "below base -> base");

  // Doubling, then a hard ceiling -- never the old hour-long sleep.
  CHECK(onvif::next_backoff_sec(30, base, max) == 60,  "30 -> 60");
  CHECK(onvif::next_backoff_sec(60, base, max) == 120, "60 -> 120");
  CHECK(onvif::next_backoff_sec(120, base, max) == 240, "120 -> 240");
  CHECK(onvif::next_backoff_sec(240, base, max) == max, "240 clamps to max");
  CHECK(onvif::next_backoff_sec(max, base, max) == max, "max is a fixed point");
  CHECK(onvif::next_backoff_sec(99999, base, max) == max, "over max clamps");

  // Degenerate config must not yield a zero-length or negative sleep.
  CHECK(onvif::next_backoff_sec(0, 0, 0) >= 1, "zero config -> >=1s");
  CHECK(onvif::next_backoff_sec(10, 60, 30) == 60, "max<base collapses to base");

  // The whole point: bounded. Walk the ladder and assert the ceiling holds.
  int b = onvif::next_backoff_sec(0, base, max);
  for (int i = 0; i < 50; ++i) {
    b = onvif::next_backoff_sec(b, base, max);
    CHECK(b <= max, "backoff never exceeds max");
    CHECK(b >= base, "backoff never drops below base");
  }
  CHECK(b == max, "ladder settles at max");
}

// A stop() that arrives before run() -- e.g. SIGTERM from the admin
// page's "Save & restart" while the service is still starting up -- must
// make run() return instead of being overwritten.
static void test_stop_before_run() {
  onvif::OnvifListener listener;
  listener.stop();
  std::atomic<bool> returned{false};
  std::thread t([&] {
    listener.run([](const onvif::OnvifEvent&) {});
    returned = true;
  });
  for (int i = 0; i < 50 && !returned; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(returned, "run() must return promptly after an earlier stop()");
  if (!returned) listener.stop();
  t.join();
}

int main(int argc, char* argv[]) {
  if (argc < 19) {
    std::cerr << "Usage: " << argv[0] << "\n"
              << "  <hikvision_compatible.jsonl>\n"
              << "  <dahua_dh_sd4a425db_hny.jsonl>\n"
              << "  <cell_motion_camera.jsonl>\n"
              << "  <cell_motion_no_events.jsonl>\n"
              << "  <thingino_wyze.jsonl>\n"
              << "  <html_404_camera.jsonl>\n"
              << "  <unvr_78.jsonl>\n"
              << "  <unvr_119.jsonl>\n"
              << "  <unvr_173.jsonl>\n"
              << "  <unvr_227.jsonl>\n"
              << "  <unvr_245.jsonl>\n"
              << "  <unvr_251.jsonl>\n"
              << "  <unvr_1_45.jsonl>\n"
              << "  <unvr_1_47.jsonl>\n"
              << "  <reolink_bullet.jsonl>\n"
              << "  <hikvision_pullpoint_notauthorized.jsonl>\n"
              << "  <dahua_ivs_linecross.jsonl>\n"
              << "  <reolink_ai_doorbell.jsonl>\n";
    return 1;
  }
  const std::string hikvision_jsonl      = argv[1];
  const std::string dahua_jsonl          = argv[2];
  const std::string cell_motion_jsonl    = argv[3];
  const std::string cell_no_events_jsonl = argv[4];
  const std::string thingino_jsonl       = argv[5];
  const std::string html_404_jsonl       = argv[6];
  const std::string unvr_78_jsonl        = argv[7];
  const std::string unvr_119_jsonl       = argv[8];
  const std::string unvr_173_jsonl       = argv[9];
  const std::string unvr_227_jsonl       = argv[10];
  const std::string unvr_245_jsonl       = argv[11];
  const std::string unvr_251_jsonl       = argv[12];
  const std::string unvr_1_45_jsonl      = argv[13];
  const std::string unvr_1_47_jsonl      = argv[14];
  const std::string reolink_jsonl        = argv[15];
  const std::string hik_notauth_jsonl    = argv[16];
  const std::string dahua_ivs_jsonl      = argv[17];
  const std::string reolink_ai_jsonl     = argv[18];

  onvif::global_init();

  run_test("stop_before_run", [] { test_stop_before_run(); });
  run_test("backoff_doubles_and_clamps",
           [&] { test_backoff_doubles_and_clamps(); });
  const std::string tmp_dir = make_temp_dir();
  run_test("proxy_tap_before_callback",
           [&] { test_proxy_tap_before_callback(hikvision_jsonl); });
  run_test("proxy_taps_silent_when_not_shared",
           [&] { test_proxy_taps_silent_when_not_shared(hikvision_jsonl); });
  run_test("proxy_create_failed_details",
           [&] { test_proxy_create_failed_details(dahua_jsonl, html_404_jsonl); });
  run_test("renew_failure_resubscribes_shared_only",
           [&] { test_renew_failure_resubscribes_shared_only(dahua_jsonl); });
  run_test("nudge_wakes_backoff",
           [&] { test_nudge_wakes_backoff(dahua_jsonl); });
  run_test("resubscribe_creates_once",
           [&] { test_resubscribe_creates_once(dahua_jsonl); });
  run_test("async_delivery",
           [&] { test_async_delivery(hikvision_jsonl); });
  run_test("resume_across_restart",
           [&] { test_resume_across_restart(dahua_jsonl, tmp_dir); });
  run_test("resume_fallbacks",
           [&] { test_resume_fallbacks(dahua_jsonl, tmp_dir); });
  run_test("resume_unclaimed_released",
           [&] { test_resume_unclaimed_released(dahua_jsonl, tmp_dir); });
  run_test("hikvision_basic",
           [&] { test_hikvision_basic(hikvision_jsonl); });
  run_test("hot_add",
           [&] { test_hot_add(hikvision_jsonl); });
  run_test("hikvision_changed_events",
           [&] { test_hikvision_changed_events(hikvision_jsonl); });
  run_test("dahua_retries",
           [&] { test_dahua_retries(dahua_jsonl); });
  run_test("dahua_topics",
           [&] { test_dahua_topics(dahua_jsonl); });
  run_test("cell_motion_basic",
           [&] { test_cell_motion_basic(cell_motion_jsonl); });
  run_test("cell_motion_events",
           [&] { test_cell_motion_events(cell_motion_jsonl); });
  run_test("cell_motion_no_events",
           [&] { test_cell_motion_no_events(cell_no_events_jsonl); });
  run_test("thingino_graceful_stop",
           [&] { test_thingino_graceful_stop(thingino_jsonl); });
  run_test("html_404_graceful_stop",
           [&] { test_html_404_graceful_stop(html_404_jsonl); });
  run_test("unvr_78",   [&] { test_unvr_cell_motion(unvr_78_jsonl); });
  run_test("unvr_119",  [&] { test_unvr_cell_motion(unvr_119_jsonl); });
  run_test("unvr_173",  [&] { test_unvr_cell_motion(unvr_173_jsonl); });
  run_test("unvr_227",  [&] { test_cell_motion_no_events(unvr_227_jsonl); });
  run_test("unvr_245",  [&] { test_unvr_cell_motion(unvr_245_jsonl); });
  run_test("unvr_251",  [&] { test_unvr_cell_motion(unvr_251_jsonl); });
  run_test("unvr_1_45", [&] { test_unvr_cell_motion(unvr_1_45_jsonl); });
  run_test("unvr_1_47", [&] { test_unvr_cell_motion(unvr_1_47_jsonl); });
  run_test("reolink_namespace",
           [&] { test_reolink_namespace(reolink_jsonl); });
  run_test("hikvision_pullpoint_notauthorized",
           [&] { test_hikvision_pullpoint_notauthorized(hik_notauth_jsonl); });
  run_test("both_cameras",
           [&] { test_both_cameras(hikvision_jsonl, dahua_jsonl); });
  run_test("axis_ref_params",
           [] { test_axis_ref_params(); });
  run_test("dahua_ivs_line_crossing",
           [&] { test_dahua_ivs_line_crossing(dahua_ivs_jsonl); });
  run_test("reolink_ai_doorbell_classes",
           [&] { test_reolink_ai_doorbell_classes(reolink_ai_jsonl); });

  onvif::global_cleanup();

  std::cout << "================================================\n"
            << "Results: " << g_pass << " checks passed, "
            << g_fail    << " checks failed\n"
            << "================================================\n";

  return g_fail > 0 ? 1 : 0;
}
