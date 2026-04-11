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

#include "unifi_camera_config.hpp"

#include <libpq-fe.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "camera_change_log.hpp"

namespace unifi {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string build_connstr(const DbConfig& db) {
  std::string s =
    "host="    + db.host +
    " port="   + std::to_string(db.port) +
    " dbname=" + db.dbname +
    " user="   + db.user;
  if (!db.password.empty())
    s += " password=" + db.password;
  return s;
}

// RAII wrapper for PGconn to avoid repetitive cleanup code.
struct PgConn {
  PGconn* conn{nullptr};
  explicit PgConn(const DbConfig& db) {
    conn = PQconnectdb(build_connstr(db).c_str());
  }
  ~PgConn() { if (conn) PQfinish(conn); }
  PgConn(const PgConn&)            = delete;
  PgConn& operator=(const PgConn&) = delete;
  bool ok() const { return PQstatus(conn) == CONNECTION_OK; }
  std::string error() const { return PQerrorMessage(conn); }
};

// ---------------------------------------------------------------------------
// Minimal flat-JSON string-value extractor.
//
// Handles the subset produced by PostgreSQL's JSONB output for the
// thirdPartyCameraInfo column: a single-level object with string or null
// values.  Returns an empty string when the key is absent or its value is
// null.
// ---------------------------------------------------------------------------
static std::string json_get(const std::string& json, const std::string& key) {
  std::string needle = "\"" + key + "\":";
  auto pos = json.find(needle);
  if (pos == std::string::npos) return {};
  pos += needle.size();

  // skip optional whitespace
  while (pos < json.size() && json[pos] == ' ') ++pos;
  if (pos >= json.size()) return {};

  if (json[pos] == 'n') return {};  // null

  if (json[pos] != '"') {
    // bare token (number / bool) -- read until delimiter
    std::string val;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}')
      val += json[pos++];
    return val;
  }

  // quoted string with basic escape handling
  ++pos;
  std::string val;
  while (pos < json.size() && json[pos] != '"') {
    if (json[pos] == '\\' && pos + 1 < json.size()) {
      ++pos;
      switch (json[pos]) {
        case '"':  val += '"';  break;
        case '\\': val += '\\'; break;
        case '/':  val += '/';  break;
        case 'n':  val += '\n'; break;
        case 'r':  val += '\r'; break;
        case 't':  val += '\t'; break;
        default:   val += json[pos]; break;
      }
    } else {
      val += json[pos];
    }
    ++pos;
  }
  return val;
}

// ---------------------------------------------------------------------------
// Build a PostgreSQL array literal from a vector of strings:
//   {"id1","id2","id3"}
// ---------------------------------------------------------------------------
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
// load_cameras (third-party)
// ---------------------------------------------------------------------------

absl::StatusOr<std::vector<onvif::CameraConfig>> load_cameras(
    const DbConfig& db) {
  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError("unifi::load_cameras: " + pg.error());

  const char* sql =
    "SELECT id, mac, host, \"thirdPartyCameraInfo\" "
    "FROM cameras "
    "WHERE \"isThirdPartyCamera\" = true "
    "  AND \"isAdopted\" = true "
    "  AND host IS NOT NULL";

  PGresult* res = PQexec(pg.conn, sql);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError("unifi::load_cameras query: " + err);
  }

  std::vector<onvif::CameraConfig> cameras;
  int nrows = PQntuples(res);
  for (int i = 0; i < nrows; ++i) {
    const char* id_c   = PQgetvalue(res, i, 0);
    const char* mac_c  = PQgetvalue(res, i, 1);
    const char* host_c = PQgetvalue(res, i, 2);
    const char* info_c = PQgetvalue(res, i, 3);
    if (!id_c || !host_c || !info_c || PQgetisnull(res, i, 2)) continue;

    std::string info(info_c);
    std::string username     = json_get(info, "username");
    std::string password     = json_get(info, "password");
    std::string snapshot_url = json_get(info, "snapshotUrl");
    std::string port         = json_get(info, "port");
    if (username.empty() || password.empty()) continue;

    std::string ip = host_c;
    if (!port.empty() && port != "80" && port != "0")
      ip += ":" + port;

    onvif::CameraConfig cam;
    cam.id       = std::string(id_c);
    cam.mac      = mac_c ? std::string(mac_c) : std::string();
    cam.ip       = ip;
    cam.user     = username;
    cam.password = password;

    if (!snapshot_url.empty()) {
      const auto scheme_end = snapshot_url.find("://");
      const auto path_start =
          (scheme_end == std::string::npos) ? 0 : scheme_end + 3;
      const auto slash = snapshot_url.find('/', path_start);
      cam.snapshot_url = cam.http_base()
          + ((slash == std::string::npos) ? "/" : snapshot_url.substr(slash));
    }

    cameras.push_back(cam);
  }

  PQclear(res);
  return cameras;
}

// ---------------------------------------------------------------------------
// load_first_party_cameras (by explicit ID list)
// ---------------------------------------------------------------------------

absl::StatusOr<std::vector<FirstPartyCamera>> load_first_party_cameras(
    const std::vector<std::string>& camera_ids,
    const DbConfig& db) {
  if (camera_ids.empty()) return std::vector<FirstPartyCamera>{};

  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError(
        "unifi::load_first_party_cameras: " + pg.error());

  std::string arr = pg_array(camera_ids);
  const char* sql =
    "SELECT id, name, mac "
    "FROM cameras "
    "WHERE id = ANY($1) "
    "  AND \"isThirdPartyCamera\" = false "
    "  AND \"isAdopted\" = true";

  const char* params[1] = { arr.c_str() };
  PGresult* res = PQexecParams(pg.conn, sql, 1, nullptr, params,
                               nullptr, nullptr, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError(
        "unifi::load_first_party_cameras query: " + err);
  }

  std::vector<FirstPartyCamera> cameras;
  int nrows = PQntuples(res);
  for (int i = 0; i < nrows; ++i) {
    FirstPartyCamera c;
    c.id   = PQgetvalue(res, i, 0);
    c.name = PQgetisnull(res, i, 1) ? "" : PQgetvalue(res, i, 1);
    c.mac  = PQgetisnull(res, i, 2) ? "" : PQgetvalue(res, i, 2);
    cameras.push_back(std::move(c));
  }

  PQclear(res);
  return cameras;
}

// ---------------------------------------------------------------------------
// load_all_nonsmartdetect_first_party (auto-discover)
// ---------------------------------------------------------------------------

absl::StatusOr<std::vector<FirstPartyCamera>>
load_all_nonsmartdetect_first_party(const DbConfig& db) {
  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError(
        "unifi::load_all_nonsmartdetect_first_party: " + pg.error());

  const char* sql =
    "SELECT id, name, mac "
    "FROM cameras "
    "WHERE \"isThirdPartyCamera\" = false "
    "  AND \"isAdopted\" = true "
    "  AND (\"featureFlags\"::jsonb->>'hasSmartDetect' IS NULL "
    "       OR \"featureFlags\"::jsonb->>'hasSmartDetect' = 'false')";

  PGresult* res = PQexec(pg.conn, sql);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError(
        "unifi::load_all_nonsmartdetect_first_party query: " + err);
  }

  std::vector<FirstPartyCamera> cameras;
  int nrows = PQntuples(res);
  for (int i = 0; i < nrows; ++i) {
    FirstPartyCamera c;
    c.id   = PQgetvalue(res, i, 0);
    c.name = PQgetisnull(res, i, 1) ? "" : PQgetvalue(res, i, 1);
    c.mac  = PQgetisnull(res, i, 2) ? "" : PQgetvalue(res, i, 2);
    cameras.push_back(std::move(c));
  }

  PQclear(res);
  return cameras;
}

// ---------------------------------------------------------------------------
// enable_smart_detect — internal per-camera implementation
// ---------------------------------------------------------------------------

static absl::Status enable_smart_detect_impl(
    PGconn* conn,
    const std::string& cam_id,
    CameraChangeLog* log) {
  // When logging, capture old values first.
  if (log) {
    const char* sel_sql =
      "SELECT \"featureFlags\"::jsonb->'smartDetectTypes', "
      "       \"smartDetectSettings\"::jsonb->'objectTypes', "
      "       \"featureFlags\"::jsonb->'hasSmartDetect' "
      "FROM cameras WHERE id = $1";
    const char* p[1] = { cam_id.c_str() };
    PGresult* sel = PQexecParams(conn, sel_sql, 1, nullptr, p,
                                 nullptr, nullptr, 0);
    if (PQresultStatus(sel) == PGRES_TUPLES_OK && PQntuples(sel) > 0) {
      std::string old_sdt = PQgetisnull(sel, 0, 0)
          ? "" : PQgetvalue(sel, 0, 0);
      std::string old_ot  = PQgetisnull(sel, 0, 1)
          ? "" : PQgetvalue(sel, 0, 1);
      std::string old_hsd = PQgetisnull(sel, 0, 2)
          ? "" : PQgetvalue(sel, 0, 2);
      // We'll log after the UPDATE succeeds, using these captured values.
      PQclear(sel);

      // Perform the UPDATE.
      const char* upd_sql =
        "UPDATE cameras "
        "SET \"featureFlags\" = jsonb_set(jsonb_set("
        "      \"featureFlags\"::jsonb,"
        "      '{smartDetectTypes}',"
        "      '[\"person\",\"vehicle\",\"animal\","
        "\"package\",\"licensePlate\"]'::jsonb"
        "    ), '{hasSmartDetect}', 'true'::jsonb)::json,"
        "    \"smartDetectSettings\" = jsonb_set("
        "      \"smartDetectSettings\"::jsonb,"
        "      '{objectTypes}',"
        "      '[\"person\",\"vehicle\",\"animal\",\"package\"]'::jsonb"
        "    )::json,"
        "    \"updatedAt\" = NOW() "
        "WHERE id = $1 "
        "  AND ("
        "    (\"featureFlags\"::jsonb -> 'smartDetectTypes') IS NULL"
        "    OR (\"featureFlags\"::jsonb -> 'smartDetectTypes') = '[]'::jsonb"
        "    OR NOT (\"featureFlags\"::jsonb -> 'smartDetectTypes')"
        "           @> '\"licensePlate\"'::jsonb"
        "    OR (\"smartDetectSettings\"::jsonb -> 'objectTypes') IS NULL"
        "    OR (\"smartDetectSettings\"::jsonb -> 'objectTypes') = '[]'::jsonb"
        "    OR (\"featureFlags\"::jsonb -> 'hasSmartDetect') IS NULL"
        "    OR (\"featureFlags\"::jsonb -> 'hasSmartDetect') <> 'true'::jsonb"
        "  )";
      PGresult* upd = PQexecParams(conn, upd_sql, 1, nullptr, p,
                                   nullptr, nullptr, 0);
      if (PQresultStatus(upd) != PGRES_COMMAND_OK) {
        std::string err = PQresultErrorMessage(upd);
        PQclear(upd);
        return absl::InternalError(
            "unifi::enable_smart_detect update: " + err);
      }
      // Log only if the UPDATE actually changed a row.
      const char* ct = PQcmdTuples(upd);
      if (ct && ct[0] != '0') {
        static const char kNewSdt[] =
            "[\"person\",\"vehicle\",\"animal\","
            "\"package\",\"licensePlate\"]";
        static const char kNewOt[] =
            "[\"person\",\"vehicle\",\"animal\",\"package\"]";
        log->record(cam_id, "featureFlags.smartDetectTypes", old_sdt, kNewSdt);
        log->record(cam_id, "smartDetectSettings.objectTypes", old_ot, kNewOt);
        log->record(cam_id, "featureFlags.hasSmartDetect", old_hsd, "true");
      }
      PQclear(upd);
      return absl::OkStatus();
    }
    PQclear(sel);
  }

  // No-log path (or SELECT failed — fall through to normal UPDATE).
  const char* sql =
    "UPDATE cameras "
    "SET \"featureFlags\" = jsonb_set(jsonb_set("
    "      \"featureFlags\"::jsonb,"
    "      '{smartDetectTypes}',"
    "      '[\"person\",\"vehicle\",\"animal\","
    "\"package\",\"licensePlate\"]'::jsonb"
    "    ), '{hasSmartDetect}', 'true'::jsonb)::json,"
    "    \"smartDetectSettings\" = jsonb_set("
    "      \"smartDetectSettings\"::jsonb,"
    "      '{objectTypes}',"
    "      '[\"person\",\"vehicle\",\"animal\",\"package\"]'::jsonb"
    "    )::json,"
    "    \"updatedAt\" = NOW() "
    "WHERE id = $1 "
    "  AND ("
    "    (\"featureFlags\"::jsonb -> 'smartDetectTypes') IS NULL"
    "    OR (\"featureFlags\"::jsonb -> 'smartDetectTypes') = '[]'::jsonb"
    "    OR NOT (\"featureFlags\"::jsonb -> 'smartDetectTypes')"
    "           @> '\"licensePlate\"'::jsonb"
    "    OR (\"smartDetectSettings\"::jsonb -> 'objectTypes') IS NULL"
    "    OR (\"smartDetectSettings\"::jsonb -> 'objectTypes') = '[]'::jsonb"
    "    OR (\"featureFlags\"::jsonb -> 'hasSmartDetect') IS NULL"
    "    OR (\"featureFlags\"::jsonb -> 'hasSmartDetect') <> 'true'::jsonb"
    "  )";
  const char* params[1] = { cam_id.c_str() };
  PGresult* res = PQexecParams(conn, sql, 1, nullptr, params,
                               nullptr, nullptr, 0);
  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError("unifi::enable_smart_detect update: " + err);
  }
  PQclear(res);
  return absl::OkStatus();
}

// ---------------------------------------------------------------------------
// enable_smart_detect (CameraConfig overload — third-party cameras)
// ---------------------------------------------------------------------------

absl::Status enable_smart_detect(
    const std::vector<onvif::CameraConfig>& cameras,
    const DbConfig& db,
    CameraChangeLog* log) {
  if (cameras.empty()) return absl::OkStatus();

  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError("unifi::enable_smart_detect: " + pg.error());

  for (const auto& cam : cameras) {
    auto s = enable_smart_detect_impl(pg.conn, cam.id, log);
    if (!s.ok()) return s;
  }
  return absl::OkStatus();
}

// ---------------------------------------------------------------------------
// enable_smart_detect (FirstPartyCamera overload)
// ---------------------------------------------------------------------------

absl::Status enable_smart_detect(
    const std::vector<FirstPartyCamera>& cameras,
    const DbConfig& db,
    CameraChangeLog* log) {
  if (cameras.empty()) return absl::OkStatus();

  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError("unifi::enable_smart_detect: " + pg.error());

  for (const auto& cam : cameras) {
    auto s = enable_smart_detect_impl(pg.conn, cam.id, log);
    if (!s.ok()) return s;
  }
  return absl::OkStatus();
}

// ---------------------------------------------------------------------------
// ensure_smart_detect_zones — internal implementation
//
// Updates cameras in @p ids that have an empty smartDetectZones array.
// ---------------------------------------------------------------------------

static absl::Status ensure_zones_impl(
    PGconn* conn,
    const std::vector<std::string>& ids,
    CameraChangeLog* log) {
  static const char kDefaultZone[] =
    "[{\"id\":1,\"name\":\"Default\",\"color\":\"#AB46BC\","
    "\"points\":[[0,0],[1,0],[1,1],[0,1]],\"sensitivity\":50,"
    "\"objectTypes\":[\"person\",\"vehicle\",\"animal\",\"package\"],"
    "\"isTriggerLightEnabled\":false,\"source\":\"unifi-protect\","
    "\"triggerAccessTypes\":[],\"enableAccessLPOnlyMode\":false,"
    "\"mergeId\":\"Default-1\"}]";

  if (log) {
    // When logging: per-camera SELECT + UPDATE to capture old value.
    for (const auto& id : ids) {
      const char* sel_sql =
        "SELECT \"smartDetectZones\"::text "
        "FROM cameras WHERE id = $1 "
        "  AND (\"smartDetectZones\" IS NULL "
        "       OR \"smartDetectZones\"::jsonb = '[]'::jsonb)";
      const char* p[1] = { id.c_str() };
      PGresult* sel = PQexecParams(conn, sel_sql, 1, nullptr, p,
                                   nullptr, nullptr, 0);
      if (PQresultStatus(sel) != PGRES_TUPLES_OK || PQntuples(sel) == 0) {
        PQclear(sel);
        continue;  // already has zones
      }
      std::string old_val = PQgetisnull(sel, 0, 0)
          ? "" : PQgetvalue(sel, 0, 0);
      PQclear(sel);

      const char* upd_sql =
        "UPDATE cameras "
        "SET \"smartDetectZones\" = $1::json, "
        "    \"updatedAt\" = NOW() "
        "WHERE id = $2 "
        "  AND (\"smartDetectZones\" IS NULL "
        "       OR \"smartDetectZones\"::jsonb = '[]'::jsonb)";
      const char* up[2] = { kDefaultZone, id.c_str() };
      PGresult* upd = PQexecParams(conn, upd_sql, 2, nullptr, up,
                                   nullptr, nullptr, 0);
      if (PQresultStatus(upd) != PGRES_COMMAND_OK) {
        std::string err = PQresultErrorMessage(upd);
        PQclear(upd);
        return absl::InternalError(
            "unifi::ensure_smart_detect_zones update: " + err);
      }
      const char* ct = PQcmdTuples(upd);
      if (ct && ct[0] != '0')
        log->record(id, "smartDetectZones", old_val, kDefaultZone);
      PQclear(upd);
    }
    return absl::OkStatus();
  }

  // Batch update (no-log path): single UPDATE for all provided camera IDs.
  std::string arr = pg_array(ids);
  const char* sql =
    "UPDATE cameras "
    "SET \"smartDetectZones\" = $1::json, "
    "    \"updatedAt\" = NOW() "
    "WHERE id = ANY($2) "
    "  AND (\"smartDetectZones\" IS NULL "
    "       OR \"smartDetectZones\"::jsonb = '[]'::jsonb)";
  const char* params[2] = { kDefaultZone, arr.c_str() };
  PGresult* res = PQexecParams(conn, sql, 2, nullptr, params,
                               nullptr, nullptr, 0);
  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError(
        "unifi::ensure_smart_detect_zones update: " + err);
  }
  PQclear(res);
  return absl::OkStatus();
}

// ---------------------------------------------------------------------------
// ensure_smart_detect_zones (CameraConfig overload — third-party cameras)
// ---------------------------------------------------------------------------

absl::Status ensure_smart_detect_zones(
    const std::vector<onvif::CameraConfig>& cameras,
    const DbConfig& db,
    CameraChangeLog* log) {
  if (cameras.empty()) return absl::OkStatus();

  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError(
        "unifi::ensure_smart_detect_zones: " + pg.error());

  std::vector<std::string> ids;
  ids.reserve(cameras.size());
  for (const auto& c : cameras) ids.push_back(c.id);
  return ensure_zones_impl(pg.conn, ids, log);
}

// ---------------------------------------------------------------------------
// ensure_smart_detect_zones (FirstPartyCamera overload)
// ---------------------------------------------------------------------------

absl::Status ensure_smart_detect_zones(
    const std::vector<FirstPartyCamera>& cameras,
    const DbConfig& db,
    CameraChangeLog* log) {
  if (cameras.empty()) return absl::OkStatus();

  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError(
        "unifi::ensure_smart_detect_zones: " + pg.error());

  std::vector<std::string> ids;
  ids.reserve(cameras.size());
  for (const auto& c : cameras) ids.push_back(c.id);
  return ensure_zones_impl(pg.conn, ids, log);
}

// ---------------------------------------------------------------------------
// set_rtsp_audio
// ---------------------------------------------------------------------------

absl::Status set_rtsp_audio(bool enable, const DbConfig& db,
                             CameraChangeLog* log) {
  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError("unifi::set_rtsp_audio: " + pg.error());

  if (log) {
    // Per-camera: capture old value before update.
    const char* sel_sql =
      "SELECT id, "
      "       \"thirdPartyCameraInfo\"::jsonb->>'enableRtspAudio' "
      "FROM cameras "
      "WHERE \"isThirdPartyCamera\" = true "
      "  AND \"isAdopted\" = true "
      "  AND (\"thirdPartyCameraInfo\"::jsonb->>'hasAudio') = 'true'";
    PGresult* sel = PQexec(pg.conn, sel_sql);
    if (PQresultStatus(sel) == PGRES_TUPLES_OK) {
      int nrows = PQntuples(sel);
      for (int i = 0; i < nrows; ++i) {
        std::string cam_id = PQgetvalue(sel, i, 0);
        std::string old_val = PQgetisnull(sel, i, 1)
            ? "" : PQgetvalue(sel, i, 1);
        log->record(cam_id,
                    "thirdPartyCameraInfo.enableRtspAudio",
                    old_val,
                    enable ? "true" : "false");
      }
    }
    PQclear(sel);
  }

  const char* val = enable ? "true" : "false";
  const char* sql =
    "UPDATE cameras "
    "SET \"thirdPartyCameraInfo\" = jsonb_set("
    "      \"thirdPartyCameraInfo\"::jsonb,"
    "      '{enableRtspAudio}',"
    "      $1::jsonb"
    "    )::json,"
    "    \"updatedAt\" = NOW() "
    "WHERE \"isThirdPartyCamera\" = true "
    "  AND \"isAdopted\" = true "
    "  AND (\"thirdPartyCameraInfo\"::jsonb->>'hasAudio') = 'true'";

  const char* params[1] = { val };
  PGresult* res = PQexecParams(pg.conn, sql, 1, nullptr, params,
                               nullptr, nullptr, 0);
  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError("unifi::set_rtsp_audio update: " + err);
  }
  PQclear(res);
  return absl::OkStatus();
}

// ---------------------------------------------------------------------------
// rollback_camera_changes
// ---------------------------------------------------------------------------

absl::StatusOr<int> rollback_camera_changes(
    const std::string& scope,
    const std::string& log_path,
    const std::vector<std::string>& first_party_ids,
    const DbConfig& db) {
  const bool do_tp = (scope == "third_party" || scope == "all");
  const bool do_fp = (scope == "first_party" || scope == "all");

  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError(
        "unifi::rollback_camera_changes: " + pg.error());

  auto records = CameraChangeLog::read_all(log_path);
  int updated = 0;

  if (!records.empty()) {
    // Build the earliest old_value per (camera_id, column).
    struct Original {
      std::string old_value;
    };
    std::map<std::pair<std::string, std::string>, Original> originals;
    for (const auto& r : records) {
      auto key = std::make_pair(r.camera_id, r.column);
      if (originals.find(key) == originals.end())
        originals[key] = { r.old_value };
    }

    // Determine which camera IDs are first-party (from the provided list).
    std::set<std::string> fp_set(first_party_ids.begin(),
                                  first_party_ids.end());

    for (const auto& [key, orig] : originals) {
      const auto& cam_id = key.first;
      const auto& column = key.second;

      // Check scope: is this camera in the right set?
      bool is_fp = fp_set.count(cam_id) > 0;
      if (is_fp && !do_fp) continue;
      if (!is_fp && !do_tp) continue;

      // Apply the original value back.  Map column names to SQL.
      std::string sql;
      if (column == "featureFlags.smartDetectTypes") {
        sql =
          "UPDATE cameras "
          "SET \"featureFlags\" = jsonb_set("
          "      \"featureFlags\"::jsonb,"
          "      '{smartDetectTypes}',"
          "      $1::jsonb"
          "    )::json,"
          "    \"updatedAt\" = NOW() "
          "WHERE id = $2";
      } else if (column == "smartDetectSettings.objectTypes") {
        sql =
          "UPDATE cameras "
          "SET \"smartDetectSettings\" = jsonb_set("
          "      \"smartDetectSettings\"::jsonb,"
          "      '{objectTypes}',"
          "      $1::jsonb"
          "    )::json,"
          "    \"updatedAt\" = NOW() "
          "WHERE id = $2";
      } else if (column == "smartDetectZones") {
        sql =
          "UPDATE cameras "
          "SET \"smartDetectZones\" = $1::json, "
          "    \"updatedAt\" = NOW() "
          "WHERE id = $2";
      } else if (column == "featureFlags.hasSmartDetect") {
        sql =
          "UPDATE cameras "
          "SET \"featureFlags\" = jsonb_set("
          "      \"featureFlags\"::jsonb,"
          "      '{hasSmartDetect}',"
          "      $1::jsonb"
          "    )::json,"
          "    \"updatedAt\" = NOW() "
          "WHERE id = $2";
      } else if (column == "thirdPartyCameraInfo.enableRtspAudio") {
        sql =
          "UPDATE cameras "
          "SET \"thirdPartyCameraInfo\" = jsonb_set("
          "      \"thirdPartyCameraInfo\"::jsonb,"
          "      '{enableRtspAudio}',"
          "      $1::jsonb"
          "    )::json,"
          "    \"updatedAt\" = NOW() "
          "WHERE id = $2";
      } else {
        LOG(WARNING) << "[rollback] unknown column: " << column
                     << " (camera " << cam_id << ")";
        continue;
      }

      // Use the recorded old value.  If empty, reset to empty array (for
      // array columns) or null (for scalar columns like hasSmartDetect).
      std::string val;
      if (!orig.old_value.empty()) {
        val = orig.old_value;
      } else if (column == "featureFlags.hasSmartDetect") {
        val = "null";
      } else {
        val = "[]";
      }
      const char* params[2] = { val.c_str(), cam_id.c_str() };
      PGresult* res = PQexecParams(pg.conn, sql.c_str(), 2, nullptr, params,
                                   nullptr, nullptr, 0);
      if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG(WARNING) << "[rollback] failed for camera " << cam_id
                     << " col " << column << ": "
                     << PQresultErrorMessage(res);
      } else {
        const char* ct = PQcmdTuples(res);
        if (ct && ct[0] != '0') ++updated;
      }
      PQclear(res);
    }

    LOG(INFO) << "[rollback] restored " << updated
              << " camera column(s) from change log";
    return updated;
  }

  // No change log file — guesstimate based on current code.
  if (do_tp) {
    LOG(INFO) << "[rollback] no change log found; resetting third-party "
              << "cameras to empty smart detect config";

    const char* sql =
      "UPDATE cameras "
      "SET \"featureFlags\" = jsonb_set("
      "      jsonb_set("
      "        \"featureFlags\"::jsonb,"
      "        '{smartDetectTypes}',"
      "        '[]'::jsonb"
      "      ),"
      "      '{hasSmartDetect}',"
      "      'null'::jsonb"
      "    )::json,"
      "    \"smartDetectSettings\" = jsonb_set("
      "      \"smartDetectSettings\"::jsonb,"
      "      '{objectTypes}',"
      "      '[]'::jsonb"
      "    )::json,"
      "    \"smartDetectZones\" = '[]'::json,"
      "    \"updatedAt\" = NOW() "
      "WHERE \"isThirdPartyCamera\" = true "
      "  AND \"isAdopted\" = true";

    PGresult* res = PQexec(pg.conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
      std::string err = PQresultErrorMessage(res);
      PQclear(res);
      return absl::InternalError(
          "unifi::rollback guesstimate update: " + err);
    }
    const char* ct = PQcmdTuples(res);
    if (ct) updated = std::atoi(ct);
    PQclear(res);

    LOG(INFO) << "[rollback] reset " << updated << " third-party camera(s)";
  }

  if (do_fp) {
    LOG(WARNING) << "[rollback] no change log found; cannot rollback "
                 << "first-party cameras without a log";
  }

  return updated;
}

}  // namespace unifi
