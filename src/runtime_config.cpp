// Copyright 2026 Daniel W
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "runtime_config.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "absl/flags/commandlineflag.h"
#include "absl/flags/reflection.h"
#include "absl/log/log.h"
#include "absl/status/status.h"

#include "unifi_camera_config.hpp"  // for internal::json_get

namespace runtime_config {

const std::vector<Entry>& Schema() {
  static const std::vector<Entry> kEntries = {
    // ---- Detection ----
    {"detect", Type::Bool,
     "Run NanoDet-M as a fallback when the camera supplies no ONVIF bbox.",
     "Detection"},
    {"detect_override", Type::Bool,
     "Always run NanoDet-M, ignoring the ONVIF bbox.  Implies --detect.",
     "Detection"},
    {"coalesce_window_sec", Type::Int,
     "Merge consecutive detections from the same camera within N seconds. "
     "Set to 0 to disable.",
     "Detection"},
    {"max_events_per_hour", Type::Int,
     "Per-camera detection rate limit per hour.  Set to 0 for unlimited.",
     "Detection"},
    {"pre_buffer_sec", Type::Int,
     "Seconds of padding added before a detection's start time, so the "
     "recorded clip begins slightly before the camera first saw the "
     "subject.  Raise it if clips feel like they start mid-action.",
     "Detection"},
    {"post_buffer_sec", Type::Int,
     "Seconds of padding added after a detection's end time.  Raise it if "
     "clips cut off before the subject has left frame.",
     "Detection"},
    {"momentary_event_sec", Type::Int,
     "Synthetic duration for momentary ONVIF events -- topics that fire "
     "once and never report an end, of which IVS line crossing is the "
     "usual case.  Such an event is recorded as "
     "(start - pre_buffer) .. (start + this + post_buffer) and closed "
     "immediately.  Raise it if line-crossing clips are too short to "
     "review.",
     "Detection"},
    {"coalesce_history", Type::Bool,
     "On startup, merge already-recorded events older than coalesce_window.",
     "Detection"},
    {"coalesce_history_days", Type::Int,
     "How far back to scan during the startup coalesce pass.",
     "Detection"},
    {"default_object_type", Type::String,
     "Default object type for events that arrive without a class "
     "(e.g. 'motion', 'person', 'vehicle').",
     "Detection"},
    {"drop_unclassified_motion", Type::Bool,
     "Drop generic motion events with no ONVIF class that NanoDet-M cannot "
     "classify, instead of recording them as default_object_type.  Camera AI "
     "events (Person/Vehicle/Pet) and per-camera overrides are unaffected.  "
     "Cuts false-positive clutter from AI cameras (e.g. Reolink) that also "
     "emit noisy basic-motion events.",
     "Detection"},
    {"camera_object_types", Type::String,
     "Per-camera type overrides as comma-separated ip=type pairs, e.g. "
     "192.168.1.108=person,192.168.1.109=vehicle.",
     "Detection"},
    {"camera_coalesce_window_sec", Type::String,
     "Per-camera coalesce-window overrides as comma-separated ip=sec pairs, "
     "e.g. 192.168.1.108=120,192.168.1.109=60.  Bumping the window for a "
     "noisy camera reduces duplicate events when its onboard tracker "
     "briefly loses sight and re-fires (issue #29).",
     "Detection"},

    {"excluded_cameras", Type::String,
     "Comma-separated MAC addresses of cameras onvif-recorder should "
     "ignore entirely (no ONVIF subscription, no motion polling).  Use "
     "the Enabled tickbox on the Camera Health card above to manage "
     "this.  Useful when another integration (e.g. a UniFi AI Port) is "
     "already producing detection events for the same physical camera.",
     "Detection"},

    // ---- First-party cameras ----
    {"first_party_cameras", Type::String,
     "Comma-separated camera IDs to enable smart-detect for.  Use the "
     "tickbox list above to manage this.",
     "First-party cameras"},
    {"first_party_camera_models", Type::String,
     "Comma-separated model substrings to match (e.g. G3 Instant,G4 Bullet).",
     "First-party cameras"},
    {"poll_interval_sec", Type::Int,
     "Seconds between motion-event poll cycles for first-party cameras.",
     "First-party cameras"},
    {"first_party_always_smart_detect", Type::Bool,
     "Write a smartDetectZone event for EVERY motion event from an opted-in "
     "first-party camera, even when NanoDet-M doesn't find a security-relevant "
     "subject.  Lets the Protect UI's smart-detect filter surface every motion "
     "instead of silently dropping the ones the on-device AI doesn't recognise.  "
     "Default on.  Disable to revert to the older skip-on-no-detection behaviour.",
     "First-party cameras"},
    {"first_party_fallback_class", Type::String,
     "smartDetectType used when --first_party_always_smart_detect=true and "
     "NanoDet-M couldn't classify the motion event.  Default 'person'.  "
     "Other accepted values: 'vehicle', 'animal', 'package'.",
     "First-party cameras"},
    {"motion_video_sample_secs", Type::Int,
     "When > 0, motion_poller samples N seconds of video frames (1s apart) "
     "from /api/cameras/<id>/snapshot?ts=<ms> starting at the real "
     "motion-start moment, runs NanoDet-M on each, and uses the highest-"
     "confidence security-relevant class.  A baseline frame at (start - 5s) "
     "is subtracted so persistent scene objects (e.g. a parked car) do not "
     "trigger on their own appearance.  N is capped at min(this value, "
     "event length in seconds).  Set to 0 to disable and use only Protect's "
     "stored cropped thumbnail.",
     "First-party cameras"},
    {"camera_video_sample_secs", Type::String,
     "Per-camera overrides for motion_video_sample_secs as comma-separated "
     "camera_id=secs pairs (camera IDs are 24-char hex from the cameras "
     "table).  Useful when one camera benefits from more samples while "
     "others can stay at the global default.",
     "First-party cameras"},
    {"motion_backfill_days", Type::Int,
     "On startup, retry NanoDet-M classification on motion events from the "
     "last N days that didn't produce a smartDetectZone (e.g. because of "
     "older code paths that lost detections to Protect's 640x360 snapshot "
     "endpoint).  Events with an existing overlapping smartDetectZone are "
     "skipped automatically; alarm notifications are suppressed for events "
     "older than 60 seconds.  Default 7.  Set to 0 to disable.",
     "First-party cameras"},

    // ---- Cameras ----
    {"rtsp_audio", Type::String,
     "Set to 'enable' or 'disable' to flip RTSP audio on every "
     "audio-capable camera at startup.  Empty leaves the existing setting "
     "alone.",
     "Cameras"},
    {"camera_snapshot_urls", Type::String,
     "Per-camera snapshot-path overrides as comma-separated ip=path pairs, "
     "e.g. 192.168.1.107=/cgi-bin/snapshot.cgi.  Useful when the "
     "ONVIF-advertised snapshotUrl is wrong (common on Dahua, issue #32).",
     "Cameras"},
    {"camera_snapshot_via_protect", Type::String,
     "Comma-separated list of camera IPs whose thumbnails should be "
     "fetched via Protect's own snapshot API instead of a direct HTTP "
     "GET against the camera.  Recommended for cameras with strict "
     "concurrent-HTTP-session limits (many Amcrest / Dahua firmwares) "
     "that sporadically 5xx while RTSP is streaming.",
     "Cameras"},

    // ---- MSR forwarding ----
    {"msr_url", Type::String,
     "Base URL of the local MSR gRPC service.  Default works on Dream "
     "Routers / NVRs out of the box; set to empty string to disable MSR "
     "thumbnail forwarding (third-party thumbnails will be stored in the "
     "DB only).",
     "MSR forwarding"},
    {"backfill_apply", Type::Bool,
     "When true, the startup MSR-thumbnail backfill writes changes.  Set "
     "to false for a dry-run that logs what would be migrated without "
     "touching MSR or the events table.",
     "MSR forwarding"},

    // ---- Logging ----
    {"verbose", Type::Bool,
     "Verbose log level (INFO and above).  Disabled by default writes only "
     "errors.",
     "Logging"},

    // ---- Auto-recovery (events from on-device backups) ----
    {"auto_recover_events", Type::Bool,
     "On startup, restore events + detectionLabels + labels from Protect's "
     "most recent on-device DB backup if the events table looks wiped "
     "(oldest event much newer than oldest recording).  Gated by "
     "Protect >= 7.1.0.  Disable if you don't want startup to touch the "
     "DB beyond the cluster's own initialisation.",
     "Auto-recovery"},
    {"auto_recover_threshold_hours", Type::Int,
     "Threshold for --auto_recover_events.  Recovery fires only when the "
     "gap exceeds this many hours.  Default 24h.",
     "Auto-recovery"},

    // ---- Auto-update / recovery ----
    {"autoupdate_enabled", Type::Bool,
     "Whether the device should auto-recover the package after a firmware "
     "wipe.  Default on.  When off, post-firmware-upgrade boots will still "
     "restore /etc/onvif-recorder/* from the config backup but won't "
     "re-run apt install.  Note: if /usr/bin/onvif-recorder is missing "
     "entirely (firmware wipe scenario), the recovery layer overrides this "
     "and reinstalls anyway -- a missing binary is recovery, not auto-update.",
     "Auto-update"},

    // ---- Profiling ----
    {"cpu_profile_hz", Type::Int,
     "Frequency (Hz) at which the CPU profiler samples thread call "
     "stacks.  0 disables sampling (default).  10–100 is a useful "
     "range; output appears at /api/cpuz under the admin URL.",
     "Profiling"},
  };
  return kEntries;
}

namespace {

// Escape @p s for embedding inside a JSON string literal.
std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string read_file(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

int LoadFromFile(const std::string& path) {
  const std::string content = read_file(path);
  if (content.empty()) return 0;

  int applied = 0;
  for (const auto& e : Schema()) {
    const std::string raw = unifi::internal::json_get(content, e.name);
    if (raw.empty()) continue;  // not set, or explicitly empty
    auto* flag = absl::FindCommandLineFlag(e.name);
    if (flag == nullptr) {
      LOG(WARNING) << "[config] unknown flag: " << e.name;
      continue;
    }
    std::string err;
    if (!flag->ParseFrom(raw, &err)) {
      LOG(WARNING) << "[config] " << e.name << "=" << raw
                   << " rejected: " << err;
      continue;
    }
    LOG(INFO) << "[config] override " << e.name << "=" << raw;
    ++applied;
  }
  return applied;
}

std::map<std::string, std::string> ReadFromFile(const std::string& path) {
  std::map<std::string, std::string> out;
  const std::string content = read_file(path);
  for (const auto& e : Schema()) {
    out[e.name] = content.empty()
        ? std::string()
        : unifi::internal::json_get(content, e.name);
  }
  return out;
}

absl::Status WriteToFile(const std::string& path,
                         const std::map<std::string, std::string>& values) {
  std::string out = "{\n";
  bool first = true;
  for (const auto& e : Schema()) {
    if (!first) out += ",\n";
    first = false;
    auto it = values.find(e.name);
    const std::string v = (it != values.end()) ? it->second : std::string();
    out += "  \"";
    out += e.name;
    out += "\": \"";
    out += json_escape(v);
    out += "\"";
  }
  out += "\n}\n";

  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp);
    if (!f.is_open())
      return absl::InternalError("cannot open " + tmp + " for write");
    f << out;
    if (!f.good())
      return absl::InternalError("write failed for " + tmp);
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0)
    return absl::InternalError("rename " + tmp + " -> " + path + " failed");
  return absl::OkStatus();
}

}  // namespace runtime_config
