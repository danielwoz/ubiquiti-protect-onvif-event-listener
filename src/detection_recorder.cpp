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
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "contention_profiler.hpp"
#include "event_enricher.hpp"
#include "pg_util.hpp"
#include "protect_version.hpp"
#include "util.hpp"
#include "alarm_notifier.hpp"
#include "jpeg_crop.hpp"
#include "msr_client.hpp"
#include "object_detect.hpp"
#include "protect_user_id_provider.hpp"
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
  // True for ONVIF topics that fire once with no matching "ended"
  // notification (line crossing being the canonical case).  on_event()
  // gives these a synthetic duration and closes them immediately rather
  // than leaving a row with end IS NULL for the stale-event purge to
  // delete five minutes later.
  bool        momentary{false};
};

// Topics that are valid ONVIF but carry no detection we can act on --
// device telemetry, I/O pin state, and scene-change/tamper signals.
// Listed explicitly so the unhandled-topic warning stays a signal about
// missing *detection* support rather than firing on every camera's
// routine housekeeping.  Monitoring/OperatingTime/LastClockSynchronization
// in particular sends PropertyOperation="Changed" on a timer, so without
// this it would warn on virtually every camera.
bool is_known_non_detection_topic(const std::string& topic) {
  static const char* kPrefixes[] = {
    "tns1:Monitoring/",            // ProcessorUsage, OperatingTime/*
    "tns1:Device/Trigger/",        // DigitalInput, Relay
    "tns1:VideoSource/GlobalSceneChange/",  // tamper / camera moved
    "tns1:VideoSource/ImageTooBlurry",
    "tns1:VideoSource/ImageTooDark",
    "tns1:VideoSource/ImageTooBright",
    "tns1:VideoSource/SignalLoss",
    "tns1:VideoSource/ImagingService/",
    "tns1:Media/",
    "tns1:RecordingConfig/",
    "tns1:PTZController/",
  };
  for (const char* p : kPrefixes) {
    if (topic.rfind(p, 0) == 0) return true;
  }
  // Some firmware emits a bare "tns1:Device" with a single "device"
  // field as a keepalive.  Not a detection, and not worth a warning.
  return topic == "tns1:Device";
}

// Map an ONVIF tt:ClassType token to our internal detection type.
// Returns empty when the value is absent or something we don't model.
//
// ClassTypes is the ONVIF-standard object class and is far more
// trustworthy than a vendor's Rule name -- the analytics engine that
// tracked the object put it there.  Observed values in the field:
// Human, Vehicle, Animal.
//
// The spec also allows Face and LicensePlate.  Protect has no filter
// for either, so each maps to the class of the thing it is attached
// to: a face means a person is there, a license plate means a vehicle
// is there.
std::string class_from_class_types(const OnvifEvent& ev) {
  auto it = ev.data.find("ClassTypes");
  if (it == ev.data.end() || it->second.empty()) return {};
  const std::string& v = it->second;
  if (v == "Vehicle" || v == "LicensePlate") return "vehicle";
  if (v == "Animal")                         return "animal";
  if (v == "Human"   || v == "Face")         return "human";
  return {};
}

std::optional<Detection> classify(const OnvifEvent& ev,
                                   const std::string& fallback_type) {
  // --- Camera 108 style: FieldDetector ObjectsInside ---
  if (ev.topic == "tns1:RuleEngine/FieldDetector/ObjectsInside") {
    auto rule_it   = ev.source.find("Rule");
    auto inside_it = ev.data.find("IsInside");
    if (rule_it == ev.source.end() || inside_it == ev.data.end())
      return {};

    // Prefer the ONVIF-standard ClassTypes when the camera sends it --
    // it comes from the analytics engine that tracked the object.  Only
    // fall back to reading the vendor Rule name when it is absent.
    //
    // Hikvision sends the rule name in the Rule field (e.g.
    // "MyFieldDetector1") -- the camera's AcuSense classifier has
    // already filtered to human/vehicle implicitly, so fall back to the
    // default object type and let NanoDet-M refine the class on the
    // snapshot.
    std::string type = class_from_class_types(ev);
    if (type.empty()) {
      if      (rule_it->second == "Human")   type = "human";
      else if (rule_it->second == "Vehicle") type = "vehicle";
      else                                   type = fallback_type;
    }

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

  // --- Reolink: face detection (MyRuleDetector) ---
  // Same Source/State shape as the People/Vehicle siblings above.  A face
  // implies a person, so it maps to the same object type Protect shows for
  // PeopleDetect rather than introducing a class Protect has no filter for.
  if (ev.topic == "tns1:RuleEngine/MyRuleDetector/FaceDetect") {
    auto it = ev.data.find("State");
    if (it == ev.data.end()) return {};
    return Detection{"human", it->second == "true", ev.event_time};
  }

  // --- Reolink: animal detection (MyRuleDetector) ---
  // Reolink names this DogCatDetect; Protect's equivalent smart-detect
  // class is the broader "animal".
  if (ev.topic == "tns1:RuleEngine/MyRuleDetector/DogCatDetect") {
    auto it = ev.data.find("State");
    if (it == ev.data.end()) return {};
    return Detection{"animal", it->second == "true", ev.event_time};
  }

  // --- Reolink doorbell: visitor at the door (MyRuleDetector) ---
  // Doorbell-specific "someone is standing here" signal; a visitor is a
  // person as far as Protect's smart-detect classes go.
  if (ev.topic == "tns1:RuleEngine/MyRuleDetector/Visitor") {
    auto it = ev.data.find("State");
    if (it == ev.data.end()) return {};
    return Detection{"human", it->second == "true", ev.event_time};
  }

  // --- Reolink: package detection (MyRuleDetector) ---
  // Maps straight onto Protect's own "package" smart-detect class.
  if (ev.topic == "tns1:RuleEngine/MyRuleDetector/Package") {
    auto it = ev.data.find("State");
    if (it == ev.data.end()) return {};
    return Detection{"package", it->second == "true", ev.event_time};
  }

  // --- IVS line crossing (Dahua and compatibles) ---
  // Momentary: the camera reports the instant an object crossed the line
  // and never sends a matching "ended" notification, so there is no
  // State/IsInside boolean to read.  ONVIF only guarantees ObjectId here;
  // no object class is carried, so this behaves like generic motion --
  // fallback_type unless NanoDet-M can refine it from the snapshot
  // (from_fallback=true).  on_event() applies the synthetic duration.
  if (ev.topic == "tns1:RuleEngine/LineDetector/Crossed") {
    // Stateful topics can safely act on PropertyOperation="Initialized"
    // because it carries a real current State/IsInside value.  A
    // momentary topic has no state to replay -- an Initialized crossing
    // is the camera echoing its subscription set, not something that
    // just happened -- so acting on it would invent an event on every
    // reconnect.
    if (ev.property_op == "Initialized") return {};
    // Field captures show this topic carrying ClassTypes (Human /
    // Vehicle / Animal) alongside ObjectId, so we usually get a real
    // class straight from the camera.  Only when it's missing do we
    // fall back and let NanoDet-M infer from the snapshot.
    const std::string cls = class_from_class_types(ev);
    return Detection{cls.empty() ? fallback_type : cls,
                     /*started=*/true, ev.event_time,
                     /*from_fallback=*/cls.empty(), /*momentary=*/true};
  }

  // --- Generic CellMotionDetector/Motion (Amcrest, Lorex, Dahua, etc.) ---
  // Basic pixel-change motion; no object class from ONVIF.  Uses fallback_type
  // unless NanoDet-M (--detect / --detect_override) infers a class from the
  // snapshot — marked from_fallback=true so on_event() can apply that override.
  // The second spelling is a firmware bug seen in the field: the camera
  // leaks the tt: schema prefix into the topic path and truncates the
  // trailing /Motion, emitting "tns1:RuleEngine/tt:CellMotionDetector".
  // The payload is identical (IsMotion), so treat it as the same topic
  // rather than making the user chase a malformed string.
  if (ev.topic == "tns1:RuleEngine/CellMotionDetector/Motion" ||
      ev.topic == "tns1:RuleEngine/tt:CellMotionDetector") {
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
// Detection type helpers (file-local)
// ============================================================
namespace {

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

// Single HTTP GET attempt with optional Basic/Digest auth.  Returns
// empty on any non-200 or transport error.  Called by fetch_snapshot()
// which layers an anonymous-fallback retry on top.
static std::vector<unsigned char> fetch_snapshot_once(
    const std::string& url,
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
    if (http_code != 200) {
      LOG(WARNING) << "[snapshot] HTTP " << http_code << " from " << url
                   << (user.empty() ? " (anon)" : " (auth)");
      buf.clear();
    }
  } else {
    LOG(WARNING) << "[snapshot] curl error: " << curl_easy_strerror(rc)
                 << " for " << url;
    buf.clear();
  }

  curl_easy_cleanup(curl);
  return buf;
}

// Single HTTP GET against Protect's /api/... with an X-UserId header
// derived from @p user_id.  Populates @p http_code with the response
// code (0 on transport error).  Returns the response body or empty on
// any failure.
static std::vector<unsigned char> fetch_via_protect_once(
    const std::string& url,
    const std::string& user_id,
    long* http_code) {  // NOLINT(runtime/int)
  *http_code = 0;
  std::string buf;
  CURL* curl = curl_easy_init();
  if (!curl) return {};
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
  if (rc == CURLE_OK)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    LOG(WARNING) << "[snapshot] via-protect curl error: "
                 << curl_easy_strerror(rc) << " for " << url;
    return {};
  }
  return *http_code == 200
             ? std::vector<unsigned char>(buf.begin(), buf.end())
             : std::vector<unsigned char>{};
}

// GET Protect's /api/cameras/<id>/snapshot?ts=<ms> with X-UserId auth.
// Returns bytes on 200 or empty on failure.  On 401 asks the provider
// to re-discover the user_id and retries once -- matches
// motion_poller::fetch_camera_snapshot_at_ts.
static std::vector<unsigned char> fetch_via_protect(
    const std::string& base_url,
    onvif::ProtectUserIdProvider* provider,
    const std::string& cam_uuid,
    uint64_t ts_ms) {
  if (base_url.empty() || !provider || cam_uuid.empty()) return {};
  const std::string user_id = provider->current();
  if (user_id.empty()) return {};
  const std::string url = base_url + "/api/cameras/" + cam_uuid +
                          "/snapshot?ts=" + std::to_string(ts_ms);
  long http_code = 0;  // NOLINT(runtime/int)
  auto body = fetch_via_protect_once(url, user_id, &http_code);
  if (!body.empty()) return body;
  if (http_code == 401 && provider->try_refresh()) {
    LOG(INFO) << "[snapshot] via-protect 401; retrying after user_id refresh";
    body = fetch_via_protect_once(url, provider->current(), &http_code);
    if (!body.empty()) return body;
  }
  if (http_code != 0) {
    LOG(WARNING) << "[snapshot] via-protect HTTP " << http_code
                 << " from " << url;
  }
  return body;
}

// Fetch a camera snapshot with credentials, falling back to an
// unauthenticated GET on failure.  Common on cameras whose HTTP
// snapshot endpoint accepts anonymous requests even though ONVIF
// requires WSSE, and rescues installs where Protect stored corrupt
// credentials (field-observed on issue #34: literal single-space
// username + password stored in cameras.thirdPartyCameraInfo).
static std::vector<unsigned char> fetch_snapshot(const std::string& url,
                                                  const std::string& user,
                                                  const std::string& password) {
  std::vector<unsigned char> buf = fetch_snapshot_once(url, user, password);
  if (buf.empty() && !user.empty()) {
    LOG(INFO) << "[snapshot] auth fetch of " << url
              << " failed; retrying anonymously";
    buf = fetch_snapshot_once(url, "", "");
    if (!buf.empty()) {
      LOG(INFO) << "[snapshot] anonymous fetch of " << url << " succeeded";
    }
  }
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
  bool use_msr_thumb_ids_{false};

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
    PGresult* res = onvif::pg::ExecWithTimeout(conn_, -1,
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

  void set_use_msr_thumbnail_ids(bool use_msr) override {
    use_msr_thumb_ids_ = use_msr;
  }

  std::string make_thumbnail_id(const std::string& camera_ip,
                                uint64_t           ts_ms) override {
    if (use_msr_thumb_ids_) {
      auto it = ip_to_mac_.find(camera_ip);
      if (it != ip_to_mac_.end() && !it->second.empty())
        return util::make_msr_thumbnail_id(it->second, ts_ms);
    }
    return util::generate_24hex_id();
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
  // Returns -1 on error so callers can detect failure.
  int exec_purge(const char* sql, const char* label) {
    maybe_reconnect();
    PGresult* res = onvif::pg::ExecWithTimeout(conn_, -1, sql);
    int deleted = 0;
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
      const char* tag = PQcmdTuples(res);
      if (tag && *tag) deleted = std::atoi(tag);
    } else {
      LOG(ERROR) << "[pg] " << label << " failed: " << pg_errmsg(res);
      PQclear(res);
      return -1;
    }
    PQclear(res);
    return deleted;
  }

  // Transaction helpers.
  bool begin_txn() {
    PGresult* r = onvif::pg::ExecWithTimeout(conn_, -1, "BEGIN");
    bool ok = PQresultStatus(r) == PGRES_COMMAND_OK;
    if (!ok) LOG(ERROR) << "[pg] BEGIN failed: " << pg_errmsg(r);
    PQclear(r);
    return ok;
  }
  bool commit_txn() {
    PGresult* r = onvif::pg::ExecWithTimeout(conn_, -1, "COMMIT");
    bool ok = PQresultStatus(r) == PGRES_COMMAND_OK;
    if (!ok) LOG(ERROR) << "[pg] COMMIT failed: " << pg_errmsg(r);
    PQclear(r);
    return ok;
  }
  void rollback_txn() {
    PGresult* r = onvif::pg::ExecWithTimeout(conn_, -1, "ROLLBACK");
    if (PQresultStatus(r) != PGRES_COMMAND_OK)
      LOG(ERROR) << "[pg] ROLLBACK failed: " << pg_errmsg(r);
    PQclear(r);
  }

  // Execute a parameterized DML statement; logs errors to stderr (non-fatal).
  void exec_params(const char*        sql,
                   int                nparams,
                   const char* const* params,
                   const int*         lengths = nullptr,
                   const int*         formats = nullptr) {
    maybe_reconnect();
    PGresult* res = onvif::pg::ExecParamsWithTimeout(conn_, -1, sql, nparams, nullptr,
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
                    const std::string& now_str,
                    const std::string& metadata = "",
                    const std::string& thumb_fullfov_id = "") override {
    const std::string& cam_id = camera_id(camera_ip);
    const std::string  ts     = std::to_string(ts_ms);

    // Legacy sparse path: hardcoded "source" marker only.  Triggered when
    // protect_version < 7.1.0 (the caller passes metadata="").  The
    // marker is what coalesce_history + the orphan-purge routines key on
    // to identify our writes among Protect's own.
    if (metadata.empty()) {
      const char* params[] = {
        id.c_str(), ts.c_str(), cam_id.c_str(),
        sdt_json.c_str(), thumb_id.c_str(), now_str.c_str(), now_str.c_str()
      };
      exec_params(
        "INSERT INTO events"
        " (id, type, start, \"cameraId\", score, \"smartDetectTypes\","
        "  metadata, locked, \"thumbnailId\", \"createdAt\", \"updatedAt\")"
        " VALUES ($1, 'smartDetectZone', $2::bigint, $3, 100, $4::json,"
        "         '{\"source\":\"onvif-recorder\"}'::json, false, $5, $6, $7)",
        7, params);
      return;
    }

    // Rich path (Protect 7.1+): caller supplies the full metadata JSON.
    // The "source" marker is still embedded inside the metadata so our
    // orphan-purge / coalesce-history queries continue to identify our
    // writes -- event_enricher::BuildEnrichedMetadata is responsible for
    // including it.  thumbnailFullfovId may be empty (NULL column) when
    // we lack a full-FOV crop; otherwise it points at an MSR-format id.
    const std::string fullfov_or_null = thumb_fullfov_id;  // may be empty -> NULL
    const char* params[] = {
      id.c_str(), ts.c_str(), cam_id.c_str(),
      sdt_json.c_str(), thumb_id.c_str(), now_str.c_str(), now_str.c_str(),
      metadata.c_str(),
      fullfov_or_null.empty() ? nullptr : fullfov_or_null.c_str(),
    };
    exec_params(
      "INSERT INTO events"
      " (id, type, start, \"cameraId\", score, \"smartDetectTypes\","
      "  metadata, locked, \"thumbnailId\", \"createdAt\", \"updatedAt\","
      "  \"thumbnailFullfovId\")"
      " VALUES ($1, 'smartDetectZone', $2::bigint, $3, 100, $4::json,"
      "         $8::json, false, $5, $6, $7, $9)",
      9, params);
  }

  void insert_smart_detect_object_area(
      const std::string& id,
      const std::string& sdo_id,
      int bbox_x1, int bbox_y1, int bbox_x2, int bbox_y2,
      uint64_t detected_at_ms,
      uint64_t last_seen_ms,
      const std::string& now_str) override {
    // TODO: derive areaIndexes from bbox intersection with the 12x10 grid,
    // not the full grid as we do here.  Until then we use full coverage so
    // the UI's "show areas" overlay renders (and the on-disk shape matches
    // event_enricher's synthesised output).
    const std::string area_idx = onvif::enricher::FullGridAreaIndexesSqlArray();
    const std::string x1 = std::to_string(bbox_x1);
    const std::string y1 = std::to_string(bbox_y1);
    const std::string x2 = std::to_string(bbox_x2);
    const std::string y2 = std::to_string(bbox_y2);
    const std::string da = std::to_string(detected_at_ms);
    const std::string la = std::to_string(last_seen_ms);
    (void)now_str;  // smartDetectObjectAreas has no createdAt/updatedAt cols.
    const char* params[] = {
      id.c_str(), sdo_id.c_str(),
      x1.c_str(), y1.c_str(), x2.c_str(), y2.c_str(),
      da.c_str(), la.c_str(),
    };
    // Embed the areaIndexes literal directly (it's a constant ARRAY[...]::int[]).
    const std::string sql =
        "INSERT INTO \"smartDetectObjectAreas\""
        " (id, \"smartDetectObjectId\", \"areaIndexes\","
        "  \"boundingX1\", \"boundingY1\", \"boundingX2\", \"boundingY2\","
        "  \"detectedAt\", \"lastSeenAt\")"
        " VALUES ($1, $2, " + area_idx + ", "
        " $3::bigint, $4::bigint, $5::bigint, $6::bigint,"
        " $7::bigint, $8::bigint)"
        " ON CONFLICT (id) DO NOTHING";
    exec_params(sql.c_str(), 8, params);
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

  // Insert one row into smartDetectTracks.  Required for the iOS
  // app's Find Anything filter to surface our events alongside native
  // first-party smart-detect events.  Payload mirrors the array of
  // track samples Protect itself writes; we only have one sample per
  // event so the array carries a single entry with [-1,-1,-1,-1] as
  // the unknown-bbox sentinel.
  void insert_smart_detect_track(const std::string& id,
                                  const std::string& event_id,
                                  const std::string& camera_ip,
                                  uint64_t           start_ms,
                                  uint64_t           end_ms,
                                  const std::string& obj_type,
                                  int                confidence,
                                  const std::string& now_str) override {
    const std::string& cam_id  = camera_id(camera_ip);
    const uint64_t duration_sec =
        (end_ms > start_ms) ? (end_ms - start_ms) / 1000 : 0;
    const std::string start_str = std::to_string(start_ms);
    const std::string end_str   = std::to_string(end_ms);
    const std::string conf_str  = std::to_string(confidence);
    const std::string dur_str   = std::to_string(duration_sec);
    const std::string payload =
        std::string("[{")
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
    const char* params[] = {
      id.c_str(), event_id.c_str(), cam_id.c_str(),
      payload.c_str(), now_str.c_str(), now_str.c_str()
    };
    exec_params(
      "INSERT INTO \"smartDetectTracks\""
      " (id, \"eventId\", \"cameraId\", payload,"
      "  \"createdAt\", \"updatedAt\")"
      " VALUES ($1, $2, $3, $4::json, $5, $6)",
      6, params);
  }

  // Upsert one label name; return its serial lid, or 0 on failure.
  int upsert_label(const std::string& name, const std::string& now_str) {
    auto it = label_cache_.find(name);
    if (it != label_cache_.end()) return it->second;

    maybe_reconnect();
    const std::string label_id = util::generate_uuid();
    const char* params[] = {
      label_id.c_str(), name.c_str(), now_str.c_str()
    };
    PGresult* res = onvif::pg::ExecParamsWithTimeout(conn_, -1,
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

    const std::string dl_id = util::generate_uuid();
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
        util::now_ms() - static_cast<uint64_t>(days) * 86400000ULL;
    const std::string since_str = std::to_string(since_ms);
    const char* params[] = { since_str.c_str() };
    PGresult* res = onvif::pg::ExecParamsWithTimeout(conn_, -1,
      "SELECT e.id, e.\"cameraId\", e.\"smartDetectTypes\"::text, e.start, e.\"end\""
      " FROM events e"
      " JOIN cameras c ON c.id = e.\"cameraId\""
      "   AND (c.\"isThirdPartyCamera\" = true"
      "        OR (e.metadata::jsonb->>'source') = 'onvif-recorder')"
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

  // Orphan purges are bounded to the last kPurgeWindowDays.  Without a
  // bound this scanned every smartDetectRaws row ever written and
  // evaluated an unindexed metadata::jsonb->>'source' extract per row,
  // which reliably blew the 60 s statement budget on busy consoles
  // (issue #34 -- gleep52's dump logs this exact timeout on every
  // start-up, so the purge never actually ran).  Orphans are created by
  // our own coalesce/delete paths, so anything older than the window was
  // already handled by a previous run.
  static constexpr int kPurgeWindowDays = 7;

  int purge_orphaned_smart_detect_raws() override {
    return exec_purge(
        "DELETE FROM \"smartDetectRaws\" sdr"
        " USING cameras c"
        " WHERE c.id = sdr.\"cameraId\""
        "   AND sdr.timestamp > ("
        "       extract(epoch from now()) * 1000"
        "       - 7 * 86400000)::bigint"
        "   AND (c.\"isThirdPartyCamera\" = true"
        "        OR EXISTS ("
        "          SELECT 1 FROM events em"
        "          WHERE em.\"cameraId\" = sdr.\"cameraId\""
        "            AND (em.metadata::jsonb->>'source') = 'onvif-recorder'"
        "          LIMIT 1"
        "        ))"
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
        "   AND (c.\"isThirdPartyCamera\" = true"
        "        OR EXISTS ("
        "          SELECT 1 FROM events em"
        "          WHERE em.\"cameraId\" = t.\"cameraId\""
        "            AND (em.metadata::jsonb->>'source') = 'onvif-recorder'"
        "          LIMIT 1"
        "        ))"
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
        "   AND (c.\"isThirdPartyCamera\" = true"
        "        OR EXISTS ("
        "          SELECT 1 FROM events em"
        "          WHERE em.\"cameraId\" = o.\"cameraId\""
        "            AND (em.metadata::jsonb->>'source') = 'onvif-recorder'"
        "          LIMIT 1"
        "        ))"
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

  int purge_all_orphaned_rows() override {
    // Each DELETE auto-commits individually so a slow purge on one
    // table (e.g. smartDetectRaws timing out on a busy Protect DB --
    // issue #34 gleep52 dump repeatedly hit the 60s ceiling here)
    // doesn't roll back the others.  Purges are startup-once
    // housekeeping; partial progress is strictly better than an
    // all-or-nothing rollback that leaves everything for next boot.
    maybe_reconnect();
    int total = 0;
    for (const auto& step : {
        std::pair{&PgBackend::purge_orphaned_smart_detect_raws,
                  "smartDetectRaws"},
        std::pair{&PgBackend::purge_orphaned_thumbnails,
                  "thumbnails"},
        std::pair{&PgBackend::purge_orphaned_smart_detect_objects,
                  "smartDetectObjects"},
        std::pair{&PgBackend::purge_orphaned_detection_labels,
                  "detectionLabels"}}) {
      const int n = (this->*step.first)();
      if (n < 0) {
        LOG(WARNING) << "[pg] purge " << step.second
                     << " skipped (DB slow / query cancelled); "
                        "will retry next startup";
        continue;
      }
      total += n;
    }
    return total;
  }

  int purge_stale_open_events(uint64_t older_than_ms) override {
    maybe_reconnect();
    if (!begin_txn()) return 0;

    const std::string cutoff = std::to_string(
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count())
        - older_than_ms);
    const char* p[] = { cutoff.c_str() };

    // Helper: run a parameterized DELETE inside the transaction.
    // Returns false on error (caller should rollback).
    auto exec_step = [&](const char* sql, const char* label) -> bool {
      PGresult* r = onvif::pg::ExecParamsWithTimeout(
          conn_, -1, sql, 1, nullptr, p, nullptr, nullptr, 0);
      if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        LOG(ERROR) << "[pg] purge_stale_open_events (" << label
                   << "): " << pg_errmsg(r);
        PQclear(r);
        return false;
      }
      PQclear(r);
      return true;
    };

    // Delete dependent rows first (smartDetectRaws by timestamp window).
    if (!exec_step(
        "DELETE FROM \"smartDetectRaws\" sdr"
        " USING events e"
        " JOIN cameras c ON c.id = e.\"cameraId\""
        "   AND (c.\"isThirdPartyCamera\" = true"
        "        OR (e.metadata::jsonb->>'source') = 'onvif-recorder')"
        " WHERE e.type = 'smartDetectZone'"
        "   AND e.\"end\" IS NULL"
        "   AND e.start < $1::bigint"
        "   AND sdr.\"cameraId\" = e.\"cameraId\""
        "   AND sdr.timestamp >= e.start"
        "   AND sdr.timestamp <= e.start + 60000",
        "raws")) { rollback_txn(); return 0; }

    if (!exec_step(
        "DELETE FROM thumbnails t"
        " USING events e"
        " JOIN cameras c ON c.id = e.\"cameraId\""
        "   AND (c.\"isThirdPartyCamera\" = true"
        "        OR (e.metadata::jsonb->>'source') = 'onvif-recorder')"
        " WHERE e.type = 'smartDetectZone'"
        "   AND e.\"end\" IS NULL"
        "   AND e.start < $1::bigint"
        "   AND t.\"eventId\" = e.id",
        "thumbnails")) { rollback_txn(); return 0; }

    if (!exec_step(
        "DELETE FROM \"smartDetectObjects\" o"
        " USING events e"
        " JOIN cameras c ON c.id = e.\"cameraId\""
        "   AND (c.\"isThirdPartyCamera\" = true"
        "        OR (e.metadata::jsonb->>'source') = 'onvif-recorder')"
        " WHERE e.type = 'smartDetectZone'"
        "   AND e.\"end\" IS NULL"
        "   AND e.start < $1::bigint"
        "   AND o.\"eventId\" = e.id",
        "sdo")) { rollback_txn(); return 0; }

    if (!exec_step(
        "DELETE FROM \"detectionLabels\" dl"
        " USING events e"
        " JOIN cameras c ON c.id = e.\"cameraId\""
        "   AND (c.\"isThirdPartyCamera\" = true"
        "        OR (e.metadata::jsonb->>'source') = 'onvif-recorder')"
        " WHERE e.type = 'smartDetectZone'"
        "   AND e.\"end\" IS NULL"
        "   AND e.start < $1::bigint"
        "   AND dl.\"eventId\" = e.id",
        "labels")) { rollback_txn(); return 0; }

    // Delete the stale event rows and return count.
    PGresult* r = onvif::pg::ExecParamsWithTimeout(conn_, -1,
        "DELETE FROM events e"
        " USING cameras c"
        " WHERE c.id = e.\"cameraId\""
        "   AND (c.\"isThirdPartyCamera\" = true"
        "        OR (e.metadata::jsonb->>'source') = 'onvif-recorder')"
        "   AND e.type = 'smartDetectZone'"
        "   AND e.\"end\" IS NULL"
        "   AND e.start < $1::bigint",
        1, nullptr, p, nullptr, nullptr, 0);
    int deleted = 0;
    if (PQresultStatus(r) == PGRES_COMMAND_OK) {
      const char* tag = PQcmdTuples(r);
      if (tag && *tag) deleted = std::atoi(tag);
    } else {
      LOG(ERROR) << "[pg] purge_stale_open_events (events): "
                 << pg_errmsg(r);
      PQclear(r);
      rollback_txn();
      return 0;
    }
    PQclear(r);

    if (!commit_txn()) return 0;
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
    PGresult* res = onvif::pg::ExecParamsWithTimeout(conn_, -1,
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

DetectionRecorder::DetectionRecorder() {
  ContentionProfiler::instance().register_mutex(&mu_, "detection_recorder");
}

DetectionRecorder::~DetectionRecorder() = default;

void DetectionRecorder::maybe_emit_hourly_stats() {
  constexpr uint64_t kWindowMs = 3600000ULL;  // 1 hour
  uint64_t expected = stats_window_start_ms_.load();
  const uint64_t now = util::now_ms();
  if (expected == 0) {
    // First call: initialise the window without emitting.
    stats_window_start_ms_.compare_exchange_strong(expected, now);
    return;
  }
  if (now - expected < kWindowMs) return;
  // Try to take ownership of the emit; only one thread wins.
  if (!stats_window_start_ms_.compare_exchange_strong(expected, now)) return;

  const uint64_t events       = stats_events_.exchange(0);
  const uint64_t coalesced    = stats_coalesced_.exchange(0);
  const uint64_t rate_limited = stats_rate_limited_.exchange(0);
  const uint64_t snapshots    = stats_snapshots_.exchange(0);
  const uint64_t msr_ok       = stats_msr_ok_.exchange(0);
  const uint64_t msr_fail     = stats_msr_fail_.exchange(0);
  LOG(INFO) << "[recorder] last 1h: events=" << events
            << " coalesced=" << coalesced
            << " rate_limited=" << rate_limited
            << " snapshots=" << snapshots
            << " msr_ok=" << msr_ok
            << " msr_fail=" << msr_fail;
}

void DetectionRecorder::set_snapshot(const CameraConfig& cam) {
  absl::MutexLock lk(&mu_);
  snapshot_info_[cam.ip] = {cam.snapshot_url, cam.user, cam.password};
  db_->register_camera(cam.ip, cam.id, cam.mac);
  if (!cam.id.empty())  camera_ids_[cam.ip]  = cam.id;
  if (!cam.mac.empty()) camera_macs_[cam.ip] = cam.mac;
}

void DetectionRecorder::set_alarm_notifier(AlarmNotifier* notifier) {
  absl::MutexLock lk(&mu_);
  alarm_notifier_ = notifier;
}

void DetectionRecorder::set_msr_client(MsrClient* msr) {
  absl::MutexLock lk(&mu_);
  msr_client_ = msr;
}

void DetectionRecorder::set_msr_drop_on_failure(bool drop) {
  absl::MutexLock lk(&mu_);
  msr_drop_on_failure_ = drop;
}

void DetectionRecorder::set_msr_burst_window_ms(uint64_t ms) {
  absl::MutexLock lk(&mu_);
  msr_burst_window_ms_ = ms;
}

uint64_t DetectionRecorder::msr_calls_for_testing() const {
  return stats_msr_ok_.load() + stats_msr_fail_.load();
}

DetectionRecorder::MsrSnapshot DetectionRecorder::msr_snapshot() const {
  const uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  auto to_ms_since = [now_ns](uint64_t stamp_ns) -> int64_t {
    if (stamp_ns == 0) return -1;
    if (stamp_ns > now_ns) return 0;
    return static_cast<int64_t>((now_ns - stamp_ns) / 1'000'000);
  };
  MsrSnapshot s;
  s.total_ok      = stats_msr_ok_.load();
  s.total_fail    = stats_msr_fail_.load();
  s.ms_since_ok   = to_ms_since(stats_msr_last_ok_ns_.load());
  s.ms_since_fail = to_ms_since(stats_msr_last_fail_ns_.load());
  return s;
}

uint64_t DetectionRecorder::msr_burst_reuses_for_testing() const {
  return stats_msr_burst_reuses_.load();
}

void DetectionRecorder::set_detector(
    const object_detect::ObjectDetector* detector) {
  absl::MutexLock lk(&mu_);
  detector_ = detector;
}

void DetectionRecorder::set_detect_override(bool override) {
  absl::MutexLock lk(&mu_);
  detect_override_ = override;
}

void DetectionRecorder::set_drop_unclassified_motion(bool drop) {
  absl::MutexLock lk(&mu_);
  drop_unclassified_motion_ = drop;
}

void DetectionRecorder::set_use_msr_thumbnail_ids(bool use_msr) {
  absl::MutexLock lk(&mu_);
  use_msr_thumb_ids_ = use_msr;
  if (db_) db_->set_use_msr_thumbnail_ids(use_msr);
}

void DetectionRecorder::set_buffer(uint32_t pre_sec, uint32_t post_sec) {
  absl::MutexLock lk(&mu_);
  pre_buffer_ms_  = static_cast<uint64_t>(pre_sec)  * 1000;
  post_buffer_ms_ = static_cast<uint64_t>(post_sec) * 1000;
}

void DetectionRecorder::set_momentary_event_sec(uint32_t sec) {
  absl::MutexLock lk(&mu_);
  momentary_event_ms_ = static_cast<uint64_t>(sec) * 1000;
}

void DetectionRecorder::set_coalesce_window(uint32_t sec) {
  absl::MutexLock lk(&mu_);
  coalesce_window_ms_ = static_cast<uint64_t>(sec) * 1000;
}

void DetectionRecorder::set_max_events_per_hour(uint32_t n) {
  absl::MutexLock lk(&mu_);
  max_events_per_hour_ = n;
}

void DetectionRecorder::set_ubv_dir(const std::string& dir) {
  absl::MutexLock lk(&mu_);
  ubv_dir_ = dir;
  // Create the base directory if it doesn't exist (needed for legacy
  // IP-based flat paths; Protect-native paths create subdirs on demand).
  struct stat st{};
  if (stat(dir.c_str(), &st) != 0)
    mkdir(dir.c_str(), 0755);
}

void DetectionRecorder::on_event(const OnvifEvent& ev) {
  stats_events_.fetch_add(1);
  maybe_emit_hourly_stats();

  // --- Per-camera capability tracking and basic-motion suppression ---
  // AI events (FieldDetector, HumanShapeDetect) mark the camera as AI-capable
  // so that subsequent CellMotionDetector events are suppressed.  This avoids
  // false positives from PTZ patrol sweeps on cameras like the Dahua SD4A425DB
  // that emit CellMotionDetector/Motion during every pan.
  // MotionAlarm fires alongside CellMotionDetector on virtually all cameras;
  // it is only used as a fallback for cameras that have neither.
  std::string default_obj_type, cam_override_type;
  {
    absl::MutexLock lk(&mu_);
    if ((ev.topic == "tns1:RuleEngine/FieldDetector/ObjectsInside" ||
         ev.topic == "tns1:UserAlarm/IVA/HumanShapeDetect"         ||
         ev.topic == "tns1:VehicleAlarm/IVB/VehicleDetect"         ||
         ev.topic == "tns1:RuleEngine/MyRuleDetector/PeopleDetect"  ||
         ev.topic == "tns1:RuleEngine/MyRuleDetector/VehicleDetect" ||
         ev.topic == "tns1:RuleEngine/MyRuleDetector/FaceDetect"    ||
         ev.topic == "tns1:RuleEngine/MyRuleDetector/DogCatDetect"  ||
         ev.topic == "tns1:RuleEngine/MyRuleDetector/Visitor"       ||
         ev.topic == "tns1:RuleEngine/MyRuleDetector/Package"       ||
         ev.topic == "tns1:RuleEngine/LineDetector/Crossed") &&
        ev.property_op != "Initialized") {
      ai_capable_cameras_.insert(ev.camera_ip);
    } else if ((ev.topic == "tns1:RuleEngine/CellMotionDetector/Motion" ||
                ev.topic == "tns1:RuleEngine/tt:CellMotionDetector") &&
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
  if (!det) {
    // classify() didn't recognise this topic (or the payload was missing
    // the state field it needs).  Report it once per camera+topic so a
    // camera that only ever emits topics we don't support is visible in
    // the journal -- and therefore in every diagnostic dump -- instead
    // of looking silently dead.  The data keys go in the same line
    // because they are what a handler for the topic would need.
    if (ev.property_op != "Initialized" &&
        !is_known_non_detection_topic(ev.topic)) {
      const std::string key = ev.camera_ip + "|" + ev.topic;
      bool first = false;
      {
        absl::MutexLock lk(&mu_);
        first = reported_unhandled_topics_.insert(key).second;
      }
      if (first) {
        std::string keys;
        for (const auto& [k, v] : ev.data) {
          if (!keys.empty()) keys += ", ";
          keys += k + "=" + v;
        }
        if (keys.empty()) keys = "(no data fields)";
        LOG(WARNING) << '[' << ev.camera_ip << "] unhandled ONVIF topic \""
                     << ev.topic << "\" -- no detection recorded. "
                     << "Data: " << keys
                     << ". Please report this line at "
                     << "https://github.com/danielwoz/"
                        "ubiquiti-protect-onvif-event-listener/issues "
                     << "so support can be added.";
      }
    }
    return;
  }
  if (!cam_override_type.empty())
    det->type = cam_override_type;

  auto key = std::make_pair(ev.camera_ip, det->type);

  if (det->started) {
    // 1. Coalesce / rate-limit checks then snapshot + config read (brief lock, no I/O).
    std::string snap_url, snap_user, snap_pass, ubv_dir, cam_uuid, cam_mac;
    uint64_t pre_ms;
    const object_detect::ObjectDetector* det_ptr;
    bool det_override;
    bool drop_unclassified;
    AlarmNotifier* alarm_notif;
    MsrClient* msr;
    bool msr_drop_on_failure;
    uint64_t msr_burst_window_ms;
    std::string burst_cached_id;  // empty if no recent id within window
    // Non-empty when merging this detection into an existing event row.
    std::string coalesced_event_id;
    // Snapshot-source routing captured under the lock (see below).
    bool        snap_via_protect = false;
    std::string protect_url_copy;
    onvif::ProtectUserIdProvider* protect_user_id_provider_copy = nullptr;
    {
      absl::MutexLock lk(&mu_);

      // Coalescing: merge into an existing event rather than creating a new one.
      // Two cases:
      //   1. Key is already open: camera re-fired "started" before "ended".
      //      Reuse the open event so the burst stays as a single event in the DB.
      //   2. Previous detection ended recently (within coalesce_window_ms_):
      //      re-open the last event so back-to-back detections merge naturally.
      // We don't return early: an SDO + detection labels are still inserted so
      // each detection occurrence is recorded within the coalesced event.
      // Resolve the per-camera coalesce window: --camera_coalesce_window_sec
      // can bump (or drop) the window for a noisy camera (#29).
      uint64_t cam_coalesce_ms = coalesce_window_ms_;
      {
        auto cit = camera_coalesce_window_ms_.find(ev.camera_ip);
        if (cit != camera_coalesce_window_ms_.end())
          cam_coalesce_ms = cit->second;
      }
      if (cam_coalesce_ms > 0) {
        auto oit = open_.find(key);
        if (oit != open_.end()) {
          // Case 1: event is still open — reuse it without touching last_event_.
          coalesced_event_id = oit->second;
        } else {
          // Case 2: event ended recently — re-open it.
          auto lei = last_event_.find(key);
          if (lei != last_event_.end() && lei->second.real_end_ms > 0) {
            const uint64_t cur = util::now_ms();
            if (cur >= lei->second.real_end_ms &&
                cur - lei->second.real_end_ms <= cam_coalesce_ms) {
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
        const uint64_t cutoff = util::now_ms() - 3600000;
        while (!times.empty() && times.front() < cutoff) times.pop_front();
        if (times.size() >= static_cast<std::size_t>(max_events_per_hour_)) {
          LOG(WARNING) << '[' << ev.camera_ip << "] rate limit ("
                       << max_events_per_hour_ << "/h): dropping detection";
          stats_rate_limited_.fetch_add(1);
          return;
        }
      }
      if (!coalesced_event_id.empty())
        stats_coalesced_.fetch_add(1);

      auto it = snapshot_info_.find(ev.camera_ip);
      if (it != snapshot_info_.end()) {
        snap_url  = it->second.url;
        snap_user = it->second.user;
        snap_pass = it->second.password;
      }
      // --camera_snapshot_urls override: rewrite the snapshot URL to
      // http://<camera_ip><path> (auth from the original cam config still
      // applies).  Useful when the ONVIF-advertised snapshotUrl is wrong,
      // e.g. some Dahuas advertise /onvif/snapshot which 404s while
      // /cgi-bin/snapshot.cgi works (#32).
      {
        auto pit = camera_snapshot_url_paths_.find(ev.camera_ip);
        if (pit != camera_snapshot_url_paths_.end() && !pit->second.empty()) {
          const std::string& path = pit->second;
          snap_url = "http://" + ev.camera_ip +
              (path.empty() || path[0] == '/' ? path : "/" + path);
        }
      }
      auto cit = camera_ids_.find(ev.camera_ip);
      if (cit != camera_ids_.end()) cam_uuid = cit->second;
      auto mit = camera_macs_.find(ev.camera_ip);
      if (mit != camera_macs_.end()) cam_mac = mit->second;
      ubv_dir      = ubv_dir_;
      pre_ms       = pre_buffer_ms_;
      det_ptr      = detector_;
      det_override = detect_override_;
      drop_unclassified = drop_unclassified_motion_;
      alarm_notif  = alarm_notifier_;
      msr          = msr_client_;
      msr_drop_on_failure = msr_drop_on_failure_;
      msr_burst_window_ms = msr_burst_window_ms_;
      snap_via_protect =
          camera_snapshot_via_protect_.count(ev.camera_ip) > 0;
      protect_url_copy = protect_url_;
      protect_user_id_provider_copy = protect_user_id_provider_;
      // Probe the burst cache while we still hold the lock.  We can't
      // know the camera_mac yet (cit/mit lookups happen above this
      // line so cam_mac is set), so do this after camera-mac resolution.
      if (msr_burst_window_ms > 0 && !cam_mac.empty()) {
        auto bit = msr_burst_cache_.find(cam_mac);
        if (bit != msr_burst_cache_.end()) {
          const uint64_t age =
              util::now_ms() > bit->second.ts_ms
                  ? util::now_ms() - bit->second.ts_ms : 0;
          if (age <= msr_burst_window_ms) {
            burst_cached_id = bit->second.id;
          }
        }
      }
    }

    // 2. Compute timestamps and IDs (no lock -- needs ip_to_mac_ which is read-only).
    const uint64_t    ts_ms      = util::now_ms() - pre_ms;  // padded start
    const std::string now_str    = util::utc_now_iso8601();
    // Use the existing event ID when coalescing; generate a fresh one otherwise.
    const std::string event_id   = coalesced_event_id.empty() ? util::generate_uuid()
                                                              : coalesced_event_id;
    const std::string sdo_id     = util::generate_uuid();
    const std::string sdr_id     = util::generate_uuid();
    const std::string sdtrk_id   = util::generate_uuid();
    // thumb_id is finalised below, after a potential MSR StoreSnapshots call.
    std::string thumb_id;
    std::string sdt_json   = smart_detect_types_json(det->type);
    std::string obj_type   = sdo_type(det->type);

    // 3. Fetch snapshot if the backend needs it.
    //
    // Two possible sources:
    //   * Direct HTTP against the camera's own snapshot endpoint
    //     (snap_url + snap_user/pass).  Default path.
    //   * Protect's own /api/cameras/<id>/snapshot?ts=<ms> endpoint,
    //     when the camera IP is in --camera_snapshot_via_protect.
    //     Bypasses direct HTTP sessions -- useful for Amcrest/Dahua
    //     firmwares that limit concurrent HTTP and drop snapshot
    //     requests while RTSP is active (issue #34, field-observed).
    //     Falls back to the direct path if Protect can't serve it.
    std::vector<unsigned char> snapshot;
    if (db_->needs_snapshot() && (!snap_url.empty() || snap_via_protect)) {
      if (snap_via_protect && !protect_url_copy.empty() &&
          protect_user_id_provider_copy && !cam_uuid.empty()) {
        LOG(INFO) << '[' << ev.camera_ip
                  << "] fetching snapshot via Protect API for camera "
                  << cam_uuid;
        snapshot = fetch_via_protect(protect_url_copy,
                                     protect_user_id_provider_copy,
                                     cam_uuid, ts_ms);
        if (snapshot.empty() && !snap_url.empty()) {
          LOG(INFO) << '[' << ev.camera_ip
                    << "] via-Protect fetch failed; falling back to "
                    << snap_url;
        }
      }
      if (snapshot.empty() && !snap_url.empty()) {
        LOG(INFO) << '[' << ev.camera_ip << "] fetching snapshot from "
                  << snap_url;
        snapshot = fetch_snapshot(snap_url, snap_user, snap_pass);
      }
      if (snapshot.empty()) {
        LOG(WARNING) << '[' << ev.camera_ip << "] snapshot fetch failed or "
                     << "returned empty (thumbnail will be missing)";
      } else {
        LOG(INFO) << '[' << ev.camera_ip << "] snapshot fetched: "
                  << snapshot.size() << " bytes";
        stats_snapshots_.fetch_add(1);
      }
      // Crop snapshot.
      //   default (no detector):  ONVIF bbox → crop; no bbox → full image
      //   --detect:               ONVIF bbox → crop; no bbox → NanoDet-M → smart-crop
      //   --detect_override:      NanoDet-M → smart-crop (ONVIF bbox ignored)
      if (!snapshot.empty()) {
        int img_w = 0, img_h = 0;
        if (jpeg_read_dimensions(snapshot, &img_w, &img_h)) {
          LOG(INFO) << '[' << ev.camera_ip << "] snapshot dimensions: "
                    << img_w << "x" << img_h;
          const jpeg_crop::BoundingBox* onvif_bbox =
              (ev.bbox && !det_override) ? &*ev.bbox : nullptr;
          std::optional<object_detect::Detection> det_result;
          if (det_ptr && (!onvif_bbox || det_override))
            det_result = det_ptr->detect(snapshot);
          const jpeg_crop::BoundingBox* det_bbox =
              det_result ? &det_result->bbox : nullptr;
          if (onvif_bbox) {
            LOG(INFO) << '[' << ev.camera_ip << "] using ONVIF bbox for crop";
          } else if (det_result) {
            LOG(INFO) << '[' << ev.camera_ip << "] NanoDet-M detected class_id="
                      << det_result->class_id << " ("
                      << object_detect::detection_type(det_result->class_id)
                      << ")";
          } else if (det_ptr) {
            LOG(INFO) << '[' << ev.camera_ip
                      << "] NanoDet-M returned no detection, using smart-crop";
          }
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
          // Drop unclassified generic motion when --drop_unclassified_motion
          // is set: a pixel-change motion event with no per-camera override
          // and no NanoDet-M detection would otherwise be recorded as the
          // default_object_type (person), which floods the timeline with
          // false positives on AI cameras that already emit proper
          // Person/Vehicle/Pet events.  Discard it instead.  Only new events
          // are dropped; a coalescing merge into an existing real event is
          // left untouched.
          //
          // Matched on topic rather than det->from_fallback: a
          // LineDetector/Crossed event that arrives without ClassTypes is
          // also from_fallback, but it is a real rule-engine detection the
          // camera deliberately reported, not pixel noise, so it stays.
          const bool generic_motion =
              ev.topic == "tns1:RuleEngine/CellMotionDetector/Motion" ||
              ev.topic == "tns1:RuleEngine/tt:CellMotionDetector"     ||
              ev.topic == "tns1:VideoSource/MotionAlarm";
          if (drop_unclassified && generic_motion && det->from_fallback &&
              cam_override_type.empty() && !det_result &&
              coalesced_event_id.empty()) {
            LOG(INFO) << '[' << ev.camera_ip
                      << "] dropping unclassified motion event "
                         "(--drop_unclassified_motion, no NanoDet-M detection)";
            return;
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
        } else {
          LOG(WARNING) << '[' << ev.camera_ip << "] failed to read JPEG "
                       << "dimensions from snapshot (" << snapshot.size()
                       << " bytes) — storing uncropped";
        }
      }
    } else if (db_->needs_snapshot() && snap_url.empty()) {
      LOG(INFO) << '[' << ev.camera_ip << "] no snapshot URL configured "
                << "— thumbnail will be missing";
    }

    // 3b. Forward the cropped JPEG to MSR when configured.  MSR persists it as
    // a native UBV thumbnail and returns an id that the Protect UI serves via
    // MSP TCP — indistinguishable from first-party cameras.
    //
    // Burst-coalesce: if a recent successful MSR id is cached for this
    // camera (within msr_burst_window_ms), reuse it and skip the call.
    // Each detection still gets its own DB rows; they all reference the
    // cached thumbnail, so MSR / Protect's thumbnails table see one
    // write per burst rather than one per event.
    bool stored_by_msr = false;
    if (msr && !snapshot.empty() && !cam_mac.empty()
        && !burst_cached_id.empty()) {
      thumb_id = burst_cached_id;
      stored_by_msr = true;
      stats_msr_burst_reuses_.fetch_add(1);
      LOG(INFO) << '[' << ev.camera_ip << "] MSR burst-reuse id="
                << burst_cached_id;
    }
    if (msr && !stored_by_msr && !snapshot.empty() && !cam_mac.empty()) {
      std::string msr_id = msr->StoreSnapshot(
          cam_mac, snapshot.data(), snapshot.size());
      if (!msr_id.empty()) {
        thumb_id = msr_id;
        stored_by_msr = true;
        LOG(INFO) << '[' << ev.camera_ip << "] MSR stored snapshot as id="
                  << msr_id;
        stats_msr_ok_.fetch_add(1);
        stats_msr_last_ok_ns_.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        // Cache for the next event in this burst (under main lock to
        // keep msr_burst_cache_ access serialised).
        if (msr_burst_window_ms > 0) {
          absl::MutexLock lk(&mu_);
          msr_burst_cache_[cam_mac] = {msr_id, util::now_ms()};
        }
      } else {
        stats_msr_fail_.fetch_add(1);
        stats_msr_last_fail_ns_.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (msr_drop_on_failure) {
          // Dropping the snapshot is the right call here: writing it
          // ourselves into the same `thumbnails` table that Protect
          // (via MSR) is also trying to write piles contention onto a
          // hot path and is what knocks Protect's UI offline (#24).
          // Clear the snapshot so the UBV + thumbnails INSERT below
          // are skipped.  thumb_id is left empty; make_thumbnail_id()
          // below still generates a placeholder id for event/SDO rows.
          LOG(WARNING) << '[' << ev.camera_ip
                       << "] MSR StoreSnapshots failed; "
                       << "dropping snapshot (msr_drop_on_failure=true)";
          snapshot.clear();
        } else {
          LOG(WARNING) << '[' << ev.camera_ip
                       << "] MSR StoreSnapshots failed; "
                       << "falling back to local thumbnail write";
        }
      }
    }
    if (thumb_id.empty()) {
      thumb_id = db_->make_thumbnail_id(ev.camera_ip, ts_ms);
    }

    // Build attributes here, after the NanoDet override above may have
    // mutated obj_type -- Protect's Find-Anything filter joins on
    // attributes->>'objectType', so it must match the outer SDO type column.
    //
    // confidence + zone must mirror what Protect's native AI writes for a
    // smart-detect row: a non-zero confidence (we use 100 to match the
    // events.score literal we set below) and a non-empty zone array.  The
    // iOS app dereferences attributes.zone[0] without a length check, so
    // writing zone:[] crashes the mobile app on every recent event for
    // third-party cameras.
    const std::string attributes =
        std::string("{")
        + "\"associatedFaceTrackerID\":null,"
        + "\"blurness\":null,"
        + "\"color\":null,"
        + "\"confidence\":100,"
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

    // Version gate: on Protect 7.1+, write the rich events.metadata + the
    // thumbnailFullfovId + a smartDetectObjectAreas row.  On earlier
    // versions, keep the legacy sparse path EXACTLY as before so existing
    // installs see no behavioural change.
    const bool rich_path = onvif::protect_version::IsAtLeast(7, 1, 0);

    // Build the rich events.metadata + bbox lazily; cheap when not used.
    std::string rich_metadata;
    onvif::enricher::Bbox rich_bbox{0, 0, 0, 0};
    std::string rich_area_id;
    if (rich_path) {
      onvif::enricher::EventInput ein;
      ein.event_id          = event_id;
      ein.camera_id         = cam_uuid;  // populated from camera_ids_ earlier
      ein.event_type        = "smartDetectZone";
      ein.smart_detect_types = {obj_type};
      ein.score             = 100;            // matches sparse-path literal
      ein.thumbnail_id      = thumb_id;
      ein.start_ms          = ts_ms;
      ein.end_ms            = ts_ms;
      // TODO: thread the camera's actual image dimensions through to here.
      // Defaulting matches event_enricher's default; the bbox is placeholder
      // anyway.  When we wire ONVIF tt:BoundingBox into the live path we
      // can compute the real bbox + grid intersection here too.
      ein.image_width       = 2560;
      ein.image_height      = 1440;
      ein.object_ids        = {sdo_id};
      rich_metadata = onvif::enricher::BuildEnrichedMetadata(ein);
      rich_bbox = onvif::enricher::PlaceholderBbox(
          ein.image_width, ein.image_height, obj_type);
      rich_area_id = "sda-" + sdo_id;
    }

    // 4. INSERT into both tables, write thumbnail -- all under lock.
    {
      absl::MutexLock lk(&mu_);

      if (coalesced_event_id.empty()) {
        // New event: insert event row, open it, and record rate-limit timestamp.
        // Rich path passes the enriched metadata + thumbnailFullfovId so the
        // 7.1 UI renders the bbox overlay / weather pill / etc.  Empty
        // strings (legacy path) trigger the backend's hardcoded sparse SQL.
        // TODO: thumbnailFullfovId should be a separate full-FOV snapshot;
        // for now we reuse thumb_id so the column is non-null on 7.1+.
        db_->insert_event(event_id, ts_ms, ev.camera_ip, sdt_json, thumb_id, now_str,
                          rich_metadata,
                          rich_path ? thumb_id : std::string());
        open_[key] = event_id;
        if (max_events_per_hour_ > 0)
          recent_event_times_[ev.camera_ip].push_back(util::now_ms());
      }

      // Always insert SDO + smartDetectRaw + smartDetectTrack for every
      // detection occurrence, even when coalescing -- this records each
      // hit within the merged event.
      db_->insert_sdo(sdo_id, event_id, thumb_id, ev.camera_ip,
                      obj_type, attributes, ts_ms, now_str);
      db_->insert_smart_detect_raw(sdr_id, ev.camera_ip, ts_ms, obj_type, now_str);
      db_->insert_smart_detect_track(sdtrk_id, event_id, ev.camera_ip,
                                      ts_ms, ts_ms, obj_type,
                                      /*confidence=*/100, now_str);
      if (rich_path) {
        db_->insert_smart_detect_object_area(
            rich_area_id, sdo_id,
            rich_bbox.x1, rich_bbox.y1, rich_bbox.x2, rich_bbox.y2,
            ts_ms, ts_ms, now_str);
      }

      // Insert detectionLabels rows so events appear in Protect's find-anything
      // endpoint (which does INNER JOIN on detectionLabels WHERE objectId IS NULL).
      // When coalescing, skip the event-level row (objectId IS NULL) -- the
      // surviving event already has one; only insert the SDO-level row.
      {
        std::vector<std::string> label_names = {
          "eventType:smartDetectZone",
          "smartDetectType:" + obj_type,
        };
        if (!cam_uuid.empty()) {
          label_names.push_back("camera:" + cam_uuid);
          label_names.push_back("zone:" + cam_uuid + ":1");
        }
        const std::vector<int> lids = db_->upsert_labels(label_names, now_str);
        if (!lids.empty()) {
          if (coalesced_event_id.empty())
            db_->insert_detection_label(event_id, "", lids, now_str);  // event-level
          db_->insert_detection_label(event_id, sdo_id, lids, now_str);  // SDO-level
        }
      }

      // Thumbnail: UBV file (if ubv_dir set) + PG thumbnails table.
      // Skipped when MSR has already stored the snapshot natively — MSR owns
      // both the UBV file and MSP TCP lookup path in that case.
      if (!snapshot.empty() && !stored_by_msr) {
        if (!ubv_dir.empty()) {
          std::string ubv_path;
          if (!cam_mac.empty()) {
            // Native Protect path: {base}/YYYY/MM/DD/{MAC}_0_thumbnails_{ts}.ubv
            std::time_t sec = static_cast<std::time_t>(ts_ms / 1000);
            std::tm tm{};
            gmtime_r(&sec, &tm);
            char date_buf[16];
            std::strftime(date_buf, sizeof(date_buf), "%Y/%m/%d", &tm);
            std::string date_str(date_buf);
            // mu_ is already held by the outer lock_guard (line ~1194).
            auto& cached = ubv_path_cache_[cam_mac];
            if (cached.first != date_str) {
              cached.first = date_str;
              cached.second = ubv::protect_path(ubv_dir, cam_mac, ts_ms);
            }
            ubv_path = cached.second;
          } else {
            // Legacy fallback: flat directory with IP-based naming.
            ubv_path = ubv_dir + "/" + ev.camera_ip + "_thumbnails.ubv";
          }
          auto s = ubv::append(ubv_path, {ts_ms, snapshot});
          if (!s.ok()) {
            LOG(WARNING) << "[ubv] append failed (non-fatal): " << s.message();
          }
        }
        db_->write_thumbnail(thumb_id, event_id, ev.camera_ip,
                             ts_ms, now_str, snapshot);
      }
    }  // lock released

    // 5. Trigger Protect automations (HTTP I/O, must be outside lock).
    // AlarmNotifier calls POST /api/automations/{id}/run on the local Protect API.
    // Only notify for new events, not for detections coalesced into an existing one.
    if (coalesced_event_id.empty() && alarm_notif && !cam_mac.empty()) {
      alarm_notif->notify(obj_type, cam_mac, event_id, ts_ms);
    }

    // 6. Momentary topics (line crossing) never send an "ended"
    // notification, so close the row here with a synthetic duration
    // instead of leaving end IS NULL until purge_stale_open_events
    // *deletes* it five minutes later.  Mirrors the normal close path
    // below, but the end is derived from the event timestamp plus the
    // configured window rather than from wall-clock at close time.
    if (det->momentary) {
      absl::MutexLock lk(&mu_);
      auto it = open_.find(key);
      if (it != open_.end()) {
        const uint64_t unpadded_end = ts_ms + momentary_event_ms_;
        const uint64_t end_ms       = unpadded_end + post_buffer_ms_;
        const std::string now_str   = util::utc_now_iso8601();
        const std::string ended_id  = it->second;
        db_->update_event_end(ended_id, end_ms, now_str);
        open_.erase(it);
        // Same bookkeeping the normal close does, so a burst of
        // crossings coalesces into one event rather than N adjacent
        // ones.
        last_event_[key] = {ended_id, unpadded_end};
      }
    }

  } else {
    // Detection ended -- UPDATE the open events row with end time + updatedAt.
    absl::MutexLock lk(&mu_);
    auto it = open_.find(key);
    if (it == open_.end()) return;

    const uint64_t    wall_now = util::now_ms();
    const uint64_t    end_ms   = wall_now + post_buffer_ms_;  // padded end
    const std::string now_str  = util::utc_now_iso8601();
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
    absl::MutexLock lk(&mu_);
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
        const std::string now_str  = util::utc_now_iso8601();
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
  return db_->purge_all_orphaned_rows();
}

int DetectionRecorder::purge_stale_open_events(uint64_t older_than_ms) {
  return db_->purge_stale_open_events(older_than_ms);
}

void DetectionRecorder::set_default_object_type(const std::string& type) {
  absl::MutexLock lk(&mu_);
  default_object_type_ = type;
}

void DetectionRecorder::set_camera_object_type(const std::string& ip,
                                                const std::string& type) {
  absl::MutexLock lk(&mu_);
  camera_object_types_[ip] = type;
}

void DetectionRecorder::set_camera_coalesce_window(const std::string& ip,
                                                    uint32_t sec) {
  absl::MutexLock lk(&mu_);
  if (sec == 0)
    camera_coalesce_window_ms_.erase(ip);
  else
    camera_coalesce_window_ms_[ip] = static_cast<uint64_t>(sec) * 1000;
}

void DetectionRecorder::set_camera_snapshot_url_path(const std::string& ip,
                                                     const std::string& path) {
  absl::MutexLock lk(&mu_);
  if (path.empty())
    camera_snapshot_url_paths_.erase(ip);
  else
    camera_snapshot_url_paths_[ip] = path;
}

void DetectionRecorder::set_protect_snapshot_source(
    const std::string& base_url,
    onvif::ProtectUserIdProvider* provider) {
  absl::MutexLock lk(&mu_);
  protect_url_               = base_url;
  protect_user_id_provider_  = provider;
}

void DetectionRecorder::set_camera_snapshot_via_protect(
    const std::string& camera_ip) {
  absl::MutexLock lk(&mu_);
  camera_snapshot_via_protect_.insert(camera_ip);
}

}  // namespace onvif
