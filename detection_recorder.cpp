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

#include "detection_recorder.hpp"

#include <curl/curl.h>
#include <libpq-fe.h>
#include <stddef.h>   // jpeglib.h needs size_t
#include <stdio.h>    // jpeglib.h needs FILE
#include <sys/stat.h>
#include <jpeglib.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "alarm_notifier.hpp"
#include "jpeg_crop.hpp"
#include "object_detect.hpp"
#include "ubv_thumbnail.hpp"

namespace onvif {

// ============================================================
// Event classifier (file-local)
// ============================================================
namespace {

struct Detection {
  std::string type;          // "human", "vehicle", or fallback type
  bool        started;       // true = detection began, false = detection ended
  std::string time;          // ISO-8601 UTC event_time from the OnvifEvent
  bool        from_fallback{false};  // true for generic motion (no ONVIF class)
};

std::optional<Detection> classify(const OnvifEvent& ev,
                                   const std::string& fallback_type) {
  // --- Camera 108 style: FieldDetector ObjectsInside ---
  if (ev.topic == "tns1:RuleEngine/FieldDetector/ObjectsInside") {
    auto rule_it   = ev.source.find("Rule");
    auto inside_it = ev.data.find("IsInside");
    if (rule_it == ev.source.end() || inside_it == ev.data.end())
      return {};

    std::string type;
    if      (rule_it->second == "Human")   type = "human";
    else if (rule_it->second == "Vehicle") type = "vehicle";
    else return {};

    return Detection{type, inside_it->second == "true", ev.event_time};
  }

  // --- HumanShapeDetect (Hikvision knockoff / Dahua) ---
  if (ev.topic == "tns1:UserAlarm/IVA/HumanShapeDetect") {
    auto it = ev.data.find("State");
    if (it == ev.data.end()) return {};
    return Detection{"human", it->second == "true", ev.event_time};
  }

  // --- VehicleDetect (Hikvision knockoff / Dahua) ---
  if (ev.topic == "tns1:VehicleAlarm/IVB/VehicleDetect") {
    auto it = ev.data.find("State");
    if (it == ev.data.end()) return {};
    return Detection{"vehicle", it->second == "true", ev.event_time};
  }

  // --- Reolink: person detection (MyRuleDetector) ---
  if (ev.topic == "tns1:RuleEngine/MyRuleDetector/PeopleDetect") {
    auto it = ev.data.find("State");
    if (it == ev.data.end()) return {};
    return Detection{"human", it->second == "true", ev.event_time};
  }

  // --- Reolink: vehicle detection (MyRuleDetector) ---
  if (ev.topic == "tns1:RuleEngine/MyRuleDetector/VehicleDetect") {
    auto it = ev.data.find("State");
    if (it == ev.data.end()) return {};
    return Detection{"vehicle", it->second == "true", ev.event_time};
  }

  // --- Generic CellMotionDetector/Motion (Amcrest, Lorex, Dahua, etc.) ---
  // Basic pixel-change motion; no object class from ONVIF.  Uses fallback_type
  // unless NanoDet-M (--detect / --detect_override) infers a class from the
  // snapshot — marked from_fallback=true so on_event() can apply that override.
  if (ev.topic == "tns1:RuleEngine/CellMotionDetector/Motion") {
    auto it = ev.data.find("IsMotion");
    if (it == ev.data.end()) return {};
    return Detection{fallback_type, it->second == "true", ev.event_time, true};
  }

  // --- VideoSource/MotionAlarm fallback ---
  // Fires on most cameras alongside CellMotionDetector.  Used only for
  // cameras that have neither CellMotionDetector nor AI events (suppression
  // is handled in on_event()).  Also marked from_fallback=true for NCNN
  // type inference.
  if (ev.topic == "tns1:VideoSource/MotionAlarm") {
    auto it = ev.data.find("State");
    if (it == ev.data.end()) return {};
    return Detection{fallback_type, it->second == "true", ev.event_time, true};
  }

  return {};
}

}  // anonymous namespace

// ============================================================
// Timestamp and ID helpers (file-local)
// ============================================================
namespace {

// Current time as milliseconds since Unix epoch.
static uint64_t now_ms() {
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

// Current time as ISO-8601 UTC string: "YYYY-MM-DDTHH:MM:SS.mmmZ"
static std::string utc_now_iso8601() {
  auto   now = std::chrono::system_clock::now();
  auto   t   = std::chrono::system_clock::to_time_t(now);
  auto   ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
  struct tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  char out[48];
  std::snprintf(out, sizeof(out), "%s.%03dZ", buf, static_cast<int>(ms.count()));
  return out;
}

// Generate a UUID v4 string.
static std::string generate_uuid() {
  static std::random_device rd;
  thread_local std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;

  uint64_t hi = dis(gen);
  uint64_t lo = dis(gen);

  hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
  lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

  char buf[37];
  std::snprintf(buf, sizeof(buf),
    "%08x-%04x-%04x-%04x-%012" PRIx64,
    static_cast<unsigned>(hi >> 32),
    static_cast<unsigned>((hi >> 16) & 0xFFFF),
    static_cast<unsigned>(hi & 0xFFFF),
    static_cast<unsigned>((lo >> 48) & 0xFFFF),
    static_cast<uint64_t>(lo & 0x0000FFFFFFFFFFFFull));
  return buf;
}

// Generate a 24-char lowercase hex ID (12 random bytes).
// Protect routes thumbnailIds with length == 24 to its local DB (thumbnails
// table) rather than to the msp media server, so this format lets Protect
// serve thumbnails we insert directly.
static std::string generate_24hex_id() {
  static std::random_device rd2;
  thread_local std::mt19937_64 gen(rd2());
  std::uniform_int_distribution<uint64_t> dis;
  uint64_t a = dis(gen);
  uint64_t b = dis(gen);
  char buf[25];
  std::snprintf(buf, sizeof(buf), "%016" PRIx64 "%08" PRIx64,
                static_cast<uint64_t>(a),
                static_cast<uint64_t>(b & 0xFFFFFFFFull));
  return buf;
}

// Map our internal detection type to the Ubiquiti smartDetect object type.
static const char* sdo_type(const std::string& det_type) {
  if (det_type == "vehicle") return "vehicle";
  if (det_type == "animal")  return "animal";
  if (det_type == "package") return "package";
  return "person";  // "human" -> "person"
}

// Build the smartDetectTypes JSON array from our detection type.
static std::string smart_detect_types_json(const std::string& det_type) {
  if (det_type == "vehicle") return "[\"vehicle\"]";
  if (det_type == "animal")  return "[\"animal\"]";
  if (det_type == "package") return "[\"package\"]";
  return "[\"person\"]";
}

}  // anonymous namespace

// ============================================================
// Snapshot fetch helpers (file-local)
// ============================================================
namespace {

static size_t curl_write_cb(void* data, size_t size, size_t nmemb, void* userp) {
  auto* buf = static_cast<std::vector<unsigned char>*>(userp);
  const size_t total = size * nmemb;
  const auto*  bytes = static_cast<unsigned char*>(data);
  buf->insert(buf->end(), bytes, bytes + total);
  return total;
}

static std::vector<unsigned char> fetch_snapshot(const std::string& url,
                                                  const std::string& user,
                                                  const std::string& password) {
  std::vector<unsigned char> buf;
  CURL* curl = curl_easy_init();
  if (!curl) return buf;

  curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,        5L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);

  if (!user.empty()) {
    std::string userpwd = user + ":" + password;
    curl_easy_setopt(curl, CURLOPT_USERPWD,  userpwd.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH,  // NOLINT(runtime/int)
                     static_cast<long>(CURLAUTH_DIGEST | CURLAUTH_BASIC));  // NOLINT(runtime/int)
  }

  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK) {
    long http_code = 0;  // NOLINT(runtime/int)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) buf.clear();
  } else {
    buf.clear();
  }

  curl_easy_cleanup(curl);
  return buf;
}

// JPEG error manager for header-only dimension reads.
struct JpegDimErr {
  jpeg_error_mgr base;  // must be first
  bool           fatal;
};

static void jpeg_dim_err_exit(j_common_ptr cinfo) {
  reinterpret_cast<JpegDimErr*>(cinfo->err)->fatal = true;
}
static void jpeg_dim_out_msg(j_common_ptr /*cinfo*/) {}

// Read JPEG image dimensions from header only (no full decompress).
// Returns false on error.
static bool jpeg_read_dimensions(const std::vector<unsigned char>& data,
                                  int* out_w, int* out_h) {
  JpegDimErr err{};
  err.fatal = false;
  jpeg_decompress_struct info{};
  info.err = jpeg_std_error(&err.base);
  err.base.error_exit     = jpeg_dim_err_exit;
  err.base.output_message = jpeg_dim_out_msg;
  jpeg_create_decompress(&info);
  jpeg_mem_src(&info,
               const_cast<unsigned char*>(
                   reinterpret_cast<const unsigned char*>(data.data())),
               static_cast<unsigned long>(data.size()));  // NOLINT(runtime/int)
  jpeg_read_header(&info, TRUE);
  *out_w = static_cast<int>(info.image_width);
  *out_h = static_cast<int>(info.image_height);
  jpeg_destroy_decompress(&info);
  return !err.fatal && *out_w > 0 && *out_h > 0;
}

}  // anonymous namespace

// ============================================================
// PostgreSQL backend
// ============================================================
namespace {

struct PgBackend final : DetectionRecorder::IDbBackend {
  PGconn* conn_{nullptr};

  // Camera IP -> Protect database UUID and MAC.  Populated by register_camera()
  // before the listener starts, then read-only.
  std::map<std::string, std::string> ip_to_id_;
  std::map<std::string, std::string> ip_to_mac_;

  // Cache of label name -> serial lid value from the `labels` table.
  // Populated lazily by upsert_label(); avoids redundant DB queries.
  std::map<std::string, int> label_cache_;

  static absl::StatusOr<std::unique_ptr<PgBackend>> Create(
      const std::string& conninfo) {
    auto b = std::make_unique<PgBackend>(conninfo);
    if (!b->conn_) {
      return absl::InternalError("PostgreSQL connect failed");
    }
    return b;
  }

  explicit PgBackend(const std::string& conninfo) {
    conn_ = PQconnectdb(conninfo.c_str());
    if (PQstatus(conn_) != CONNECTION_OK) {
      PQfinish(conn_);
      conn_ = nullptr;
    }
  }

  ~PgBackend() override {
    if (conn_) PQfinish(conn_);
  }

  // Verify the expected tables are accessible.  The schema already exists
  // in UniFi Protect's database -- we never try to create it.
  absl::Status create_schema() override {
    PGresult* res = PQexec(conn_,
      "SELECT 1 FROM events LIMIT 0;"
      "SELECT 1 FROM \"smartDetectObjects\" LIMIT 0;"
      "SELECT 1 FROM \"smartDetectRaws\" LIMIT 0;"
      "SELECT 1 FROM thumbnails LIMIT 0;");
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
      std::string msg = PQresultErrorMessage(res);
      PQclear(res);
      return absl::InternalError("PostgreSQL table check failed: " + msg);
    }
    PQclear(res);
    return absl::OkStatus();
  }

  void register_camera(const std::string& ip,
                       const std::string& id,
                       const std::string& mac) override {
    if (!id.empty())  ip_to_id_[ip]  = id;
    if (!mac.empty()) ip_to_mac_[ip] = mac;
  }

  std::string make_thumbnail_id(const std::string& /*camera_ip*/,
                                uint64_t           /*ts_ms*/) override {
    // 24-char hex IDs are routed by Protect to its thumbnails DB table
    // (not to msp TCP), letting us insert JPEG data directly.
    return generate_24hex_id();
  }

  bool needs_snapshot() const override { return true; }

  // Look up camera UUID; falls back to the IP string if not registered
  // (prevents a crash while still giving a diagnostic clue in the DB).
  const std::string& camera_id(const std::string& ip) const {
    auto it = ip_to_id_.find(ip);
    return (it != ip_to_id_.end()) ? it->second : ip;
  }

  // Attempt to restore a broken connection.  Called before every query.
  // PQreset() re-uses the same conninfo string; safe to call on a live conn.
  void maybe_reconnect() {
    if (PQstatus(conn_) != CONNECTION_BAD) return;
    LOG(WARNING) << "[pg] connection lost — attempting reconnect";
    PQreset(conn_);
    if (PQstatus(conn_) == CONNECTION_OK)
      LOG(INFO) << "[pg] reconnected";
    else
      LOG(ERROR) << "[pg] reconnect failed: " << PQerrorMessage(conn_);
  }

  // Return the best available error string for a failed query.
  const char* pg_errmsg(PGresult* res) const {
    // PQresultErrorMessage is empty for connection-drop failures;
    // fall back to the connection-level error string.
    const char* msg = res ? PQresultErrorMessage(res) : nullptr;
    return (msg && *msg) ? msg : PQerrorMessage(conn_);
  }

  // Execute a no-parameter DELETE and return the row count; logs on failure.
  int exec_purge(const char* sql, const char* label) {
    maybe_reconnect();
    PGresult* res = PQexec(conn_, sql);
    int deleted = 0;
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
      const char* tag = PQcmdTuples(res);
      if (tag && *tag) deleted = std::atoi(tag);
    } else {
      LOG(ERROR) << "[pg] " << label << " failed: " << pg_errmsg(res);
    }
    PQclear(res);
    return deleted;
  }

  // Execute a parameterized DML statement; logs errors to stderr (non-fatal).
  void exec_params(const char*        sql,
                   int                nparams,
                   const char* const* params,
                   const int*         lengths = nullptr,
                   const int*         formats = nullptr) {
    maybe_reconnect();
    PGresult* res = PQexecParams(conn_, sql, nparams, nullptr,
                                 params, lengths, formats, 0);
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK)
      LOG(ERROR) << "[pg] query failed: " << pg_errmsg(res);
    PQclear(res);
  }

  void insert_event(const std::string& id,
                    uint64_t           ts_ms,
                    const std::string& camera_ip,
                    const std::string& sdt_json,
                    const std::string& thumb_id,
                    const std::string& now_str) override {
    const std::string& cam_id = camera_id(camera_ip);
    const std::string  ts     = std::to_string(ts_ms);
    const char* params[] = {
      id.c_str(), ts.c_str(), cam_id.c_str(),
      sdt_json.c_str(), thumb_id.c_str(), now_str.c_str(), now_str.c_str()
    };
    // locked = false (boolean literal); metadata/score are literals.
    // $4::json casts the JSON text to the json column type.
    exec_params(
      "INSERT INTO events"
      " (id, type, start, \"cameraId\", score, \"smartDetectTypes\","
      "  metadata, locked, \"thumbnailId\", \"createdAt\", \"updatedAt\")"
      " VALUES ($1, 'smartDetectZone', $2::bigint, $3, 100, $4::json,"
      "         '{}'::json, false, $5, $6, $7)",
      7, params);
  }

  void insert_sdo(const std::string& id,
                  const std::string& event_id,
                  const std::string& thumb_id,
                  const std::string& camera_ip,
                  const std::string& obj_type,
                  const std::string& attributes,
                  uint64_t           ts_ms,
                  const std::string& now_str) override {
    const std::string& cam_id = camera_id(camera_ip);
    const std::string  ts     = std::to_string(ts_ms);
    const char* params[] = {
      id.c_str(), event_id.c_str(), thumb_id.c_str(), cam_id.c_str(),
      obj_type.c_str(), attributes.c_str(), ts.c_str(),
      now_str.c_str(), now_str.c_str()
    };
    exec_params(
      "INSERT INTO \"smartDetectObjects\""
      " (id, \"eventId\", \"thumbnailId\", \"cameraId\", type, attributes,"
      "  \"detectedAt\", metadata, \"createdAt\", \"updatedAt\")"
      " VALUES ($1, $2, $3, $4, $5, $6::json, $7::bigint,"
      "         '{}'::jsonb, $8, $9)",
      9, params);
  }

  void update_event_end(const std::string& event_id,
                        uint64_t           end_ms,
                        const std::string& now_str) override {
    const std::string ts = std::to_string(end_ms);
    const char* params[] = { ts.c_str(), now_str.c_str(), event_id.c_str() };
    exec_params(
      "UPDATE events SET \"end\" = $1::bigint, \"updatedAt\" = $2 WHERE id = $3",
      3, params);
  }

  // Insert JPEG thumbnail directly into Protect's thumbnails table.
  // Protect serves 24-char thumbnailIds from this table (not via msp TCP).
  void insert_smart_detect_raw(const std::string& id,
                               const std::string& camera_ip,
                               uint64_t           ts_ms,
                               const std::string& obj_type,
                               const std::string& now_str) override {
    const std::string& cam_id = camera_id(camera_ip);
    const std::string  ts = std::to_string(ts_ms);
    // Minimal payload matching the Protect smartDetectRaws schema.
    // coord [-1,-1,-1,-1] signals "no pixel-space bounding box available".
    const std::string payload =
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
    const char* params[] = {
      id.c_str(), cam_id.c_str(), payload.c_str(),
      ts.c_str(), now_str.c_str(), now_str.c_str()
    };
    exec_params(
      "INSERT INTO \"smartDetectRaws\""
      " (id, \"cameraId\", payload, timestamp, \"createdAt\", \"updatedAt\")"
      " VALUES ($1, $2, $3::json, $4::bigint, $5, $6)",
      6, params);
  }

  // Upsert one label name; return its serial lid, or 0 on failure.
  int upsert_label(const std::string& name, const std::string& now_str) {
    auto it = label_cache_.find(name);
    if (it != label_cache_.end()) return it->second;

    maybe_reconnect();
    const std::string label_id = generate_uuid();
    const char* params[] = {
      label_id.c_str(), name.c_str(), now_str.c_str()
    };
    PGresult* res = PQexecParams(conn_,
      "INSERT INTO labels (id, name, \"createdAt\", \"updatedAt\")"
      " VALUES ($1, $2, $3, $3)"
      " ON CONFLICT (name) DO UPDATE SET \"updatedAt\" = EXCLUDED.\"updatedAt\""
      " RETURNING lid",
      3, nullptr, params, nullptr, nullptr, 0);
    int lid = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
      lid = std::atoi(PQgetvalue(res, 0, 0));
      label_cache_[name] = lid;
    } else {
      LOG(ERROR) << "[pg] upsert label failed: " << pg_errmsg(res);
    }
    PQclear(res);
    return lid;
  }

  std::vector<int> upsert_labels(const std::vector<std::string>& names,
                                  const std::string& now_str) override {
    std::vector<int> lids;
    for (const auto& name : names) {
      int lid = upsert_label(name, now_str);
      if (lid > 0) lids.push_back(lid);
    }
    return lids;
  }

  void insert_detection_label(const std::string&      event_id,
                              const std::string&      object_id,
                              const std::vector<int>& lids,
                              const std::string&      now_str) override {
    if (lids.empty()) return;

    // Build PostgreSQL integer array literal: {1,2,3}
    std::string arr = "{";
    for (std::size_t i = 0; i < lids.size(); ++i) {
      if (i > 0) arr += ',';
      arr += std::to_string(lids[i]);
    }
    arr += '}';

    const std::string dl_id = generate_uuid();
    // Pass empty string for NULL object_id; NULLIF converts it to SQL NULL.
    const char* params[] = {
      dl_id.c_str(), event_id.c_str(), object_id.c_str(),
      arr.c_str(), now_str.c_str(), now_str.c_str()
    };
    exec_params(
      "INSERT INTO \"detectionLabels\""
      " (id, \"eventId\", \"objectId\", labels, \"createdAt\", \"updatedAt\")"
      " VALUES ($1, $2, NULLIF($3, ''), $4::integer[], $5, $6)",
      6, params);
  }

  std::vector<IDbBackend::EventSummary> query_recent_events(int days) override {
    maybe_reconnect();
    const uint64_t since_ms =
        now_ms() - static_cast<uint64_t>(days) * 86400000ULL;
    const std::string since_str = std::to_string(since_ms);
    const char* params[] = { since_str.c_str() };
    PGresult* res = PQexecParams(conn_,
      "SELECT e.id, e.\"cameraId\", e.\"smartDetectTypes\"::text, e.start, e.\"end\""
      " FROM events e"
      " JOIN cameras c ON c.id = e.\"cameraId\" AND c.\"isThirdPartyCamera\" = true"
      " WHERE e.type = 'smartDetectZone'"
      "   AND e.\"end\" IS NOT NULL"
      "   AND e.start >= $1::bigint"
      " ORDER BY e.\"cameraId\", e.\"smartDetectTypes\"::text, e.start",
      1, nullptr, params, nullptr, nullptr, 0);
    std::vector<IDbBackend::EventSummary> result;
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
      LOG(ERROR) << "[pg] query_recent_events failed: " << pg_errmsg(res);
      PQclear(res);
      return result;
    }
    const int n = PQntuples(res);
    result.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
      IDbBackend::EventSummary e;
      e.id        = PQgetvalue(res, i, 0);
      e.camera_id = PQgetvalue(res, i, 1);
      e.sdt_json  = PQgetvalue(res, i, 2);
      e.start_ms  =
          static_cast<uint64_t>(std::stoull(PQgetvalue(res, i, 3)));
      e.end_ms    =
          static_cast<uint64_t>(std::stoull(PQgetvalue(res, i, 4)));
      result.push_back(std::move(e));
    }
    PQclear(res);
    return result;
  }

  void coalesce_events(const std::string& into_id,
                        uint64_t           new_end_ms,
                        const std::string& from_id,
                        const std::string& now_str) override {
    // Extend the surviving event's end time.
    update_event_end(into_id, new_end_ms, now_str);
    // Delete dependent rows and the merged event itself.
    // smartDetectRaws has no eventId column; delete by matching cameraId and
    // timestamp range against the event row before it is removed.
    const char* p[] = { from_id.c_str() };
    exec_params(
        "DELETE FROM \"smartDetectRaws\" sdr"
        " USING events e"
        " WHERE e.id = $1"
        "   AND sdr.\"cameraId\" = e.\"cameraId\""
        "   AND sdr.timestamp >= e.start"
        "   AND sdr.timestamp <= e.\"end\"",
        1, p);
    exec_params(
        "DELETE FROM thumbnails WHERE \"eventId\" = $1", 1, p);
    exec_params(
        "DELETE FROM \"smartDetectObjects\" WHERE \"eventId\" = $1", 1, p);
    exec_params(
        "DELETE FROM \"detectionLabels\" WHERE \"eventId\" = $1", 1, p);
    exec_params(
        "DELETE FROM events WHERE id = $1", 1, p);
  }

  int purge_orphaned_smart_detect_raws() override {
    return exec_purge(
        "DELETE FROM \"smartDetectRaws\" sdr"
        " USING cameras c"
        " WHERE c.id = sdr.\"cameraId\""
        "   AND c.\"isThirdPartyCamera\" = true"
        "   AND NOT EXISTS ("
        "       SELECT 1 FROM events e"
        "       WHERE e.\"cameraId\" = sdr.\"cameraId\""
        "         AND e.type = 'smartDetectZone'"
        "         AND e.start <= sdr.timestamp"
        "         AND (e.\"end\" IS NULL OR e.\"end\" >= sdr.timestamp)"
        "   )",
        "purge_orphaned_smart_detect_raws");
  }

  int purge_orphaned_thumbnails() override {
    return exec_purge(
        "DELETE FROM thumbnails t"
        " USING cameras c"
        " WHERE c.id = t.\"cameraId\""
        "   AND c.\"isThirdPartyCamera\" = true"
        "   AND NOT EXISTS ("
        "       SELECT 1 FROM events e WHERE e.id = t.\"eventId\""
        "   )",
        "purge_orphaned_thumbnails");
  }

  int purge_orphaned_smart_detect_objects() override {
    return exec_purge(
        "DELETE FROM \"smartDetectObjects\" o"
        " USING cameras c"
        " WHERE c.id = o.\"cameraId\""
        "   AND c.\"isThirdPartyCamera\" = true"
        "   AND NOT EXISTS ("
        "       SELECT 1 FROM events e WHERE e.id = o.\"eventId\""
        "   )",
        "purge_orphaned_smart_detect_objects");
  }

  int purge_orphaned_detection_labels() override {
    // detectionLabels has no cameraId column; all orphaned rows are removed.
    return exec_purge(
        "DELETE FROM \"detectionLabels\" dl"
        " WHERE NOT EXISTS ("
        "       SELECT 1 FROM events e WHERE e.id = dl.\"eventId\""
        "   )",
        "purge_orphaned_detection_labels");
  }

  int purge_stale_open_events(uint64_t older_than_ms) override {
    maybe_reconnect();
    const std::string cutoff = std::to_string(
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count())
        - older_than_ms);
    const char* p[] = { cutoff.c_str() };

    // Delete dependent rows first (smartDetectRaws by timestamp window).
    PGresult* r = PQexecParams(conn_,
        "DELETE FROM \"smartDetectRaws\" sdr"
        " USING events e"
        " JOIN cameras c ON c.id = e.\"cameraId\""
        "   AND c.\"isThirdPartyCamera\" = true"
        " WHERE e.type = 'smartDetectZone'"
        "   AND e.\"end\" IS NULL"
        "   AND e.start < $1::bigint"
        "   AND sdr.\"cameraId\" = e.\"cameraId\""
        "   AND sdr.timestamp >= e.start"
        "   AND sdr.timestamp <= e.start + 60000",
        1, nullptr, p, nullptr, nullptr, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK)
      LOG(ERROR) << "[pg] purge_stale_open_events (raws): " << pg_errmsg(r);
    PQclear(r);

    r = PQexecParams(conn_,
        "DELETE FROM thumbnails t"
        " USING events e"
        " JOIN cameras c ON c.id = e.\"cameraId\""
        "   AND c.\"isThirdPartyCamera\" = true"
        " WHERE e.type = 'smartDetectZone'"
        "   AND e.\"end\" IS NULL"
        "   AND e.start < $1::bigint"
        "   AND t.\"eventId\" = e.id",
        1, nullptr, p, nullptr, nullptr, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK)
      LOG(ERROR) << "[pg] purge_stale_open_events (thumbnails): " << pg_errmsg(r);
    PQclear(r);

    r = PQexecParams(conn_,
        "DELETE FROM \"smartDetectObjects\" o"
        " USING events e"
        " JOIN cameras c ON c.id = e.\"cameraId\""
        "   AND c.\"isThirdPartyCamera\" = true"
        " WHERE e.type = 'smartDetectZone'"
        "   AND e.\"end\" IS NULL"
        "   AND e.start < $1::bigint"
        "   AND o.\"eventId\" = e.id",
        1, nullptr, p, nullptr, nullptr, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK)
      LOG(ERROR) << "[pg] purge_stale_open_events (sdo): " << pg_errmsg(r);
    PQclear(r);

    r = PQexecParams(conn_,
        "DELETE FROM \"detectionLabels\" dl"
        " USING events e"
        " JOIN cameras c ON c.id = e.\"cameraId\""
        "   AND c.\"isThirdPartyCamera\" = true"
        " WHERE e.type = 'smartDetectZone'"
        "   AND e.\"end\" IS NULL"
        "   AND e.start < $1::bigint"
        "   AND dl.\"eventId\" = e.id",
        1, nullptr, p, nullptr, nullptr, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK)
      LOG(ERROR) << "[pg] purge_stale_open_events (labels): " << pg_errmsg(r);
    PQclear(r);

    // Delete the stale event rows and return count.
    r = PQexecParams(conn_,
        "DELETE FROM events e"
        " USING cameras c"
        " WHERE c.id = e.\"cameraId\""
        "   AND c.\"isThirdPartyCamera\" = true"
        "   AND e.type = 'smartDetectZone'"
        "   AND e.\"end\" IS NULL"
        "   AND e.start < $1::bigint",
        1, nullptr, p, nullptr, nullptr, 0);
    int deleted = 0;
    if (PQresultStatus(r) == PGRES_COMMAND_OK) {
      const char* tag = PQcmdTuples(r);
      if (tag && *tag) deleted = std::atoi(tag);
    } else {
      LOG(ERROR) << "[pg] purge_stale_open_events (events): " << pg_errmsg(r);
    }
    PQclear(r);
    return deleted;
  }

  void write_thumbnail(const std::string&              thumb_id,
                       const std::string&              event_id,
                       const std::string&              camera_ip,
                       uint64_t                        ts_ms,
                       const std::string&              now_str,
                       const std::vector<unsigned char>& jpeg) override {
    if (jpeg.empty()) return;

    const std::string& cam_id = camera_id(camera_ip);
    const std::string  ts     = std::to_string(ts_ms);

    // Binary parameters: thumb_id, cam_id, event_id, ts, now_str, now_str
    // + jpeg (binary, format=1)
    const char* params[7] = {
      thumb_id.c_str(),
      cam_id.c_str(),
      event_id.c_str(),
      ts.c_str(),
      now_str.c_str(),
      now_str.c_str(),
      reinterpret_cast<const char*>(jpeg.data())
    };
    const int lengths[7] = {
      0, 0, 0, 0, 0, 0,
      static_cast<int>(jpeg.size())
    };
    const int formats[7] = { 0, 0, 0, 0, 0, 0, 1 };  // $7 is binary

    maybe_reconnect();
    PGresult* res = PQexecParams(conn_,
      "INSERT INTO thumbnails"
      " (id, \"cameraId\", \"eventId\", timestamp, \"createdAt\","
      "  \"updatedAt\", content, \"isFullfov\")"
      " VALUES ($1, $2, $3, $4::bigint, $5, $6, $7, false)"
      " ON CONFLICT (id) DO NOTHING",
      7, nullptr, params, lengths, formats, 0);
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK)
      LOG(ERROR) << "[pg] thumbnail insert failed: " << pg_errmsg(res);
    PQclear(res);
  }
};

}  // anonymous namespace

// ============================================================
// DetectionRecorder
// ============================================================

// static factory
absl::StatusOr<std::unique_ptr<DetectionRecorder>> DetectionRecorder::Create(
    const std::string& conn) {
  auto dr = std::unique_ptr<DetectionRecorder>(new DetectionRecorder());
  auto b_or = PgBackend::Create(conn);
  if (!b_or.ok()) return b_or.status();
  dr->db_ = std::move(*b_or);
  absl::Status s = dr->db_->create_schema();
  if (!s.ok()) return s;
  return dr;
}

// static factory for testing
absl::StatusOr<std::unique_ptr<DetectionRecorder>>
DetectionRecorder::CreateWithBackend(std::unique_ptr<IDbBackend> backend) {
  auto dr = std::unique_ptr<DetectionRecorder>(new DetectionRecorder());
  dr->db_ = std::move(backend);
  absl::Status s = dr->db_->create_schema();
  if (!s.ok()) return s;
  return dr;
}

DetectionRecorder::~DetectionRecorder() = default;

void DetectionRecorder::set_snapshot(const CameraConfig& cam) {
  std::lock_guard<std::mutex> lk(mu_);
  snapshot_info_[cam.ip] = {cam.snapshot_url, cam.user, cam.password};
  db_->register_camera(cam.ip, cam.id, cam.mac);
  if (!cam.id.empty())  camera_ids_[cam.ip]  = cam.id;
  if (!cam.mac.empty()) camera_macs_[cam.ip] = cam.mac;
}

void DetectionRecorder::set_alarm_notifier(AlarmNotifier* notifier) {
  std::lock_guard<std::mutex> lk(mu_);
  alarm_notifier_ = notifier;
}

void DetectionRecorder::set_detector(
    const object_detect::ObjectDetector* detector) {
  std::lock_guard<std::mutex> lk(mu_);
  detector_ = detector;
}

void DetectionRecorder::set_detect_override(bool override) {
  std::lock_guard<std::mutex> lk(mu_);
  detect_override_ = override;
}

void DetectionRecorder::set_buffer(uint32_t pre_sec, uint32_t post_sec) {
  std::lock_guard<std::mutex> lk(mu_);
  pre_buffer_ms_  = static_cast<uint64_t>(pre_sec)  * 1000;
  post_buffer_ms_ = static_cast<uint64_t>(post_sec) * 1000;
}

void DetectionRecorder::set_coalesce_window(uint32_t sec) {
  std::lock_guard<std::mutex> lk(mu_);
  coalesce_window_ms_ = static_cast<uint64_t>(sec) * 1000;
}

void DetectionRecorder::set_max_events_per_hour(uint32_t n) {
  std::lock_guard<std::mutex> lk(mu_);
  max_events_per_hour_ = n;
}

void DetectionRecorder::set_ubv_dir(const std::string& dir) {
  std::lock_guard<std::mutex> lk(mu_);
  ubv_dir_ = dir;
  struct stat st{};
  if (stat(dir.c_str(), &st) != 0)
    mkdir(dir.c_str(), 0755);
}

void DetectionRecorder::on_event(const OnvifEvent& ev) {
  // --- Per-camera capability tracking and basic-motion suppression ---
  // AI events (FieldDetector, HumanShapeDetect) mark the camera as AI-capable
  // so that subsequent CellMotionDetector events are suppressed.  This avoids
  // false positives from PTZ patrol sweeps on cameras like the Dahua SD4A425DB
  // that emit CellMotionDetector/Motion during every pan.
  // MotionAlarm fires alongside CellMotionDetector on virtually all cameras;
  // it is only used as a fallback for cameras that have neither.
  std::string default_obj_type, cam_override_type;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if ((ev.topic == "tns1:RuleEngine/FieldDetector/ObjectsInside" ||
         ev.topic == "tns1:UserAlarm/IVA/HumanShapeDetect"         ||
         ev.topic == "tns1:VehicleAlarm/IVB/VehicleDetect"         ||
         ev.topic == "tns1:RuleEngine/MyRuleDetector/PeopleDetect"  ||
         ev.topic == "tns1:RuleEngine/MyRuleDetector/VehicleDetect") &&
        ev.property_op != "Initialized") {
      ai_capable_cameras_.insert(ev.camera_ip);
    } else if (ev.topic == "tns1:RuleEngine/CellMotionDetector/Motion" &&
               ev.property_op != "Initialized") {
      if (ai_capable_cameras_.count(ev.camera_ip)) return;
      cell_motion_cameras_.insert(ev.camera_ip);
    } else if (ev.topic == "tns1:VideoSource/MotionAlarm" &&
               ev.property_op != "Initialized") {
      if (ai_capable_cameras_.count(ev.camera_ip) ||
          cell_motion_cameras_.count(ev.camera_ip)) return;
    }
    default_obj_type = default_object_type_;
    auto it = camera_object_types_.find(ev.camera_ip);
    if (it != camera_object_types_.end())
      cam_override_type = it->second;
  }

  auto det = classify(ev, default_obj_type);
  if (!det) return;
  if (!cam_override_type.empty())
    det->type = cam_override_type;

  auto key = std::make_pair(ev.camera_ip, det->type);

  if (det->started) {
    // 1. Coalesce / rate-limit checks then snapshot + config read (brief lock, no I/O).
    std::string snap_url, snap_user, snap_pass, ubv_dir, cam_uuid, cam_mac;
    uint64_t pre_ms;
    const object_detect::ObjectDetector* det_ptr;
    bool det_override;
    AlarmNotifier* alarm_notif;
    // Non-empty when merging this detection into an existing event row.
    std::string coalesced_event_id;
    {
      std::lock_guard<std::mutex> lk(mu_);

      // Coalescing: merge into an existing event rather than creating a new one.
      // Two cases:
      //   1. Key is already open: camera re-fired "started" before "ended".
      //      Reuse the open event so the burst stays as a single event in the DB.
      //   2. Previous detection ended recently (within coalesce_window_ms_):
      //      re-open the last event so back-to-back detections merge naturally.
      // We don't return early: an SDO + detection labels are still inserted so
      // each detection occurrence is recorded within the coalesced event.
      if (coalesce_window_ms_ > 0) {
        auto oit = open_.find(key);
        if (oit != open_.end()) {
          // Case 1: event is still open — reuse it without touching last_event_.
          coalesced_event_id = oit->second;
        } else {
          // Case 2: event ended recently — re-open it.
          auto lei = last_event_.find(key);
          if (lei != last_event_.end() && lei->second.real_end_ms > 0) {
            const uint64_t cur = now_ms();
            if (cur >= lei->second.real_end_ms &&
                cur - lei->second.real_end_ms <= coalesce_window_ms_) {
              coalesced_event_id = lei->second.event_id;
              open_[key] = coalesced_event_id;
              lei->second.real_end_ms = 0;  // mark as re-opened
            }
          }
        }
      }

      // Rate limiting: only for new events, not coalesced detections.
      if (coalesced_event_id.empty() && max_events_per_hour_ > 0) {
        auto& times = recent_event_times_[ev.camera_ip];
        const uint64_t cutoff = now_ms() - 3600000;
        while (!times.empty() && times.front() < cutoff) times.pop_front();
        if (times.size() >= static_cast<std::size_t>(max_events_per_hour_)) {
          LOG(WARNING) << '[' << ev.camera_ip << "] rate limit ("
                       << max_events_per_hour_ << "/h): dropping detection";
          return;
        }
      }

      auto it = snapshot_info_.find(ev.camera_ip);
      if (it != snapshot_info_.end()) {
        snap_url  = it->second.url;
        snap_user = it->second.user;
        snap_pass = it->second.password;
      }
      auto cit = camera_ids_.find(ev.camera_ip);
      if (cit != camera_ids_.end()) cam_uuid = cit->second;
      auto mit = camera_macs_.find(ev.camera_ip);
      if (mit != camera_macs_.end()) cam_mac = mit->second;
      ubv_dir      = ubv_dir_;
      pre_ms       = pre_buffer_ms_;
      det_ptr      = detector_;
      det_override = detect_override_;
      alarm_notif  = alarm_notifier_;
    }

    // 2. Compute timestamps and IDs (no lock -- needs ip_to_mac_ which is read-only).
    const uint64_t    ts_ms      = now_ms() - pre_ms;  // padded start
    const std::string now_str    = utc_now_iso8601();
    // Use the existing event ID when coalescing; generate a fresh one otherwise.
    const std::string event_id   = coalesced_event_id.empty() ? generate_uuid()
                                                              : coalesced_event_id;
    const std::string sdo_id     = generate_uuid();
    const std::string sdr_id     = generate_uuid();
    const std::string thumb_id   = db_->make_thumbnail_id(ev.camera_ip, ts_ms);
    std::string sdt_json   = smart_detect_types_json(det->type);
    std::string obj_type   = sdo_type(det->type);
    const std::string attributes = "{\"confidence\":0}";

    // 3. Fetch snapshot if the backend needs it.
    std::vector<unsigned char> snapshot;
    if (db_->needs_snapshot() && !snap_url.empty()) {
      snapshot = fetch_snapshot(snap_url, snap_user, snap_pass);
      // Crop snapshot.
      //   default (no detector):  ONVIF bbox → crop; no bbox → full image
      //   --detect:               ONVIF bbox → crop; no bbox → NanoDet-M → smart-crop
      //   --detect_override:      NanoDet-M → smart-crop (ONVIF bbox ignored)
      if (!snapshot.empty()) {
        int img_w = 0, img_h = 0;
        if (jpeg_read_dimensions(snapshot, &img_w, &img_h)) {
          const jpeg_crop::BoundingBox* onvif_bbox =
              (ev.bbox && !det_override) ? &*ev.bbox : nullptr;
          std::optional<object_detect::Detection> det_result;
          if (det_ptr && (!onvif_bbox || det_override))
            det_result = det_ptr->detect(snapshot);
          const jpeg_crop::BoundingBox* det_bbox =
              det_result ? &det_result->bbox : nullptr;
          // If this was a generic motion event (no ONVIF class) and NCNN
          // identified a security-relevant object, use its class to set the
          // detection type (person / vehicle / animal) in all tables.
          // Per-camera type overrides (cam_override_type) take priority.
          if (det->from_fallback && cam_override_type.empty() && det_result) {
            const std::string inferred =
                object_detect::detection_type(det_result->class_id);
            obj_type = inferred;
            sdt_json = smart_detect_types_json(inferred);
          }
          // Only crop when we have a basis: an ONVIF bbox, or a loaded
          // detector (which may yield a result or fall back to smart-crop).
          // Without either, store the full uncropped image.
          if (onvif_bbox || det_ptr) {
            const jpeg_crop::BoundingBox box =
                jpeg_crop::select_crop_box(img_w, img_h, onvif_bbox, det_bbox);
            auto cropped = jpeg_crop::crop(snapshot, box);
            if (!cropped.empty())
              snapshot = std::move(cropped);
          }
        }
      }
    }

    // 4. INSERT into both tables, write thumbnail -- all under lock.
    {
      std::lock_guard<std::mutex> lk(mu_);

      if (coalesced_event_id.empty()) {
        // New event: insert event row, open it, and record rate-limit timestamp.
        db_->insert_event(event_id, ts_ms, ev.camera_ip, sdt_json, thumb_id, now_str);
        open_[key] = event_id;
        if (max_events_per_hour_ > 0)
          recent_event_times_[ev.camera_ip].push_back(now_ms());
      }

      // Always insert SDO + smartDetectRaw for every detection occurrence,
      // even when coalescing -- this records each hit within the merged event.
      db_->insert_sdo(sdo_id, event_id, thumb_id, ev.camera_ip,
                      obj_type, attributes, ts_ms, now_str);
      db_->insert_smart_detect_raw(sdr_id, ev.camera_ip, ts_ms, obj_type, now_str);

      // Insert detectionLabels rows so events appear in Protect's find-anything
      // endpoint (which does INNER JOIN on detectionLabels WHERE objectId IS NULL).
      // When coalescing, skip the event-level row (objectId IS NULL) -- the
      // surviving event already has one; only insert the SDO-level row.
      {
        std::vector<std::string> label_names = {
          "eventType:smartDetectZone",
          "smartDetectType:" + obj_type,
        };
        if (!cam_uuid.empty())
          label_names.push_back("camera:" + cam_uuid);
        const std::vector<int> lids = db_->upsert_labels(label_names, now_str);
        if (!lids.empty()) {
          if (coalesced_event_id.empty())
            db_->insert_detection_label(event_id, "", lids, now_str);  // event-level
          db_->insert_detection_label(event_id, sdo_id, lids, now_str);  // SDO-level
        }
      }

      // Thumbnail: UBV file (if ubv_dir set) + PG thumbnails table.
      if (!snapshot.empty()) {
        if (!ubv_dir.empty()) {
          const std::string ubv_path =
            ubv_dir + "/" + ev.camera_ip + "_thumbnails.ubv";
          auto s = ubv::append(ubv_path, {ts_ms, snapshot});
          if (!s.ok()) {
            LOG(WARNING) << "[ubv] append failed (non-fatal): " << s.message();
          }
        }
        db_->write_thumbnail(thumb_id, event_id, ev.camera_ip,
                             ts_ms, now_str, snapshot);
      }
    }  // lock released

    // 5. Notify UOS alarm manager (HTTP I/O, must be outside lock).
    // alarm_url is non-empty when the camera advertised an alarm service via
    // GetServices; empty means it is not Protect-managed -- skip in that case.
    // The AlarmNotifier always uses its configured --uos_url (host:port).
    // Only notify for new events, not for detections coalesced into an existing one.
    if (coalesced_event_id.empty() && alarm_notif &&
        !cam_mac.empty() && !ev.alarm_url.empty()) {
      alarm_notif->notify(obj_type, cam_mac, event_id, ts_ms);
    }

  } else {
    // Detection ended -- UPDATE the open events row with end time + updatedAt.
    std::lock_guard<std::mutex> lk(mu_);
    auto it = open_.find(key);
    if (it == open_.end()) return;

    const uint64_t    wall_now = now_ms();
    const uint64_t    end_ms   = wall_now + post_buffer_ms_;  // padded end
    const std::string now_str  = utc_now_iso8601();
    const std::string ended_id = it->second;

    db_->update_event_end(ended_id, end_ms, now_str);
    open_.erase(it);

    // Coalescing: remember when this detection ended (wall-clock, unpadded)
    // so the next detection within coalesce_window_ms_ can extend this event.
    last_event_[key] = {ended_id, wall_now};
  }
}

int DetectionRecorder::coalesce_history(int days) {
  uint64_t coalesce_ms;
  {
    std::lock_guard<std::mutex> lk(mu_);
    coalesce_ms = coalesce_window_ms_;
  }
  if (coalesce_ms == 0) return 0;

  // Events are sorted by (camera_id, sdt_json, start_ms) by the backend query.
  auto events = db_->query_recent_events(days);
  if (events.empty()) return 0;

  int merged = 0;
  std::size_t i = 0;
  while (i < events.size()) {
    // Find the end of this (camera_id, sdt_json) group.
    std::size_t j = i + 1;
    while (j < events.size() &&
           events[j].camera_id == events[i].camera_id &&
           events[j].sdt_json  == events[i].sdt_json) {
      ++j;
    }
    // Walk events[i..j): merge adjacent events within the coalesce window.
    std::size_t base = i;
    for (std::size_t k = i + 1; k < j; ++k) {
      const auto& b = events[base];
      const auto& c = events[k];
      // within_window: overlapping OR gap <= coalesce_ms.
      // Short-circuit prevents underflow on the subtraction.
      const bool within =
          (c.start_ms <= b.end_ms) ||
          (c.start_ms - b.end_ms <= coalesce_ms);
      if (within) {
        const uint64_t    new_end  = std::max(b.end_ms, c.end_ms);
        const std::string now_str  = utc_now_iso8601();
        db_->coalesce_events(b.id, new_end, c.id, now_str);
        events[base].end_ms = new_end;
        ++merged;
        // Keep base at the same index so it can absorb further events.
      } else {
        base = k;
      }
    }
    i = j;
  }

  if (merged > 0)
    LOG(INFO) << "[coalesce_history] merged " << merged
              << " event(s) over the last " << days << " day(s)";
  return merged;
}

int DetectionRecorder::purge_orphaned_rows() {
  int total = 0;
  total += db_->purge_orphaned_smart_detect_raws();
  total += db_->purge_orphaned_thumbnails();
  total += db_->purge_orphaned_smart_detect_objects();
  total += db_->purge_orphaned_detection_labels();
  return total;
}

int DetectionRecorder::purge_stale_open_events(uint64_t older_than_ms) {
  return db_->purge_stale_open_events(older_than_ms);
}

void DetectionRecorder::set_default_object_type(const std::string& type) {
  std::lock_guard<std::mutex> lk(mu_);
  default_object_type_ = type;
}

void DetectionRecorder::set_camera_object_type(const std::string& ip,
                                                const std::string& type) {
  std::lock_guard<std::mutex> lk(mu_);
  camera_object_types_[ip] = type;
}

}  // namespace onvif
