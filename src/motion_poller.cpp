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

#include <libpq-fe.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "alarm_notifier.hpp"
#include "jpeg_crop.hpp"
#include "object_detect.hpp"
#include "ubv_thumbnail.hpp"
#include "util.hpp"

namespace onvif {

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

std::string build_sdr_payload(uint64_t ts_ms, const std::string& obj_type) {
  const std::string ts = std::to_string(ts_ms);
  return
      std::string("{")
      + "\"attributesForTracks\":null,"
      + "\"clockStream\":" + ts + ","
      + "\"clockStreamRate\":1000,"
      + "\"clockWall\":" + ts + ","
      + "\"descriptors\":[{"
      + "\"attributes\":null,"
      + "\"confidence\":75,"
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

}  // namespace motion_poller_internal

// Build a PostgreSQL text-array literal: {"id1","id2","id3"}
static std::string pg_array(const std::vector<std::string>& ids) {
  std::string out = "{";
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i > 0) out += ',';
    out += "\"" + ids[i] + "\"";
  }
  out += '}';
  return out;
}

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
  int poll_interval_sec{10};
  uint32_t coalesce_window_sec{30};
  bool use_msr_thumb_ids{false};

  // UBV path cache: MAC -> (date string, file path).
  std::map<std::string, std::pair<std::string, std::string>> ubv_cache;

  // High-water mark: last processed motion event start per camera.
  std::map<std::string, uint64_t> hwm;

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

void MotionPoller::set_poll_interval(int sec) {
  impl_->poll_interval_sec = sec;
}

void MotionPoller::set_coalesce_window(uint32_t sec) {
  impl_->coalesce_window_sec = sec;
}

void MotionPoller::set_use_msr_thumbnail_ids(bool use_msr) {
  impl_->use_msr_thumb_ids = use_msr;
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

  for (const auto& cam_id : impl_->camera_ids) {
    impl_->maybe_reconnect();
    const std::string def_str = std::to_string(default_hwm);
    const char* params[] = { def_str.c_str(), cam_id.c_str() };
    PGresult* res = PQexecParams(impl_->conn,
      "SELECT COALESCE(MAX(e.start), $1::bigint) "
      "FROM events e "
      "WHERE e.type = 'smartDetectZone' "
      "  AND e.\"cameraId\" = $2 "
      "  AND (e.metadata::jsonb->>'source') = 'onvif-recorder'",
      2, nullptr, params, nullptr, nullptr, 0);

    uint64_t hwm = default_hwm;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
      hwm = static_cast<uint64_t>(std::stoull(PQgetvalue(res, 0, 0)));
    PQclear(res);

    impl_->hwm[cam_id] = hwm;
    LOG(INFO) << "[motion_poller] camera " << cam_id
              << " high-water mark: " << hwm;
  }
}

// ---------------------------------------------------------------------------
// Main poll loop
// ---------------------------------------------------------------------------

void MotionPoller::poll_loop() {
  LOG(INFO) << "[motion_poller] started, watching "
            << impl_->camera_ids.size() << " camera(s), interval "
            << impl_->poll_interval_sec << "s";

  init_high_water_marks();

  // Pre-build the PostgreSQL array of camera IDs.
  const std::string cam_arr = pg_array(impl_->camera_ids);
  while (running_.load()) {
    impl_->maybe_reconnect();

    // Use the minimum hwm across all cameras for the poll query.
    uint64_t min_hwm = UINT64_MAX;
    for (const auto& [id, h] : impl_->hwm)
      if (h < min_hwm) min_hwm = h;
    if (min_hwm == UINT64_MAX) min_hwm = util::now_ms() - 3600000ULL;

    const std::string hwm_str = std::to_string(min_hwm);
    const char* params[] = { cam_arr.c_str(), hwm_str.c_str() };
    PGresult* res = PQexecParams(impl_->conn,
      "SELECT e.id, e.start, e.\"end\", e.\"cameraId\", e.\"thumbnailId\" "
      "FROM events e "
      "WHERE e.type = 'motion' "
      "  AND e.\"cameraId\" = ANY($1) "
      "  AND e.start > $2::bigint "
      "  AND e.\"thumbnailId\" IS NOT NULL "
      "  AND e.\"end\" IS NOT NULL "
      "  AND NOT EXISTS ( "
      "    SELECT 1 FROM events e2 "
      "    WHERE e2.type = 'smartDetectZone' "
      "      AND e2.\"cameraId\" = e.\"cameraId\" "
      "      AND e2.start >= e.start - 5000 "
      "      AND e2.start <= e.\"end\" + 5000 "
      "  ) "
      "ORDER BY e.start ASC LIMIT 50",
      2, nullptr, params, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
      LOG(ERROR) << "[motion_poller] poll query failed: "
                 << PQerrorMessage(impl_->conn);
      PQclear(res);
      goto sleep;
    }

    {
      const int nrows = PQntuples(res);
      if (nrows > 0)
        LOG(INFO) << "[motion_poller] poll returned " << nrows
                  << " motion event(s) to process";
      for (int i = 0; i < nrows && running_.load(); ++i) {
        const std::string ev_id      = PQgetvalue(res, i, 0);
        const uint64_t    start_ms   =
            static_cast<uint64_t>(std::stoull(PQgetvalue(res, i, 1)));
        const uint64_t    end_ms     =
            static_cast<uint64_t>(std::stoull(PQgetvalue(res, i, 2)));
        const std::string cam_id     = PQgetvalue(res, i, 3);
        const std::string thumb_id   = PQgetvalue(res, i, 4);

        // Skip if already processed (per-camera hwm check).
        auto hwm_it = impl_->hwm.find(cam_id);
        if (hwm_it != impl_->hwm.end() && start_ms <= hwm_it->second) {
          LOG(INFO) << "[motion_poller] skip event " << ev_id
                    << " (camera " << cam_id
                    << "): already processed (hwm=" << hwm_it->second << ")";
          continue;
        }

        LOG(INFO) << "[motion_poller] processing motion event " << ev_id
                  << " camera=" << cam_id << " thumb=" << thumb_id
                  << " start=" << start_ms << " end=" << end_ms;

        // Fetch the thumbnail JPEG from the thumbnails table.
        impl_->maybe_reconnect();
        const char* tp[] = { thumb_id.c_str() };
        PGresult* thumb_res = PQexecParams(impl_->conn,
          "SELECT content FROM thumbnails WHERE id = $1",
          1, nullptr, tp, nullptr, nullptr, 1);

        if (PQresultStatus(thumb_res) != PGRES_TUPLES_OK ||
            PQntuples(thumb_res) == 0 ||
            PQgetisnull(thumb_res, 0, 0)) {
          LOG(INFO) << "[motion_poller] skip event " << ev_id
                    << ": thumbnail " << thumb_id << " not found in DB"
                    << " (query status="
                    << PQresStatus(PQresultStatus(thumb_res)) << ")";
          PQclear(thumb_res);
          // Update hwm even on skip so we don't retry endlessly.
          impl_->hwm[cam_id] = start_ms;
          continue;
        }

        const char* raw = PQgetvalue(thumb_res, 0, 0);
        const int raw_len = PQgetlength(thumb_res, 0, 0);
        std::vector<uint8_t> jpeg(raw, raw + raw_len);
        PQclear(thumb_res);

        if (jpeg.empty()) {
          LOG(INFO) << "[motion_poller] skip event " << ev_id
                    << ": thumbnail " << thumb_id
                    << " exists but has zero bytes";
          impl_->hwm[cam_id] = start_ms;
          continue;
        }

        LOG(INFO) << "[motion_poller] event " << ev_id
                  << ": fetched thumbnail " << thumb_id
                  << " (" << jpeg.size() << " bytes), running NanoDet-M";

        // Run NanoDet-M on the thumbnail.
        auto det = impl_->detector->detect(jpeg);
        if (!det.has_value()) {
          LOG(INFO) << "[motion_poller] skip event " << ev_id
                    << ": NanoDet-M returned no detection"
                    << " (no object above confidence threshold)";
          impl_->hwm[cam_id] = start_ms;
          continue;
        }
        if (!object_detect::is_security_relevant(det->class_id)) {
          LOG(INFO) << "[motion_poller] skip event " << ev_id
                    << ": NanoDet-M detected class_id=" << det->class_id
                    << " which is not security-relevant"
                    << " (person/vehicle/animal)";
          impl_->hwm[cam_id] = start_ms;
          continue;
        }

        const std::string obj_type =
            object_detect::detection_type(det->class_id);
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
        const std::string new_thumb_id =
            (impl_->use_msr_thumb_ids && !cam_mac.empty())
                ? util::make_msr_thumbnail_id(cam_mac, start_ms)
                : util::generate_24hex_id();
        const std::string now_str      = util::utc_now_iso8601();
        const std::string start_str    = std::to_string(start_ms);
        const std::string end_str      = std::to_string(end_ms);
        const std::string ts_str       = std::to_string(start_ms);

        // Crop the thumbnail using the detection bbox.
        std::vector<uint8_t> cropped = jpeg_crop::crop(jpeg, det->bbox);
        if (cropped.empty()) cropped = std::move(jpeg);

        // INSERT event.
        {
          const char* ep[] = {
            new_event_id.c_str(), start_str.c_str(), cam_id.c_str(),
            sdt_json.c_str(), new_thumb_id.c_str(),
            now_str.c_str(), now_str.c_str(), end_str.c_str()
          };
          impl_->maybe_reconnect();
          PGresult* r = PQexecParams(impl_->conn,
            "INSERT INTO events"
            " (id, type, start, \"cameraId\", score, \"smartDetectTypes\","
            "  metadata, locked, \"thumbnailId\", \"createdAt\","
            "  \"updatedAt\", \"end\")"
            " VALUES ($1, 'smartDetectZone', $2::bigint, $3, 100, $4::json,"
            "  '{\"source\":\"onvif-recorder\"}'::json, false, $5, $6, $7,"
            "  $8::bigint)",
            8, nullptr, ep, nullptr, nullptr, 0);
          if (PQresultStatus(r) != PGRES_COMMAND_OK)
            LOG(ERROR) << "[motion_poller] insert event: "
                       << PQerrorMessage(impl_->conn);
          PQclear(r);
        }

        // INSERT smartDetectObjects.
        {
          static const char kAttr[] = "{\"confidence\":75}";
          const char* sp[] = {
            sdo_id.c_str(), new_event_id.c_str(), new_thumb_id.c_str(),
            cam_id.c_str(), obj_type.c_str(), kAttr,
            ts_str.c_str(), now_str.c_str(), now_str.c_str()
          };
          PGresult* r = PQexecParams(impl_->conn,
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

        // INSERT smartDetectRaws.
        {
          const std::string payload =
              motion_poller_internal::build_sdr_payload(start_ms, obj_type);
          const char* rp[] = {
            sdr_id.c_str(), cam_id.c_str(), payload.c_str(),
            ts_str.c_str(), now_str.c_str(), now_str.c_str()
          };
          PGresult* r = PQexecParams(impl_->conn,
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

        // INSERT thumbnail (cropped JPEG).
        {
          const int jpeg_len = static_cast<int>(cropped.size());
          const char* jpeg_ptr = reinterpret_cast<const char*>(cropped.data());
          const char* tp2[] = {
            new_thumb_id.c_str(), cam_id.c_str(), new_event_id.c_str(),
            ts_str.c_str(), now_str.c_str(), now_str.c_str(), jpeg_ptr
          };
          const int lengths[] = { 0, 0, 0, 0, 0, 0, jpeg_len };
          const int formats[] = { 0, 0, 0, 0, 0, 0, 1 };  // last = binary
          PGresult* r = PQexecParams(impl_->conn,
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

        // Fire alarm notification.
        if (impl_->alarm_notifier && !cam_mac.empty()) {
          impl_->alarm_notifier->notify(obj_type, cam_mac,
                                        new_event_id, start_ms);
        }

        LOG(INFO) << "[motion_poller] " << obj_type << " detected in "
                  << cam_id << " (motion event " << ev_id << ")";

        // Update high-water mark.
        impl_->hwm[cam_id] = start_ms;
      }
    }

    PQclear(res);

  sleep:
    // Sleep in short increments so stop() is responsive.
    for (int s = 0; s < impl_->poll_interval_sec && running_.load(); ++s)
      std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  LOG(INFO) << "[motion_poller] stopped";
}

}  // namespace onvif
