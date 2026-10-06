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

#include "motion_poller.hpp"

#include <curl/curl.h>
#include <libpq-fe.h>
#include <sys/select.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "alarm_notifier.hpp"
#include "event_enricher.hpp"
#include "jpeg_crop.hpp"
#include "msr_client.hpp"
#include "object_detect.hpp"
#include "pg_util.hpp"
#include "protect_user_id_provider.hpp"
#include "protect_version.hpp"
#include "ubv_thumbnail.hpp"
#include "util.hpp"

namespace onvif {

namespace {

// LISTEN/NOTIFY channel that the events-INSERT trigger publishes to.
// The trigger is idempotent (CREATE OR REPLACE + DROP IF EXISTS) so the
// motion poller can reinstall it on every start without side effects.
constexpr const char* kMotionChannel = "onvif_recorder_motion";

constexpr const char* kMotionTriggerFnSql = R"(
CREATE OR REPLACE FUNCTION onvif_recorder_motion_notify()
  RETURNS TRIGGER
  LANGUAGE plpgsql
  AS $body$
BEGIN
  IF NEW.type = 'motion' THEN
    PERFORM pg_notify('onvif_recorder_motion', NEW.id);
  END IF;
  RETURN NULL;
END;
$body$;
)";

constexpr const char* kMotionTriggerDropSql = "DROP TRIGGER IF EXISTS onvif_recorder_motion ON events";  // NOLINT(whitespace/line_length)

constexpr const char* kMotionTriggerCreateSql = R"(
CREATE TRIGGER onvif_recorder_motion
  AFTER INSERT ON events
  FOR EACH ROW
  EXECUTE FUNCTION onvif_recorder_motion_notify();
)";

// Safety-net poll timeout when running in push mode.  If NOTIFY drops on
// the floor (Postgres does not queue notifications for disconnected
// listeners), the socket wait falls through after this many milliseconds
// and forces one poll cycle regardless.
constexpr int kSafetyNetTimeoutMs = 60'000;

// Install the events-INSERT trigger + LISTEN on @p conn.  Returns true on
// success; on failure logs a warning and returns false so the caller can
// fall back to the classic interval poll.  Idempotent -- calling twice
// simply refreshes the trigger.
bool try_install_motion_trigger_and_listen(PGconn* conn) {
  auto run = [conn](const char* sql, const char* label) -> bool {
    PGresult* r = onvif::pg::ExecWithTimeout(conn, -1, sql);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
      LOG(WARNING) << "[motion_poller] " << label << ": "
                   << PQresultErrorMessage(r)
                   << " -- falling back to interval poll";
      PQclear(r);
      return false;
    }
    PQclear(r);
    return true;
  };
  if (!run(kMotionTriggerFnSql,     "trigger function"))    return false;
  if (!run(kMotionTriggerDropSql,   "trigger drop"))        return false;
  if (!run(kMotionTriggerCreateSql, "trigger create"))      return false;
  if (!run("LISTEN onvif_recorder_motion", "LISTEN"))       return false;
  LOG(INFO) << "[motion_poller] push mode active: LISTEN "
            << kMotionChannel;
  return true;
}

// Wait for either a NOTIFY on the connection socket or @p timeout_ms
// elapsing, whichever comes first.  Returns quickly on stop() by polling
// the running flag in 1-second slices.  Notifications are drained
// (their id payloads are not needed -- the subsequent poll query is
// hwm-driven and picks up whatever landed).
void wait_for_notify_or_timeout(PGconn* conn,
                                 int timeout_ms,
                                 const std::atomic<bool>& running) {
  // libpq's query machinery drains the socket internally, so any
  // NOTIFY that arrives during the preceding poll SELECT is already
  // queued in the connection's notification list -- select() would
  // block for the full timeout waiting for more socket bytes.  Drain
  // the queue first; if anything is there, return immediately so the
  // next poll cycle picks it up.
  {
    PGnotify* n = nullptr;
    bool got_any = false;
    while ((n = PQnotifies(conn)) != nullptr) {
      PQfreemem(n);
      got_any = true;
    }
    if (got_any) return;
  }
  const int sock = PQsocket(conn);
  const auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(timeout_ms);
  while (running.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return;
    const int remaining_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count());
    // Cap the individual select() at 1 s so stop() gets promptly noticed.
    const int slice_ms = std::min(1000, remaining_ms);
    fd_set rfds;
    FD_ZERO(&rfds);
    if (sock >= 0) FD_SET(sock, &rfds);
    timeval tv{slice_ms / 1000, (slice_ms % 1000) * 1000};
    int rc = ::select(sock >= 0 ? sock + 1 : 0,
                      sock >= 0 ? &rfds : nullptr,
                      nullptr, nullptr, &tv);
    if (rc < 0) {
      if (errno == EINTR) continue;
      return;  // caller re-tries the loop, which reconnects on demand
    }
    if (rc == 0) continue;  // timeout slice -- loop and check running
    if (!PQconsumeInput(conn)) return;  // conn broken; caller reconnects
    PGnotify* n = nullptr;
    bool got_any = false;
    while ((n = PQnotifies(conn)) != nullptr) {
      PQfreemem(n);
      got_any = true;
    }
    if (got_any) return;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Helpers (exposed via namespace motion_poller_internal for testing)
// ---------------------------------------------------------------------------

namespace motion_poller_internal {

std::string smart_detect_types_json(const std::string& det_type) {
  if (det_type == "vehicle") return "[\"vehicle\"]";
  if (det_type == "animal")  return "[\"animal\"]";
  if (det_type == "package") return "[\"package\"]";
  return "[\"person\"]";
}

std::string build_sdr_payload(uint64_t ts_ms, const std::string& obj_type,
                              int confidence) {
  const std::string ts = std::to_string(ts_ms);
  const std::string conf_str = std::to_string(confidence);
  return
      std::string("{")
      + "\"attributesForTracks\":null,"
      + "\"clockStream\":" + ts + ","
      + "\"clockStreamRate\":1000,"
      + "\"clockWall\":" + ts + ","
      + "\"descriptors\":[{"
      + "\"attributes\":null,"
      + "\"confidence\":" + conf_str + ","
      + "\"coord\":[-1.0,-1.0,-1.0,-1.0],"
      + "\"coord3d\":[-1.0,-1.0],"
      + "\"depth\":null,"
      + "\"duration\":0,"
      + "\"firstShownTimeMs\":" + ts + ","
      + "\"id\":\"1\","
      + "\"idleSinceTimeMs\":" + ts + ","
      + "\"licensePlate\":null,"
      + "\"lines\":[],"
      + "\"loiterZones\":[],"
      + "\"matchedId\":null,"
      + "\"name\":\"\","
      + "\"objectType\":\"" + obj_type + "\","
      + "\"speed\":null,"
      + "\"stationary\":false,"
      + "\"timestamp\":" + ts + ","
      + "\"zones\":[]"
      + "}],"
      + "\"edgeType\":\"none\","
      + "\"linesStatus\":null,"
      + "\"loiterZonesStatus\":null,"
      + "\"smartDetectSnapshotFullFoV\":\"\","
      + "\"smartDetectSnapshots\":null,"
      + "\"tamperStatus\":null,"
      + "\"trackerIdAttrMap\":null,"
      + "\"zonesStatus\":{}"
      + "}";
}

std::string build_sdo_attributes(const std::string& obj_type, int confidence) {
  // Mirrors the attributes column shape that native first-party
  // smartDetectZone events write.  Nulls are sentinels Protect's UI /
  // iOS app expect to be present (not absent).  trackerId=1 because we
  // only emit one detection per event.  zone defaults to [1]: the iOS
  // app dereferences attributes.zone[0] without checking length, so an
  // empty array crashes it.  Native events always carry at least the
  // default zone id [1]; we match that here until we propagate real
  // smartDetectZones intersections.
  return std::string("{")
      + "\"associatedFaceTrackerID\":null,"
      + "\"blurness\":null,"
      + "\"color\":null,"
      + "\"confidence\":" + std::to_string(confidence) + ","
      + "\"faceEmbed\":null,"
      + "\"faceLandmarks\":null,"
      + "\"faceMask\":null,"
      + "\"facePose\":null,"
      + "\"faceVerifyStatus\":null,"
      + "\"line\":null,"
      + "\"matchedId\":null,"
      + "\"matchedName\":null,"
      + "\"namesTopK\":null,"
      + "\"objectType\":\"" + obj_type + "\","
      + "\"personEmbedFromCamera\":null,"
      + "\"qualityScore\":null,"
      + "\"topKCandidate\":null,"
      + "\"trackerId\":1,"
      + "\"vehicleType\":null,"
      + "\"zone\":[1]"
      + "}";
}

int select_best_candidate_index(
    const std::vector<std::string>& baseline_classes,
    const std::vector<Candidate>& event_candidates) {
  // Baseline lookup is O(N) per candidate -- baseline is usually 0-3
  // entries so a hash set would be premature optimisation.
  auto baseline_contains = [&](const std::string& cls) {
    for (const auto& b : baseline_classes)
      if (b == cls) return true;
    return false;
  };
  int best_idx = -1;
  int best_conf = -1;
  for (size_t i = 0; i < event_candidates.size(); ++i) {
    const auto& c = event_candidates[i];
    if (baseline_contains(c.obj_type)) continue;
    if (c.confidence_pct > best_conf) {
      best_conf = c.confidence_pct;
      best_idx = static_cast<int>(i);
    }
    // strictly > so the lowest index wins ties (earliest frame).
  }
  return best_idx;
}

std::string build_sdt_payload(uint64_t start_ms,
                               uint64_t end_ms,
                               const std::string& obj_type,
                               int confidence) {
  // Native Protect writes one row in smartDetectTracks per event,
  // payload = JSON array of track samples.  We only have one detection
  // per event so the array carries a single entry.  duration is in
  // seconds in native rows; coord is unknown (we don't propagate
  // pixel-space bbox here) so use the same [-1, -1, -1, -1] sentinel
  // smartDetectRaws uses.
  const uint64_t duration_sec =
      (end_ms > start_ms) ? (end_ms - start_ms) / 1000 : 0;
  const std::string start_str = std::to_string(start_ms);
  const std::string end_str   = std::to_string(end_ms);
  const std::string conf_str  = std::to_string(confidence);
  const std::string dur_str   = std::to_string(duration_sec);
  return std::string("[{")
      + "\"attributes\":null,"
      + "\"confidence\":" + conf_str + ","
      + "\"coord\":[-1.0,-1.0,-1.0,-1.0],"
      + "\"coord3d\":[-1.0,-1.0],"
      + "\"depth\":null,"
      + "\"duration\":" + dur_str + ","
      + "\"firstShownTimeMs\":" + start_str + ","
      + "\"id\":\"1\","
      + "\"idleSinceTimeMs\":0,"
      + "\"licensePlate\":null,"
      + "\"lines\":[],"
      + "\"loiterZones\":[],"
      + "\"matchedId\":null,"
      + "\"name\":\"\","
      + "\"objectType\":\"" + obj_type + "\","
      + "\"speed\":null,"
      + "\"stationary\":false,"
      + "\"timestamp\":" + end_str + ","
      + "\"zones\":[]"
      + "}]";
}

}  // namespace motion_poller_internal

namespace {

// One-shot HTTP GET to the Protect API.  Returns (http_code, body).
// http_code == 0 indicates a curl-level failure (network/timeout).
struct ProtectGetResult {
  long code;  // NOLINT(runtime/int)
  std::vector<uint8_t> body;
};
ProtectGetResult perform_protect_get(const std::string& base_url,
                                     const std::string& user_id,
                                     const std::string& url_path) {
  ProtectGetResult out{0, {}};
  if (base_url.empty() || user_id.empty()) return out;
  const std::string url = base_url + url_path;
  std::string buf;
  CURL* curl = curl_easy_init();
  if (!curl) return out;
  struct curl_slist* hdrs = nullptr;
  const std::string user_hdr = "X-UserId: " + user_id;
  hdrs = curl_slist_append(hdrs, user_hdr.c_str());
  hdrs = curl_slist_append(hdrs, "X-Source: unifi-os");
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
      +[](char* p, size_t s, size_t n, void* ud) -> size_t {
        static_cast<std::string*>(ud)->append(p, s * n);
        return s * n;
      });
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
  const CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.code);
  }
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  if (out.code == 200) {
    out.body.assign(buf.begin(), buf.end());
  }
  return out;
}

// GET <protect_url>/api/thumbnails/<id> with X-UserId auth.  Returns
// the JPEG bytes on 200, or an empty vector on any error / non-200.
// Used to pull MSR-format (non-24-char) thumbnails that Protect stores
// as native UBV files served by the msp media server, since those never
// land in the Postgres `thumbnails` table.
//
// On observed 401, asks the provider to re-discover the user_id and
// retries the request once with the new value.
std::vector<uint8_t> fetch_thumbnail_via_protect(
    const std::string& base_url,
    ProtectUserIdProvider* provider,
    const std::string& thumb_id) {
  if (base_url.empty() || !provider) return {};
  const std::string path = "/api/thumbnails/" + thumb_id;
  ProtectGetResult r = perform_protect_get(base_url, provider->current(), path);
  if (r.code == 401 && provider->try_refresh()) {
    LOG(INFO) << "[motion_poller] retrying thumbnail GET after user_id refresh";
    r = perform_protect_get(base_url, provider->current(), path);
  }
  return r.body;
}

// GET <protect_url>/api/cameras/<cam_id>/snapshot?ts=<ms>
//
// Returns the camera's full-FOV JPEG at @p ts_ms (or live frame if no
// recording exists at that timestamp).  Used by the motion poller to look
// at the moment the camera *actually* detected motion -- start + pre-pad
// -- rather than Protect's cropped/late thumbnailId, which can pick a
// 360x360 region around its guess at the motion centroid and miss the
// subject when its algorithm is wrong.
std::vector<uint8_t> fetch_camera_snapshot_at_ts(
    const std::string& base_url,
    ProtectUserIdProvider* provider,
    const std::string& cam_id,
    uint64_t ts_ms) {
  if (base_url.empty() || !provider || cam_id.empty()) return {};
  const std::string path = "/api/cameras/" + cam_id +
                           "/snapshot?ts=" + std::to_string(ts_ms);
  ProtectGetResult r = perform_protect_get(base_url, provider->current(), path);
  if (r.code == 401 && provider->try_refresh()) {
    LOG(INFO) << "[motion_poller] retrying snapshot GET after user_id refresh";
    r = perform_protect_get(base_url, provider->current(), path);
  }
  return r.body;
}

}  // namespace

// ---------------------------------------------------------------------------
// Implementation struct
// ---------------------------------------------------------------------------

struct MotionPoller::Impl {
  PGconn* conn{nullptr};
  std::vector<std::string> camera_ids;
  std::map<std::string, std::string> id_to_mac;
  const object_detect::ObjectDetector* detector{nullptr};
  AlarmNotifier* alarm_notifier{nullptr};
  std::string ubv_dir;
  std::string protect_url;       // empty = HTTP-fallback disabled
  ProtectUserIdProvider* protect_user_id_provider{nullptr};
  int poll_interval_sec{10};
  bool push_mode{true};
  bool trigger_installed{false};    // true once LISTEN is up on impl_->conn
  uint32_t coalesce_window_sec{30};
  bool use_msr_thumb_ids{false};
  MsrClient* msr_client{nullptr};

  // UBV path cache: MAC -> (date string, file path).
  std::map<std::string, std::pair<std::string, std::string>> ubv_cache;

  // High-water mark: last processed motion event start per camera.
  std::map<std::string, uint64_t> hwm;
  int backfill_lookback_days{0};
  bool always_smart_detect{true};
  std::string fallback_object_type{"person"};

  // Time-travel sampling: global cap and per-camera overrides.
  // 0 means "disabled" (poller stays on the legacy single-snapshot path).
  int video_sample_global_secs{0};
  std::map<std::string, int> video_sample_per_camera;

  // Effective N for a given camera + event length, clamped to [1, cap].
  // Returns 0 when sampling is disabled for this camera.
  int effective_sample_secs(const std::string& cam_id,
                             uint64_t start_ms, uint64_t end_ms) const {
    int cap = video_sample_global_secs;
    auto it = video_sample_per_camera.find(cam_id);
    if (it != video_sample_per_camera.end()) cap = it->second;
    if (cap <= 0) return 0;
    const int64_t span_ms = (end_ms > start_ms)
        ? static_cast<int64_t>(end_ms - start_ms) : 0;
    // Round up to whole seconds: a 6.5s event still gets up to 7 samples
    // if the cap allows.
    const int event_secs = static_cast<int>((span_ms + 999) / 1000);
    return std::max(1, std::min(cap, std::max(1, event_secs)));
  }

  ~Impl() {
    if (conn) PQfinish(conn);
  }

  // Attempt to restore a broken connection.
  void maybe_reconnect() {
    if (PQstatus(conn) != CONNECTION_BAD) return;
    LOG(WARNING) << "[motion_poller] connection lost — reconnecting";
    PQreset(conn);
    if (PQstatus(conn) == CONNECTION_OK)
      LOG(INFO) << "[motion_poller] reconnected";
    else
      LOG(ERROR) << "[motion_poller] reconnect failed: "
                 << PQerrorMessage(conn);
  }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

absl::StatusOr<std::unique_ptr<MotionPoller>> MotionPoller::Create(
    const std::string& db_connstr) {
  auto poller = std::unique_ptr<MotionPoller>(new MotionPoller());
  poller->impl_ = std::make_unique<Impl>();
  poller->impl_->conn = PQconnectdb(db_connstr.c_str());
  if (PQstatus(poller->impl_->conn) != CONNECTION_OK) {
    std::string err = PQerrorMessage(poller->impl_->conn);
    return absl::InternalError("MotionPoller: " + err);
  }
  return poller;
}

MotionPoller::~MotionPoller() { stop(); }

void MotionPoller::set_camera_ids(const std::vector<std::string>& ids) {
  impl_->camera_ids = ids;
}

void MotionPoller::set_camera_macs(
    const std::map<std::string, std::string>& id_to_mac) {
  impl_->id_to_mac = id_to_mac;
}

void MotionPoller::set_detector(
    const object_detect::ObjectDetector* detector) {
  impl_->detector = detector;
}

void MotionPoller::set_alarm_notifier(AlarmNotifier* notifier) {
  impl_->alarm_notifier = notifier;
}

void MotionPoller::set_ubv_dir(const std::string& dir) {
  impl_->ubv_dir = dir;
}

void MotionPoller::set_backfill_lookback_days(int days) {
  impl_->backfill_lookback_days = std::max(0, days);
}

void MotionPoller::set_always_smart_detect(bool on,
                                            const std::string& fallback) {
  impl_->always_smart_detect = on;
  impl_->fallback_object_type = fallback.empty() ? "person" : fallback;
}

void MotionPoller::set_poll_interval(int sec) {
  impl_->poll_interval_sec = sec;
}

void MotionPoller::set_push_mode(bool enabled) {
  impl_->push_mode = enabled;
}

void MotionPoller::set_coalesce_window(uint32_t sec) {
  impl_->coalesce_window_sec = sec;
}

void MotionPoller::set_video_sample_secs(
    int global_secs,
    const std::map<std::string, int>& per_camera) {
  impl_->video_sample_global_secs = global_secs;
  impl_->video_sample_per_camera = per_camera;
}

void MotionPoller::set_use_msr_thumbnail_ids(bool use_msr) {
  impl_->use_msr_thumb_ids = use_msr;
}

void MotionPoller::set_msr_client(MsrClient* msr) {
  impl_->msr_client = msr;
}

void MotionPoller::set_protect_api(const std::string& url,
                                   ProtectUserIdProvider* provider) {
  impl_->protect_url = url;
  impl_->protect_user_id_provider = provider;
}

void MotionPoller::start() {
  if (impl_->camera_ids.empty() || !impl_->detector) return;
  running_.store(true);
  thread_ = std::thread([this] { poll_loop(); });
}

void MotionPoller::stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

// ---------------------------------------------------------------------------
// High-water mark initialisation
// ---------------------------------------------------------------------------

void MotionPoller::init_high_water_marks() {
  const uint64_t default_hwm = util::now_ms() - 3600000ULL;  // 1 hour ago

  // Pre-populate every camera's hwm with default_hwm so any camera missing
  // from the aggregate result (no recent smartDetectZone from us) starts
  // out at (now - 1h) without a per-camera round-trip.
  for (const auto& cam_id : impl_->camera_ids)
    impl_->hwm[cam_id] = default_hwm;

  if (impl_->camera_ids.empty()) return;

  // One aggregate round-trip for every camera, bounded to the last 7 days.
  // Without the time bound this hit the `metadata::jsonb->>'source'`
  // expression on every historical smartDetectZone row and repeatedly
  // timed out at 60s per camera on active-events tables (issue #34).
  //
  // Backfill (--motion_backfill_days) still pulls the hwm back further
  // below; we do not need older rows here because the poll loop's
  // NOT EXISTS smartDetectZone filter skips already-classified events.
  std::string cam_array = "{";
  bool first_id = true;
  for (const auto& cam_id : impl_->camera_ids) {
    if (!first_id) cam_array += ',';
    first_id = false;
    cam_array += cam_id;  // 24-char hex, no quoting needed
  }
  cam_array += "}";

  const uint64_t floor_ms = util::now_ms() - 7ULL * 86'400'000ULL;
  const std::string floor_str = std::to_string(floor_ms);

  impl_->maybe_reconnect();
  const char* params[] = { cam_array.c_str(), floor_str.c_str() };
  PGresult* res = onvif::pg::ExecParamsWithTimeout(impl_->conn, 15'000,
    "SELECT e.\"cameraId\", MAX(e.start) "
    "FROM events e "
    "WHERE e.type = 'smartDetectZone' "
    "  AND e.\"cameraId\" = ANY($1::text[]) "
    "  AND e.start > $2::bigint "
    "  AND (e.metadata::jsonb->>'source') = 'onvif-recorder' "
    "GROUP BY e.\"cameraId\"",
    2, nullptr, params, nullptr, nullptr, 0);

  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    LOG(WARNING) << "[motion_poller] batched hwm query failed; "
                    "using default hwm (now - 1h) for every camera";
    PQclear(res);
  } else {
    for (int i = 0; i < PQntuples(res); ++i) {
      const std::string cam_id = PQgetvalue(res, i, 0);
      auto it = impl_->hwm.find(cam_id);
      if (it != impl_->hwm.end())
        it->second = static_cast<uint64_t>(std::stoull(PQgetvalue(res, i, 1)));
    }
    PQclear(res);
  }

  // Apply optional backfill pull-back per camera (typical reason: an older
  // code path or a NanoDet miss).  The poll SELECT's NOT EXISTS
  // smartDetectZone filter means we naturally skip events that already
  // have one -- only the ones we previously missed get reprocessed.  Live
  // SDZ events from less than 60s ago will still trigger automations (see
  // ts_ms / alarm age check in the live processing loop); older ones
  // won't.
  if (impl_->backfill_lookback_days > 0) {
    const uint64_t backfill_floor = util::now_ms()
        - static_cast<uint64_t>(impl_->backfill_lookback_days) * 86'400'000ULL;
    for (auto& [cam_id, hwm] : impl_->hwm) {
      if (backfill_floor < hwm) {
        LOG(INFO) << "[motion_poller] camera " << cam_id
                  << " pulling hwm back from " << hwm
                  << " to " << backfill_floor
                  << " for " << impl_->backfill_lookback_days
                  << "-day backfill scan";
        hwm = backfill_floor;
      }
    }
  }

  for (const auto& [cam_id, hwm] : impl_->hwm) {
    LOG(INFO) << "[motion_poller] camera " << cam_id
              << " high-water mark: " << hwm;
  }
}

// ---------------------------------------------------------------------------
// Main poll loop
// ---------------------------------------------------------------------------

// Log the recent event-type distribution for every watched camera.
// Used at startup and periodically so a journal capture immediately
// shows whether each camera is producing the `motion` events the poller
// looks for, vs `smartDetectZone` (already-classified) or nothing at all.
void MotionPoller::log_event_type_breakdown(const char* label) {
  if (impl_->camera_ids.empty()) return;

  const uint64_t since_ms = util::now_ms() - 3600000ULL;  // last hour
  const std::string since_str = std::to_string(since_ms);

  // Batched into one round-trip via cameraId = ANY($1::text[]).  The old
  // per-camera loop issued N queries at startup + every 30 minutes;
  // consolidating to one aggregate scan cuts the DB traffic by (N-1)/N
  // (7/8 for gleep52's 8-camera fleet on issue #34).
  std::string cam_array = "{";
  bool first_id = true;
  for (const auto& cam_id : impl_->camera_ids) {
    if (!first_id) cam_array += ',';
    first_id = false;
    cam_array += cam_id;  // 24-char hex, no quoting needed
  }
  cam_array += "}";

  impl_->maybe_reconnect();
  const char* params[] = { cam_array.c_str(), since_str.c_str() };
  PGresult* res = onvif::pg::ExecParamsWithTimeout(impl_->conn, -1,
    "SELECT \"cameraId\", type, COUNT(*) FROM events "
    "WHERE \"cameraId\" = ANY($1::text[]) AND start > $2::bigint "
    "GROUP BY \"cameraId\", type",
    2, nullptr, params, nullptr, nullptr, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    LOG(WARNING) << "[motion_poller] batched event-type breakdown failed: "
                 << PQerrorMessage(impl_->conn);
    PQclear(res);
    return;
  }

  // Assemble per-camera summaries: {cam_id -> [(type, count), ...]}
  // sorted by count desc for readability.
  std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>
      per_camera;
  const int n = PQntuples(res);
  for (int i = 0; i < n; ++i) {
    per_camera[PQgetvalue(res, i, 0)].emplace_back(
        PQgetvalue(res, i, 1),
        std::atoi(PQgetvalue(res, i, 2)));
  }
  PQclear(res);
  for (auto& [cam_id, rows] : per_camera) {
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
  }

  // Emit one line per camera in the order the poller was configured, so
  // the journal preserves the visual grouping users are used to.
  for (const auto& cam_id : impl_->camera_ids) {
    auto it = per_camera.find(cam_id);
    std::string summary;
    if (it == per_camera.end() || it->second.empty()) {
      summary = "(no events in last 1h)";
    } else {
      for (size_t i = 0; i < it->second.size(); ++i) {
        if (i > 0) summary += ' ';
        summary += it->second[i].first;
        summary += '=';
        summary += std::to_string(it->second[i].second);
      }
    }
    LOG(INFO) << "[motion_poller] " << label << " camera " << cam_id
              << " last 1h: " << summary;
  }
}

void MotionPoller::poll_loop() {
  LOG(INFO) << "[motion_poller] started, watching "
            << impl_->camera_ids.size() << " camera(s), interval "
            << impl_->poll_interval_sec << "s";

  // (A) Detector readiness.  start() already returns early when the
  // detector is null, but log explicitly so the journal makes it
  // unambiguous what classifier (if any) the poller will run.
  if (impl_->detector != nullptr) {
    LOG(INFO) << "[motion_poller] detector ready: NanoDet-M loaded";
  } else {
    LOG(WARNING) << "[motion_poller] detector is null; motion events "
                 << "will not be classified";
  }

  init_high_water_marks();

  // (A2) Optional push mode: install the events-INSERT trigger and
  // LISTEN for NOTIFYs on the recorder's DB connection.  When active,
  // the poll cycle at the bottom of this loop wakes on the first
  // matching NOTIFY rather than sleeping for poll_interval_sec, so
  // steady-state query rate drops from
  //   (cameras x cycles/min) to (motion events/min).
  // On any failure (trigger cannot be created, LISTEN rejected, ...)
  // we fall back to the classic interval poll for the rest of this
  // run.
  if (impl_->push_mode) {
    impl_->maybe_reconnect();
    impl_->trigger_installed =
        try_install_motion_trigger_and_listen(impl_->conn);
  } else {
    LOG(INFO) << "[motion_poller] push mode disabled -- interval poll";
  }

  // (B) Per-camera event-type breakdown at startup.  Tells us
  // immediately whether the cameras are emitting `motion` (what the
  // poller looks for) or `smartDetectZone` directly (native AI) or
  // nothing at all -- a frequent source of "first-party not classified"
  // confusion.
  log_event_type_breakdown("startup");

  // (C) Periodic heartbeat: aggregate counts across the last
  // kHeartbeatEveryPolls polls and emit a single line summary.
  // (B) Periodic event-type breakdown: same query as startup, every
  // kBreakdownEveryPolls polls.
  constexpr int kHeartbeatEveryPolls = 5;
  constexpr int kBreakdownEveryPolls = 180;  // ~30 min at 10s interval
  int polls_since_heartbeat = 0;
  int polls_since_breakdown = 0;
  uint64_t hb_candidates = 0;
  uint64_t hb_classified = 0;
  uint64_t hb_skipped_no_thumb = 0;
  uint64_t hb_skipped_overlap = 0;

  while (running_.load()) {
    impl_->maybe_reconnect();

    // Per-camera poll: each camera gets its own hwm-bounded query.
    // Earlier versions used a single global query parameterised by
    // MIN(per-camera hwm), but that caused a livelock — inactive
    // cameras held the global min back, the LIMIT-50 window filled
    // with already-processed events from active cameras, and newer
    // events never reached the loop.  Querying per camera removes
    // that head-of-line blocking entirely.
    for (const std::string& cam_id : impl_->camera_ids) {
      if (!running_.load()) break;
      impl_->maybe_reconnect();

      auto hwm_it = impl_->hwm.find(cam_id);
      const uint64_t cam_hwm = (hwm_it != impl_->hwm.end())
          ? hwm_it->second : (util::now_ms() - 3600000ULL);
      const std::string hwm_str = std::to_string(cam_hwm);
      const char* params[] = { cam_id.c_str(), hwm_str.c_str() };

      // Single per-camera SELECT that returns both the motion candidates
      // AND a boolean `has_smartdetect_overlap` for each one, so we can
      // both process the non-overlapping rows AND count the overlapping
      // ones for the heartbeat display in one round-trip instead of
      // two.  Previously the poll issued two nearly-identical queries
      // (one COUNT with EXISTS(...), one SELECT with NOT EXISTS(...))
      // per camera per cycle -- halving that cut ~96 correlated-subquery
      // scans/min for an 8-camera first-party fleet on issue #34.
      //
      // The overlap count is now bounded to the same LIMIT window as
      // the processing set; if a camera has more than 50 overlap events
      // queued since hwm, the heartbeat under-counts them, but the log
      // line is informational only and hwm advances every poll so the
      // undercount converges to zero.
      PGresult* res = onvif::pg::ExecParamsWithTimeout(impl_->conn, -1,
        "SELECT e.id, e.start, e.\"end\", e.\"cameraId\", e.\"thumbnailId\", "
        "       COALESCE("
        "         (c.\"recordingSettings\"::jsonb->>'prePaddingSecs')::int,"
        "         2) AS pre_padding_secs, "
        "       EXISTS ( "
        "         SELECT 1 FROM events e2 "
        "         WHERE e2.type = 'smartDetectZone' "
        "           AND e2.\"cameraId\" = e.\"cameraId\" "
        "           AND e2.start >= e.start - 5000 "
        "           AND e2.start <= e.\"end\" + 5000 "
        "       ) AS has_smartdetect_overlap "
        "FROM events e JOIN cameras c ON c.id = e.\"cameraId\" "
        "WHERE e.type = 'motion' "
        "  AND e.\"cameraId\" = $1 "
        "  AND e.start > $2::bigint "
        "  AND e.\"thumbnailId\" IS NOT NULL "
        "  AND e.\"end\" IS NOT NULL "
        "ORDER BY e.start ASC LIMIT 50",
        2, nullptr, params, nullptr, nullptr, 0);

      if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        LOG(ERROR) << "[motion_poller] poll query failed (camera "
                   << cam_id << "): " << PQerrorMessage(impl_->conn);
        PQclear(res);
        continue;
      }

      const int nrows = PQntuples(res);
      // hb_candidates counts every motion candidate returned by the
      // combined query; hb_skipped_overlap counts the subset whose
      // has_smartdetect_overlap column is true.  The processing loop
      // below skips those rows the same way the NOT EXISTS clause used
      // to.
      hb_candidates += static_cast<uint64_t>(nrows);
      if (nrows > 0)
        LOG(INFO) << "[motion_poller] poll returned " << nrows
                  << " motion event(s) to consider for camera " << cam_id;
      for (int i = 0; i < nrows && running_.load(); ++i) {
        const bool has_overlap =
            std::string(PQgetvalue(res, i, 6)) == "t";
        if (has_overlap) {
          ++hb_skipped_overlap;
          continue;
        }
        const std::string ev_id      = PQgetvalue(res, i, 0);
        const uint64_t    start_ms   =
            static_cast<uint64_t>(std::stoull(PQgetvalue(res, i, 1)));
        const uint64_t    end_ms     =
            static_cast<uint64_t>(std::stoull(PQgetvalue(res, i, 2)));
        // cameraId column is redundant (it always matches cam_id from
        // the outer loop), but keeping it makes the row tuple uniform
        // with the prior global-query layout — useful when grepping
        // dumps that span versions.
        (void)PQgetvalue(res, i, 3);
        const std::string thumb_id   = PQgetvalue(res, i, 4);
        int pre_padding_secs = std::atoi(PQgetvalue(res, i, 5));
        if (pre_padding_secs < 0) pre_padding_secs = 0;
        const uint64_t real_motion_start_ms =
            start_ms + static_cast<uint64_t>(pre_padding_secs) * 1000;

        LOG(INFO) << "[motion_poller] processing motion event " << ev_id
                  << " camera=" << cam_id << " thumb=" << thumb_id
                  << " start=" << start_ms << " end=" << end_ms
                  << " prePad=" << pre_padding_secs << "s"
                  << " realStart=" << real_motion_start_ms;

        // Output of the classification stage below: the JPEG that produced
        // `det` (used downstream for thumbnail cropping + MSR upload) and
        // the Detection itself.  Either of three paths can populate it:
        //   1. Time-travel sampling (preferred when enabled): N frames at
        //      1s offsets from real_motion_start_ms, with a baseline frame
        //      at -5s subtracted from the class set.
        //   2. Legacy single-snapshot + cropped-thumb (fallback).
        //   3. always-smart-detect fallback (no detection, just write the
        //      event tagged with fallback_object_type and confidence=0).
        std::optional<object_detect::Detection> det;
        std::vector<uint8_t> jpeg;  // image that actually produced det
        std::vector<uint8_t> jpeg_wide;   // (A) wide 640x360 (legacy fallback)
        std::vector<uint8_t> jpeg_crop;   // (B) cropped 360x360 (legacy fallback)

        // ----- (1) Time-travel video sampling -----------------------------
        const int sample_secs = impl_->effective_sample_secs(
            cam_id, start_ms, end_ms);
        if (sample_secs > 0 && !impl_->protect_url.empty() &&
            impl_->protect_user_id_provider) {
          // Baseline at real_motion_start - 5s: enumerate all classes
          // already in the scene so we can subtract them below.
          std::vector<std::string> baseline_classes;
          const uint64_t baseline_ts =
              real_motion_start_ms > 5000ULL ? real_motion_start_ms - 5000ULL
                                              : 0ULL;
          if (baseline_ts > 0) {
            auto baseline_jpeg = fetch_camera_snapshot_at_ts(
                impl_->protect_url, impl_->protect_user_id_provider,
                cam_id, baseline_ts);
            if (!baseline_jpeg.empty()) {
              const auto bdets = impl_->detector->detect_all(baseline_jpeg);
              for (const auto& d : bdets)
                baseline_classes.push_back(
                    object_detect::detection_type(d.class_id));
            }
          }

          // N event frames at +0, +1s, ..., +(N-1)s.  We keep every
          // frame's JPEG in memory so we can pick the winning one after
          // baseline subtraction; ~30 KB × 5 = 150 KB worst case.
          std::vector<std::vector<uint8_t>>           frame_jpegs;
          std::vector<object_detect::Detection>       frame_dets_flat;
          std::vector<int>                             frame_dets_jpeg_idx;
          std::vector<motion_poller_internal::Candidate> candidates;

          for (int s = 0; s < sample_secs && running_.load(); ++s) {
            const uint64_t ts =
                real_motion_start_ms + static_cast<uint64_t>(s) * 1000ULL;
            auto frame = fetch_camera_snapshot_at_ts(
                impl_->protect_url, impl_->protect_user_id_provider,
                cam_id, ts);
            if (frame.empty()) continue;
            const auto fdets = impl_->detector->detect_all(frame);
            if (fdets.empty()) {
              // Even if nothing classified, keep the JPEG -- the last
              // resort fallback path below may still use it as the
              // thumbnail image.
              continue;
            }
            frame_jpegs.push_back(std::move(frame));
            const int jpeg_idx = static_cast<int>(frame_jpegs.size()) - 1;
            for (const auto& d : fdets) {
              frame_dets_flat.push_back(d);
              frame_dets_jpeg_idx.push_back(jpeg_idx);
              candidates.push_back({
                  object_detect::detection_type(d.class_id),
                  std::max(0, std::min(100,
                      static_cast<int>(std::lround(d.confidence * 100.0f))))
              });
            }
          }

          LOG(INFO) << "[motion_poller] event " << ev_id
                    << ": time-travel sample N=" << sample_secs
                    << " baseline_classes=" << baseline_classes.size()
                    << " event_frames_with_dets=" << frame_jpegs.size()
                    << " event_candidates=" << candidates.size();

          const int winner = motion_poller_internal::select_best_candidate_index(
              baseline_classes, candidates);
          if (winner >= 0) {
            det = frame_dets_flat[winner];
            jpeg = std::move(frame_jpegs[frame_dets_jpeg_idx[winner]]);
            LOG(INFO) << "[motion_poller] event " << ev_id
                      << ": time-travel HIT class="
                      << object_detect::detection_type(det->class_id)
                      << " conf=" << candidates[winner].confidence_pct;
          } else if (!candidates.empty()) {
            LOG(INFO) << "[motion_poller] event " << ev_id
                      << ": time-travel suppressed (all classes in baseline)";
          }

          // If sampling produced a JPEG but no winning detection, keep
          // the first sampled JPEG as the fallback "wide" image so the
          // thumbnail still looks reasonable when the always-smart-detect
          // path writes a confidence=0 row.
          if (!det.has_value() && !frame_jpegs.empty())
            jpeg_wide = std::move(frame_jpegs.front());
        }

        // ----- (2) Legacy single-snapshot + cropped-thumb -----------------
        // Skipped entirely when sampling already produced a hit.  Still runs
        // when sampling ran-but-missed (the legacy crop-thumb sometimes
        // catches things the wide sampling misses, e.g. a small/distant
        // subject Protect cropped onto and that the 640x360 wide downscale
        // squashes below NanoDet's confidence threshold).
        if (!det.has_value()) {
          if (jpeg_wide.empty() && !impl_->protect_url.empty() &&
              impl_->protect_user_id_provider) {
            jpeg_wide = fetch_camera_snapshot_at_ts(
                impl_->protect_url, impl_->protect_user_id_provider,
                cam_id, real_motion_start_ms);
          }
          // Thumbnail-by-id: 24-char IDs live in the `thumbnails` Postgres
          // table; anything else (MSR "{MAC}-{ts}" form) is served by msp
          // via /api/thumbnails/<id>.
          if (thumb_id.size() == 24) {
            impl_->maybe_reconnect();
            const char* tp[] = { thumb_id.c_str() };
            PGresult* thumb_res = onvif::pg::ExecParamsWithTimeout(impl_->conn, -1,
              "SELECT content FROM thumbnails WHERE id = $1",
              1, nullptr, tp, nullptr, nullptr, 1);
            if (PQresultStatus(thumb_res) == PGRES_TUPLES_OK &&
                PQntuples(thumb_res) > 0 &&
                !PQgetisnull(thumb_res, 0, 0)) {
              const char* raw = PQgetvalue(thumb_res, 0, 0);
              const int raw_len = PQgetlength(thumb_res, 0, 0);
              jpeg_crop.assign(raw, raw + raw_len);
            }
            PQclear(thumb_res);
          } else if (!impl_->protect_url.empty() &&
                     impl_->protect_user_id_provider &&
                     !impl_->protect_user_id_provider->empty()) {
            jpeg_crop = fetch_thumbnail_via_protect(
                impl_->protect_url, impl_->protect_user_id_provider, thumb_id);
          }

          if (jpeg_wide.empty() && jpeg_crop.empty()) {
            LOG(INFO) << "[motion_poller] skip event " << ev_id
                      << ": no snapshot or thumbnail available";
            ++hb_skipped_no_thumb;
            impl_->hwm[cam_id] = start_ms;
            continue;
          }

          LOG(INFO) << "[motion_poller] event " << ev_id
                    << ": running legacy NanoDet-M on "
                    << (jpeg_wide.empty()
                            ? ""
                            : "snapshot(" + std::to_string(jpeg_wide.size())
                                  + "B) ")
                    << (jpeg_crop.empty()
                            ? ""
                            : "thumb(" + std::to_string(jpeg_crop.size())
                                  + "B)");

          // Try the tight-crop image first -- empirically higher hit rate
          // because the subject occupies more of the frame.  Fall back to
          // the wide snapshot if the crop comes up empty.
          for (const auto* candidate : {&jpeg_crop, &jpeg_wide}) {
            if (candidate->empty()) continue;
            auto d = impl_->detector->detect(*candidate);
            if (d.has_value() &&
                object_detect::is_security_relevant(d->class_id)) {
              det = d;
              jpeg = *candidate;
              break;
            }
          }
        }

        // When NanoDet doesn't return a security-relevant class but the
        // camera is opted in via --first_party_cameras, the user wants the
        // motion event to surface through Protect's smart-detect filter
        // anyway.  Fall back to the configured class (default "person")
        // and use whichever image we have -- preferring the crop (which
        // is closer to motion centroid) so the thumbnail still looks
        // reasonable in the UI.
        bool fell_back = false;
        if (!det.has_value() && impl_->always_smart_detect) {
          jpeg = !jpeg_crop.empty() ? jpeg_crop : jpeg_wide;
          if (!jpeg.empty()) {
            fell_back = true;
            LOG(INFO) << "[motion_poller] event " << ev_id
                      << ": no NanoDet-M hit; falling back to class '"
                      << impl_->fallback_object_type << "' (confidence=0)";
          }
        }

        if (!det.has_value() && !fell_back) {
          LOG(INFO) << "[motion_poller] skip event " << ev_id
                    << ": NanoDet-M found nothing security-relevant"
                    << " in either snapshot or thumbnail"
                    << " (and always_smart_detect is off)";
          impl_->hwm[cam_id] = start_ms;
          continue;
        }

        // NanoDet-M reports confidence in [0, 1].  UniFi Protect's
        // events.score / smartDetectObjects.attributes.confidence
        // schema uses an integer in [0, 100], so scale + clamp here
        // and reuse for both the SQL parameter and the in-row JSON
        // blobs we build below.
        //
        // event_score keeps the historical analytic distinction: a row
        // with events.score = 0 was generated by the always-smart-detect
        // fallback (no NanoDet hit), while score > 0 was AI-classified.
        //
        // attr_confidence is what goes into the per-row JSON attribute
        // blobs (smartDetectObjects / smartDetectTracks / smartDetectRaws).
        // The Android Protect app dereferences attributes.confidence
        // without checking and crashes the event view on confidence == 0,
        // so we floor the JSON value at 100 in the fallback path.  Any
        // 'is this a fallback row?' analysis should read events.score,
        // not attributes.confidence.
        const int event_score = det.has_value()
            ? std::max(0, std::min(100,
                static_cast<int>(std::lround(det->confidence * 100.0f))))
            : 0;
        const int confidence = (event_score > 0) ? event_score : 100;
        const std::string conf_str = std::to_string(event_score);
        const std::string obj_type = det.has_value()
            ? object_detect::detection_type(det->class_id)
            : impl_->fallback_object_type;
        const std::string sdt_json =
            motion_poller_internal::smart_detect_types_json(obj_type);
        const std::string new_event_id = util::generate_uuid();
        const std::string sdo_id       = util::generate_uuid();
        const std::string sdr_id       = util::generate_uuid();
        // Resolve MAC for MSR-format thumbnail IDs and UBV writing.
        std::string cam_mac;
        {
          auto mac_it = impl_->id_to_mac.find(cam_id);
          if (mac_it != impl_->id_to_mac.end())
            cam_mac = mac_it->second;
        }
        const std::string now_str      = util::utc_now_iso8601();
        const std::string start_str    = std::to_string(start_ms);
        const std::string end_str      = std::to_string(end_ms);
        const std::string ts_str       = std::to_string(start_ms);

        // Crop the thumbnail using the detection bbox.  In the fallback
        // path (no NanoDet hit) we use the whole image -- there's no
        // bbox to crop with and the UI gets a wider context shot.
        std::vector<uint8_t> cropped = det.has_value()
            ? jpeg_crop::crop(jpeg, det->bbox)
            : std::vector<uint8_t>{};
        if (cropped.empty()) cropped = std::move(jpeg);

        // Forward the cropped JPEG to MSR's StoreSnapshots when an MsrClient
        // is wired in.  MSR persists the bytes as a native UBV file and
        // returns a "{MAC}-{ms}" id that the msp media server serves; that
        // format is what Protect 7.1+'s detection-search recognises, so
        // first-party events finally surface in Find Anything once they're
        // stored through this path (issue #16 / #27).  On MSR failure we
        // fall through to the legacy DB-stored 24-char-hex id and the
        // event remains queryable by ID even if the Web UI filter ignores
        // it.
        std::string new_thumb_id;
        bool stored_by_msr = false;
        if (impl_->msr_client && !cam_mac.empty()) {
          new_thumb_id = impl_->msr_client->StoreSnapshot(
              cam_mac, cropped.data(), cropped.size());
          if (!new_thumb_id.empty()) {
            stored_by_msr = true;
            LOG(INFO) << "[motion_poller] event " << new_event_id
                      << ": MSR stored snapshot as id=" << new_thumb_id;
          }
        }
        if (new_thumb_id.empty()) {
          new_thumb_id =
              (impl_->use_msr_thumb_ids && !cam_mac.empty())
                  ? util::make_msr_thumbnail_id(cam_mac, start_ms)
                  : util::generate_24hex_id();
        }

        // Version gate: on Protect 7.1+, write the rich events.metadata +
        // thumbnailFullfovId.  On older versions, keep the legacy sparse SQL
        // exactly as before so existing installs see no behavioural change.
        const bool rich_path = onvif::protect_version::IsAtLeast(7, 1, 0);
        std::string rich_metadata;
        if (rich_path) {
          onvif::enricher::EventInput ein;
          ein.event_id          = new_event_id;
          ein.camera_id         = cam_id;
          ein.event_type        = "smartDetectZone";
          ein.smart_detect_types = {obj_type};
          ein.score             = event_score;
          ein.thumbnail_id      = new_thumb_id;
          ein.start_ms          = start_ms;
          ein.end_ms            = end_ms;
          // TODO: thread the real image dimensions through from the camera
          // record so the placeholder bbox matches the camera's frame size.
          // First-party cameras: G3 Dome=1920x1080, G4 Doorbell=1600x1200, etc.
          ein.image_width       = 2560;
          ein.image_height      = 1440;
          ein.object_ids        = {sdo_id};
          rich_metadata = onvif::enricher::BuildEnrichedMetadata(ein);
        }

        // INSERT event.
        {
          impl_->maybe_reconnect();
          if (!rich_path) {
            // Legacy sparse SQL -- unchanged from before the 7.1 migration.
            const char* ep[] = {
              new_event_id.c_str(), start_str.c_str(), cam_id.c_str(),
              sdt_json.c_str(), new_thumb_id.c_str(),
              now_str.c_str(), now_str.c_str(), end_str.c_str(),
              conf_str.c_str()
            };
            PGresult* r = onvif::pg::ExecParamsWithTimeout(impl_->conn, -1,
              "INSERT INTO events"
              " (id, type, start, \"cameraId\", score, \"smartDetectTypes\","
              "  metadata, locked, \"thumbnailId\", \"createdAt\","
              "  \"updatedAt\", \"end\")"
              " VALUES ($1, 'smartDetectZone', $2::bigint, $3, $9::int, "
              "  $4::json, '{\"source\":\"onvif-recorder\"}'::json, false, $5,"
              "  $6, $7, $8::bigint)",
              9, nullptr, ep, nullptr, nullptr, 0);
            if (PQresultStatus(r) != PGRES_COMMAND_OK)
              LOG(ERROR) << "[motion_poller] insert event: "
                         << PQerrorMessage(impl_->conn);
            PQclear(r);
          } else {
            // Rich path (Protect 7.1+): metadata is the enriched JSON;
            // thumbnailFullfovId reuses thumbnailId until we have a separate
            // full-FOV crop.  See TODO note where rich_metadata is built.
            const char* ep[] = {
              new_event_id.c_str(), start_str.c_str(), cam_id.c_str(),
              sdt_json.c_str(), new_thumb_id.c_str(),
              now_str.c_str(), now_str.c_str(), end_str.c_str(),
              conf_str.c_str(),
              rich_metadata.c_str(),
              new_thumb_id.c_str(),  // TODO: separate full-FOV thumb id
            };
            PGresult* r = onvif::pg::ExecParamsWithTimeout(impl_->conn, -1,
              "INSERT INTO events"
              " (id, type, start, \"cameraId\", score, \"smartDetectTypes\","
              "  metadata, locked, \"thumbnailId\", \"createdAt\","
              "  \"updatedAt\", \"end\", \"thumbnailFullfovId\")"
              " VALUES ($1, 'smartDetectZone', $2::bigint, $3, $9::int, "
              "  $4::json, $10::json, false, $5,"
              "  $6, $7, $8::bigint, $11)",
              11, nullptr, ep, nullptr, nullptr, 0);
            if (PQresultStatus(r) != PGRES_COMMAND_OK)
              LOG(ERROR) << "[motion_poller] insert event (rich): "
                         << PQerrorMessage(impl_->conn);
            PQclear(r);
          }
        }

        // INSERT smartDetectObjects.
        {
          const std::string attr_json =
              motion_poller_internal::build_sdo_attributes(
                  obj_type, confidence);
          const char* sp[] = {
            sdo_id.c_str(), new_event_id.c_str(), new_thumb_id.c_str(),
            cam_id.c_str(), obj_type.c_str(), attr_json.c_str(),
            ts_str.c_str(), now_str.c_str(), now_str.c_str()
          };
          PGresult* r = onvif::pg::ExecParamsWithTimeout(impl_->conn, -1,
            "INSERT INTO \"smartDetectObjects\""
            " (id, \"eventId\", \"thumbnailId\", \"cameraId\", type,"
            "  attributes, \"detectedAt\", metadata,"
            "  \"createdAt\", \"updatedAt\")"
            " VALUES ($1, $2, $3, $4, $5, $6::json, $7::bigint,"
            "  '{}'::jsonb, $8, $9)",
            9, nullptr, sp, nullptr, nullptr, 0);
          if (PQresultStatus(r) != PGRES_COMMAND_OK)
            LOG(ERROR) << "[motion_poller] insert sdo: "
                       << PQerrorMessage(impl_->conn);
          PQclear(r);
        }

        // INSERT smartDetectObjectAreas on Protect 7.1+ -- the bbox + grid
        // cells the UI uses for the overlay.  Uses placeholder bbox (centred
        // quad) until we wire in real per-frame bounding boxes from the
        // camera analytics stream.
        if (rich_path) {
          const auto bb = onvif::enricher::PlaceholderBbox(
              2560, 1440, obj_type);
          const std::string area_id = "sda-" + sdo_id;
          const std::string area_idx_sql =
              onvif::enricher::FullGridAreaIndexesSqlArray();
          const std::string x1 = std::to_string(bb.x1);
          const std::string y1 = std::to_string(bb.y1);
          const std::string x2 = std::to_string(bb.x2);
          const std::string y2 = std::to_string(bb.y2);
          const char* ap[] = {
            area_id.c_str(), sdo_id.c_str(),
            x1.c_str(), y1.c_str(), x2.c_str(), y2.c_str(),
            ts_str.c_str(), end_str.c_str(),
          };
          const std::string sql =
              "INSERT INTO \"smartDetectObjectAreas\""
              " (id, \"smartDetectObjectId\", \"areaIndexes\","
              "  \"boundingX1\", \"boundingY1\", \"boundingX2\", \"boundingY2\","
              "  \"detectedAt\", \"lastSeenAt\")"
              " VALUES ($1, $2, " + area_idx_sql + ","
              " $3::bigint, $4::bigint, $5::bigint, $6::bigint,"
              " $7::bigint, $8::bigint)"
              " ON CONFLICT (id) DO NOTHING";
          // Best effort, as in detection_recorder: overlay data only.
          auto& gate = onvif::pg::AreaInsertGate();
          if (gate.allowed()) {
            PGresult* r = onvif::pg::ExecParamsWithTimeout(
                impl_->conn, onvif::pg::kBestEffortTimeoutMs, sql.c_str(),
                8, nullptr, ap, nullptr, nullptr, 0);
            gate.record(r == nullptr);
            if (r != nullptr && PQresultStatus(r) != PGRES_COMMAND_OK)
              LOG(WARNING) << "[motion_poller] insert sda: "
                           << PQerrorMessage(impl_->conn);
            PQclear(r);
          }
        }

        // INSERT detectionLabels.  The /api/detection-search endpoint
        // (Find Anything in the Web UI) does a GIN-index lookup on
        // detectionLabels.labels; events without a row here are invisible
        // to Web/iOS filters even when smartDetectTypes is set.
        // detection_recorder.cpp writes these for third-party cameras;
        // the motion poller has to do the same for first-party ones.
        {
          const std::string label_event_type =
              std::string("eventType:smartDetectZone");
          const std::string label_sdt = "smartDetectType:" + obj_type;
          const std::string label_camera = "camera:" + cam_id;
          const std::string label_zone = "zone:" + cam_id + ":1";

          // Upsert all four labels in a single round-trip.
          const char* up_p[] = {
            label_event_type.c_str(), label_sdt.c_str(),
            label_camera.c_str(),     label_zone.c_str()
          };
          PGresult* ur = onvif::pg::ExecParamsWithTimeout(impl_->conn, -1,
            "INSERT INTO labels (id, name, \"createdAt\", \"updatedAt\") "
            "VALUES (gen_random_uuid(), $1, NOW(), NOW()),"
            "       (gen_random_uuid(), $2, NOW(), NOW()),"
            "       (gen_random_uuid(), $3, NOW(), NOW()),"
            "       (gen_random_uuid(), $4, NOW(), NOW()) "
            "ON CONFLICT (name) DO NOTHING",
            4, nullptr, up_p, nullptr, nullptr, 0);
          if (PQresultStatus(ur) != PGRES_COMMAND_OK)
            LOG(WARNING) << "[motion_poller] upsert labels: "
                         << PQerrorMessage(impl_->conn);
          PQclear(ur);

          // Fetch the four lids in name order matching $1..$4.
          int lids[4] = {0, 0, 0, 0};
          PGresult* lr = onvif::pg::ExecParamsWithTimeout(impl_->conn, -1,
            "SELECT lid, name FROM labels "
            "WHERE name IN ($1, $2, $3, $4)",
            4, nullptr, up_p, nullptr, nullptr, 0);
          int n_lids = 0;
          if (PQresultStatus(lr) == PGRES_TUPLES_OK) {
            for (int i = 0; i < PQntuples(lr) && n_lids < 4; ++i) {
              lids[n_lids++] = std::atoi(PQgetvalue(lr, i, 0));
            }
          } else {
            LOG(WARNING) << "[motion_poller] select lids: "
                         << PQerrorMessage(impl_->conn);
          }
          PQclear(lr);

          if (n_lids == 4) {
            // Insert event-level row (objectId IS NULL) and SDO-level row
            // (objectId = sdo_id) in a single statement.
            const std::string dl_event_id = util::generate_uuid();
            const std::string dl_sdo_id   = util::generate_uuid();
            const std::string l0 = std::to_string(lids[0]);
            const std::string l1 = std::to_string(lids[1]);
            const std::string l2 = std::to_string(lids[2]);
            const std::string l3 = std::to_string(lids[3]);
            const char* dlp[] = {
              dl_event_id.c_str(), new_event_id.c_str(),
              l0.c_str(), l1.c_str(), l2.c_str(), l3.c_str(),
              now_str.c_str(), now_str.c_str(),
              dl_sdo_id.c_str(), sdo_id.c_str()
            };
            PGresult* dlr = onvif::pg::ExecParamsWithTimeout(impl_->conn, -1,
              "INSERT INTO \"detectionLabels\""
              "  (id, \"eventId\", \"objectId\", labels,"
              "   \"createdAt\", \"updatedAt\") "
              "VALUES "
              "  ($1, $2, NULL, "
              "   ARRAY[$3::int,$4::int,$5::int,$6::int], $7, $8),"
              "  ($9, $2, $10, "
              "   ARRAY[$3::int,$4::int,$5::int,$6::int], $7, $8) "
              "ON CONFLICT DO NOTHING",
              10, nullptr, dlp, nullptr, nullptr, 0);
            if (PQresultStatus(dlr) != PGRES_COMMAND_OK)
              LOG(WARNING) << "[motion_poller] insert detectionLabels: "
                           << PQerrorMessage(impl_->conn);
            PQclear(dlr);
          }
        }

        // INSERT smartDetectTracks.  Required for the iOS app's Find
        // Anything filter to surface our events alongside native ones.
        {
          const std::string sdtrk_id      = util::generate_uuid();
          const std::string sdtrk_payload =
              motion_poller_internal::build_sdt_payload(
                  start_ms, end_ms, obj_type, confidence);
          const char* tp[] = {
            sdtrk_id.c_str(), new_event_id.c_str(), cam_id.c_str(),
            sdtrk_payload.c_str(), now_str.c_str(), now_str.c_str()
          };
          PGresult* r = onvif::pg::ExecParamsWithTimeout(impl_->conn, -1,
            "INSERT INTO \"smartDetectTracks\""
            " (id, \"eventId\", \"cameraId\", payload,"
            "  \"createdAt\", \"updatedAt\")"
            " VALUES ($1, $2, $3, $4::json, $5, $6)",
            6, nullptr, tp, nullptr, nullptr, 0);
          if (PQresultStatus(r) != PGRES_COMMAND_OK)
            LOG(ERROR) << "[motion_poller] insert sdt: "
                       << PQerrorMessage(impl_->conn);
          PQclear(r);
        }

        // INSERT smartDetectRaws.
        {
          const std::string payload =
              motion_poller_internal::build_sdr_payload(
                  start_ms, obj_type, confidence);
          const char* rp[] = {
            sdr_id.c_str(), cam_id.c_str(), payload.c_str(),
            ts_str.c_str(), now_str.c_str(), now_str.c_str()
          };
          PGresult* r = onvif::pg::ExecParamsWithTimeout(impl_->conn, -1,
            "INSERT INTO \"smartDetectRaws\""
            " (id, \"cameraId\", payload, timestamp,"
            "  \"createdAt\", \"updatedAt\")"
            " VALUES ($1, $2, $3::json, $4::bigint, $5, $6)",
            6, nullptr, rp, nullptr, nullptr, 0);
          if (PQresultStatus(r) != PGRES_COMMAND_OK)
            LOG(ERROR) << "[motion_poller] insert sdr: "
                       << PQerrorMessage(impl_->conn);
          PQclear(r);
        }

        // INSERT thumbnail (cropped JPEG).  Skipped when MSR already
        // persisted the bytes natively -- writing into the same `thumbnails`
        // table that MSR/Protect own is the contention path that knocked
        // Protect's UI offline in issue #24.
        if (!stored_by_msr) {
          const int jpeg_len = static_cast<int>(cropped.size());
          const char* jpeg_ptr = reinterpret_cast<const char*>(cropped.data());
          const char* tp2[] = {
            new_thumb_id.c_str(), cam_id.c_str(), new_event_id.c_str(),
            ts_str.c_str(), now_str.c_str(), now_str.c_str(), jpeg_ptr
          };
          const int lengths[] = { 0, 0, 0, 0, 0, 0, jpeg_len };
          const int formats[] = { 0, 0, 0, 0, 0, 0, 1 };  // last = binary
          PGresult* r = onvif::pg::ExecParamsWithTimeout(impl_->conn, -1,
            "INSERT INTO thumbnails"
            " (id, \"cameraId\", \"eventId\", timestamp,"
            "  \"createdAt\", \"updatedAt\", content, \"isFullfov\")"
            " VALUES ($1, $2, $3, $4::bigint, $5, $6, $7, false)"
            " ON CONFLICT (id) DO NOTHING",
            7, nullptr, tp2, lengths, formats, 0);
          if (PQresultStatus(r) != PGRES_COMMAND_OK)
            LOG(ERROR) << "[motion_poller] insert thumbnail: "
                       << PQerrorMessage(impl_->conn);
          PQclear(r);
        }

        // Write UBV thumbnail file (native Protect path).
        if (!impl_->ubv_dir.empty() && !cam_mac.empty()) {
          {
            const std::string& mac = cam_mac;
            std::time_t tsec = static_cast<std::time_t>(start_ms / 1000);
            std::tm ttm{};
            gmtime_r(&tsec, &ttm);
            char dbuf[16];
            std::strftime(dbuf, sizeof(dbuf), "%Y/%m/%d", &ttm);
            std::string dstr(dbuf);

            auto& cached = impl_->ubv_cache[mac];
            if (cached.first != dstr) {
              cached.first = dstr;
              cached.second = ubv::protect_path(impl_->ubv_dir, mac, start_ms);
            }
            auto us = ubv::append(cached.second, {start_ms, cropped});
            if (!us.ok())
              LOG(WARNING) << "[motion_poller] ubv append: " << us.message();
          }
        }

        // Fire alarm notification -- only for live events, never for the
        // 7-day backfill pass.  Threshold is wall-clock age of the source
        // motion event: under 60s = "live", anything older we treat as
        // historical and skip the automation trigger.  Otherwise a startup
        // backfill would replay every alarm sound + automation from the
        // last week.
        const uint64_t event_age_ms =
            (util::now_ms() > start_ms) ? (util::now_ms() - start_ms) : 0;
        const bool live_event = event_age_ms < 60'000ULL;
        if (live_event && impl_->alarm_notifier && !cam_mac.empty()) {
          impl_->alarm_notifier->notify(obj_type, cam_mac,
                                        new_event_id, start_ms);
        } else if (!live_event) {
          LOG(INFO) << "[motion_poller] backfill: skipping alarm trigger for "
                    << new_event_id << " (event is "
                    << (event_age_ms / 1000) << "s old)";
        }

        LOG(INFO) << "[motion_poller] " << obj_type << " detected in "
                  << cam_id << " (motion event " << ev_id << ")";

        ++hb_classified;
        // Update high-water mark.
        impl_->hwm[cam_id] = start_ms;
      }

      PQclear(res);
    }  // end per-camera loop


    // (C) Heartbeat: every kHeartbeatEveryPolls cycles, summarise what
    // happened.  Always fires even when the pipeline is idle so the
    // journal makes "the poller is alive but nothing matched" obvious.
    if (++polls_since_heartbeat >= kHeartbeatEveryPolls) {
      uint64_t min_hwm_log = UINT64_MAX;
      for (const auto& [id, h] : impl_->hwm)
        if (h < min_hwm_log) min_hwm_log = h;
      LOG(INFO) << "[motion_poller] heartbeat: " << kHeartbeatEveryPolls
                << " polls, " << hb_candidates << " candidates, "
                << hb_classified << " classified, "
                << hb_skipped_no_thumb << " skipped(no_thumb), "
                << hb_skipped_overlap
                << " excluded(already smartDetectZone), hwm-min="
                << (min_hwm_log == UINT64_MAX ? 0 : min_hwm_log);
      polls_since_heartbeat = 0;
      hb_candidates = 0;
      hb_classified = 0;
      hb_skipped_no_thumb = 0;
      hb_skipped_overlap = 0;
    }

    // (B) Periodic event-type breakdown so a long-running journal still
    // carries fresh visibility into what each camera is producing.
    if (++polls_since_breakdown >= kBreakdownEveryPolls) {
      log_event_type_breakdown("periodic");
      polls_since_breakdown = 0;
    }

    // Wait for the next work unit.  Push mode: block on the
    // recorder's DB socket until a NOTIFY arrives on
    // `onvif_recorder_motion`, with a 60 s safety-net timeout so
    // dropped notifications can't cause missed events.  Interval mode
    // (or push-install failed): sleep for poll_interval_sec in 1 s
    // slices so stop() is responsive.
    if (impl_->trigger_installed) {
      wait_for_notify_or_timeout(impl_->conn, kSafetyNetTimeoutMs,
                                  running_);
    } else {
      for (int s = 0; s < impl_->poll_interval_sec && running_.load(); ++s)
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  LOG(INFO) << "[motion_poller] stopped";
}

}  // namespace onvif
