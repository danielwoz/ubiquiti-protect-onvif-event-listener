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

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace unifi {

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

absl::StatusOr<std::vector<onvif::CameraConfig>> load_cameras(
    const DbConfig& db) {
  std::string connstr =
    "host="   + db.host   +
    " port="  + std::to_string(db.port) +
    " dbname=" + db.dbname +
    " user="  + db.user;
  if (!db.password.empty())
    connstr += " password=" + db.password;

  PGconn* conn = PQconnectdb(connstr.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    std::string err = PQerrorMessage(conn);
    PQfinish(conn);
    return absl::InternalError("unifi::load_cameras: " + err);
  }

  const char* sql =
    "SELECT id, mac, host, \"thirdPartyCameraInfo\" "
    "FROM cameras "
    "WHERE \"isThirdPartyCamera\" = true "
    "  AND \"isAdopted\" = true "
    "  AND host IS NOT NULL";

  PGresult* res = PQexec(conn, sql);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    PQfinish(conn);
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
    if (username.empty() || password.empty()) continue;

    cameras.push_back({std::string(id_c),
                       mac_c ? std::string(mac_c) : std::string(),
                       std::string(host_c),
                       username, password, snapshot_url});
  }

  PQclear(res);
  PQfinish(conn);
  return cameras;
}

// ---------------------------------------------------------------------------

absl::Status enable_smart_detect(
    const std::vector<onvif::CameraConfig>& cameras,
    const DbConfig& db) {
  if (cameras.empty()) return absl::OkStatus();

  std::string connstr =
    "host="    + db.host   +
    " port="   + std::to_string(db.port) +
    " dbname=" + db.dbname +
    " user="   + db.user;
  if (!db.password.empty())
    connstr += " password=" + db.password;

  PGconn* conn = PQconnectdb(connstr.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    std::string err = PQerrorMessage(conn);
    PQfinish(conn);
    return absl::InternalError("unifi::enable_smart_detect: " + err);
  }

  // For each camera: if either smartDetectTypes (featureFlags) or objectTypes
  // (smartDetectSettings) is missing or empty, fill both with person+vehicle.
  // Cast via ::jsonb so jsonb_set works even though the columns are json type.
  const char* sql =
    "UPDATE cameras "
    "SET \"featureFlags\" = jsonb_set("
    "      \"featureFlags\"::jsonb,"
    "      '{smartDetectTypes}',"
    "      '[\"person\",\"vehicle\"]'::jsonb"
    "    )::json,"
    "    \"smartDetectSettings\" = jsonb_set("
    "      \"smartDetectSettings\"::jsonb,"
    "      '{objectTypes}',"
    "      '[\"person\",\"vehicle\"]'::jsonb"
    "    )::json,"
    "    \"updatedAt\" = NOW() "
    "WHERE id = $1 "
    "  AND ("
    "    (\"featureFlags\"::jsonb -> 'smartDetectTypes') IS NULL"
    "    OR (\"featureFlags\"::jsonb -> 'smartDetectTypes') = '[]'::jsonb"
    "    OR (\"smartDetectSettings\"::jsonb -> 'objectTypes') IS NULL"
    "    OR (\"smartDetectSettings\"::jsonb -> 'objectTypes') = '[]'::jsonb"
    "  )";

  for (const auto& cam : cameras) {
    const char* params[1] = { cam.id.c_str() };
    PGresult* res = PQexecParams(conn, sql, 1, nullptr, params,
                                 nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
      std::string err = PQresultErrorMessage(res);
      PQclear(res);
      PQfinish(conn);
      return absl::InternalError("unifi::enable_smart_detect update: " + err);
    }
    PQclear(res);
  }

  PQfinish(conn);
  return absl::OkStatus();
}

// ---------------------------------------------------------------------------

absl::Status ensure_smart_detect_zones(
    const std::vector<onvif::CameraConfig>& cameras,
    const DbConfig& db) {
  if (cameras.empty()) return absl::OkStatus();

  std::string connstr =
    "host="    + db.host   +
    " port="   + std::to_string(db.port) +
    " dbname=" + db.dbname +
    " user="   + db.user;
  if (!db.password.empty())
    connstr += " password=" + db.password;

  PGconn* conn = PQconnectdb(connstr.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    std::string err = PQerrorMessage(conn);
    PQfinish(conn);
    return absl::InternalError("unifi::ensure_smart_detect_zones: " + err);
  }

  // A single full-frame Default zone covering person + vehicle.  Matches the
  // format written by Protect for native smart cameras so the UI treats it
  // identically (scope_all_smart_cameras_with_zones filter requires length > 0).
  static const char kDefaultZone[] =
    "[{\"id\":1,\"name\":\"Default\",\"color\":\"#AB46BC\","
    "\"points\":[[0,0],[1,0],[1,1],[0,1]],\"sensitivity\":50,"
    "\"objectTypes\":[\"person\",\"vehicle\"],"
    "\"isTriggerLightEnabled\":false,\"source\":\"unifi-protect\","
    "\"triggerAccessTypes\":[],\"enableAccessLPOnlyMode\":false,"
    "\"mergeId\":\"Default-1\"}]";

  // Update all adopted ONVIF cameras that currently have an empty zone list.
  const char* sql =
    "UPDATE cameras "
    "SET \"smartDetectZones\" = $1::json, "
    "    \"updatedAt\" = NOW() "
    "WHERE \"isThirdPartyCamera\" = true "
    "  AND (\"smartDetectZones\" IS NULL "
    "       OR \"smartDetectZones\"::jsonb = '[]'::jsonb)";

  const char* params[1] = { kDefaultZone };
  PGresult* res = PQexecParams(conn, sql, 1, nullptr, params,
                               nullptr, nullptr, 0);
  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    PQfinish(conn);
    return absl::InternalError(
        "unifi::ensure_smart_detect_zones update: " + err);
  }
  PQclear(res);
  PQfinish(conn);
  return absl::OkStatus();
}

}  // namespace unifi
