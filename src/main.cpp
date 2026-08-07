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
 *   - polls motion events from first-party cameras and runs NanoDet-M detection
 */

#include <libpq-fe.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

#include <atomic>
#include <cstdio>
#include <csignal>
#include <chrono>  // NOLINT(build/c++11)
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/log/log_sink_registry.h"
#include "alarm_notifier.hpp"
#include "contention_profiler.hpp"
#include "cpu_profiler.hpp"
#include "msr_backfill.hpp"
#include "msr_client.hpp"
#include "camera_change_log.hpp"
#include "detection_recorder.hpp"
#include "event_recorder.hpp"
#include "log_ring.hpp"
#include "admin_server.hpp"
#include "log_server.hpp"

// Fallback version string when running outside a .deb install. The admin UI
// prefers `dpkg-query -W -f='${Version}' onvif-recorder` at request time and
// only falls back to this constant if the package is not installed.
#ifndef ONVIF_RECORDER_VERSION
#define ONVIF_RECORDER_VERSION "dev"
#endif
#include "cameras_change_listener.hpp"
#include "event_recovery.hpp"
#include "motion_poller.hpp"
#include "wedge_healer.hpp"
#include "protect_version.hpp"
#include "object_detect.hpp"
#include "onvif_listener.hpp"
#include "patch_watcher.hpp"
#include "protect_pid_watcher.hpp"
#include "protect_ui_patch.hpp"
#include "protect_user_id_provider.hpp"
#include "runtime_config.hpp"
#include "unifi_camera_config.hpp"
#include "util.hpp"

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
    "Directory for per-camera UBV thumbnail files. "
    "Auto-detected on Dream Routers (/srv/unifi-protect/video).");
ABSL_FLAG(int32_t, pre_buffer_sec, 2,
    "Seconds to buffer before the first detection event.");
ABSL_FLAG(int32_t, post_buffer_sec, 2,
    "Seconds to buffer after the last detection event.");
ABSL_FLAG(int32_t, momentary_event_sec, 4,
    "Synthetic duration in seconds for momentary ONVIF events -- topics "
    "that fire once and never report an end, of which IVS line crossing "
    "(tns1:RuleEngine/LineDetector/Crossed) is the canonical case. The "
    "recorded event spans (start - pre_buffer) .. (start + this + "
    "post_buffer) and is closed immediately, so it never lingers with a "
    "null end for the stale-event purge to delete. Raise it if line-cross "
    "clips feel too short to review.");
ABSL_FLAG(bool, verbose, false,
    "Enable verbose logging (INFO level): subscription lifecycle, "
    "events received, and renewals. Default logs errors only.");
ABSL_FLAG(int32_t, cpu_profile_hz, 0,
    "Frequency (Hz) at which the CPU profiler samples the call "
    "stacks of all threads via SIGRTMIN+1.  0 disables sampling. "
    "10-100 is typical; output is exposed at /api/cpuz under the "
    "auth-gated admin API.");
ABSL_FLAG(std::string, model_dir, "/usr/share/onvif-recorder/models",
    "Directory containing nanodet_m.param and nanodet_m.bin. "
    "Models are shipped in the Debian package at this path; if missing, "
    "they are downloaded automatically.");
ABSL_FLAG(std::string, state_dir, "/var/lib/onvif-recorder",
    "Directory for persistent runtime state (API-key cache, etc.). "
    "Created on first run if missing.");
ABSL_FLAG(bool, detect, true,
    "Enable NanoDet-M object detection for thumbnail cropping. "
    "Runs as fallback when the camera provides no ONVIF bounding box.");
ABSL_FLAG(bool, detect_override, false,
    "Run NanoDet-M on every thumbnail regardless of whether the camera "
    "provides an ONVIF bounding box. Implies --detect.");
ABSL_FLAG(bool, drop_unclassified_motion, false,
    "Drop generic motion events (CellMotionDetector, VideoSource/MotionAlarm) "
    "that carry no ONVIF object class and that NanoDet-M cannot classify, "
    "instead of recording them as --default_object_type. Real camera AI "
    "events (Person/Vehicle/Pet) and --camera_object_types overrides are "
    "unaffected. Useful with Reolink/other AI cameras that also emit noisy "
    "basic-motion events. Default false.");
ABSL_FLAG(std::string, event_log, "",
    "Path for the parsed-event JSON Lines log file. "
    "Each line is one ONVIF event with topic, source, data, and timestamp. "
    "Empty (default) disables this log.");
ABSL_FLAG(std::string, raw_log, "",
    "Path for the raw SOAP exchange JSON Lines log file. "
    "Each line is one full HTTP request/response pair (large). "
    "Empty (default) disables this log.");
ABSL_FLAG(std::string, protect_url, "http://localhost:7080",
    "Base URL for the local Protect API used to trigger automations "
    "(e.g. 'Make Sound for detection') on smart detection events.");
ABSL_FLAG(std::string, msr_url, "http://127.0.0.1:7700",
    "Base URL for the local UniFi Media Server Recording (MSR) gRPC "
    "service.  Detection thumbnails are forwarded via "
    "RecordingAPI.StoreSnapshots so that MSR stores them as native UBV "
    "files owned by ms:unifi-streaming — making third-party thumbnails "
    "indistinguishable from first-party camera thumbnails in the "
    "Protect UI.  Default 'http://127.0.0.1:7700' works out of the box "
    "on UniFi Dream Routers/NVRs; set to empty string to disable.");
ABSL_FLAG(bool, msr_serialize, true,
    "When true (default), only one MSR StoreSnapshots call is in "
    "flight at a time across the whole process.  Caps our submitted "
    "load on MSR's image-processing thread pool at 1 of 128 slots, "
    "leaving the rest available for Protect's own video-segment "
    "queries.  Set to false to revert to the legacy concurrent path "
    "(every camera thread can hit MSR simultaneously).");
ABSL_FLAG(int32_t, msr_suspend_cooldown_sec, 30,
    "When > 0 and the MSR client has entered the 'suspended' state "
    "(5 consecutive failures), subsequent calls return empty without "
    "contacting MSR until this many seconds have elapsed since the "
    "last failure.  Prevents us from continuing to pile failed RPCs "
    "onto an already-overloaded MSR.  Set to 0 to revert to legacy "
    "behaviour (every call attempts MSR even while suspended).");
ABSL_FLAG(bool, msr_drop_on_failure, true,
    "When true (default), an MSR StoreSnapshots failure causes the "
    "thumbnail to be dropped entirely instead of falling back to a "
    "direct INSERT into Protect's `thumbnails` table.  The legacy "
    "fallback compounded DB contention exactly when MSR was already "
    "stalled — see issue #24.  Set to false to restore the legacy "
    "fallback path.");
ABSL_FLAG(int32_t, msr_burst_window_ms, 2000,
    "Burst-coalesce window in milliseconds: when a successful MSR id "
    "is cached for a camera within this window, subsequent detections "
    "for the same camera reuse the cached id instead of calling MSR "
    "again.  Each detection still produces its own DB rows; only the "
    "MSR snapshot upload is coalesced.  Set to 0 to disable (legacy "
    "behaviour: every detection makes its own MSR call).");
ABSL_FLAG(int32_t, backfill_msr_thumbnails, 60,
    "Migrate pre-MSR third-party thumbnails from the `thumbnails` table "
    "into native MSR-backed storage by re-uploading each JPEG via "
    "MsrClient::StoreSnapshot and updating events.thumbnailId + "
    "smartDetectObjects.thumbnailId.  Value is the lookback window in "
    "days; 0 disables.  Runs on every startup as a background thread "
    "when --msr_url is set; the query is self-filtering on "
    "LENGTH(thumbnailId)=24, so once the migration is complete, "
    "subsequent startups are effectively a no-op (~16 orphan rows "
    "checked against the thumbnails table, no MSR uploads).");
ABSL_FLAG(bool, backfill_apply, true,
    "When false, --backfill_msr_thumbnails runs as a dry run "
    "(no MSR uploads, no DB writes).  Default true.");
ABSL_FLAG(std::string, protect_user_id, "",
    "X-UserId header value for Protect API auth bypass from localhost. "
    "Obtain with: psql -h /run/postgresql -p 5432 -d unifi-core "
    "-c \"SELECT id FROM users LIMIT 1\"");
ABSL_FLAG(std::string, default_object_type, "person",
    "Object type reported for generic motion events (CellMotionDetector, "
    "VideoSource/MotionAlarm) when the camera does not identify a type. "
    "Valid values: person, vehicle, animal, package.");
ABSL_FLAG(std::string, camera_object_types, "",
    "Per-camera object type overrides as comma-separated ip=type pairs, "
    "e.g. '192.168.1.108=animal,192.168.1.109=package'. "
    "Overrides the detection type for all events from that camera. "
    "Valid types: person, vehicle, animal, package.");
ABSL_FLAG(std::string, camera_coalesce_window_sec, "",
    "Per-camera coalesce-window overrides as comma-separated ip=sec pairs, "
    "e.g. '192.168.1.108=120,192.168.1.109=60'. "
    "Overrides --coalesce_window_sec for that camera only.  Useful for "
    "noisy cameras whose onboard tracker briefly loses sight and re-fires "
    "events as 'new' tracks (issue #29).");
ABSL_FLAG(std::string, camera_snapshot_urls, "",
    "Per-camera snapshot path overrides as comma-separated ip=path pairs, "
    "e.g. '192.168.1.107=/cgi-bin/snapshot.cgi'. "
    "Useful for cameras whose ONVIF-advertised snapshotUrl is wrong "
    "(common on Dahua, see issue #32).  The path is appended to "
    "http://<camera_ip>/ and authenticated with the camera's username "
    "and password from the Protect cameras table.");
ABSL_FLAG(std::string, camera_snapshot_via_protect, "",
    "Comma-separated list of camera IPs whose detection thumbnails "
    "should be fetched via Protect's own /api/cameras/<id>/snapshot?"
    "ts=<ms> endpoint instead of a direct HTTP GET against the "
    "camera's snapshot URL.  Protect sources those from MSR (or the "
    "live recording pipeline), so no additional concurrent HTTP "
    "session is opened against the camera -- recommended for cameras "
    "with strict concurrent-HTTP-session limits (many Amcrest / Dahua "
    "firmwares) whose /snapshot endpoint sporadically 5xx's while RTSP "
    "is active.  Requires --protect_url + Protect >= 7.1.0.  Falls "
    "back to the direct HTTP path on failure.");
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

ABSL_FLAG(bool, first_party_always_smart_detect, true,
    "When true, motion_poller writes a smartDetectZone event for EVERY motion "
    "event from opted-in first-party cameras (--first_party_cameras), even when "
    "NanoDet-M doesn't find a security-relevant subject.  The fallback event uses "
    "--first_party_fallback_class as its smartDetectType and confidence=0 so the "
    "data clearly shows it wasn't AI-classified.  Disable to keep the older "
    "behaviour where unclassified motion events stay invisible to Protect's "
    "smart-detect filter.");
ABSL_FLAG(std::string, first_party_fallback_class, "person",
    "smartDetectType written when --first_party_always_smart_detect=true and "
    "NanoDet-M didn't classify the motion event.  Use 'person' / 'vehicle' / "
    "'animal' / 'package' or any string Protect's UI recognises.  Default "
    "'person' (the most common case for a typical home camera).");

ABSL_FLAG(int32_t, motion_video_sample_secs, 5,
    "When > 0, motion_poller pulls multiple snapshot frames per first-party "
    "motion event by calling /api/cameras/<id>/snapshot?ts=<ms> at 1-second "
    "offsets starting at the real motion-start time, for N seconds where "
    "N = min(this flag, event length in seconds).  A single baseline frame "
    "is fetched at (start - 5s) and any class already present in that frame "
    "is suppressed from the event sample (so a constantly-parked car does "
    "not trigger on its own appearance).  Set to 0 to disable time-travel "
    "sampling and use only Protect's stored cropped thumbnail (legacy "
    "behaviour).  Default 5.");
ABSL_FLAG(std::string, camera_video_sample_secs, "",
    "Per-camera overrides for --motion_video_sample_secs as comma-separated "
    "<camera_id>=<secs> pairs, e.g. "
    "'699bf42400214a03e4000459=7,abcd...=3'.  Camera IDs are the 24-char hex "
    "ids from the cameras table (same as --first_party_cameras).  When "
    "present, overrides the global cap for that camera.");
ABSL_FLAG(int32_t, motion_backfill_days, 7,
    "On startup, motion_poller pulls its per-camera high-water mark back "
    "to (now - N days) so the live poll loop re-scans recent motion events "
    "and re-classifies any that didn't produce a smartDetectZone last time "
    "(typical reason: an older code path that lost NanoDet hits because "
    "Protect's /snapshot endpoint returned 640x360 instead of full FOV).  "
    "Events with an existing overlapping smartDetectZone are still excluded "
    "so no duplicates are written.  Alarm notifications are suppressed for "
    "events older than 60 seconds.  Set to 0 to disable.");

ABSL_FLAG(bool, auto_recover_events, true,
    "On startup, if onvif-recorder detects the events table has been wiped "
    "while video recordings still exist (oldest event is more than --auto_"
    "recover_threshold_hours newer than the oldest recordingFile), restore "
    "events + detectionLabels + labels from Protect's most recent on-device "
    "DB backup at /srv/unifi-protect/dbBackups/ and re-run the 7.1+ enricher "
    "over them.  Gated by Protect >= 7.1.0 (see protect_version.hpp).  Set "
    "to false to disable.");
ABSL_FLAG(int32_t, auto_recover_threshold_hours, 24,
    "Threshold for --auto_recover_events.  If oldest_event - oldest_recording "
    "exceeds this many hours, recovery fires.  Setting too low risks "
    "spurious restores during normal cluster maintenance; default 24h is "
    "well above any expected legitimate gap.");

ABSL_FLAG(bool, autoupdate_enabled, true,
    "Whether the device should auto-recover the onvif-recorder package after "
    "a firmware wipe.  Read by /data/onvif-recorder/install.sh post-install "
    "and written to /data/onvif-recorder/.autoupdate-consent.  Default true: "
    "running install.sh is itself an act of consent.  Set to false to opt out, "
    "in which case a firmware wipe will require manually re-running the apt "
    "installer.  See the wipe-recovery branch in /data/onvif-recorder/"
    "boot-restore.sh -- a missing /usr/bin/onvif-recorder ignores this flag "
    "and reinstalls anyway, because losing the binary you didn't ask to lose "
    "is recovery, not auto-update.");

// --- New flags for first-party support, change log, and rollback ---

ABSL_FLAG(std::string, change_log, "",
    "Path for the cameras-table change log (JSON Lines). Records every "
    "cameras-table modification with old/new values for rollback support. "
    "Empty (default) disables change logging.");
ABSL_FLAG(std::string, rollback, "",
    "Undo cameras-table changes and exit. "
    "Values: 'third_party', 'first_party', 'all'. "
    "Uses --change_log file if it exists; otherwise guesstimates "
    "from current code for third-party cameras.");
ABSL_FLAG(std::string, excluded_cameras, "",
    "Comma-separated list of camera MAC addresses that onvif-recorder "
    "should ignore entirely -- no ONVIF subscription (third-party) and "
    "no motion-poll cycles (first-party).  Useful when a camera is "
    "already managed by another integration (e.g. a UniFi AI Port) and "
    "would otherwise produce duplicate detection events (issue #42).  "
    "Managed via a per-row tickbox on the Camera Health card at "
    "/onvif/admin/.");
ABSL_FLAG(std::string, first_party_cameras, "",
    "Comma-separated camera IDs of first-party cameras to enable smart "
    "detection flags for in the cameras table. Enables the Protect UI "
    "smart detection panel for those cameras.");
ABSL_FLAG(std::string, first_party_camera_models, "",
    "Comma-separated model substrings to match first-party cameras. "
    "For example, 'G3 Instant,G4 Bullet' matches cameras whose type "
    "contains either substring (case-insensitive). Matched cameras are "
    "merged with any explicit --first_party_cameras IDs.");
ABSL_FLAG(int32_t, poll_interval_sec, 10,
    "Seconds between motion-event poll cycles for first-party cameras. "
    "Only active when first-party cameras are discovered.");
ABSL_FLAG(bool, auto_heal_protect, true,
    "When Protect's Postgres or MSR stops responding for more than 60 s "
    "continuously, automatically run 'systemctl restart' on the affected "
    "service(s). Two independent triggers: DB wedge (>=1 pg query timeout "
    "in the window AND no successful queries in the same window) restarts "
    "msp+msr+unifi-protect; MSR wedge (MSR StoreSnapshots failing "
    "continuously) restarts msr only. Safeguards: no restart in the first "
    "5 min after start, 5 min cooldown between restarts, hard cap of 4 "
    "restarts per 24 h. Every restart is logged at ERROR level and "
    "surfaced on the admin page's Recent errors card. Set to false to "
    "disable and rely on the user to restart Protect manually.");
ABSL_FLAG(bool, motion_push, true,
    "Reactive motion polling via Postgres LISTEN/NOTIFY. When true "
    "(default), motion_poller installs an AFTER-INSERT trigger on the "
    "`events` table that fires pg_notify('onvif_recorder_motion', <id>) "
    "for type='motion' rows, then blocks on the LISTEN socket instead "
    "of polling on a fixed interval -- the poller wakes on the first "
    "matching event and steady-state DB query rate drops from "
    "(cameras x cycles/min) to (motion events/min). A 60-second "
    "safety-net poll still runs regardless so a dropped notification "
    "can't cause missed events. Set to false to keep the legacy "
    "interval poll (--poll_interval_sec).");
ABSL_FLAG(bool, patch_alarm_picker, true,
    "Live-patch the Protect UI (swai.js) to allow third-party cameras "
    "in the alarm creation picker. The patch is idempotent and re-applied "
    "on every startup so it survives firmware updates. The original file "
    "is backed up to swai.js.bak before each patch.");
ABSL_FLAG(uint16_t, log_port, 7890,
    "TCP port for the in-memory log viewer (loopback only). "
    "Served via nginx at /onvif/events/log with Protect auth.");
ABSL_FLAG(uint16_t, admin_port, 7891,
    "TCP port for the package admin page (loopback only). "
    "Served via nginx at /onvif/admin/ with Protect auth.");
ABSL_FLAG(std::string, channel_file, "/etc/onvif-recorder/channel",
    "Path to the APT-channel selector file read/written by the admin page.");

// ============================================================
// Signal handling
// ============================================================
static onvif::OnvifListener* g_listener = nullptr;
static onvif::MotionPoller*  g_poller   = nullptr;
// Flipped by signal_handler to signal an in-flight auto_recover
// background thread that it should exit at the next batch boundary
// (or during an inter-batch sleep slice).  Read via a pointer stored
// in EnrichOptions.cancelled -- see the auto_recover block below.
static std::atomic<bool>     g_recovery_shutdown{false};

static void signal_handler(int) {
  if (g_listener) g_listener->stop();
  if (g_poller)   g_poller->stop();
  g_recovery_shutdown.store(true, std::memory_order_relaxed);
}

// ============================================================
// Parse comma-separated IDs into a vector
// ============================================================
static std::vector<std::string> parse_csv(const std::string& s) {
  std::vector<std::string> result;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t comma = s.find(',', pos);
    if (comma == std::string::npos) comma = s.size();
    std::string item = s.substr(pos, comma - pos);
    // Trim whitespace.
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    while (!item.empty() && item.back() == ' ')  item.pop_back();
    if (!item.empty()) result.push_back(std::move(item));
    pos = comma + 1;
  }
  return result;
}

// ============================================================
// System information (logged at startup into the ring buffer)
// ============================================================

// Read the installed version of a dpkg package.  Returns empty on failure.
static std::string dpkg_version(const char* pkg) {
  std::string cmd = "dpkg-query -W -f '${Version}' ";
  cmd += pkg;
  cmd += " 2>/dev/null";
  std::FILE* p = popen(cmd.c_str(), "r");  // NOLINT(runtime/int)
  if (!p) return {};
  char buf[128] = {};
  char* ok = std::fgets(buf, sizeof(buf), p);
  pclose(p);  // NOLINT(runtime/int)
  if (!ok) return {};
  // Strip trailing whitespace / newline.
  std::string v(buf);
  while (!v.empty() && (v.back() == '\n' || v.back() == ' '))
    v.pop_back();
  return v;
}

// Read a single-line /proc or /sys file.  Returns empty on failure.
static std::string read_proc_line(const char* path) {
  std::ifstream f(path);
  if (!f) return {};
  std::string line;
  std::getline(f, line);
  // Trim trailing NULs (common in device-tree files).
  while (!line.empty() && line.back() == '\0') line.pop_back();
  return line;
}

static void log_system_info() {
  // Kernel and architecture.
  struct utsname u{};
  if (uname(&u) == 0) {
    LOG(INFO) << "Kernel      : " << u.release
              << " (" << u.machine << ")";
  }

  // CPU cores.
  struct sysinfo si{};
  if (sysinfo(&si) == 0) {
    long pages = sysconf(_SC_PHYS_PAGES);  // NOLINT(runtime/int)
    long psize = sysconf(_SC_PAGE_SIZE);   // NOLINT(runtime/int)
    uint64_t ram_mb = 0;
    if (pages > 0 && psize > 0)
      ram_mb = static_cast<uint64_t>(pages) *
               static_cast<uint64_t>(psize) / (1024 * 1024);
    LOG(INFO) << "Hardware    : " << get_nprocs() << " CPU cores, "
              << ram_mb << " MiB RAM";
  }

  // SoC model from device-tree (e.g. "Annapurna Labs Alpine V2 UBNT").
  std::string soc =
      read_proc_line("/sys/firmware/devicetree/base/model");
  if (!soc.empty())
    LOG(INFO) << "SoC         : " << soc;

  // Key Ubiquiti package versions.
  static const char* kPackages[] = {
      "unifi-core", "unifi-protect", "uos",
      "unifi-native", nullptr};
  for (const char** p = kPackages; *p; ++p) {
    std::string ver = dpkg_version(*p);
    if (!ver.empty())
      LOG(INFO) << "Package     : " << *p << " " << ver;
  }
}

// ============================================================
// Entry point
// ============================================================
int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  // Wire absl's mutex tracer to our per-mutex contention aggregator.
  // Must run before any absl::Mutex traffic — singletons like
  // RawSink and the registered classes self-register from their
  // constructors, but the tracer callback only starts collecting
  // wait-cycle data once installed.
  onvif::ContentionProfiler::install();

  // Always capture INFO+ messages for the in-memory ring buffer.
  // The stderr threshold controls what the user sees in the terminal.
  absl::SetMinLogLevel(absl::LogSeverityAtLeast::kInfo);

  static onvif::LogRing log_ring;
  absl::AddLogSink(&log_ring);

  // Apply config-file overrides BEFORE the first GetFlag read so every
  // subsequent flag access sees the user's saved values from the admin
  // UI.  Empty / missing keys leave the underlying flag untouched.
  const std::string kConfigPath = "/etc/onvif-recorder/config.json";
  runtime_config::LoadFromFile(kConfigPath);

  // Optional periodic CPU sampler.  hz = 0 (default) leaves the
  // sampler dormant; runtime_config can flip it on (the admin
  // Configuration card writes to kConfigPath above).  Must run
  // after LoadFromFile so the flag override is in effect.
  onvif::CpuProfiler::instance().start(absl::GetFlag(FLAGS_cpu_profile_hz));

  const bool verbose = absl::GetFlag(FLAGS_verbose);
  if (verbose) {
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);
  } else {
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kError);
  }

  const std::string db_conn     = absl::GetFlag(FLAGS_db_conn);
  const std::string db_host     = absl::GetFlag(FLAGS_db_host);
  const std::string event_log   = absl::GetFlag(FLAGS_event_log);

  // UBV thumbnail directory: use --ubv_dir if set, otherwise auto-detect
  // the native Protect video directory on Dream Routers / NVRs.
  std::string thumbs_dir = absl::GetFlag(FLAGS_ubv_dir);
  if (thumbs_dir.empty()) {
    static const char kProtectVideoDir[] = "/srv/unifi-protect/video";
    struct stat st{};
    if (stat(kProtectVideoDir, &st) == 0 && S_ISDIR(st.st_mode))
      thumbs_dir = kProtectVideoDir;
  }
  const std::string raw_log     = absl::GetFlag(FLAGS_raw_log);
  const std::string change_log  = absl::GetFlag(FLAGS_change_log);
  const std::string rollback    = absl::GetFlag(FLAGS_rollback);
  const std::string fp_cam_str  = absl::GetFlag(FLAGS_first_party_cameras);
  const std::string fp_model_str =
      absl::GetFlag(FLAGS_first_party_camera_models);

  // --excluded_cameras (issue #42): lower-cased, colon-stripped MAC set.
  // Both the third-party listener add loop and the first-party motion
  // poller add loop consult it.  Normalising both the flag values and
  // each camera's MAC before comparison handles the mix of colonised /
  // bare-hex forms present across the codebase.
  auto norm_mac = [](std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
      if (c == ':' || c == '-' || c == ' ') continue;
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
  };
  std::set<std::string> excluded_macs;
  for (const auto& raw : parse_csv(absl::GetFlag(FLAGS_excluded_cameras))) {
    const std::string m = norm_mac(raw);
    if (!m.empty()) excluded_macs.insert(m);
  }
  if (!excluded_macs.empty()) {
    LOG(INFO) << "[excluded_cameras] " << excluded_macs.size()
              << " camera MAC(s) will be skipped";
  }
  const uint32_t pre_buf_sec    =
      static_cast<uint32_t>(absl::GetFlag(FLAGS_pre_buffer_sec));
  const uint32_t post_buf_sec   =
      static_cast<uint32_t>(absl::GetFlag(FLAGS_post_buffer_sec));
  const uint32_t momentary_sec  =
      static_cast<uint32_t>(absl::GetFlag(FLAGS_momentary_event_sec));

  LOG(INFO) << "ONVIF Event Recorder starting";
  log_system_info();
  LOG(INFO) << "DB conn     : " << db_conn;
  LOG(INFO) << "DB host     : " << (db_host.empty() ? "(default)" : db_host);
  LOG(INFO) << "Pre-buffer  : " << pre_buf_sec << " s";
  LOG(INFO) << "Post-buffer : " << post_buf_sec << " s";
  LOG(INFO) << "Coalesce    : " << absl::GetFlag(FLAGS_coalesce_window_sec) << " s";
  LOG(INFO) << "Max/hr      : " << absl::GetFlag(FLAGS_max_events_per_hour);
  LOG(INFO) << "Event log   : " << (event_log.empty() ? "(disabled)" : event_log);
  LOG(INFO) << "Raw log     : " << (raw_log.empty() ? "(disabled)" : raw_log);
  LOG(INFO) << "Change log  : " << (change_log.empty() ? "(disabled)" : change_log);
  LOG(INFO) << "UBV dir     : " << (thumbs_dir.empty() ? "(disabled)" : thumbs_dir);

  // Camera DB config (shared by all camera-config functions).
  unifi::DbConfig cam_db;
  cam_db.host = db_host.empty() ? "/run/postgresql" : db_host;

  // Parse first-party camera IDs.
  const auto fp_camera_ids = parse_csv(fp_cam_str);

  // ----------------------------------------------------------------
  // Rollback mode: undo cameras-table changes and exit.
  // ----------------------------------------------------------------
  if (!rollback.empty()) {
    if (rollback != "third_party" && rollback != "first_party" &&
        rollback != "all") {
      LOG(ERROR) << "Fatal: --rollback must be 'third_party', "
                 << "'first_party', or 'all' (got '" << rollback << "')";
      return 1;
    }
    // Force verbose for rollback output.
    absl::SetMinLogLevel(absl::LogSeverityAtLeast::kInfo);

    // Cameras-table rollback: best-effort.  On package removal the DB may
    // already be down, but we still want the UI/nginx reverts below to run.
    auto result = unifi::rollback_camera_changes(
        rollback, change_log, fp_camera_ids, cam_db);
    if (!result.ok()) {
      LOG(WARNING) << "[rollback] cameras-table: "
                   << result.status().message();
    } else {
      LOG(INFO) << "[rollback] cameras-table: " << *result
                << " camera(s) updated";
    }

    // Revert Protect UI patches if they were applied.
    auto ui_s = protect_ui::revert_alarm_picker();
    if (!ui_s.ok())
      LOG(WARNING) << "[rollback] " << ui_s.message();

    // Revert nginx log proxy block.
    auto ng_s = protect_ui::revert_nginx_log_proxy();
    if (!ng_s.ok())
      LOG(WARNING) << "[rollback] " << ng_s.message();

    // Revert nginx admin proxy block.
    auto ng_a = protect_ui::revert_nginx_admin_proxy();
    if (!ng_a.ok())
      LOG(WARNING) << "[rollback] " << ng_a.message();

    return 0;
  }

  onvif::global_init();

  // Patch the Protect UI alarm picker to include third-party cameras.
  if (absl::GetFlag(FLAGS_patch_alarm_picker)) {
    auto s = protect_ui::patch_alarm_picker();
    if (s.ok()) {
      LOG(INFO) << "[ui_patch] success";
    } else {
      LOG(WARNING) << "[ui_patch] " << s.message();
    }
  }

  // Start the in-memory log viewer (loopback HTTP, proxied by nginx).
  onvif::LogServer log_server;
  const uint16_t log_port = absl::GetFlag(FLAGS_log_port);
  if (log_server.start(&log_ring, log_port)) {
    LOG(INFO) << "[log_server] listening on 127.0.0.1:" << log_port;
    auto ng = protect_ui::patch_nginx_log_proxy(log_port);
    if (!ng.ok())
      LOG(WARNING) << "[log_server] nginx patch: " << ng.message();
  } else {
    LOG(WARNING) << "[log_server] failed to start on port " << log_port;
  }

  // Discover the Protect API user-id early so the admin server can proxy
  // MSR-stored thumbnails through Protect's local /api/thumbnails/<id>
  // endpoint.  AlarmNotifier (started later) reuses the same value, and
  // also receives the cache_path so it can self-heal across Protect
  // user_id rotations (re-discover on 401, persist back to the cache).
  std::string protect_user_id = absl::GetFlag(FLAGS_protect_user_id);
  const std::string state_dir = absl::GetFlag(FLAGS_state_dir);
  (void)mkdir(state_dir.c_str(), 0755);
  const std::string protect_user_id_cache_path =
      state_dir + "/protect-user-id";
  if (protect_user_id.empty()) {
    struct stat st;
    if (stat(protect_user_id_cache_path.c_str(), &st) != 0) {
      const char kLegacyPath[] = "/root/.config/onvif-recorder-api-key";
      std::ifstream in(kLegacyPath, std::ios::binary);
      if (in.is_open()) {
        std::ofstream out(protect_user_id_cache_path, std::ios::binary);
        if (out.is_open()) {
          out << in.rdbuf();
          LOG(INFO) << "[alarm] migrated cache " << kLegacyPath
                    << " -> " << protect_user_id_cache_path;
        }
      }
    }
    protect_user_id =
        onvif::discover_protect_user_id(protect_user_id_cache_path);
  }
  // Single shared provider used by AlarmNotifier, motion_poller and the
  // admin server's thumbnail proxy.  Owns the user_id and the on-disk
  // cache, and re-queries unifi-core (rate-limited) when any consumer
  // observes a 401 from the Protect API.
  onvif::ProtectUserIdProvider protect_user_id_provider(
      protect_user_id, protect_user_id_cache_path);

  // Start the admin page (loopback HTTP, proxied at /onvif/admin/).
  onvif::AdminServer admin_server;
  admin_server.set_log_ring(&log_ring);
  const uint16_t admin_port = absl::GetFlag(FLAGS_admin_port);
  const std::string channel_file = absl::GetFlag(FLAGS_channel_file);
  if (admin_server.start(ONVIF_RECORDER_VERSION, channel_file, admin_port,
                         kConfigPath, cam_db,
                         absl::GetFlag(FLAGS_protect_url),
                         &protect_user_id_provider,
                         event_log,
                         [] {
                           return g_listener
                             ? g_listener->healths()
                             : std::vector<onvif::CameraHealth>{};
                         })) {
    LOG(INFO) << "[admin_server] listening on 127.0.0.1:" << admin_port;
    auto ng_a = protect_ui::patch_nginx_admin_proxy(admin_port);
    if (!ng_a.ok())
      LOG(WARNING) << "[admin_server] nginx patch: " << ng_a.message();
  } else {
    LOG(WARNING) << "[admin_server] failed to start on port " << admin_port;
  }

  // Self-heal across UniFi OS firmware-overlay updates that bypass apt.
  // (apt-driven Protect upgrades fire our dpkg trigger, which restarts
  // the service and re-runs the startup patch pass; the inotify watcher
  // covers the non-dpkg case.)  Re-runs are idempotent: each patch
  // detects "already patched" and no-ops.
  std::unique_ptr<protect_ui::PatchWatcher> patch_watcher;
  if (absl::GetFlag(FLAGS_patch_alarm_picker)) {
    std::vector<protect_ui::WatchSpec> specs = {
      {"/usr/share/unifi-protect/app/node_modules/@ubnt/"
           "unifi-protect-ui-internal/dist",
       {"swai", "vantage"}},
      {"/usr/share/unifi-protect/app", {"service"}},
      {"/data/unifi-core/config/http", {"site-local-ip"}},
      // /etc/nginx/nginx.conf is regenerated by unrelated UBNT
      // maintenance oneshots (observed: postgresql-cluster-14-protect-
      // upgrade.service).  If a rewrite ever drops the `include
      // site-local-ip.conf` line, our block in site-local-ip.conf
      // becomes an orphan.  Re-running the patcher on any nginx.conf
      // change re-verifies + reinjects both blocks.
      {"/etc/nginx", {"nginx"}},
    };
    patch_watcher = std::make_unique<protect_ui::PatchWatcher>(
        std::move(specs),
        [log_port, admin_port]() {
          auto sp = protect_ui::patch_alarm_picker();
          if (!sp.ok())
            LOG(WARNING) << "[patch_watcher] alarm picker: " << sp.message();
          auto sl = protect_ui::patch_nginx_log_proxy(log_port);
          if (!sl.ok())
            LOG(WARNING) << "[patch_watcher] nginx log: " << sl.message();
          auto sa = protect_ui::patch_nginx_admin_proxy(admin_port);
          if (!sa.ok())
            LOG(WARNING) << "[patch_watcher] nginx admin: " << sa.message();
        });
    if (!patch_watcher->start())
      LOG(WARNING) << "[patch_watcher] failed to start";
  }

  // Open the change log if configured.
  std::unique_ptr<unifi::CameraChangeLog> change_log_storage;
  unifi::CameraChangeLog* cam_log = nullptr;
  if (!change_log.empty()) {
    auto cl_or = unifi::CameraChangeLog::Create(change_log);
    if (!cl_or.ok()) {
      LOG(ERROR) << "Fatal: " << cl_or.status().message();
      onvif::global_cleanup();
      return 1;
    }
    change_log_storage = std::move(*cl_or);
    cam_log = change_log_storage.get();
  }

  // EventRecorder (optional)
  std::unique_ptr<onvif::EventRecorder> event_rec_storage;
  onvif::EventRecorder* event_rec = nullptr;
  if (!event_log.empty()) {
    auto er_or = onvif::EventRecorder::Create(event_log);
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
  det_rec.set_momentary_event_sec(momentary_sec);
  det_rec.set_coalesce_window(
      static_cast<uint32_t>(absl::GetFlag(FLAGS_coalesce_window_sec)));
  det_rec.set_max_events_per_hour(
      static_cast<uint32_t>(absl::GetFlag(FLAGS_max_events_per_hour)));

  // Object type configuration.
  det_rec.set_default_object_type(absl::GetFlag(FLAGS_default_object_type));
  det_rec.set_drop_unclassified_motion(
      absl::GetFlag(FLAGS_drop_unclassified_motion));

  // Helper to walk a comma-separated `ip=value` list and call a per-pair
  // callback.  Used by --camera_object_types / --camera_coalesce_window_sec
  // / --camera_snapshot_urls below.
  auto for_each_ip_value =
      [&](const std::string& csv,
          const std::function<void(const std::string&,
                                   const std::string&)>& fn) {
    size_t pos = 0;
    while (pos < csv.size()) {
      size_t comma = csv.find(',', pos);
      if (comma == std::string::npos) comma = csv.size();
      const std::string pair = csv.substr(pos, comma - pos);
      const size_t eq = pair.find('=');
      if (eq != std::string::npos)
        fn(pair.substr(0, eq), pair.substr(eq + 1));
      pos = comma + 1;
    }
  };

  for_each_ip_value(absl::GetFlag(FLAGS_camera_object_types),
      [&](const std::string& ip, const std::string& v) {
        det_rec.set_camera_object_type(ip, v);
      });
  for_each_ip_value(absl::GetFlag(FLAGS_camera_coalesce_window_sec),
      [&](const std::string& ip, const std::string& v) {
        const uint32_t sec = static_cast<uint32_t>(std::atoi(v.c_str()));
        det_rec.set_camera_coalesce_window(ip, sec);
      });
  for_each_ip_value(absl::GetFlag(FLAGS_camera_snapshot_urls),
      [&](const std::string& ip, const std::string& v) {
        det_rec.set_camera_snapshot_url_path(ip, v);
      });
  det_rec.set_protect_snapshot_source(absl::GetFlag(FLAGS_protect_url),
                                       &protect_user_id_provider);
  {
    const std::string list = absl::GetFlag(FLAGS_camera_snapshot_via_protect);
    for (const auto& ip : parse_csv(list)) {
      if (!ip.empty()) det_rec.set_camera_snapshot_via_protect(ip);
    }
  }

  // Load NanoDet-M for thumbnail subject cropping, downloading if needed.
  std::unique_ptr<object_detect::ObjectDetector> detector;
  const std::string model_dir       = absl::GetFlag(FLAGS_model_dir);
  const bool        detect          = absl::GetFlag(FLAGS_detect);
  const bool        detect_override = absl::GetFlag(FLAGS_detect_override);
  if ((detect || detect_override) && !model_dir.empty()) {
    if (!object_detect::ObjectDetector::EnsureModels(model_dir)) {
      LOG(WARNING) << "[detect] model files missing and download failed";
    }
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

  // Always use 24-char hex IDs.  MSR builds its thumbnail index from
  // frames it writes itself; a UBV file dropped on disk is invisible to
  // it, so non-24-char IDs route to MSR TCP and fail with error 414.
  // 24-char hex IDs route to the thumbnails DB table, which we populate
  // directly and which works on all Protect versions.
  const bool use_msr_thumb_ids = false;
  det_rec.set_use_msr_thumbnail_ids(use_msr_thumb_ids);

  // ----------------------------------------------------------------
  // Load third-party cameras from the UniFi Protect database.
  // ----------------------------------------------------------------
  auto cams_or = unifi::load_cameras(cam_db);
  if (!cams_or.ok()) {
    LOG(ERROR) << "Fatal: " << cams_or.status().message();
    onvif::global_cleanup();
    return 1;
  }
  auto cameras = std::move(*cams_or);

  if (auto s = unifi::enable_smart_detect(cameras, cam_db, cam_log); !s.ok()) {
    LOG(ERROR) << "Fatal: " << s.message();
    onvif::global_cleanup();
    return 1;
  }

  if (auto s = unifi::ensure_smart_detect_zones(cameras, cam_db, cam_log);
      !s.ok()) {
    LOG(ERROR) << "Fatal: " << s.message();
    onvif::global_cleanup();
    return 1;
  }

  {
    const std::string rtsp_audio = absl::GetFlag(FLAGS_rtsp_audio);
    if (rtsp_audio == "enable" || rtsp_audio == "disable") {
      const bool enable = (rtsp_audio == "enable");
      if (auto s = unifi::set_rtsp_audio(enable, cam_db, cam_log); !s.ok()) {
        LOG(ERROR) << "Fatal: " << s.message();
        onvif::global_cleanup();
        return 1;
      }
      LOG(INFO) << "[rtsp_audio] " << (enable ? "enabled" : "disabled")
                << " RTSP audio for all cameras with audio capability";
    }
  }

  // ----------------------------------------------------------------
  // First-party camera flag modification
  //   (opt-in via --first_party_cameras and/or --first_party_camera_models).
  // ----------------------------------------------------------------
  // Scoped at function level so the same set of cameras can be fed to the
  // motion poller below.  Otherwise flipping `hasSmartDetect=true` via
  // enable_smart_detect would make these cameras invisible to
  // load_all_nonsmartdetect_first_party, and they'd get flagged as
  // smart-capable in the UI without any software actually classifying
  // their events.
  std::vector<unifi::FirstPartyCamera> fp_cams;
  {
    // Collect cameras from explicit IDs.
    if (!fp_camera_ids.empty()) {
      auto fp_or = unifi::load_first_party_cameras(fp_camera_ids, cam_db);
      if (!fp_or.ok()) {
        LOG(ERROR) << "Warning: " << fp_or.status().message();
      } else {
        fp_cams = std::move(*fp_or);

        // Warn about any IDs that were not found in the DB.
        std::set<std::string> found;
        for (const auto& c : fp_cams) found.insert(c.id);
        for (const auto& id : fp_camera_ids) {
          if (found.find(id) == found.end()) {
            LOG(ERROR) << "[first_party] camera ID " << id
                       << " from --first_party_cameras was NOT found in the "
                       << "cameras table (not adopted or does not exist)";
          }
        }
      }
    }

    // Collect cameras from model substring matches.
    const auto fp_model_subs = parse_csv(fp_model_str);
    if (!fp_model_subs.empty()) {
      auto fp_model_or =
          unifi::load_first_party_cameras_by_model(fp_model_subs, cam_db);
      if (!fp_model_or.ok()) {
        LOG(ERROR) << "Warning: " << fp_model_or.status().message();
      } else {
        const auto& model_cams = *fp_model_or;
        LOG(INFO) << "[first_party] --first_party_camera_models matched "
                  << model_cams.size() << " camera(s)";
        for (const auto& c : model_cams)
          LOG(INFO) << "[first_party]   " << c.id << " (" << c.name << ")";

        // Merge, deduplicating by ID.
        std::set<std::string> existing;
        for (const auto& c : fp_cams) existing.insert(c.id);
        for (const auto& c : model_cams) {
          if (existing.find(c.id) == existing.end()) {
            fp_cams.push_back(c);
            existing.insert(c.id);
          }
        }
      }
    }

    if (!fp_cams.empty()) {
      LOG(INFO) << "[first_party] " << fp_cams.size()
                << " camera(s) matched for flag modification";
      for (const auto& c : fp_cams)
        LOG(INFO) << "[first_party]   " << c.id << " (" << c.name << ")";

      if (auto s = unifi::enable_smart_detect(fp_cams, cam_db, cam_log);
          !s.ok()) {
        LOG(ERROR) << "Warning: " << s.message();
      }
      if (auto s = unifi::ensure_smart_detect_zones(fp_cams, cam_db, cam_log);
          !s.ok()) {
        LOG(ERROR) << "Warning: " << s.message();
      }
    }
  }

  // ----------------------------------------------------------------
  // First-party camera motion polling.
  //
  // Watch list is the union of:
  //   - cameras auto-discovered as not having smart detect (genuine
  //     motion-only models), and
  //   - cameras forced via --first_party_cameras / --first_party_camera_models
  //     above, which now carry hasSmartDetect=true but still need our
  //     software to classify their events.
  // ----------------------------------------------------------------
  // First-party camera IDs the recorder is responsible for keeping pinned
  // in Protect's featureFlags (i.e. that need enable_smart_detect to take
  // effect and stay in effect).  Populated below alongside the motion
  // poller watch list; read after the block by the CamerasChangeListener.
  std::set<std::string> first_party_managed_ids;

  std::unique_ptr<onvif::MotionPoller> motion_poller;
  {
    std::vector<std::string> fp_ids;
    std::map<std::string, std::string> fp_macs;
    std::set<std::string> seen;

    auto add_camera = [&](const std::string& id,
                          const std::string& name,
                          const std::string& mac) {
      if (id.empty() || !seen.insert(id).second) return;
      if (!mac.empty() && excluded_macs.count(norm_mac(mac))) {
        LOG(INFO) << "[excluded_cameras] skipping first-party "
                  << name << " (" << id << ", mac=" << mac << ")";
        return;
      }
      fp_ids.push_back(id);
      first_party_managed_ids.insert(id);
      if (!mac.empty()) fp_macs[id] = mac;
      LOG(INFO) << "[motion_poller]   " << id << " (" << name << ")";
    };

    for (const auto& c : fp_cams) add_camera(c.id, c.name, c.mac);

    auto fp_all_or = unifi::load_all_nonsmartdetect_first_party(cam_db);
    if (fp_all_or.ok()) {
      for (const auto& c : *fp_all_or) add_camera(c.id, c.name, c.mac);
    } else {
      LOG(WARNING) << "[motion_poller] "
                   << fp_all_or.status().message();
    }

    if (!fp_ids.empty() && detector) {
      LOG(INFO) << "[motion_poller] watching " << fp_ids.size()
                << " first-party camera(s)";
      auto mp_or = onvif::MotionPoller::Create(db_conn);
      if (mp_or.ok()) {
        motion_poller = std::move(*mp_or);
        motion_poller->set_camera_ids(fp_ids);
        motion_poller->set_camera_macs(fp_macs);
        motion_poller->set_detector(detector.get());
        if (!thumbs_dir.empty())
          motion_poller->set_ubv_dir(thumbs_dir);
        motion_poller->set_poll_interval(
            absl::GetFlag(FLAGS_poll_interval_sec));
        motion_poller->set_push_mode(absl::GetFlag(FLAGS_motion_push));
        motion_poller->set_coalesce_window(
            static_cast<uint32_t>(absl::GetFlag(FLAGS_coalesce_window_sec)));
        motion_poller->set_use_msr_thumbnail_ids(use_msr_thumb_ids);
        motion_poller->set_protect_api(
            absl::GetFlag(FLAGS_protect_url), &protect_user_id_provider);
        motion_poller->set_backfill_lookback_days(
            absl::GetFlag(FLAGS_motion_backfill_days));
        motion_poller->set_always_smart_detect(
            absl::GetFlag(FLAGS_first_party_always_smart_detect),
            absl::GetFlag(FLAGS_first_party_fallback_class));
        // Time-travel sampling: global cap + per-camera overrides (camera-id
        // keyed, parallel to the IP-keyed detection_recorder flags above).
        std::map<std::string, int> per_cam_video_secs;
        for_each_ip_value(absl::GetFlag(FLAGS_camera_video_sample_secs),
            [&](const std::string& id, const std::string& v) {
              per_cam_video_secs[id] = std::atoi(v.c_str());
            });
        motion_poller->set_video_sample_secs(
            absl::GetFlag(FLAGS_motion_video_sample_secs),
            per_cam_video_secs);
      } else {
        LOG(ERROR) << "[motion_poller] " << mp_or.status().message();
      }
    } else if (!fp_ids.empty() && !detector) {
      LOG(INFO) << "[motion_poller] " << fp_ids.size()
                << " first-party camera(s) found but --detect/--model_dir "
                << "not set; motion polling disabled";
    }
  }

  // AlarmNotifier: triggers Protect automations (e.g. chime play) on
  // detections.  user-id was discovered earlier (above admin_server.start).
  std::unique_ptr<onvif::AlarmNotifier> alarm_notifier;
  if (!protect_user_id_provider.empty()) {
    alarm_notifier = std::make_unique<onvif::AlarmNotifier>(
        absl::GetFlag(FLAGS_protect_url), &protect_user_id_provider, db_conn);
    alarm_notifier->refresh_alarms();
    det_rec.set_alarm_notifier(alarm_notifier.get());
  } else {
    // WARNING (not INFO) because the failure mode is silent to the
    // user: third-party detection events still record fine, but no
    // automations / push notifications fire, and there's no other
    // visible symptom.  Bumping the level makes it grep-able in
    // journalctl without --verbose (issue #41).  Pair it with the
    // reporter's manual override for setups where auto-discovery
    // can't work.
    LOG(WARNING) << "[alarm] user ID not found; automation triggers "
                    "disabled (pass --protect_user_id=<UUID> to override; "
                    "see issue #41)";
  }
  if (motion_poller && alarm_notifier)
    motion_poller->set_alarm_notifier(alarm_notifier.get());

  // MsrClient: forwards thumbnails to MSR so Protect serves them via MSP TCP.
  std::unique_ptr<onvif::MsrClient> msr_client;
  {
    const std::string msr_url = absl::GetFlag(FLAGS_msr_url);
    if (!msr_url.empty()) {
      msr_client = std::make_unique<onvif::MsrClient>(msr_url);
      // Apply the issue-#24 protections: single-flight, cool-down, and
      // burst-window coalescing.  All three are flag-gated so an
      // operator can revert to the legacy concurrent path.
      const bool serialize     = absl::GetFlag(FLAGS_msr_serialize);
      const int  cooldown_sec  = absl::GetFlag(FLAGS_msr_suspend_cooldown_sec);
      const bool drop_on_fail  = absl::GetFlag(FLAGS_msr_drop_on_failure);
      const int  burst_ms      = absl::GetFlag(FLAGS_msr_burst_window_ms);
      msr_client->set_serialize(serialize);
      msr_client->set_suspend_cooldown(cooldown_sec);
      det_rec.set_msr_client(msr_client.get());
      det_rec.set_msr_drop_on_failure(drop_on_fail);
      det_rec.set_msr_burst_window_ms(burst_ms > 0 ? burst_ms : 0);
      // First-party path: motion_poller also routes thumbnails through MSR
      // so its smartDetectZone events get MSR-format thumbnailIds (#16/#27).
      if (motion_poller)
        motion_poller->set_msr_client(msr_client.get());
      LOG(INFO) << "[msr] StoreSnapshots forwarding enabled: " << msr_url
                << " serialize=" << (serialize ? "on" : "off")
                << " cooldown_sec=" << cooldown_sec
                << " drop_on_failure=" << (drop_on_fail ? "on" : "off")
                << " burst_window_ms=" << burst_ms;
    }
  }

  // Backfill pre-MSR third-party thumbnails in the background.  Runs on
  // every startup when --msr_url is set; the SQL candidate query filters
  // on LENGTH(thumbnailId)=24, so it becomes a cheap no-op once all
  // eligible events have been migrated.  A background thread keeps the
  // event loop responsive while the migration runs.
  std::thread backfill_thread;
  {
    const int  days  = absl::GetFlag(FLAGS_backfill_msr_thumbnails);
    const bool apply = absl::GetFlag(FLAGS_backfill_apply);
    if (days > 0 && msr_client) {
      onvif::MsrClient* msr_ptr = msr_client.get();
      backfill_thread = std::thread([db_conn, msr_ptr, days, apply]() {
        onvif::msr_backfill::run(db_conn, msr_ptr, days, apply);
      });
    } else if (days > 0 && apply) {
      LOG(INFO) << "[backfill] skipped: --msr_url not set";
    }
  }

  // Optional startup coalescing: merge nearby events already in the database.
  // Purge stuck-open events (end IS NULL older than 5 minutes) left behind
  // by a previous crash or rapid-fire "started" bursts from cameras.
  // Must run before coalesce_history so the NULL-end rows don't interfere.
  {
    const int n = det_rec.purge_stale_open_events(300000);
    if (n > 0)
      LOG(INFO) << "[startup] purged " << n << " stuck-open event(s)";
  }

  // Auto-recover events from on-device backup if the events table looks wiped
  // (oldest event much newer than the oldest recordingFile we're keeping),
  // then run the enricher unconditionally to backfill any restored events
  // that still need the rich-format detail rows (smartDetectObjects /
  // smartDetectObjectAreas).  Both phases are gated by Protect >= 7.1.0
  // inside the library and by --auto_recover_events here.  EnrichRestored
  // is idempotent -- the SELECT it issues filters down to events that
  // actually need work (sparse metadata OR orphaned detectionLabels.objectId
  // without a matching SDO), so on a healthy fully-enriched DB it's a
  // single empty-result query and exits immediately.
  //
  // Runs on a background thread so the main startup path isn't blocked
  // by potentially many minutes of DB work.  The enrich phase issues
  // small SELECTs (LIMIT-paginated, adaptive size, ~2 s target per
  // batch) with a 50 %% duty-cycle sleep between them so the normal
  // Protect DB traffic isn't starved.  Shutdown cooperatively cancels
  // via g_recovery_shutdown at the next batch boundary or inter-batch
  // sleep slice, so the process exits promptly on SIGTERM without
  // having to wait for a whole scan to finish.
  std::thread auto_recover_thread;
  if (absl::GetFlag(FLAGS_auto_recover_events)) {
    const uint64_t thresh_ms =
        static_cast<uint64_t>(absl::GetFlag(FLAGS_auto_recover_threshold_hours))
        * 3'600'000ULL;
    auto_recover_thread = std::thread([db_conn, thresh_ms]() {
      PGconn* rec_conn = PQconnectdb(db_conn.c_str());
      if (PQstatus(rec_conn) != CONNECTION_OK) {
        LOG(WARNING) << "[auto_recover] DB connect failed: "
                     << PQerrorMessage(rec_conn);
        PQfinish(rec_conn);
        return;
      }
      // Phase 1: restore from backup if needed.  Small + fast; not
      // batched.  Skip if shutdown was already requested before the
      // thread got scheduled.
      if (!g_recovery_shutdown.load(std::memory_order_relaxed) &&
          onvif::event_recovery::ShouldRecover(rec_conn, thresh_ms)) {
        auto backup = onvif::event_recovery::FindLatestBackup(
            "/srv/unifi-protect/dbBackups");
        if (backup) {
          LOG(INFO) << "[auto_recover] restoring from " << backup->path;
          auto s = onvif::event_recovery::RestoreFromBackup(rec_conn,
                                                            backup->path);
          if (!s.ok())
            LOG(ERROR) << "[auto_recover] restore failed: " << s.message();
        } else {
          LOG(INFO) << "[auto_recover] no backup file under "
                       "/srv/unifi-protect/dbBackups";
        }
      }
      // Phase 2: enrich any events still missing rich-format detail rows.
      // Runs unconditionally (still version-gated inside) -- catches both
      // the events Phase 1 just restored AND events from earlier restores
      // whose SDO/SDA rows weren't preserved.
      if (!g_recovery_shutdown.load(std::memory_order_relaxed) &&
          onvif::protect_version::IsAtLeast(7, 1, 0)) {
        onvif::event_recovery::EnrichOptions opts;
        opts.cancelled = &g_recovery_shutdown;
        auto s = onvif::event_recovery::EnrichRestored(rec_conn, opts);
        if (!s.ok())
          LOG(ERROR) << "[auto_recover] enrich failed: " << s.message();
      }
      PQfinish(rec_conn);
    });
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
    if (!cam.mac.empty() && excluded_macs.count(norm_mac(cam.mac))) {
      LOG(INFO) << "[excluded_cameras] skipping "
                << cam.ip << " mac=" << cam.mac;
      continue;
    }
    cam.max_consecutive_failures = 3;
    listener.add_camera(cam);
    det_rec.set_snapshot(cam);
    LOG(INFO) << "Watching camera " << cam.ip;
  }

  if (cameras.empty() && !motion_poller) {
    LOG(WARNING) << "No third-party cameras found in Protect and no "
                 << "first-party cameras configured for motion polling; "
                 << "service will stay alive but idle. Add a third-party "
                 << "ONVIF camera in Protect or pass "
                 << "--first_party_cameras / --first_party_camera_models "
                 << "to do useful work.";
  }

  if (!raw_log.empty())
    listener.enable_raw_recording(raw_log);

  // Start the motion poller before the listener (non-blocking).
  if (motion_poller) {
    g_poller = motion_poller.get();
    motion_poller->start();
  }

  // Auto-heal: monitor pg_stats + msr for wedged Protect services and
  // issue targeted `systemctl restart` if the wedge persists >60 s.
  // See --auto_heal_protect flag docs for signal + safeguards.
  std::unique_ptr<onvif::WedgeHealer> healer;
  if (absl::GetFlag(FLAGS_auto_heal_protect)) {
    healer = std::make_unique<onvif::WedgeHealer>();
    healer->set_msr_snapshot_fn([&det_rec]() {
      const auto s = det_rec.msr_snapshot();
      return onvif::WedgeHealer::MsrSnapshot{
          s.total_ok, s.total_fail, s.ms_since_ok, s.ms_since_fail};
    });
    // featureFlags drift check: compare what we wrote into Postgres
    // against what Protect reports through its own API.  A long-running
    // Protect process never re-reads the cameras row, so our writes can
    // be correct in the DB while Protect's in-memory model still says the
    // camera can't smart-detect -- and it then filters our
    // smartDetectZone events out of the UI (issue #34).
    const std::string protect_url_for_drift =
        absl::GetFlag(FLAGS_protect_url);
    healer->set_flag_drift_fn(
        [protect_url_for_drift, &protect_user_id_provider, cam_db](
            const std::vector<std::string>& ids)
            -> std::vector<std::string> {
      std::vector<std::string> drifted;
      if (protect_url_for_drift.empty()) return drifted;
      const std::string user_id = protect_user_id_provider.current();
      if (user_id.empty()) return drifted;

      auto db_or = unifi::read_smart_detect_flags(ids, cam_db);
      if (!db_or.ok()) {
        LOG(WARNING) << "[healer] drift check: " << db_or.status().message();
        return drifted;
      }
      for (const auto& [cam_id, db_json] : *db_or) {
        // We only care about cameras we actually enabled.  An empty
        // array in the DB means we never pinned this one, so there is
        // nothing for Protect to be stale about.
        if (db_json.empty() || db_json == "[]") continue;
        const std::string body = onvif::AlarmNotifier::protect_api_get(
            protect_url_for_drift + "/api/cameras/" + cam_id, user_id);
        if (body.empty()) continue;  // API unreachable; not evidence of drift
        // Protect echoes featureFlags in the camera object.  Comparing
        // for the presence of a non-empty smartDetectTypes array is
        // enough: the drift we are hunting is [] in Protect vs
        // ["person","vehicle"] in Postgres.
        const size_t k = body.find("\"smartDetectTypes\"");
        if (k == std::string::npos) continue;
        const size_t lb = body.find('[', k);
        const size_t rb = (lb == std::string::npos)
                              ? std::string::npos : body.find(']', lb);
        if (lb == std::string::npos || rb == std::string::npos) continue;
        const std::string api_arr = body.substr(lb, rb - lb + 1);
        bool api_empty = true;
        for (size_t i = 1; i + 1 < api_arr.size(); ++i) {
          if (!std::isspace(static_cast<unsigned char>(api_arr[i]))) {
            api_empty = false;
            break;
          }
        }
        if (api_empty) {
          LOG(WARNING) << "[healer] camera " << cam_id
                       << " featureFlags drift: postgres=" << db_json
                       << " protect=" << api_arr;
          drifted.push_back(cam_id);
        }
      }
      return drifted;
    });
    healer->start();
    LOG(INFO) << "[healer] auto-heal enabled (window="
              << onvif::WedgeHealer::kWindowSec << "s, warmup="
              << onvif::WedgeHealer::kWarmupSec << "s, cooldown="
              << onvif::WedgeHealer::kCooldownSec << "s, cap="
              << onvif::WedgeHealer::kMaxPerDay << "/24h)";
    // Arm the drift check for every camera we just pinned flags on at
    // start-up.  This is the exact path in issue #34: the user toggles
    // first-party smart support in the admin UI, which restarts *us*,
    // we write featureFlags into Postgres -- and a Protect that has
    // been up for days never re-reads them.
    if (!first_party_managed_ids.empty()) {
      healer->arm_flag_drift_check(
          std::vector<std::string>(first_party_managed_ids.begin(),
                                    first_party_managed_ids.end()));
      LOG(INFO) << "[healer] armed featureFlags drift check for "
                << first_party_managed_ids.size() << " first-party camera(s)";
    }
  } else {
    LOG(INFO) << "[healer] auto-heal disabled (--auto_heal_protect=false)";
  }

  // CamerasChangeListener: re-flips featureFlags when Protect's controller
  // clobbers our enable_smart_detect writes (typically on adoption,
  // reconnect, and a few minutes into Protect's own start-up).  Pairs with
  // ProtectPidWatcher below, which handles the larger "Protect restarted"
  // case via a self-restart 300 s after Protect stabilises.
  //
  // The managed set is the union of explicit --first_party_cameras IDs
  // and auto-discovered non-smart-detect first-party cameras -- both of
  // which the recorder runs enable_smart_detect on at start-up and needs
  // to keep pinned.  Third-party cameras don't need this because Protect
  // does not re-sync their featureFlags from device capabilities.
  std::unique_ptr<onvif::CamerasChangeListener> cameras_change_listener;
  if (!first_party_managed_ids.empty()) {
    cameras_change_listener = std::make_unique<onvif::CamerasChangeListener>(
        cam_db, first_party_managed_ids, cam_log);
    auto s = cameras_change_listener->start();
    if (!s.ok()) {
      LOG(WARNING) << "[cameras_listener] start failed: " << s.message()
                   << " (continuing without reactive featureFlags pin)";
      cameras_change_listener.reset();
    }
  }

  // ProtectPidWatcher: when unifi-protect's MainPID changes (firmware
  // upgrade, manual restart, crash), wait until the new PID has been
  // alive for 300 s and then SIGTERM ourselves so systemd restarts us
  // cleanly.  The fresh start-up re-runs enable_smart_detect *after*
  // Protect has finished its initial re-sync, so the flip sticks.
  // The 300 s settle window prevents us from crash-looping in step with
  // Protect when Protect itself is crash-looping.
  onvif::ProtectPidWatcher protect_pid_watcher;
  if (!protect_pid_watcher.start())
    LOG(WARNING) << "[protect_watcher] failed to start";

  // Background rescan thread: periodically re-query Protect's cameras
  // table and hot-add any cameras that appear after startup.  Lets users
  // add a third-party camera in Protect without restarting the service.
  std::atomic<bool> rescan_running{true};
  std::set<std::string> known_ids;
  for (const auto& c : cameras) known_ids.insert(c.id);
  std::thread rescan_thread([&]() {
    constexpr int kPollSec = 60;
    while (rescan_running) {
      for (int i = 0; i < kPollSec && rescan_running; ++i)
        std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!rescan_running) break;

      auto reloaded = unifi::load_cameras(cam_db);
      if (!reloaded.ok()) {
        LOG(WARNING) << "[rescan] load_cameras: "
                     << reloaded.status().message();
        continue;
      }
      std::vector<onvif::CameraConfig> fresh;
      for (const auto& c : *reloaded)
        if (!known_ids.count(c.id)) fresh.push_back(c);
      if (fresh.empty()) continue;

      LOG(INFO) << "[rescan] detected " << fresh.size()
                << " new third-party camera(s)";
      if (auto s = unifi::enable_smart_detect(fresh, cam_db, cam_log);
          !s.ok())
        LOG(WARNING) << "[rescan] enable_smart_detect: " << s.message();
      if (auto s = unifi::ensure_smart_detect_zones(fresh, cam_db, cam_log);
          !s.ok())
        LOG(WARNING) << "[rescan] ensure_smart_detect_zones: "
                     << s.message();
      for (auto cam : fresh) {
        cam.max_consecutive_failures = 3;
        known_ids.insert(cam.id);
        det_rec.set_snapshot(cam);
        listener.add_camera_live(cam);
        LOG(INFO) << "Watching camera " << cam.ip;
      }
    }
  });

  listener.run([event_rec, &det_rec](const onvif::OnvifEvent& ev) {
    if (event_rec) event_rec->write(ev);
    det_rec.on_event(ev);
  });

  // Clean shutdown.
  rescan_running = false;
  if (rescan_thread.joinable())
    rescan_thread.join();
  if (motion_poller)
    motion_poller->stop();
  if (healer)
    healer->stop();
  if (backfill_thread.joinable())
    backfill_thread.join();
  // Nudge the background auto-recover thread to exit and wait for it.
  // The signal handler already flipped g_recovery_shutdown on SIGTERM /
  // SIGINT, but if we're here via a clean listener.run() return we need
  // to set it ourselves so the loop bails at the next batch/sleep
  // boundary instead of running to completion.
  g_recovery_shutdown.store(true, std::memory_order_relaxed);
  if (auto_recover_thread.joinable())
    auto_recover_thread.join();
  g_poller   = nullptr;
  g_listener = nullptr;
  onvif::global_cleanup();
  LOG(INFO) << "Done";
  return 0;
}
