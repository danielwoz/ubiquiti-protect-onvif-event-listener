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
 *   - writes every raw event as a JSON Lines entry to a timestamped .jsonl file
 *   - records human/vehicle detection intervals to the UniFi Protect PostgreSQL DB
 */

#include <csignal>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
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
    "Enable verbose logging (lifecycle, events, renewals).");
ABSL_FLAG(std::string, model_dir, "",
    "Directory containing nanodet_m.param and nanodet_m.bin.");
ABSL_FLAG(bool, detect, false,
    "Enable NanoDet-M object detection for thumbnail cropping "
    "(requires --model_dir). Runs as fallback when the camera provides "
    "no ONVIF bounding box.");
ABSL_FLAG(bool, detect_override, false,
    "Run NanoDet-M on every thumbnail regardless of whether the camera "
    "provides an ONVIF bounding box. Implies --detect.");

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
    std::cerr << "[recorder] output -> " << path << '\n';
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

  std::time_t t0 = std::time(nullptr);
  std::tm tm0{};
  gmtime_r(&t0, &tm0);
  char ts_buf[32];
  std::strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", &tm0);
  std::string output_path  = std::string("onvif_events_") + ts_buf + ".jsonl";
  std::string raw_path     = std::string("onvif_raw_")    + ts_buf + ".jsonl";

  const std::string db_conn     = absl::GetFlag(FLAGS_db_conn);
  const std::string db_host     = absl::GetFlag(FLAGS_db_host);
  const std::string thumbs_dir  = absl::GetFlag(FLAGS_ubv_dir);
  const uint32_t pre_buf_sec    =
      static_cast<uint32_t>(absl::GetFlag(FLAGS_pre_buffer_sec));
  const uint32_t post_buf_sec   =
      static_cast<uint32_t>(absl::GetFlag(FLAGS_post_buffer_sec));
  const bool verbose            = absl::GetFlag(FLAGS_verbose);

  std::cerr << "ONVIF Event Recorder\n"
            << "Events file : " << output_path  << '\n'
            << "Raw file    : " << raw_path      << '\n'
            << "DB backend  : postgres\n"
            << "DB conn     : " << db_conn       << '\n'
            << "DB host     : " << (db_host.empty() ? "(default)" : db_host) << '\n'
            << "Pre-buffer  : " << pre_buf_sec   << " s\n"
            << "Post-buffer : " << post_buf_sec  << " s\n"
            << "Verbose     : " << (verbose ? "yes" : "no") << '\n';
  if (!thumbs_dir.empty())
    std::cerr << "UBV dir     : " << thumbs_dir << '\n';
  std::cerr << "Press Ctrl+C to stop\n\n";

  onvif::global_init();

  // EventRecorder
  auto er_or = EventRecorder::Create(output_path);
  if (!er_or.ok()) {
    std::cerr << "Fatal: " << er_or.status().message() << '\n';
    onvif::global_cleanup();
    return 1;
  }
  EventRecorder& event_rec = **er_or;

  // DetectionRecorder
  auto dr_or = onvif::DetectionRecorder::Create(db_conn);
  if (!dr_or.ok()) {
    std::cerr << "Fatal: " << dr_or.status().message() << '\n';
    onvif::global_cleanup();
    return 1;
  }
  onvif::DetectionRecorder& det_rec = **dr_or;
  det_rec.set_buffer(pre_buf_sec, post_buf_sec);

  // Optional: load NanoDet-M for thumbnail subject cropping.
  std::unique_ptr<object_detect::ObjectDetector> detector;
  const std::string model_dir      = absl::GetFlag(FLAGS_model_dir);
  const bool        detect         = absl::GetFlag(FLAGS_detect);
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
        std::fprintf(stderr, "[detect] override mode: NanoDet-M from %s\n",
                     model_dir.c_str());
      } else {
        std::fprintf(stderr, "[detect] fallback mode: NanoDet-M from %s\n",
                     model_dir.c_str());
      }
    } else {
      std::fprintf(stderr,
                   "[detect] model not loaded (smart-crop fallback): %s\n",
                   std::string(det.status().message()).c_str());
    }
  }

  onvif::OnvifListener listener;
  g_listener = &listener;
  std::signal(SIGINT,  signal_handler);
  std::signal(SIGTERM, signal_handler);
  if (verbose) listener.enable_verbose_logging();

  if (!thumbs_dir.empty())
    det_rec.set_ubv_dir(thumbs_dir);

  // Camera configs are loaded from the UniFi Protect database.
  // --db_host overrides the host; empty = use the local Unix socket.
  unifi::DbConfig cam_db;
  cam_db.host = db_host.empty() ? "/run/postgresql" : db_host;

  auto cams_or = unifi::load_cameras(cam_db);
  if (!cams_or.ok()) {
    std::cerr << "Fatal: " << cams_or.status().message() << '\n';
    onvif::global_cleanup();
    return 1;
  }
  auto cameras = std::move(*cams_or);

  if (auto s = unifi::enable_smart_detect(cameras, cam_db); !s.ok()) {
    std::cerr << "Fatal: " << s.message() << '\n';
    onvif::global_cleanup();
    return 1;
  }

  for (auto cam : cameras) {
    cam.max_consecutive_failures = 3;
    listener.add_camera(cam);
    det_rec.set_snapshot(cam);
  }
  listener.enable_raw_recording(raw_path);

  listener.run([&event_rec, &det_rec](const onvif::OnvifEvent& ev) {
    event_rec.write(ev);
    det_rec.on_event(ev);
  });

  g_listener = nullptr;
  onvif::global_cleanup();
  std::cerr << "\nDone.\n"
            << "  Events     : " << output_path << '\n'
            << "  Raw        : " << raw_path    << '\n'
            << "  Database   : " << db_conn     << '\n'
            << "  Thumbnails : " << thumbs_dir  << "/\n";
  return 0;
}
