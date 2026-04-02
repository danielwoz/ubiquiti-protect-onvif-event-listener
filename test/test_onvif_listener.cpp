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

#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../onvif_listener.hpp"
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
  fn();
  bool ok = (g_fail == before);
  std::cout << (ok ? "[  OK ] " : "[FAIL ] ") << name << "\n\n";
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
// main
// ============================================================
int main(int argc, char* argv[]) {
  if (argc < 15) {
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
              << "  <unvr_1_47.jsonl>\n";
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

  onvif::global_init();

  run_test("hikvision_basic",
           [&] { test_hikvision_basic(hikvision_jsonl); });
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
  run_test("both_cameras",
           [&] { test_both_cameras(hikvision_jsonl, dahua_jsonl); });
  run_test("axis_ref_params",
           [] { test_axis_ref_params(); });

  onvif::global_cleanup();

  std::cout << "================================================\n"
            << "Results: " << g_pass << " checks passed, "
            << g_fail    << " checks failed\n"
            << "================================================\n";

  return g_fail > 0 ? 1 : 0;
}
