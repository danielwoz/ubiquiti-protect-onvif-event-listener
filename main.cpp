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
 * main.cpp -- ONVIF event recorder binary.
 *
 * Uses onvif::OnvifListener to receive events from cameras and:
 *   - optionally writes every parsed event as JSON Lines (--event_log)
 *   - optionally writes every raw SOAP exchange as JSON Lines (--raw_log)
 *   - records human/vehicle detection intervals to the UniFi Protect PostgreSQL DB
 */

#include <csignal>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "alarm_notifier.hpp"
#include "detection_recorder.hpp"
#include "object_detect.hpp"
#include "onvif_listener.hpp"
#include "unifi_camera_config.hpp"

// ============================================================
// Command-line flags
// ============================================================
ABSL_FLAG(std::string, db_conn,
    "host=/run/postgresql port=5433 dbname=unifi-protect user=postgres",
    "libpq connection string for the UniFi Protect database.");
ABSL_FLAG(std::string, db_host, "",
    "Override the PostgreSQL host for camera config loading "
    "(empty = use /run/postgresql Unix socket).");
ABSL_FLAG(std::string, ubv_dir, "",
    "Directory for per-camera UBV thumbnail files (optional).");
ABSL_FLAG(int32_t, pre_buffer_sec, 2,
    "Seconds to buffer before the first detection event.");
ABSL_FLAG(int32_t, post_buffer_sec, 2,
    "Seconds to buffer after the last detection event.");
ABSL_FLAG(bool, verbose, false,
    "Enable verbose logging (INFO level): subscription lifecycle, "
    "events received, and renewals. Default logs errors only.");
ABSL_FLAG(std::string, model_dir, "",
    "Directory containing nanodet_m.param and nanodet_m.bin.");
ABSL_FLAG(bool, detect, false,
    "Enable NanoDet-M object detection for thumbnail cropping "
    "(requires --model_dir). Runs as fallback when the camera provides "
    "no ONVIF bounding box.");
ABSL_FLAG(bool, detect_override, false,
    "Run NanoDet-M on every thumbnail regardless of whether the camera "
    "provides an ONVIF bounding box. Implies --detect.");
ABSL_FLAG(std::string, event_log, "",
    "Path for the parsed-event JSON Lines log file. "
    "Each line is one ONVIF event with topic, source, data, and timestamp. "
    "Empty (default) disables this log.");
ABSL_FLAG(std::string, raw_log, "",
    "Path for the raw SOAP exchange JSON Lines log file. "
    "Each line is one full HTTP request/response pair (large). "
    "Empty (default) disables this log.");
ABSL_FLAG(std::string, uos_url, "http://localhost:11010",
    "Base URL for the UOS external automation manager used to trigger "
    "UniFi Protect security alarms on detections.");
ABSL_FLAG(std::string, default_object_type, "person",
    "Object type reported for generic motion events (CellMotionDetector, "
    "VideoSource/MotionAlarm) when the camera does not identify a type. "
    "Valid values: person, vehicle, animal, package.");
ABSL_FLAG(std::string, camera_object_types, "",
    "Per-camera object type overrides as comma-separated ip=type pairs, "
    "e.g. '192.168.1.108=animal,192.168.1.109=package'. "
    "Overrides the detection type for all events from that camera. "
    "Valid types: person, vehicle, animal, package.");
ABSL_FLAG(std::string, rtsp_audio, "",
    "Set enableRtspAudio in the Protect database for all adopted third-party "
    "cameras that have audio capability (hasAudio=true). "
    "Values: 'enable' sets enableRtspAudio=true, 'disable' sets it to false. "
    "Empty (default) leaves the database unchanged.");
ABSL_FLAG(int32_t, coalesce_window_sec, 30,
    "Merge consecutive detections from the same camera into one event if the "
    "new detection starts within this many seconds of the previous one ending. "
    "Set to 0 to disable merging.");
ABSL_FLAG(int32_t, max_events_per_hour, 10,
    "Maximum new detection events created per camera per hour. Events beyond "
    "this limit are dropped to prevent runaway recordings. Set to 0 for unlimited.");
ABSL_FLAG(bool, coalesce_history, true,
    "On startup, scan the last --coalesce_history_days days of events in the "
    "database and merge consecutive detections from the same third-party camera "
    "that are within --coalesce_window_sec of each other.");
ABSL_FLAG(int32_t, coalesce_history_days, 30,
    "Number of days to look back when --coalesce_history is set.");

// ============================================================
// JSON helpers (used only by EventRecorder)
// ============================================================
static std::string utc_now_iso8601_ms() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  std::ostringstream oss;
  oss << buf << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
  return oss.str();
}

static std::string json_str(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 4);
  out += '"';
  for (unsigned char c : s) {
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  out += '"';
  return out;
}

static std::string json_obj(const std::map<std::string, std::string>& m) {
  std::string out = "{";
  bool first = true;
  for (auto& [k, v] : m) {
    if (!first) out += ',';
    out += json_str(k) + ':' + json_str(v);
    first = false;
  }
  out += '}';
  return out;
}

// ============================================================
// Thread-safe JSON Lines recorder
// ============================================================
class EventRecorder {
 public:
  static absl::StatusOr<std::unique_ptr<EventRecorder>> Create(
      const std::string& path) {
    auto r = std::unique_ptr<EventRecorder>(new EventRecorder(path));
    if (!r->file_.is_open())
      return absl::InternalError("Cannot open: " + path);
    LOG(INFO) << "[recorder] event log -> " << path;
    return r;
  }

  void write(const onvif::OnvifEvent& ev) {
    std::string line;
    line += '{';
    line += json_str("recorded_at") + ':' + json_str(utc_now_iso8601_ms()) + ',';
    line += json_str("camera_ip")   + ':' + json_str(ev.camera_ip)   + ',';
    line += json_str("camera_user") + ':' + json_str(ev.camera_user) + ',';
    line += json_str("event_time")  + ':' + json_str(ev.event_time)  + ',';
    line += json_str("topic")       + ':' + json_str(ev.topic)       + ',';
    line += json_str("property_op") + ':' + json_str(ev.property_op) + ',';
    line += json_str("source")      + ':' + json_obj(ev.source)      + ',';
    line += json_str("data")        + ':' + json_obj(ev.data);
    line += "}\n";

    std::lock_guard<std::mutex> lk(mu_);
    file_ << line;
    file_.flush();
  }

 private:
  explicit EventRecorder(const std::string& path) {
    file_.open(path, std::ios::app);
  }

  std::ofstream file_;
  std::mutex    mu_;
};

// ============================================================
// Signal handling
// ============================================================
static onvif::OnvifListener* g_listener = nullptr;

static void signal_handler(int) {
  if (g_listener) g_listener->stop();
}

// ============================================================
// Entry point
// ============================================================
int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  const bool verbose = absl::GetFlag(FLAGS_verbose);
  if (verbose) {
    absl::SetMinLogLevel(absl::LogSeverityAtLeast::kInfo);
  } else {
    absl::SetMinLogLevel(absl::LogSeverityAtLeast::kError);
  }

  const std::string db_conn     = absl::GetFlag(FLAGS_db_conn);
  const std::string db_host     = absl::GetFlag(FLAGS_db_host);
  const std::string thumbs_dir  = absl::GetFlag(FLAGS_ubv_dir);
  const std::string event_log   = absl::GetFlag(FLAGS_event_log);
  const std::string raw_log     = absl::GetFlag(FLAGS_raw_log);
  const uint32_t pre_buf_sec    =
      static_cast<uint32_t>(absl::GetFlag(FLAGS_pre_buffer_sec));
  const uint32_t post_buf_sec   =
      static_cast<uint32_t>(absl::GetFlag(FLAGS_post_buffer_sec));

  LOG(INFO) << "ONVIF Event Recorder starting";
  LOG(INFO) << "DB conn     : " << db_conn;
  LOG(INFO) << "DB host     : " << (db_host.empty() ? "(default)" : db_host);
  LOG(INFO) << "Pre-buffer  : " << pre_buf_sec << " s";
  LOG(INFO) << "Post-buffer : " << post_buf_sec << " s";
  LOG(INFO) << "Coalesce    : " << absl::GetFlag(FLAGS_coalesce_window_sec) << " s";
  LOG(INFO) << "Max/hr      : " << absl::GetFlag(FLAGS_max_events_per_hour);
  LOG(INFO) << "Event log   : " << (event_log.empty() ? "(disabled)" : event_log);
  LOG(INFO) << "Raw log     : " << (raw_log.empty() ? "(disabled)" : raw_log);
  if (!thumbs_dir.empty())
    LOG(INFO) << "UBV dir     : " << thumbs_dir;

  onvif::global_init();

  // EventRecorder (optional)
  std::unique_ptr<EventRecorder> event_rec_storage;
  EventRecorder* event_rec = nullptr;
  if (!event_log.empty()) {
    auto er_or = EventRecorder::Create(event_log);
    if (!er_or.ok()) {
      LOG(ERROR) << "Fatal: " << er_or.status().message();
      onvif::global_cleanup();
      return 1;
    }
    event_rec_storage = std::move(*er_or);
    event_rec = event_rec_storage.get();
  }

  // DetectionRecorder
  auto dr_or = onvif::DetectionRecorder::Create(db_conn);
  if (!dr_or.ok()) {
    LOG(ERROR) << "Fatal: " << dr_or.status().message();
    onvif::global_cleanup();
    return 1;
  }
  onvif::DetectionRecorder& det_rec = **dr_or;
  det_rec.set_buffer(pre_buf_sec, post_buf_sec);
  det_rec.set_coalesce_window(
      static_cast<uint32_t>(absl::GetFlag(FLAGS_coalesce_window_sec)));
  det_rec.set_max_events_per_hour(
      static_cast<uint32_t>(absl::GetFlag(FLAGS_max_events_per_hour)));

  // Object type configuration.
  det_rec.set_default_object_type(absl::GetFlag(FLAGS_default_object_type));
  {
    const std::string cam_types = absl::GetFlag(FLAGS_camera_object_types);
    size_t pos = 0;
    while (pos < cam_types.size()) {
      size_t comma = cam_types.find(',', pos);
      if (comma == std::string::npos) comma = cam_types.size();
      const std::string pair = cam_types.substr(pos, comma - pos);
      const size_t eq = pair.find('=');
      if (eq != std::string::npos)
        det_rec.set_camera_object_type(pair.substr(0, eq), pair.substr(eq + 1));
      pos = comma + 1;
    }
  }

  // Optional: load NanoDet-M for thumbnail subject cropping.
  std::unique_ptr<object_detect::ObjectDetector> detector;
  const std::string model_dir       = absl::GetFlag(FLAGS_model_dir);
  const bool        detect          = absl::GetFlag(FLAGS_detect);
  const bool        detect_override = absl::GetFlag(FLAGS_detect_override);
  if ((detect || detect_override) && !model_dir.empty()) {
    auto det = object_detect::ObjectDetector::Load(
        model_dir + "/nanodet_m.param",
        model_dir + "/nanodet_m.bin");
    if (det.ok()) {
      detector = std::move(*det);
      det_rec.set_detector(detector.get());
      if (detect_override) {
        det_rec.set_detect_override(true);
        LOG(INFO) << "[detect] override mode: NanoDet-M from " << model_dir;
      } else {
        LOG(INFO) << "[detect] fallback mode: NanoDet-M from " << model_dir;
      }
    } else {
      LOG(WARNING) << "[detect] model not loaded (no ML crop): "
                   << det.status().message();
    }
  }

  onvif::OnvifListener listener;
  g_listener = &listener;
  std::signal(SIGINT,  signal_handler);
  std::signal(SIGTERM, signal_handler);

  if (!thumbs_dir.empty())
    det_rec.set_ubv_dir(thumbs_dir);

  // Camera configs are loaded from the UniFi Protect database.
  // --db_host overrides the host; empty = use the local Unix socket.
  unifi::DbConfig cam_db;
  cam_db.host = db_host.empty() ? "/run/postgresql" : db_host;

  auto cams_or = unifi::load_cameras(cam_db);
  if (!cams_or.ok()) {
    LOG(ERROR) << "Fatal: " << cams_or.status().message();
    onvif::global_cleanup();
    return 1;
  }
  auto cameras = std::move(*cams_or);

  if (auto s = unifi::enable_smart_detect(cameras, cam_db); !s.ok()) {
    LOG(ERROR) << "Fatal: " << s.message();
    onvif::global_cleanup();
    return 1;
  }

  if (auto s = unifi::ensure_smart_detect_zones(cameras, cam_db); !s.ok()) {
    LOG(ERROR) << "Fatal: " << s.message();
    onvif::global_cleanup();
    return 1;
  }

  {
    const std::string rtsp_audio = absl::GetFlag(FLAGS_rtsp_audio);
    if (rtsp_audio == "enable" || rtsp_audio == "disable") {
      const bool enable = (rtsp_audio == "enable");
      if (auto s = unifi::set_rtsp_audio(enable, cam_db); !s.ok()) {
        LOG(ERROR) << "Fatal: " << s.message();
        onvif::global_cleanup();
        return 1;
      }
      LOG(INFO) << "[rtsp_audio] " << (enable ? "enabled" : "disabled")
                << " RTSP audio for all cameras with audio capability";
    }
  }

  // AlarmNotifier: triggers UniFi Protect security alarms for ONVIF cameras.
  onvif::AlarmNotifier alarm_notifier(absl::GetFlag(FLAGS_uos_url));
  alarm_notifier.refresh_alarms();
  det_rec.set_alarm_notifier(&alarm_notifier);

  // Optional startup coalescing: merge nearby events already in the database.
  // Purge stuck-open events (end IS NULL older than 5 minutes) left behind
  // by a previous crash or rapid-fire "started" bursts from cameras.
  // Must run before coalesce_history so the NULL-end rows don't interfere.
  {
    const int n = det_rec.purge_stale_open_events(300000);
    if (n > 0)
      LOG(INFO) << "[startup] purged " << n << " stuck-open event(s)";
  }

  if (absl::GetFlag(FLAGS_coalesce_history)) {
    const int days = absl::GetFlag(FLAGS_coalesce_history_days);
    LOG(INFO) << "[coalesce_history] scanning last " << days << " day(s)...";
    const int n = det_rec.coalesce_history(days);
    LOG(INFO) << "[coalesce_history] done (" << n << " event(s) merged)";
  }

  // Purge rows orphaned by coalesce operations across all dependent tables
  // (smartDetectRaws, thumbnails, smartDetectObjects, detectionLabels).
  {
    const int n = det_rec.purge_orphaned_rows();
    if (n > 0)
      LOG(INFO) << "[startup] purged " << n << " orphaned row(s)";
  }

  for (auto cam : cameras) {
    cam.max_consecutive_failures = 3;
    listener.add_camera(cam);
    det_rec.set_snapshot(cam);
    LOG(INFO) << "Watching camera " << cam.ip;
  }

  if (!raw_log.empty())
    listener.enable_raw_recording(raw_log);

  listener.run([event_rec, &det_rec](const onvif::OnvifEvent& ev) {
    if (event_rec) event_rec->write(ev);
    det_rec.on_event(ev);
  });

  g_listener = nullptr;
  onvif::global_cleanup();
  LOG(INFO) << "Done";
  return 0;
}
