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

/**
 * test_detection_recorder.cpp
 *
 * End-to-end test for DetectionRecorder.
 *
 * Two SnapshotSyntheticEmulator instances stand in for real cameras.  Each
 * emulator:
 *   - serves scripted SOAP PullMessages responses (ONVIF events)
 *   - serves a real JPEG snapshot at GET /snapshot
 *
 * After the listener collects all events the following are verified via an
 * in-memory MockBackend (no PostgreSQL required):
 *
 *   events table (MockBackend::events)
 *     * 3 rows total (Human x2 + Vehicle x1)
 *     * all type = 'smartDetectZone'
 *     * all "end" IS NOT NULL (no open/orphaned rows)
 *     * smartDetectTypes: 2 x '["person"]', 1 x '["vehicle"]'
 *     * all thumbnailId IS NOT NULL
 *     * per-camera counts correct
 *
 *   smartDetectObjects table (MockBackend::sdos)
 *     * 3 rows total
 *     * 2 x type='person', 1 x type='vehicle'
 *     * all eventId references exist in events
 *
 *   smartDetectRaws table (MockBackend::sdrs)
 *     * 3 rows total
 *     * 2 x objectType="person", 1 x objectType="vehicle"
 *
 *   UBV thumbnail files
 *     * cam108 file: 2 frames (human start + vehicle start)
 *     * cam109 file: 1 frame  (human start)
 *     * every frame contains valid JPEG bytes (FF D8 magic)
 */

#include <stddef.h>   // for size_t (needed before jpeglib.h)
#include <stdio.h>    // for FILE (needed before jpeglib.h)
#include <jpeglib.h>

#include <atomic>
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
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../alarm_notifier.hpp"
#include "../detection_recorder.hpp"
#include "../onvif_listener.hpp"
#include "../ubv_thumbnail.hpp"
#include "camera_emulators.hpp"
#include "onvif_camera_emulator.hpp"

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
// SOAP building helpers
// ============================================================
static std::string make_create_response(const std::string& real_ip) {
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope"
    " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:wsa5=\"http://www.w3.org/2005/08/addressing\""
    " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\">"
    "<s:Body>"
    "<tev:CreatePullPointSubscriptionResponse>"
    "<tev:SubscriptionReference>"
    "<wsa5:Address>http://" + real_ip + "/onvif/subscription</wsa5:Address>"
    "</tev:SubscriptionReference>"
    "</tev:CreatePullPointSubscriptionResponse>"
    "</s:Body>"
    "</s:Envelope>";
}

static std::string make_empty_pull_response() {
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    "            xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\">"
    "<s:Body><tev:PullMessagesResponse/></s:Body>"
    "</s:Envelope>";
}

// Camera 108 style: tns1:RuleEngine/FieldDetector/ObjectsInside
static std::string make_field_detector_response(
  const std::string& rule,
  bool               inside,
  const std::string& utc_time) {
  const std::string val = inside ? "true" : "false";
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope"
    " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
    " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
    "<s:Body>"
    "<tev:PullMessagesResponse>"
    "<wsnt:NotificationMessage>"
    "<wsnt:Topic>tns1:RuleEngine/FieldDetector/ObjectsInside</wsnt:Topic>"
    "<wsnt:Message>"
    "<tt:Message UtcTime=\"" + utc_time + "\" PropertyOperation=\"Changed\">"
    "<tt:Source>"
    "<tt:SimpleItem Name=\"Rule\" Value=\"" + rule + "\"/>"
    "</tt:Source>"
    "<tt:Data>"
    "<tt:SimpleItem Name=\"IsInside\" Value=\"" + val + "\"/>"
    "</tt:Data>"
    "</tt:Message>"
    "</wsnt:Message>"
    "</wsnt:NotificationMessage>"
    "</tev:PullMessagesResponse>"
    "</s:Body>"
    "</s:Envelope>";
}

// Camera 108 style with ONVIF BoundingBox in the analytics event.
// Coords are in ONVIF [-1,1] space; the listener converts them to [0,1].
static std::string make_field_detector_with_bbox_response(
  const std::string& rule,
  bool               inside,
  const std::string& utc_time,
  float left_v, float top_v, float right_v, float bottom_v) {
  const std::string val = inside ? "true" : "false";
  char bbox_buf[256];
  std::snprintf(bbox_buf, sizeof(bbox_buf),
    "<tt:BoundingBox left=\"%.3f\" top=\"%.3f\" right=\"%.3f\" bottom=\"%.3f\"/>",
    left_v, top_v, right_v, bottom_v);
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope"
    " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
    " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
    "<s:Body>"
    "<tev:PullMessagesResponse>"
    "<wsnt:NotificationMessage>"
    "<wsnt:Topic>tns1:RuleEngine/FieldDetector/ObjectsInside</wsnt:Topic>"
    "<wsnt:Message>"
    "<tt:Message UtcTime=\"" + utc_time + "\" PropertyOperation=\"Changed\">"
    "<tt:Source>"
    "<tt:SimpleItem Name=\"Rule\" Value=\"" + rule + "\"/>"
    "</tt:Source>"
    "<tt:Data>"
    "<tt:SimpleItem Name=\"IsInside\" Value=\"" + val + "\"/>"
    "</tt:Data>"
    + std::string(bbox_buf) +
    "</tt:Message>"
    "</wsnt:Message>"
    "</wsnt:NotificationMessage>"
    "</tev:PullMessagesResponse>"
    "</s:Body>"
    "</s:Envelope>";
}

// CellMotionDetector style: tns1:RuleEngine/CellMotionDetector/Motion
static std::string make_cell_motion_response(bool is_motion, const std::string& utc_time) {
  const std::string val = is_motion ? "true" : "false";
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope"
    " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
    " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
    "<s:Body>"
    "<tev:PullMessagesResponse>"
    "<wsnt:NotificationMessage>"
    "<wsnt:Topic>tns1:RuleEngine/CellMotionDetector/Motion</wsnt:Topic>"
    "<wsnt:Message>"
    "<tt:Message UtcTime=\"" + utc_time + "\" PropertyOperation=\"Changed\">"
    "<tt:Source>"
    "<tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"00000\"/>"
    "<tt:SimpleItem Name=\"VideoAnalyticsConfigurationToken\" Value=\"00000\"/>"
    "<tt:SimpleItem Name=\"Rule\" Value=\"00000\"/>"
    "</tt:Source>"
    "<tt:Data>"
    "<tt:SimpleItem Name=\"IsMotion\" Value=\"" + val + "\"/>"
    "</tt:Data>"
    "</tt:Message>"
    "</wsnt:Message>"
    "</wsnt:NotificationMessage>"
    "</tev:PullMessagesResponse>"
    "</s:Body>"
    "</s:Envelope>";
}

// Fallback: tns1:VideoSource/MotionAlarm
static std::string make_motion_alarm_response(bool state, const std::string& utc_time) {
  const std::string val = state ? "true" : "false";
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope"
    " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
    " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
    "<s:Body>"
    "<tev:PullMessagesResponse>"
    "<wsnt:NotificationMessage>"
    "<wsnt:Topic>tns1:VideoSource/MotionAlarm</wsnt:Topic>"
    "<wsnt:Message>"
    "<tt:Message UtcTime=\"" + utc_time + "\" PropertyOperation=\"Changed\">"
    "<tt:Source>"
    "<tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"VideoSourceMain\"/>"
    "</tt:Source>"
    "<tt:Data>"
    "<tt:SimpleItem Name=\"State\" Value=\"" + val + "\"/>"
    "</tt:Data>"
    "</tt:Message>"
    "</wsnt:Message>"
    "</wsnt:NotificationMessage>"
    "</tev:PullMessagesResponse>"
    "</s:Body>"
    "</s:Envelope>";
}

// Camera 109 style: tns1:UserAlarm/IVA/HumanShapeDetect
static std::string make_human_shape_response(bool state, const std::string& utc_time) {
  const std::string val = state ? "true" : "false";
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope"
    " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
    " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
    "<s:Body>"
    "<tev:PullMessagesResponse>"
    "<wsnt:NotificationMessage>"
    "<wsnt:Topic>tns1:UserAlarm/IVA/HumanShapeDetect</wsnt:Topic>"
    "<wsnt:Message>"
    "<tt:Message UtcTime=\"" + utc_time + "\" PropertyOperation=\"Changed\">"
    "<tt:Source>"
    "<tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"VideoSourceMain\"/>"
    "</tt:Source>"
    "<tt:Data>"
    "<tt:SimpleItem Name=\"State\" Value=\"" + val + "\"/>"
    "</tt:Data>"
    "</tt:Message>"
    "</wsnt:Message>"
    "</wsnt:NotificationMessage>"
    "</tev:PullMessagesResponse>"
    "</s:Body>"
    "</s:Envelope>";
}

// ============================================================
// SnapshotSyntheticEmulator
//
// Serves scripted SOAP PullMessages responses AND a JPEG snapshot at
// GET /snapshot.  The snapshot URL is http://127.0.0.1:<port>/snapshot.
// ============================================================
class SnapshotSyntheticEmulator : public OnvifCameraEmulator {
 public:
  SnapshotSyntheticEmulator(const std::string&         real_ip,
                             std::vector<std::string>   pull_responses,
                             std::vector<unsigned char> snapshot_jpeg)
    : OnvifCameraEmulator(real_ip)
    , create_resp_(make_create_response(real_ip))
    , pull_responses_(std::move(pull_responses))
    , empty_pull_(make_empty_pull_response())
    , snapshot_jpeg_(std::move(snapshot_jpeg))
  {}

  /// Full URL at which this emulator serves JPEG snapshots.
  std::string snapshot_url() const {
    return "http://127.0.0.1:" + std::to_string(port()) + "/snapshot";
  }

 protected:
  std::pair<int, std::string> handle(
    const std::string& path,
    const std::string& soap_action,
    const std::string& /*body*/) override {
    // GET /snapshot -- no SOAPAction header, just return the JPEG bytes.
    if (soap_action.empty() && path == "/snapshot") {
      return {200, std::string(
        reinterpret_cast<const char*>(snapshot_jpeg_.data()),
        snapshot_jpeg_.size())};
    }

    // SOAP dispatch: extract the last segment of the action URI.
    auto   p    = soap_action.rfind('/');
    auto   tail = (p != std::string::npos) ? soap_action.substr(p + 1) : soap_action;

    if (tail == "CreatePullPointSubscriptionRequest")
      return {200, rewrite_urls(create_resp_)};

    if (tail == "PullMessagesRequest" || tail == "RenewRequest") {
      std::lock_guard<std::mutex> lk(pull_mu_);
      if (pull_idx_ < pull_responses_.size())
        return {200, rewrite_urls(pull_responses_[pull_idx_++])};
      return {200, empty_pull_};
    }

    return {400, ""};
  }

 private:
  std::string              create_resp_;
  std::vector<std::string> pull_responses_;
  std::string              empty_pull_;
  std::vector<unsigned char> snapshot_jpeg_;
  std::size_t              pull_idx_{0};
  std::mutex               pull_mu_;
};

// ============================================================
// Helpers
// ============================================================

// Load a binary file into a byte vector.
static std::vector<unsigned char> load_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    std::fprintf(stderr, "Fatal: Cannot open: %s\n", path.c_str());
    std::abort();
  }
  auto sz = static_cast<std::size_t>(f.tellg());
  f.seekg(0);
  std::vector<unsigned char> buf(sz);
  f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
  return buf;
}

// Return the directory containing this source file (compile-time path).
static std::string source_dir() {
  std::string f = __FILE__;
  auto slash = f.rfind('/');
  return (slash != std::string::npos) ? f.substr(0, slash + 1) : "./";
}

// Read JPEG dimensions from header only (no full decompress).
// Returns false on error.
static bool jpeg_dims(const std::vector<uint8_t>& data, int* w, int* h) {
  struct JpegErr {
    jpeg_error_mgr base;  // must be first
    bool           fatal{false};
  } err;
  jpeg_decompress_struct info{};
  info.err = jpeg_std_error(&err.base);
  err.base.error_exit = [](j_common_ptr c) {
    reinterpret_cast<JpegErr*>(c->err)->fatal = true;
  };
  err.base.output_message = [](j_common_ptr) {};
  jpeg_create_decompress(&info);
  jpeg_mem_src(&info,
               const_cast<unsigned char*>(
                   reinterpret_cast<const unsigned char*>(data.data())),
               static_cast<unsigned long>(data.size()));  // NOLINT(runtime/int)
  jpeg_read_header(&info, TRUE);
  *w = static_cast<int>(info.image_width);
  *h = static_cast<int>(info.image_height);
  jpeg_destroy_decompress(&info);
  return !err.fatal && *w > 0 && *h > 0;
}

// ============================================================
// MockBackend -- in-memory IDbBackend for testing.
//
// All mutating methods are called under DetectionRecorder::mu_ so no
// additional locking is needed here.
// ============================================================
struct MockBackend : onvif::DetectionRecorder::IDbBackend {
  struct EventRow {
    std::string id;
    std::string camera_ip;
    std::string sdt_json;   // e.g. '["person"]'
    std::string thumb_id;
    uint64_t    start_ms{0};
    bool        ended{false};
    uint64_t    end_ms{0};
  };
  struct SdoRow {
    std::string id;
    std::string event_id;
    std::string camera_ip;
    std::string obj_type;   // "person" or "vehicle"
  };
  struct SdrRow {
    std::string id;
    std::string camera_ip;
    std::string obj_type;
    uint64_t    ts_ms{0};
  };
  struct DetLabelRow {
    std::string      event_id;
    std::string      object_id;  // empty = NULL
    std::vector<int> lids;
  };

  std::vector<EventRow>   events;
  std::vector<SdoRow>     sdos;
  std::vector<SdrRow>     sdrs;
  std::map<std::string, int> labels;      // name -> simulated lid
  std::vector<DetLabelRow>   det_labels;

  absl::Status create_schema() override { return absl::OkStatus(); }

  void register_camera(const std::string&, const std::string&,
                       const std::string&) override {}

  std::string make_thumbnail_id(const std::string& ip, uint64_t ts_ms) override {
    return ip + "-" + std::to_string(ts_ms);
  }

  bool needs_snapshot() const override { return true; }

  void insert_event(const std::string& id, uint64_t ts_ms,
                    const std::string& camera_ip, const std::string& sdt_json,
                    const std::string& thumb_id,
                    const std::string& /*now_str*/) override {
    events.push_back({id, camera_ip, sdt_json, thumb_id, ts_ms, false, 0});
  }

  void insert_sdo(const std::string& id, const std::string& event_id,
                  const std::string& /*thumb_id*/, const std::string& camera_ip,
                  const std::string& obj_type, const std::string& /*attributes*/,
                  uint64_t /*ts_ms*/, const std::string& /*now_str*/) override {
    sdos.push_back({id, event_id, camera_ip, obj_type});
  }

  void update_event_end(const std::string& event_id, uint64_t end_ms,
                        const std::string& /*now_str*/) override {
    for (auto& e : events)
      if (e.id == event_id) {
        e.ended = true; e.end_ms = end_ms; break;
      }
  }

  void write_thumbnail(const std::string& /*thumb_id*/,
                       const std::string& /*event_id*/,
                       const std::string& /*camera_ip*/,
                       uint64_t           /*ts_ms*/,
                       const std::string& /*now_str*/,
                       const std::vector<unsigned char>& /*jpeg*/) override {}

  void insert_smart_detect_raw(const std::string& id,
                               const std::string& camera_ip,
                               uint64_t ts_ms,
                               const std::string& obj_type,
                               const std::string& /*now_str*/) override {
    sdrs.push_back({id, camera_ip, obj_type, ts_ms});
  }

  std::vector<int> upsert_labels(
      const std::vector<std::string>& names,
      const std::string& /*now_str*/) override {
    std::vector<int> lids;
    for (const auto& name : names) {
      auto it = labels.find(name);
      if (it == labels.end()) {
        int lid = static_cast<int>(labels.size()) + 1;
        labels[name] = lid;
        lids.push_back(lid);
      } else {
        lids.push_back(it->second);
      }
    }
    return lids;
  }

  void insert_detection_label(const std::string&      event_id,
                              const std::string&      object_id,
                              const std::vector<int>& lids,
                              const std::string& /*now_str*/) override {
    det_labels.push_back({event_id, object_id, lids});
  }
};

// ============================================================
// Shared emulator / listener helper
// ============================================================

struct TestContext {
  std::string                      ubv_dir;
  // Optional: set before run_standard_script() to inject MACs into CameraConfigs
  // so AlarmNotifier can include them in UOS POST scope fields.
  std::string                      mac108;
  std::string                      mac109;
  onvif::CameraConfig              cfg108;
  onvif::CameraConfig              cfg109;
  std::unique_ptr<SnapshotSyntheticEmulator> emu108;
  std::unique_ptr<SnapshotSyntheticEmulator> emu109;
};

// Build and run emulators + listener for a standard 3-detection script.
// Returns after all 6 events (4 from cam108, 2 from cam109) have been seen.
// The recorder is expected to be fully configured by the caller before calling this.
static bool run_standard_script(TestContext& ctx,
                                onvif::DetectionRecorder& recorder) {
  const std::string dir = source_dir();

  std::vector<unsigned char> jpeg108 = load_file(dir + "testdata/snapshot_108.jpg");
  std::vector<unsigned char> jpeg109 = load_file(dir + "testdata/snapshot_109.jpg");

  std::vector<std::string> pulls_108 = {
    make_field_detector_response("Human",   true,  "2026-02-19T10:00:00Z"),
    make_field_detector_response("Vehicle", true,  "2026-02-19T10:00:01Z"),
    make_field_detector_response("Human",   false, "2026-02-19T10:00:10Z"),
    make_field_detector_response("Vehicle", false, "2026-02-19T10:00:11Z"),
  };
  std::vector<std::string> pulls_109 = {
    make_human_shape_response(true,  "2026-02-19T10:00:02Z"),
    make_human_shape_response(false, "2026-02-19T10:00:12Z"),
  };

  ctx.emu108 = std::make_unique<SnapshotSyntheticEmulator>(
    "192.168.1.108", std::move(pulls_108), jpeg108);
  ctx.emu109 = std::make_unique<SnapshotSyntheticEmulator>(
    "192.168.1.109", std::move(pulls_109), jpeg109);
  ctx.emu108->start();
  ctx.emu109->start();

  ctx.cfg108.ip                 = ctx.emu108->local_address();
  ctx.cfg108.user               = "admin";
  ctx.cfg108.password           = "test";
  ctx.cfg108.snapshot_url       = ctx.emu108->snapshot_url();
  ctx.cfg108.retry_interval_sec = 1;
  if (!ctx.mac108.empty()) ctx.cfg108.mac = ctx.mac108;

  ctx.cfg109.ip                 = ctx.emu109->local_address();
  ctx.cfg109.user               = "user";
  ctx.cfg109.password           = "test";
  ctx.cfg109.snapshot_url       = ctx.emu109->snapshot_url();
  ctx.cfg109.retry_interval_sec = 1;
  if (!ctx.mac109.empty()) ctx.cfg109.mac = ctx.mac109;

  recorder.set_ubv_dir(ctx.ubv_dir);
  recorder.set_snapshot(ctx.cfg108);
  recorder.set_snapshot(ctx.cfg109);

  std::mutex              mu;
  std::condition_variable cv;
  std::atomic<int>        events_seen{0};
  const int               needed = 6;

  onvif::OnvifListener listener;
  listener.add_camera(ctx.cfg108);
  listener.add_camera(ctx.cfg109);

  std::thread t([&] {
    listener.run([&](const onvif::OnvifEvent& ev) {
      recorder.on_event(ev);
      if (!ev.topic.empty())
        if (++events_seen >= needed) cv.notify_one();
    });
  });

  bool timed_out;
  {
    std::unique_lock<std::mutex> lk(mu);
    timed_out = !cv.wait_for(lk, std::chrono::seconds(30),
                              [&] { return events_seen.load() >= needed; });
  }
  listener.stop();
  t.join();
  return !timed_out;
}

// ============================================================
// Tests
// ============================================================
static void test_detection_e2e(const std::string& ubv_dir) {
  TestContext ctx;
  ctx.ubv_dir = ubv_dir;

  auto backend = std::make_unique<MockBackend>();
  MockBackend* bptr = backend.get();

  auto rec_or = onvif::DetectionRecorder::CreateWithBackend(std::move(backend));
  if (!rec_or.ok()) {
    CHECK(false, std::string("DetectionRecorder::CreateWithBackend failed: ")
                 + std::string(rec_or.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or;
  bool ok = run_standard_script(ctx, recorder);

  CHECK(ok, "timed out before all detection events arrived");

  // =========================================================
  // Verify events
  // =========================================================
  int total_events = static_cast<int>(bptr->events.size());
  CHECK(total_events == 3,
        "expected 3 event rows, got " + std::to_string(total_events));

  int open_events = 0;
  for (auto& e : bptr->events) if (!e.ended) ++open_events;
  CHECK(open_events == 0,
        "expected 0 open events (no NULL end), got " + std::to_string(open_events));

  int person_events = 0, vehicle_events = 0;
  for (auto& e : bptr->events) {
    if (e.sdt_json == "[\"person\"]")  ++person_events;
    if (e.sdt_json == "[\"vehicle\"]") ++vehicle_events;
  }
  CHECK(person_events == 2,
        "expected 2 person events, got " + std::to_string(person_events));
  CHECK(vehicle_events == 1,
        "expected 1 vehicle event, got " + std::to_string(vehicle_events));

  int no_thumb = 0;
  for (auto& e : bptr->events) if (e.thumb_id.empty()) ++no_thumb;
  CHECK(no_thumb == 0,
        "expected all events to have thumbnailId, got "
        + std::to_string(no_thumb) + " without");

  int cam108_events = 0, cam109_events = 0;
  for (auto& e : bptr->events) {
    if (e.camera_ip == ctx.cfg108.ip) ++cam108_events;
    if (e.camera_ip == ctx.cfg109.ip) ++cam109_events;
  }
  CHECK(cam108_events == 2,
        "expected 2 events for cam108, got " + std::to_string(cam108_events));
  CHECK(cam109_events == 1,
        "expected 1 event for cam109, got " + std::to_string(cam109_events));

  // =========================================================
  // Verify smartDetectObjects
  // =========================================================
  int total_sdo = static_cast<int>(bptr->sdos.size());
  CHECK(total_sdo == 3,
        "expected 3 smartDetectObject rows, got " + std::to_string(total_sdo));

  int person_sdo = 0, vehicle_sdo = 0;
  for (auto& s : bptr->sdos) {
    if (s.obj_type == "person")  ++person_sdo;
    if (s.obj_type == "vehicle") ++vehicle_sdo;
  }
  CHECK(person_sdo == 2,
        "expected 2 person SDOs, got " + std::to_string(person_sdo));
  CHECK(vehicle_sdo == 1,
        "expected 1 vehicle SDO, got " + std::to_string(vehicle_sdo));

  // all eventIds must reference a known event
  int orphan_sdo = 0;
  for (auto& s : bptr->sdos) {
    bool found = false;
    for (auto& e : bptr->events) {
      if (e.id == s.event_id) {
        found = true;
        break;
      }
    }
    if (!found) ++orphan_sdo;
  }
  CHECK(orphan_sdo == 0,
        "expected no orphan SDO rows, got " + std::to_string(orphan_sdo));

  // =========================================================
  // Verify smartDetectRaws
  // =========================================================
  int total_sdr = static_cast<int>(bptr->sdrs.size());
  CHECK(total_sdr == 3,
        "expected 3 smartDetectRaws rows, got " + std::to_string(total_sdr));

  int person_sdr = 0, vehicle_sdr = 0;
  for (auto& s : bptr->sdrs) {
    if (s.obj_type == "person")  ++person_sdr;
    if (s.obj_type == "vehicle") ++vehicle_sdr;
  }
  CHECK(person_sdr == 2,
        "expected 2 person smartDetectRaws rows, got " + std::to_string(person_sdr));
  CHECK(vehicle_sdr == 1,
        "expected 1 vehicle smartDetectRaws row, got " + std::to_string(vehicle_sdr));

  int sdr_zero_ts = 0;
  for (auto& s : bptr->sdrs) if (s.ts_ms == 0) ++sdr_zero_ts;
  CHECK(sdr_zero_ts == 0,
        "expected all smartDetectRaws rows to have non-zero timestamp");

  // =========================================================
  // Verify labels table
  // =========================================================
  // Expected label names: eventType:smartDetectZone, smartDetectType:person,
  // smartDetectType:vehicle (camera:<uuid> omitted -- test configs have no id).
  {
    bool has_event_type = false, has_person = false, has_vehicle = false;
    for (const auto& kv : bptr->labels) {
      if (kv.first == "eventType:smartDetectZone") has_event_type = true;
      if (kv.first == "smartDetectType:person")    has_person     = true;
      if (kv.first == "smartDetectType:vehicle")   has_vehicle    = true;
    }
    CHECK(has_event_type,
          "expected 'eventType:smartDetectZone' label in labels table");
    CHECK(has_person,
          "expected 'smartDetectType:person' label in labels table");
    CHECK(has_vehicle,
          "expected 'smartDetectType:vehicle' label in labels table");
  }

  // =========================================================
  // Verify detectionLabels rows
  // =========================================================
  // Each detection event needs one event-level row (objectId empty) so that
  // Protect's find-anything INNER JOIN on detectionLabels returns the event.
  {
    int dl_event_rows = 0;
    for (const auto& dl : bptr->det_labels)
      if (dl.object_id.empty()) ++dl_event_rows;
    CHECK(dl_event_rows == 3,
          "expected 3 event-level detectionLabels rows (objectId NULL), got "
          + std::to_string(dl_event_rows));
  }

  // Each SDO also needs an object-level row (objectId = sdo.id).
  {
    int dl_sdo_rows = 0;
    for (const auto& dl : bptr->det_labels)
      if (!dl.object_id.empty()) ++dl_sdo_rows;
    CHECK(dl_sdo_rows == 3,
          "expected 3 object-level detectionLabels rows (objectId = sdo.id), got "
          + std::to_string(dl_sdo_rows));
  }

  // Every detectionLabels row must carry at least 2 label lids
  // (eventType:smartDetectZone + smartDetectType:<type>).
  {
    int dl_few_labels = 0;
    for (const auto& dl : bptr->det_labels)
      if (dl.lids.size() < 2) ++dl_few_labels;
    CHECK(dl_few_labels == 0,
          "expected all detectionLabels rows to carry >= 2 label lids, "
          + std::to_string(dl_few_labels) + " had fewer");
  }

  // All detectionLabels rows must reference a known event.
  {
    int orphan_dl = 0;
    for (const auto& dl : bptr->det_labels) {
      bool found = false;
      for (const auto& e : bptr->events) {
        if (e.id == dl.event_id) {
          found = true;
          break;
        }
      }
      if (!found) ++orphan_dl;
    }
    CHECK(orphan_dl == 0,
          "expected no orphan detectionLabels rows, got "
          + std::to_string(orphan_dl));
  }

  // Object-level rows must reference a known SDO.
  {
    int orphan_sdo_dl = 0;
    for (const auto& dl : bptr->det_labels) {
      if (dl.object_id.empty()) continue;
      bool found = false;
      for (const auto& s : bptr->sdos) {
        if (s.id == dl.object_id) {
          found = true;
          break;
        }
      }
      if (!found) ++orphan_sdo_dl;
    }
    CHECK(orphan_sdo_dl == 0,
          "expected no detectionLabels rows with unknown objectId, got "
          + std::to_string(orphan_sdo_dl));
  }

  // =========================================================
  // Verify buffer padding: end - start >= pre_buffer + post_buffer (4000 ms)
  // =========================================================
  {
    int padded = 0;
    for (auto& e : bptr->events)
      if (e.ended && (e.end_ms - e.start_ms) >= 4000) ++padded;
    CHECK(padded == 3,
          "expected all 3 events to have end-start >= 4000 ms (2s pre + 2s post), got "
          + std::to_string(padded));
  }

  // =========================================================
  // Verify UBV thumbnail files
  // =========================================================
  const std::string ubv108 = ubv_dir + "/" + ctx.cfg108.ip + "_thumbnails.ubv";
  const std::string ubv109 = ubv_dir + "/" + ctx.cfg109.ip + "_thumbnails.ubv";

  // cam108: 2 frames (human start + vehicle start)
  {
    auto frames_or = ubv::decode(ubv108);
    if (!frames_or.ok()) {
      CHECK(false, std::string("ubv::decode failed for cam108: ")
                   + std::string(frames_or.status().message()));
    } else {
      const auto& frames = *frames_or;
      CHECK(frames.size() == 2,
            "expected 2 UBV frames for cam108, got " + std::to_string(frames.size()));
      for (std::size_t i = 0; i < frames.size(); ++i) {
        bool valid = frames[i].jpeg.size() >= 2
                  && frames[i].jpeg[0] == 0xff
                  && frames[i].jpeg[1] == 0xd8;
        CHECK(valid, "cam108 UBV frame " + std::to_string(i) + " is not a valid JPEG");
      }
    }
  }

  // cam109: 1 frame (human start)
  {
    auto frames_or = ubv::decode(ubv109);
    if (!frames_or.ok()) {
      CHECK(false, std::string("ubv::decode failed for cam109: ")
                   + std::string(frames_or.status().message()));
    } else {
      const auto& frames = *frames_or;
      CHECK(frames.size() == 1,
            "expected 1 UBV frame for cam109, got " + std::to_string(frames.size()));
      if (!frames.empty()) {
        bool valid = frames[0].jpeg.size() >= 2
                  && frames[0].jpeg[0] == 0xff
                  && frames[0].jpeg[1] == 0xd8;
        CHECK(valid, "cam109 UBV frame 0 is not a valid JPEG");
      }
    }
  }
}

// ============================================================
// Buffer padding test -- custom pre/post values
// ============================================================
static void test_buffer_padding(const std::string& ubv_dir) {
  TestContext ctx;
  ctx.ubv_dir = ubv_dir;

  const uint32_t pre_sec  = 1;
  const uint32_t post_sec = 3;
  const uint64_t min_span = (pre_sec + post_sec) * 1000;  // 4000 ms

  auto backend = std::make_unique<MockBackend>();
  MockBackend* bptr = backend.get();

  auto rec_or = onvif::DetectionRecorder::CreateWithBackend(std::move(backend));
  if (!rec_or.ok()) {
    CHECK(false, std::string("DetectionRecorder::CreateWithBackend failed: ")
                 + std::string(rec_or.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or;
  recorder.set_buffer(pre_sec, post_sec);

  bool ok = run_standard_script(ctx, recorder);
  CHECK(ok, "buffer test: timed out before all events arrived");

  // All events must have end-start >= pre+post ms.
  int padded = 0;
  for (auto& e : bptr->events)
    if (e.ended && (e.end_ms - e.start_ms) >= min_span) ++padded;
  CHECK(padded == 3,
        "expected all 3 events span >= " + std::to_string(min_span)
        + " ms (1s pre + 3s post), got " + std::to_string(padded));

  // end-start should NOT be >= (pre+post+1)*1000 -- sanity upper-bound
  // (events arrive in rapid succession so the raw interval is well under 1 s).
  int over = 0;
  for (auto& e : bptr->events)
    if (e.ended && (e.end_ms - e.start_ms) >= min_span + 1000) ++over;
  CHECK(over == 0,
        "expected no events with span >= " + std::to_string(min_span + 1000)
        + " ms (raw detection too short to reach that), got "
        + std::to_string(over));
}

// ============================================================
// Run a single-camera scripted listener test.
// Drives the given emulator until `needed` non-empty-topic events arrive.
// Returns false on timeout.
static bool run_single_camera(SnapshotSyntheticEmulator& emu,
                               onvif::DetectionRecorder&  recorder,
                               int                        needed) {
  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "password";
  cfg.snapshot_url       = emu.snapshot_url();
  cfg.retry_interval_sec = 1;
  recorder.set_snapshot(cfg);

  std::mutex              mu;
  std::condition_variable cv;
  std::atomic<int>        events_seen{0};

  onvif::OnvifListener listener;
  listener.add_camera(cfg);

  std::thread t([&] {
    listener.run([&](const onvif::OnvifEvent& ev) {
      recorder.on_event(ev);
      if (!ev.topic.empty())
        if (++events_seen >= needed) cv.notify_one();
    });
  });

  bool timed_out;
  {
    std::unique_lock<std::mutex> lk(mu);
    timed_out = !cv.wait_for(lk, std::chrono::seconds(30),
                              [&] { return events_seen.load() >= needed; });
  }
  listener.stop();
  t.join();
  return !timed_out;
}

// ============================================================
// CellMotionDetector classification test
// ============================================================
static void test_cell_motion_classification(const std::string& ubv_dir) {
  auto backend = std::make_unique<MockBackend>();
  MockBackend* bptr = backend.get();

  auto rec_or = onvif::DetectionRecorder::CreateWithBackend(std::move(backend));
  if (!rec_or.ok()) {
    CHECK(false, std::string("DetectionRecorder::CreateWithBackend failed: ")
                 + std::string(rec_or.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or;
  recorder.set_ubv_dir(ubv_dir);

  const std::string real_ip = "192.168.1.200";
  auto jpeg = load_file(source_dir() + "testdata/snapshot_108.jpg");
  SnapshotSyntheticEmulator emu(real_ip,
    {make_cell_motion_response(true,  "2026-03-21T10:00:00Z"),
     make_cell_motion_response(false, "2026-03-21T10:00:05Z")},
    jpeg);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "password";
  cfg.snapshot_url       = emu.snapshot_url();
  cfg.retry_interval_sec = 1;

  recorder.set_snapshot(cfg);

  std::mutex              mu;
  std::condition_variable cv;
  std::atomic<int>        events_seen{0};
  const int               needed = 2;

  onvif::OnvifListener listener;
  listener.add_camera(cfg);

  std::thread t([&] {
    listener.run([&](const onvif::OnvifEvent& ev) {
      recorder.on_event(ev);
      if (!ev.topic.empty())
        if (++events_seen >= needed) cv.notify_one();
    });
  });

  bool timed_out;
  {
    std::unique_lock<std::mutex> lk(mu);
    timed_out = !cv.wait_for(lk, std::chrono::seconds(30),
                              [&] { return events_seen.load() >= needed; });
  }

  listener.stop();
  t.join();

  CHECK(!timed_out, "timed out waiting for cell-motion events");
  CHECK(events_seen.load() >= 2, "expected >= 2 cell-motion events");

  int events = static_cast<int>(bptr->events.size());
  CHECK(events == 1, "expected 1 detection interval, got " + std::to_string(events));

  int person_sdo = 0;
  for (auto& s : bptr->sdos) if (s.obj_type == "person") ++person_sdo;
  CHECK(person_sdo == 1,
        "expected 1 person SDO from CellMotionDetector, got "
        + std::to_string(person_sdo));

  int sdr = static_cast<int>(bptr->sdrs.size());
  CHECK(sdr == 1,
        "cell_motion: expected 1 smartDetectRaws row, got " + std::to_string(sdr));
}

// ============================================================
// MotionAlarm fallback test
// ============================================================
static void test_motion_alarm_fallback(const std::string& ubv_dir) {
  auto backend = std::make_unique<MockBackend>();
  MockBackend* bptr = backend.get();

  auto rec_or = onvif::DetectionRecorder::CreateWithBackend(std::move(backend));
  if (!rec_or.ok()) {
    CHECK(false, std::string("DetectionRecorder::CreateWithBackend failed: ")
                 + std::string(rec_or.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or;
  recorder.set_ubv_dir(ubv_dir);

  auto jpeg = load_file(source_dir() + "testdata/snapshot_108.jpg");
  SnapshotSyntheticEmulator emu("192.168.1.201",
    {make_motion_alarm_response(true,  "2026-03-22T10:00:00Z"),
     make_motion_alarm_response(false, "2026-03-22T10:00:05Z")},
    jpeg);
  emu.start();

  bool ok = run_single_camera(emu, recorder, 2);
  CHECK(ok, "motion_alarm_fallback: timed out");

  int events = static_cast<int>(bptr->events.size());
  CHECK(events == 1,
        "motion_alarm_fallback: expected 1 detection, got " + std::to_string(events));

  int person_sdo = 0;
  for (auto& s : bptr->sdos) if (s.obj_type == "person") ++person_sdo;
  CHECK(person_sdo == 1,
        "motion_alarm_fallback: expected 1 person SDO, got "
        + std::to_string(person_sdo));

  int sdr = static_cast<int>(bptr->sdrs.size());
  CHECK(sdr == 1,
        "motion_alarm_fallback: expected 1 smartDetectRaws row, got "
        + std::to_string(sdr));
}

// ============================================================
// CellMotionDetector suppresses MotionAlarm test
// ============================================================
static void test_cell_motion_suppresses_alarm(const std::string& ubv_dir) {
  auto backend = std::make_unique<MockBackend>();
  MockBackend* bptr = backend.get();

  auto rec_or = onvif::DetectionRecorder::CreateWithBackend(std::move(backend));
  if (!rec_or.ok()) {
    CHECK(false, std::string("DetectionRecorder::CreateWithBackend failed: ")
                 + std::string(rec_or.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or;
  recorder.set_ubv_dir(ubv_dir);

  auto jpeg = load_file(source_dir() + "testdata/snapshot_108.jpg");
  SnapshotSyntheticEmulator emu("192.168.1.202",
    {make_cell_motion_response(true,  "2026-03-22T10:01:00Z"),
     make_motion_alarm_response(true,  "2026-03-22T10:01:00Z"),
     make_cell_motion_response(false, "2026-03-22T10:01:05Z"),
     make_motion_alarm_response(false, "2026-03-22T10:01:05Z")},
    jpeg);
  emu.start();

  bool ok = run_single_camera(emu, recorder, 4);
  CHECK(ok, "cell_motion_suppresses_alarm: timed out");

  int events = static_cast<int>(bptr->events.size());
  CHECK(events == 1,
        "cell_motion_suppresses_alarm: expected 1 detection (not 2), got "
        + std::to_string(events));

  int open_events = 0;
  for (auto& e : bptr->events) if (!e.ended) ++open_events;
  CHECK(open_events == 0,
        "cell_motion_suppresses_alarm: expected 0 open events, got "
        + std::to_string(open_events));

  int sdr = static_cast<int>(bptr->sdrs.size());
  CHECK(sdr == 1,
        "cell_motion_suppresses_alarm: expected 1 smartDetectRaws row (not 2), got "
        + std::to_string(sdr));
}

// ============================================================
// AI events suppress CellMotionDetector test
// ============================================================
static void test_ai_suppresses_cell_motion(const std::string& ubv_dir) {
  auto backend = std::make_unique<MockBackend>();
  MockBackend* bptr = backend.get();

  auto rec_or = onvif::DetectionRecorder::CreateWithBackend(std::move(backend));
  if (!rec_or.ok()) {
    CHECK(false, std::string("DetectionRecorder::CreateWithBackend failed: ")
                 + std::string(rec_or.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or;
  recorder.set_ubv_dir(ubv_dir);

  auto jpeg = load_file(source_dir() + "testdata/snapshot_108.jpg");
  SnapshotSyntheticEmulator emu("192.168.1.203",
    {make_field_detector_response("Human", true,  "2026-03-22T10:02:00Z"),
     make_cell_motion_response(true,              "2026-03-22T10:02:00Z"),
     make_field_detector_response("Human", false, "2026-03-22T10:02:05Z"),
     make_cell_motion_response(false,             "2026-03-22T10:02:05Z")},
    jpeg);
  emu.start();

  bool ok = run_single_camera(emu, recorder, 4);
  CHECK(ok, "ai_suppresses_cell_motion: timed out");

  int events = static_cast<int>(bptr->events.size());
  CHECK(events == 1,
        "ai_suppresses_cell_motion: expected 1 detection (AI only, not 2), got "
        + std::to_string(events));

  int open_events = 0;
  for (auto& e : bptr->events) if (!e.ended) ++open_events;
  CHECK(open_events == 0,
        "ai_suppresses_cell_motion: expected 0 open events, got "
        + std::to_string(open_events));

  int person_sdo = 0;
  for (auto& s : bptr->sdos) if (s.obj_type == "person") ++person_sdo;
  CHECK(person_sdo == 1,
        "ai_suppresses_cell_motion: expected 1 person SDO, got "
        + std::to_string(person_sdo));

  int sdr = static_cast<int>(bptr->sdrs.size());
  CHECK(sdr == 1,
        "ai_suppresses_cell_motion: expected 1 smartDetectRaws row (not 2), got "
        + std::to_string(sdr));
}

// ============================================================
// Thumbnail crop-dimension test
//
// Verifies that stored UBV thumbnails have the dimensions produced by
// smart_crop (square with side == min(orig_w, orig_h)):
//   snapshot_108.jpg  2560×1440 → 1440×1440
//   snapshot_109.jpg   720×480  →  480×480
// ============================================================
static void test_thumbnail_crop_dimensions(const std::string& ubv_dir) {
  TestContext ctx;
  ctx.ubv_dir = ubv_dir;

  auto backend = std::make_unique<MockBackend>();
  auto rec_or = onvif::DetectionRecorder::CreateWithBackend(std::move(backend));
  if (!rec_or.ok()) {
    CHECK(false, std::string("DetectionRecorder::CreateWithBackend failed: ")
                 + std::string(rec_or.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or;

  bool ok = run_standard_script(ctx, recorder);
  CHECK(ok, "thumb_crop_dims: timed out before all events arrived");
  if (!ok) return;

  // cam108: snapshot_108.jpg 2560×1440; no ONVIF bbox, no detector →
  // stored uncropped (full 2560×1440).
  {
    const std::string ubv_path =
        ubv_dir + "/" + ctx.cfg108.ip + "_thumbnails.ubv";
    auto frames_or = ubv::decode(ubv_path);
    if (!frames_or.ok()) {
      CHECK(false, "thumb_crop_dims: ubv::decode failed for cam108: "
                   + std::string(frames_or.status().message()));
    } else {
      const auto& frames = *frames_or;
      CHECK(!frames.empty(), "thumb_crop_dims: cam108 has no UBV frames");
      for (std::size_t i = 0; i < frames.size(); ++i) {
        int w = 0, h = 0;
        bool ok_d = jpeg_dims(frames[i].jpeg, &w, &h);
        CHECK(ok_d, "thumb_crop_dims: cam108 frame " + std::to_string(i)
                    + " not a valid JPEG");
        CHECK(w == 2560 && h == 1440,
              "thumb_crop_dims: cam108 frame " + std::to_string(i)
              + " expected 2560×1440, got "
              + std::to_string(w) + "×" + std::to_string(h));
      }
    }
  }

  // cam109: snapshot_109.jpg 720×480; no ONVIF bbox, no detector →
  // stored uncropped (full 720×480).
  {
    const std::string ubv_path =
        ubv_dir + "/" + ctx.cfg109.ip + "_thumbnails.ubv";
    auto frames_or = ubv::decode(ubv_path);
    if (!frames_or.ok()) {
      CHECK(false, "thumb_crop_dims: ubv::decode failed for cam109: "
                   + std::string(frames_or.status().message()));
    } else {
      const auto& frames = *frames_or;
      CHECK(!frames.empty(), "thumb_crop_dims: cam109 has no UBV frames");
      for (std::size_t i = 0; i < frames.size(); ++i) {
        int w = 0, h = 0;
        bool ok_d = jpeg_dims(frames[i].jpeg, &w, &h);
        CHECK(ok_d, "thumb_crop_dims: cam109 frame " + std::to_string(i)
                    + " not a valid JPEG");
        CHECK(w == 720 && h == 480,
              "thumb_crop_dims: cam109 frame " + std::to_string(i)
              + " expected 720×480, got "
              + std::to_string(w) + "×" + std::to_string(h));
      }
    }
  }
}

// ============================================================
// ONVIF BoundingBox crop test
// ============================================================
static void test_onvif_bbox_crop(const std::string& ubv_dir) {
  auto backend = std::make_unique<MockBackend>();
  auto rec_or = onvif::DetectionRecorder::CreateWithBackend(std::move(backend));
  if (!rec_or.ok()) {
    CHECK(false, std::string("DetectionRecorder::CreateWithBackend failed: ")
                 + std::string(rec_or.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or;
  recorder.set_ubv_dir(ubv_dir);

  auto jpeg = load_file(source_dir() + "testdata/snapshot_108.jpg");
  SnapshotSyntheticEmulator emu("192.168.1.210",
    {make_field_detector_with_bbox_response(
         "Human", true,  "2026-03-24T10:00:00Z", -0.5f, -0.5f, 0.5f, 0.5f),
     make_field_detector_with_bbox_response(
         "Human", false, "2026-03-24T10:00:05Z", -0.5f, -0.5f, 0.5f, 0.5f)},
    jpeg);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "password";
  cfg.snapshot_url       = emu.snapshot_url();
  cfg.retry_interval_sec = 1;
  recorder.set_snapshot(cfg);

  std::mutex              mu;
  std::condition_variable cv;
  std::atomic<int>        events_seen{0};
  const int               needed = 2;

  onvif::OnvifListener listener;
  listener.add_camera(cfg);

  std::thread t([&] {
    listener.run([&](const onvif::OnvifEvent& ev) {
      recorder.on_event(ev);
      if (!ev.topic.empty())
        if (++events_seen >= needed) cv.notify_one();
    });
  });

  bool timed_out;
  {
    std::unique_lock<std::mutex> lk(mu);
    timed_out = !cv.wait_for(lk, std::chrono::seconds(30),
                              [&] { return events_seen.load() >= needed; });
  }
  listener.stop();
  t.join();

  CHECK(!timed_out, "onvif_bbox_crop: timed out");
  if (timed_out) return;

  const std::string ubv_path = ubv_dir + "/" + cfg.ip + "_thumbnails.ubv";
  auto frames_or = ubv::decode(ubv_path);
  if (!frames_or.ok()) {
    CHECK(false, "onvif_bbox_crop: ubv::decode failed: "
                 + std::string(frames_or.status().message()));
    return;
  }
  const auto& frames = *frames_or;
  CHECK(frames.size() == 1,
        "onvif_bbox_crop: expected 1 UBV frame, got "
        + std::to_string(frames.size()));
  if (frames.empty()) return;

  int w = 0, h = 0;
  bool ok_d = jpeg_dims(frames[0].jpeg, &w, &h);
  CHECK(ok_d, "onvif_bbox_crop: stored frame is not a valid JPEG");

  // smart_crop of 2560×1440 yields 1440×1440; bbox crop must differ
  CHECK(!(w == 1440 && h == 1440),
        "onvif_bbox_crop: got smart_crop dims 1440×1440 — bbox was ignored");
  CHECK(w > 0 && h > 0, "onvif_bbox_crop: zero crop dimensions");
}

// ============================================================
// AlarmNotifier: single person alarm fires exactly once on person detection
// ============================================================
static void test_alarm_notify_person() {
  static const char kAlarms[] =
    "[{\"id\":\"3a4b5c6d-7e8f-9012-a3b4-c5d6e7f80912\","
    "\"title\":\"Person Alert\","
    "\"triggers_data\":[[{\"id\":\"smartDetectType:person\","
    "\"is_matched_externally\":true}]]}]";

  UosEmulator uos;
  uos.set_alarms_json(kAlarms);
  uos.start();

  onvif::AlarmNotifier notifier(uos.base_url());
  notifier.refresh_alarms();
  notifier.notify("person", "AABBCCDDEEFF", "event-uuid-abc", 1234567890000ULL);

  const auto posted = uos.posted_events();
  CHECK(posted.size() == 1,
        "alarm_notify_person: expected 1 POST, got "
        + std::to_string(posted.size()));

  if (!posted.empty()) {
    const std::string& body = posted[0];
    CHECK(body.find("smartDetectType:person") != std::string::npos,
          "alarm_notify_person: POST body missing event key");
    CHECK(body.find("3a4b5c6d-7e8f-9012-a3b4-c5d6e7f80912") != std::string::npos,
          "alarm_notify_person: POST body missing alarm ID");
    CHECK(body.find("AABBCCDDEEFF") != std::string::npos,
          "alarm_notify_person: POST body missing camera MAC in scope");
  }
}

// ============================================================
// AlarmNotifier: type filtering — person alarm ignores vehicle events;
// vehicle alarm ignores person events
// ============================================================
static void test_alarm_type_filtering() {
  static const char kAlarms[] =
    "[{\"id\":\"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\","
    "\"triggers_data\":[[{\"id\":\"smartDetectType:person\"}]]},"
    "{\"id\":\"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb\","
    "\"triggers_data\":[[{\"id\":\"smartDetectType:vehicle\"}]]}]";

  UosEmulator uos;
  uos.set_alarms_json(kAlarms);
  uos.start();

  onvif::AlarmNotifier notifier(uos.base_url());
  notifier.refresh_alarms();

  // Person detection → only person alarm fires.
  notifier.notify("person", "112233445566", "evt-p", 1000ULL);
  {
    const auto p = uos.posted_events();
    CHECK(p.size() == 1,
          "alarm_type_filtering: person: expected 1 POST, got "
          + std::to_string(p.size()));
    if (!p.empty()) {
      CHECK(p[0].find("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa") != std::string::npos,
            "alarm_type_filtering: person POST must reference person alarm");
      CHECK(p[0].find("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb") == std::string::npos,
            "alarm_type_filtering: person POST must not reference vehicle alarm");
    }
  }

  // Vehicle detection → only vehicle alarm fires.
  notifier.notify("vehicle", "112233445566", "evt-v", 2000ULL);
  {
    const auto p = uos.posted_events();
    CHECK(p.size() == 2,
          "alarm_type_filtering: vehicle: expected 2 total POSTs, got "
          + std::to_string(p.size()));
    if (p.size() == 2) {
      CHECK(p[1].find("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb") != std::string::npos,
            "alarm_type_filtering: vehicle POST must reference vehicle alarm");
      CHECK(p[1].find("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa") == std::string::npos,
            "alarm_type_filtering: vehicle POST must not reference person alarm");
    }
  }
}

// ============================================================
// AlarmNotifier: empty alarm list → zero POSTs
// ============================================================
static void test_alarm_no_alarms() {
  UosEmulator uos;
  uos.set_alarms_json("[]");
  uos.start();

  onvif::AlarmNotifier notifier(uos.base_url());
  notifier.refresh_alarms();
  notifier.notify("person", "AABBCCDDEEFF", "evt-x", 999ULL);

  const auto posted = uos.posted_events();
  CHECK(posted.empty(),
        "alarm_no_alarms: expected 0 POSTs, got " + std::to_string(posted.size()));
}

// ============================================================
// AlarmNotifier: UOS unreachable → no crash, no POSTs
// ============================================================
static void test_alarm_uos_unreachable() {
  // Port 1 is never open for regular users; curl returns ECONNREFUSED immediately.
  onvif::AlarmNotifier notifier("http://127.0.0.1:1");
  notifier.refresh_alarms();  // must not crash
  notifier.notify("person", "AABBCCDDEEFF", "evt-y", 500ULL);  // must not crash
  CHECK(true, "alarm_uos_unreachable: did not crash or hang");
}

// ============================================================
// Alarm integration e2e:
//   DetectionRecorder + AlarmNotifier + camera emulators + UosEmulator
//
// Standard 3-detection script (2 person + 1 vehicle) with two real cameras.
// Verifies UOS receives exactly 3 POSTs matching the detection types and
// that every POST includes the camera MAC in the scope.
// ============================================================
static void test_alarm_integration_e2e(const std::string& ubv_dir) {
  static const char kAlarms[] =
    "[{\"id\":\"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\","
    "\"triggers_data\":[[{\"id\":\"smartDetectType:person\"}]]},"
    "{\"id\":\"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb\","
    "\"triggers_data\":[[{\"id\":\"smartDetectType:vehicle\"}]]}]";

  UosEmulator uos;
  uos.set_alarms_json(kAlarms);
  uos.start();

  onvif::AlarmNotifier notifier(uos.base_url());
  notifier.refresh_alarms();

  TestContext ctx;
  ctx.ubv_dir = ubv_dir;
  ctx.mac108  = "A1B2C3D4E5F6";
  ctx.mac109  = "B2C3D4E5F6A7";

  auto backend = std::make_unique<MockBackend>();
  auto rec_or = onvif::DetectionRecorder::CreateWithBackend(std::move(backend));
  if (!rec_or.ok()) {
    CHECK(false, std::string("alarm_integration_e2e: CreateWithBackend failed: ")
                 + std::string(rec_or.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or;
  recorder.set_alarm_notifier(&notifier);

  bool ok = run_standard_script(ctx, recorder);
  CHECK(ok, "alarm_integration_e2e: timed out before all detection events arrived");
  if (!ok) return;

  // Standard script: cam108 emits Human+Vehicle (2 detections),
  // cam109 emits Human (1 detection).
  // With 1 person alarm + 1 vehicle alarm:
  //   2 person detections × 1 alarm = 2 POSTs
  //   1 vehicle detection × 1 alarm = 1 POST
  //   Total: 3 POSTs
  const auto posted = uos.posted_events();
  CHECK(posted.size() == 3,
        "alarm_integration_e2e: expected 3 UOS POSTs (2 person + 1 vehicle), got "
        + std::to_string(posted.size()));

  int person_posts = 0, vehicle_posts = 0;
  for (const auto& body : posted) {
    if (body.find("smartDetectType:person")  != std::string::npos) ++person_posts;
    if (body.find("smartDetectType:vehicle") != std::string::npos) ++vehicle_posts;
  }
  CHECK(person_posts == 2,
        "alarm_integration_e2e: expected 2 person POSTs, got "
        + std::to_string(person_posts));
  CHECK(vehicle_posts == 1,
        "alarm_integration_e2e: expected 1 vehicle POST, got "
        + std::to_string(vehicle_posts));

  // Each POST must carry the triggering camera's MAC in the scope.
  int with_mac = 0;
  for (const auto& body : posted) {
    if (body.find(ctx.mac108) != std::string::npos ||
        body.find(ctx.mac109) != std::string::npos)
      ++with_mac;
  }
  CHECK(with_mac == static_cast<int>(posted.size()),
        "alarm_integration_e2e: expected all POSTs to include a camera MAC, only "
        + std::to_string(with_mac) + " did");

  // Alarm IDs must match their trigger types.
  for (const auto& body : posted) {
    if (body.find("smartDetectType:person") != std::string::npos) {
      CHECK(body.find("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa") != std::string::npos,
            "alarm_integration_e2e: person POST missing person alarm ID");
    }
    if (body.find("smartDetectType:vehicle") != std::string::npos) {
      CHECK(body.find("bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb") != std::string::npos,
            "alarm_integration_e2e: vehicle POST missing vehicle alarm ID");
    }
  }
}

// ============================================================
// main
// ============================================================
int main() {
  const std::string ubv_dir = "/tmp/test_dr_thumbs";

  onvif::global_init();

  run_test("detection_e2e",             [&] { test_detection_e2e(ubv_dir); });
  run_test("buffer_padding",            [&] { test_buffer_padding(ubv_dir); });
  run_test("cell_motion_classification",
           [&] { test_cell_motion_classification(ubv_dir); });
  run_test("motion_alarm_fallback",
           [&] { test_motion_alarm_fallback(ubv_dir); });
  run_test("cell_motion_suppresses_alarm",
           [&] { test_cell_motion_suppresses_alarm(ubv_dir); });
  run_test("ai_suppresses_cell_motion",
           [&] { test_ai_suppresses_cell_motion(ubv_dir); });
  run_test("thumbnail_crop_dimensions",
           [&] { test_thumbnail_crop_dimensions(ubv_dir); });
  run_test("onvif_bbox_crop",
           [&] { test_onvif_bbox_crop(ubv_dir); });
  run_test("alarm_notify_person",        [] { test_alarm_notify_person(); });
  run_test("alarm_type_filtering",       [] { test_alarm_type_filtering(); });
  run_test("alarm_no_alarms",            [] { test_alarm_no_alarms(); });
  run_test("alarm_uos_unreachable",      [] { test_alarm_uos_unreachable(); });
  run_test("alarm_integration_e2e",
           [&] { test_alarm_integration_e2e(ubv_dir); });

  onvif::global_cleanup();

  std::cout << "================================================\n"
            << "Results: " << g_pass << " checks passed, "
            << g_fail    << " checks failed\n"
            << "================================================\n";

  return g_fail > 0 ? 1 : 0;
}
