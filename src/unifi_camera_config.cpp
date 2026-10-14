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

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "camera_change_log.hpp"
#include "pg_util.hpp"

namespace unifi {

// ---------------------------------------------------------------------------
// Helpers (exposed via namespace unifi::internal for testing)
// ---------------------------------------------------------------------------

namespace internal {

std::string build_connstr(const DbConfig& db) {
  std::string s =
    "host="    + db.host +
    " port="   + std::to_string(db.port) +
    " dbname=" + db.dbname +
    " user="   + db.user;
  if (!db.password.empty())
    s += " password=" + db.password;
  return s;
}

}  // namespace internal

// RAII wrapper for PGconn to avoid repetitive cleanup code.
struct PgConn {
  PGconn* conn{nullptr};
  explicit PgConn(const DbConfig& db) {
    conn = PQconnectdb(internal::build_connstr(db).c_str());
  }
  ~PgConn() { if (conn) PQfinish(conn); }
  PgConn(const PgConn&)            = delete;
  PgConn& operator=(const PgConn&) = delete;
  bool ok() const { return PQstatus(conn) == CONNECTION_OK; }
  std::string error() const { return PQerrorMessage(conn); }
};

namespace internal {

// ---------------------------------------------------------------------------
// Minimal flat-JSON string-value extractor.
//
// Handles the subset produced by PostgreSQL's JSONB output for the
// thirdPartyCameraInfo column: a single-level object with string or null
// values.  Returns an empty string when the key is absent or its value is
// null.
// ---------------------------------------------------------------------------
std::string json_get(const std::string& json, const std::string& key) {
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
std::string pg_array(const std::vector<std::string>& ids) {
  std::string out = "{";
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i > 0) out += ',';
    out += "\"" + ids[i] + "\"";
  }
  out += '}';
  return out;
}

// Case-insensitive substring search.  Returns true when @p needle is found
// anywhere in @p haystack.  Used to match Dahua/Amcrest model strings.
static bool icontains(const std::string& haystack, const std::string& needle) {
  if (needle.empty() || haystack.size() < needle.size()) return false;
  for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      char a = haystack[i + j], b = needle[j];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
      if (a != b) { match = false; break; }
    }
    if (match) return true;
  }
  return false;
}

std::string maybe_rewrite_dahua_snapshot_url(const std::string& camera_type,
                                              const std::string& original_url) {
  // Vendor heuristic: Dahua and Dahua-OEM rebrands.  We intentionally match
  // substrings of the cameras.type column ("Dahua DH-IPC-..." / "Amcrest"
  // etc.) rather than vendor-tag any other way.  No vendor outside this set
  // is known to serve the /onvif/snapshot path while needing /cgi-bin/...
  // instead, so this is conservative.
  const bool dahua_family = icontains(camera_type, "dahua") ||
                            icontains(camera_type, "amcrest");
  if (!dahua_family) return original_url;

  // Find the path component start: after "://host[:port]".
  // Look for the third '/' from the start (skipping the //).
  const std::string sep = "://";
  size_t after_scheme = original_url.find(sep);
  if (after_scheme == std::string::npos) return original_url;
  size_t path_start = original_url.find('/', after_scheme + sep.size());
  if (path_start == std::string::npos) return original_url;

  const std::string path_and_query = original_url.substr(path_start);
  // Match if the path part is exactly "/onvif/snapshot" -- query string OK.
  const std::string bad_path = "/onvif/snapshot";
  if (path_and_query.compare(0, bad_path.size(), bad_path) != 0) {
    return original_url;
  }
  // Only consider it a match if what follows the bad_path is end-of-string,
  // '?' (query), or another '/' (sub-path).  Avoids false positives like
  // "/onvif/snapshots-archive".
  if (path_and_query.size() > bad_path.size()) {
    char c = path_and_query[bad_path.size()];
    if (c != '?' && c != '/') return original_url;
  }

  return original_url.substr(0, path_start) + "/cgi-bin/snapshot.cgi";
}

}  // namespace internal

// ---------------------------------------------------------------------------
// load_cameras (third-party)
// ---------------------------------------------------------------------------

absl::StatusOr<std::vector<onvif::CameraConfig>> load_cameras(
    const DbConfig& db) {
  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError("unifi::load_cameras: " + pg.error());

  // NOTE: we intentionally do NOT SELECT "isConnected" here.  It exists
  // on some Protect versions and not others; the same warning fires
  // for triagers via triage.json's read of cameras.json (build_
  // cameras_json emits whatever columns the schema has).  Keeping the
  // load query narrow means we don't need per-version fallbacks.
  const char* sql =
    "SELECT id, mac, host, \"thirdPartyCameraInfo\", COALESCE(type, ''), "
    "       COALESCE(channels::text, '[]'), "
    "       COALESCE(\"videoCodec\", '') "
    "FROM cameras "
    "WHERE \"isThirdPartyCamera\" = true "
    "  AND \"isAdopted\" = true "
    "  AND host IS NOT NULL";

  PGresult* res = onvif::pg::ExecWithTimeout(pg.conn, -1, sql);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError("unifi::load_cameras query: " + err);
  }

  std::vector<onvif::CameraConfig> cameras;
  int nrows = PQntuples(res);
  auto is_blank_or_space = [](const std::string& s) {
    for (char c : s) {
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    }
    return true;  // includes s.empty()
  };

  for (int i = 0; i < nrows; ++i) {
    const char* id_c   = PQgetvalue(res, i, 0);
    const char* mac_c  = PQgetvalue(res, i, 1);
    const char* host_c = PQgetvalue(res, i, 2);
    const char* info_c = PQgetvalue(res, i, 3);
    const char* type_c = PQgetvalue(res, i, 4);
    const char* chan_c   = PQgetvalue(res, i, 5);
    const char* vcodec_c = PQgetvalue(res, i, 6);
    if (!id_c || !host_c || !info_c || PQgetisnull(res, i, 2)) continue;

    std::string info(info_c);
    std::string username     = internal::json_get(info, "username");
    std::string password     = internal::json_get(info, "password");
    std::string snapshot_url = internal::json_get(info, "snapshotUrl");
    std::string rtsp_url_lq  = internal::json_get(info, "rtspUrlLQ");
    std::string port         = internal::json_get(info, "port");
    std::string camera_type  = type_c ? std::string(type_c) : std::string();
    const std::string host_str = host_c;

    // Loud on the two silent-failure classes we've been diagnosing
    // in-thread on issue #34:
    //
    //   * credentials stored as literal whitespace (Protect's broken-
    //     adoption bug).  We were dropping these cameras silently at
    //     the empty-check below; users then wondered why the camera
    //     never appeared in the listener.  Now we log it explicitly.
    //   * no substream URL captured.  Protect derives rtspUrlLQ from
    //     the camera's own ONVIF GetProfiles at adoption time; a null
    //     here typically means the camera advertises only a main
    //     stream, which is the shape that produces "video not found"
    //     / timeline stalls in Protect's playback pipeline.
    if (is_blank_or_space(username) || is_blank_or_space(password)) {
      LOG(WARNING) << "[unifi_camera_config] " << host_str
                   << " (" << (camera_type.empty() ? "?" : camera_type)
                   << "): credentials look empty/whitespace in Protect's "
                      "thirdPartyCameraInfo -- re-adopt the camera in "
                      "Protect (issue #34).  Skipping.";
      continue;
    }
    if (username.empty() || password.empty()) continue;
    if (rtsp_url_lq.empty()) {
      LOG(WARNING) << "[unifi_camera_config] " << host_str
                   << " (" << (camera_type.empty() ? "?" : camera_type)
                   << "): no substream captured (thirdPartyCameraInfo."
                      "rtspUrlLQ is null).  Protect will likely struggle "
                      "with playback for this camera.  If the camera "
                      "supports a sub-stream, enable it in the camera's "
                      "own settings and re-adopt.";
    }

    std::string ip = host_c;
    if (!port.empty() && port != "80" && port != "0")
      ip += ":" + port;

    // Auto-fix the well-known Dahua/Amcrest "advertises /onvif/snapshot but
    // serves from /cgi-bin/snapshot.cgi" trap (issue #32).  The manual
    // --camera_snapshot_urls flag still wins because it applies after this
    // load step.  No-op for cameras that don't match the heuristic.
    {
      const std::string fixed =
          internal::maybe_rewrite_dahua_snapshot_url(camera_type, snapshot_url);
      if (fixed != snapshot_url) {
        LOG(INFO) << "[unifi_camera_config] rewriting snapshotUrl for "
                  << camera_type << " (" << ip << "): "
                  << snapshot_url << " -> " << fixed;
        snapshot_url = fixed;
      }
    }

    // Per-channel video-feed characteristics.  Codec, resolution,
    // frame rate, bitrate control mode, and IDR (keyframe) interval
    // are the fields that most often drive playback-pipeline failures
    // (e.g. H.265 substream that MSR can decode a still from but
    // can't repackage into fmp4 fragments -- issue #34 territory).
    // Fields come from Protect's `channels` JSONB column verbatim.
    // Small hand-parse so we don't drag a JSON dep into this path;
    // channels is an array of flat objects at top level.
    //
    // Field notes:
    //   * videoCodec may sit inside the channel object OR at the
    //     camera level (varies by camera type); we look for it both
    //     places.  Missing -> "?".
    //   * autoBitrate=true means VBR; false means CBR.
    //   * idrInterval is the keyframe / GOP interval in seconds.
    //   * Protect's channels don't expose an explicit b-frame count
    //     -- IDR interval is the closest proxy we can log.
    if (chan_c && *chan_c) {
      const std::string chan = chan_c;
      // Camera-level fallback for videoCodec.  Protect exposes
      // `videoCodec` as a top-level column on cameras (h264 / h265),
      // and individual channels can also declare their own codec.
      // Prefer per-channel; fall back to the row-level value.
      const std::string camera_codec = vcodec_c ? vcodec_c : "";
      // Iterate top-level {} objects in the array.
      auto scan_object = [&](const std::string& obj) {
        auto get_str = [&](const char* key) -> std::string {
          const std::string needle = std::string("\"") + key + "\":\"";
          const size_t p = obj.find(needle);
          if (p == std::string::npos) return {};
          const size_t s = p + needle.size();
          const size_t e = obj.find('"', s);
          return e == std::string::npos ? std::string()
                                        : obj.substr(s, e - s);
        };
        auto get_int = [&](const char* key) -> int64_t {
          const std::string needle = std::string("\"") + key + "\":";
          size_t p = obj.find(needle);
          if (p == std::string::npos) return -1;
          p += needle.size();
          while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t')) ++p;
          bool neg = false;
          if (p < obj.size() && obj[p] == '-') { neg = true; ++p; }
          int64_t v = 0;
          bool ok = false;
          while (p < obj.size() && obj[p] >= '0' && obj[p] <= '9') {
            v = v * 10 + (obj[p] - '0'); ok = true; ++p;
          }
          return ok ? (neg ? -v : v) : -1;
        };
        auto get_bool = [&](const char* key) -> int {
          const std::string needle = std::string("\"") + key + "\":";
          const size_t p = obj.find(needle);
          if (p == std::string::npos) return -1;
          const size_t s = p + needle.size();
          if (obj.compare(s, 4, "true")  == 0) return 1;
          if (obj.compare(s, 5, "false") == 0) return 0;
          return -1;
        };

        const int64_t ch_id = get_int("id");
        if (ch_id < 0) return;  // not an actual channel object
        const std::string name    = get_str("name");
        const int64_t width       = get_int("width");
        const int64_t height      = get_int("height");
        const int64_t fps         = get_int("fps");
        const int64_t bitrate_bps = get_int("bitrate");
        const int64_t idr_sec     = get_int("idrInterval");
        const int auto_bitrate    = get_bool("autoBitrate");
        std::string codec         = get_str("videoCodec");
        if (codec.empty()) codec = camera_codec;
        if (codec.empty()) codec = "?";
        const std::string rc =
            auto_bitrate == 1 ? "VBR" :
            auto_bitrate == 0 ? "CBR" : "rc=?";
        std::ostringstream os;
        os << "[unifi_camera_config] " << camera_type << " " << ip
           << " ch" << ch_id;
        if (!name.empty()) os << " (" << name << ")";
        os << ": ";
        if (width > 0 && height > 0) os << width << "x" << height;
        else os << "??x??";
        os << " " << codec;
        if (fps > 0) os << " " << fps << "fps";
        if (bitrate_bps > 0) os << " " << (bitrate_bps / 1000) << "kbps";
        if (idr_sec > 0) os << " IDR=" << idr_sec << "s";
        os << " " << rc;
        LOG(INFO) << os.str();
      };

      size_t depth = 0;
      size_t obj_start = std::string::npos;
      for (size_t i = 0; i < chan.size(); ++i) {
        if (chan[i] == '{') {
          if (depth == 0) obj_start = i;
          ++depth;
        } else if (chan[i] == '}') {
          if (depth > 0) --depth;
          if (depth == 0 && obj_start != std::string::npos) {
            scan_object(chan.substr(obj_start, i - obj_start + 1));
            obj_start = std::string::npos;
          }
        }
      }
    }

    onvif::CameraConfig cam;
    cam.id       = std::string(id_c);
    cam.mac      = mac_c ? std::string(mac_c) : std::string();
    cam.ip       = ip;
    cam.user     = username;
    cam.password = password;

    // Use Protect's stored URL verbatim: the snapshot endpoint often lives
    // on a different port from the ONVIF service (e.g. ONVIF on 8000, HTTP
    // snapshot CGI on 80), so reconstructing the host via the ONVIF port
    // yields the wrong URL.
    cam.snapshot_url = std::move(snapshot_url);

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

  std::string arr = internal::pg_array(camera_ids);
  const char* sql =
    "SELECT id, name, mac "
    "FROM cameras "
    "WHERE id = ANY($1) "
    "  AND \"isThirdPartyCamera\" = false "
    "  AND \"isAdopted\" = true";

  const char* params[1] = { arr.c_str() };
  PGresult* res = onvif::pg::ExecParamsWithTimeout(pg.conn, -1, sql, 1, nullptr, params,
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
// load_camera_health (admin UI Camera Health card)
// ---------------------------------------------------------------------------

absl::StatusOr<std::vector<CameraHealth>> load_camera_health(
    const DbConfig& db) {
  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError("unifi::load_camera_health: " + pg.error());

  // One round-trip: per-camera last event start + count in the last hour
  // (0 for cameras with no events).  LATERAL joins keep the per-camera
  // aggregates fast on Postgres' indexes for events.cameraId / start.
  const char* sql =
    "SELECT c.id, c.name, c.host, c.mac, c.\"isThirdPartyCamera\", "
    "       COALESCE(le.last_start, 0) AS last_start, "
    "       COALESCE(c1h.cnt, 0)       AS events_1h "
    "FROM cameras c "
    "LEFT JOIN LATERAL ("
    "  SELECT MAX(start) AS last_start FROM events e "
    "  WHERE e.\"cameraId\" = c.id"
    ") le   ON true "
    "LEFT JOIN LATERAL ("
    "  SELECT COUNT(*) AS cnt FROM events e "
    "  WHERE e.\"cameraId\" = c.id "
    "    AND e.start > "
    "        (extract(epoch from now()) * 1000 - 3600000)::bigint"
    ") c1h  ON true "
    "WHERE c.\"isAdopted\" = true "
    "ORDER BY c.\"isThirdPartyCamera\" DESC, c.name";

  PGresult* res = onvif::pg::ExecWithTimeout(pg.conn, -1, sql);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError(
        "unifi::load_camera_health query: " + err);
  }
  std::vector<CameraHealth> rows;
  int n = PQntuples(res);
  for (int i = 0; i < n; ++i) {
    CameraHealth h;
    h.id   = PQgetvalue(res, i, 0);
    h.name = PQgetisnull(res, i, 1) ? "" : PQgetvalue(res, i, 1);
    h.host = PQgetisnull(res, i, 2) ? "" : PQgetvalue(res, i, 2);
    h.mac  = PQgetisnull(res, i, 3) ? "" : PQgetvalue(res, i, 3);
    h.is_third_party =
        std::string(PQgetvalue(res, i, 4)) == "t";
    h.last_event_ms = static_cast<uint64_t>(
        std::stoull(PQgetvalue(res, i, 5)));
    h.events_1h = static_cast<uint64_t>(
        std::stoull(PQgetvalue(res, i, 6)));
    rows.push_back(std::move(h));
  }
  PQclear(res);
  return rows;
}

// ---------------------------------------------------------------------------
// load_recent_events (admin UI Recent Events panel)
// ---------------------------------------------------------------------------

absl::StatusOr<std::vector<RecentEvent>>
load_recent_events(int limit, const DbConfig& db) {
  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError("unifi::load_recent_events: " + pg.error());

  // Cap the limit to prevent abuse / runaway queries.
  if (limit <= 0)  limit = 30;
  if (limit > 200) limit = 200;
  const std::string limit_str = std::to_string(limit);

  // smartDetectZone covers the classified events the UI cares about; we
  // also include motion (only first-party that we haven't classified yet)
  // for visibility into the motion_poller backlog.  thumbnailId may be
  // null on stuck-open rows; skip those.
  const char* sql =
    "SELECT e.id, e.type, e.\"cameraId\", "
    "       COALESCE(c.name, ''), "
    "       COALESCE(e.\"thumbnailId\", ''), "
    "       COALESCE(e.\"smartDetectTypes\"::text, '[]'), "
    "       COALESCE(e.start, 0)::bigint, "
    "       COALESCE(e.\"end\", 0)::bigint, "
    "       EXISTS(SELECT 1 FROM thumbnails t "
    "              WHERE t.id = e.\"thumbnailId\") AS in_db "
    "FROM events e "
    "LEFT JOIN cameras c ON c.id = e.\"cameraId\" "
    "WHERE e.type IN "
    "      ('smartDetectZone','smartDetectLine','smartAudioDetect','motion') "
    "  AND e.\"thumbnailId\" IS NOT NULL "
    "ORDER BY e.start DESC "
    "LIMIT $1";
  const char* params[] = { limit_str.c_str() };

  PGresult* res = onvif::pg::ExecParamsWithTimeout(pg.conn, -1, sql, 1, nullptr, params,
                               nullptr, nullptr, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError(
        "unifi::load_recent_events query: " + err);
  }
  std::vector<RecentEvent> rows;
  int n = PQntuples(res);
  for (int i = 0; i < n; ++i) {
    RecentEvent r;
    r.id           = PQgetvalue(res, i, 0);
    r.type         = PQgetvalue(res, i, 1);
    r.camera_id    = PQgetvalue(res, i, 2);
    r.camera_name  = PQgetvalue(res, i, 3);
    r.thumbnail_id = PQgetvalue(res, i, 4);
    r.smart_detect_types_json = PQgetvalue(res, i, 5);
    r.start_ms     = static_cast<uint64_t>(
        std::stoull(PQgetvalue(res, i, 6)));
    r.end_ms       = static_cast<uint64_t>(
        std::stoull(PQgetvalue(res, i, 7)));
    r.thumbnail_in_db = std::string(PQgetvalue(res, i, 8)) == "t";
    rows.push_back(std::move(r));
  }
  PQclear(res);
  return rows;
}

// ---------------------------------------------------------------------------
// load_thumbnail_bytes (admin UI thumbnail proxy)
// ---------------------------------------------------------------------------

absl::StatusOr<std::vector<uint8_t>> load_thumbnail_bytes(
    const std::string& thumbnail_id, const DbConfig& db) {
  if (thumbnail_id.empty())
    return absl::InvalidArgumentError("empty thumbnail_id");

  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError("unifi::load_thumbnail_bytes: " + pg.error());

  // Binary fetch: column 0 returned in binary mode.
  const char* params[] = { thumbnail_id.c_str() };
  const int   formats[] = { 0 };
  (void)formats;
  PGresult* res = onvif::pg::ExecParamsWithTimeout(pg.conn, -1,
      "SELECT content FROM thumbnails WHERE id = $1",
      1, nullptr, params, nullptr, nullptr, /*resultFormat=*/1);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError(
        "unifi::load_thumbnail_bytes query: " + err);
  }
  if (PQntuples(res) == 0 || PQgetisnull(res, 0, 0)) {
    PQclear(res);
    return std::vector<uint8_t>{};  // miss; caller decides what to do
  }
  const char* raw = PQgetvalue(res, 0, 0);
  const int   len = PQgetlength(res, 0, 0);
  std::vector<uint8_t> out(raw, raw + len);
  PQclear(res);
  return out;
}

// ---------------------------------------------------------------------------
// load_all_first_party (admin UI tickbox list)
// ---------------------------------------------------------------------------

absl::StatusOr<std::vector<FirstPartyCamera>>
load_all_first_party(const DbConfig& db) {
  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError(
        "unifi::load_all_first_party: " + pg.error());

  const char* sql =
    "SELECT id, name, mac "
    "FROM cameras "
    "WHERE \"isThirdPartyCamera\" = false "
    "  AND \"isAdopted\" = true "
    "ORDER BY name";

  PGresult* res = onvif::pg::ExecWithTimeout(pg.conn, -1, sql);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError(
        "unifi::load_all_first_party query: " + err);
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

  PGresult* res = onvif::pg::ExecWithTimeout(pg.conn, -1, sql);
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
// load_first_party_cameras_by_model
// ---------------------------------------------------------------------------

absl::StatusOr<std::vector<FirstPartyCamera>>
load_first_party_cameras_by_model(
    const std::vector<std::string>& model_substrings,
    const DbConfig& db) {
  if (model_substrings.empty()) return std::vector<FirstPartyCamera>{};

  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError(
        "unifi::load_first_party_cameras_by_model: " + pg.error());

  // Build a WHERE clause with OR'd ILIKE conditions on the type column.
  // E.g.: (type ILIKE '%G3 Instant%' OR type ILIKE '%G4 Bullet%')
  std::string where = "(";
  for (size_t i = 0; i < model_substrings.size(); ++i) {
    if (i > 0) where += " OR ";
    // Escape single quotes in the substring.
    std::string escaped;
    for (char ch : model_substrings[i]) {
      if (ch == '\'') escaped += "''";
      else
        escaped += ch;
    }
    where += "type ILIKE '%" + escaped + "%'";
  }
  where += ')';

  std::string sql =
    "SELECT id, name, mac, type "
    "FROM cameras "
    "WHERE \"isThirdPartyCamera\" = false "
    "  AND \"isAdopted\" = true "
    "  AND " + where;

  PGresult* res = onvif::pg::ExecWithTimeout(pg.conn, -1, sql.c_str());
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError(
        "unifi::load_first_party_cameras_by_model query: " + err);
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
  static const char kSmartDetectSettings[] =
      "{\"objectTypes\":[\"person\",\"vehicle\",\"animal\",\"package\"],"
      "\"audioTypes\":[],"
      "\"autoTrackingObjectTypes\":[],"
      "\"autoTrackingWithZoom\":true,"
      "\"autoTrackingTimeoutSec\":20,"
      "\"detectionRange\":{\"max\":null,\"min\":null},"
      "\"enableTamperDetection\":false}";
  // When logging, capture old values first.
  if (log) {
    const char* sel_sql =
      "SELECT \"featureFlags\"::jsonb->'smartDetectTypes', "
      "       \"smartDetectSettings\"::text, "
      "       \"featureFlags\"::jsonb->'hasSmartDetect' "
      "FROM cameras WHERE id = $1";
    const char* p[1] = { cam_id.c_str() };
    PGresult* sel = onvif::pg::ExecParamsWithTimeout(conn, -1, sel_sql, 1, nullptr, p,
                                 nullptr, nullptr, 0);
    if (PQresultStatus(sel) == PGRES_TUPLES_OK && PQntuples(sel) > 0) {
      std::string old_sdt = PQgetisnull(sel, 0, 0)
          ? "" : PQgetvalue(sel, 0, 0);
      std::string old_sds = PQgetisnull(sel, 0, 1)
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
        "    \"smartDetectSettings\" = $2::json,"
        "    \"updatedAt\" = NOW() "
        "WHERE id = $1 "
        "  AND ("
        "    (\"featureFlags\"::jsonb -> 'smartDetectTypes') IS NULL"
        "    OR (\"featureFlags\"::jsonb -> 'smartDetectTypes') = '[]'::jsonb"
        "    OR NOT (\"featureFlags\"::jsonb -> 'smartDetectTypes')"
        "           @> '\"licensePlate\"'::jsonb"
        "    OR (\"smartDetectSettings\"::jsonb -> 'objectTypes') IS NULL"
        "    OR (\"smartDetectSettings\"::jsonb -> 'objectTypes') = '[]'::jsonb"
        "    OR (\"smartDetectSettings\"::jsonb -> 'audioTypes') IS NULL"
        "    OR (\"featureFlags\"::jsonb -> 'hasSmartDetect') IS NULL"
        "    OR (\"featureFlags\"::jsonb -> 'hasSmartDetect') <> 'true'::jsonb"
        "  )";
      const char* up[2] = { cam_id.c_str(), kSmartDetectSettings };
      PGresult* upd = onvif::pg::ExecParamsWithTimeout(conn, -1, upd_sql, 2, nullptr, up,
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
        log->record(cam_id, "featureFlags.smartDetectTypes", old_sdt, kNewSdt);
        log->record(cam_id, "smartDetectSettings", old_sds, kSmartDetectSettings);
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
    "    \"smartDetectSettings\" = $2::json,"
    "    \"updatedAt\" = NOW() "
    "WHERE id = $1 "
    "  AND ("
    "    (\"featureFlags\"::jsonb -> 'smartDetectTypes') IS NULL"
    "    OR (\"featureFlags\"::jsonb -> 'smartDetectTypes') = '[]'::jsonb"
    "    OR NOT (\"featureFlags\"::jsonb -> 'smartDetectTypes')"
    "           @> '\"licensePlate\"'::jsonb"
    "    OR (\"smartDetectSettings\"::jsonb -> 'objectTypes') IS NULL"
    "    OR (\"smartDetectSettings\"::jsonb -> 'objectTypes') = '[]'::jsonb"
    "    OR (\"smartDetectSettings\"::jsonb -> 'audioTypes') IS NULL"
    "    OR (\"featureFlags\"::jsonb -> 'hasSmartDetect') IS NULL"
    "    OR (\"featureFlags\"::jsonb -> 'hasSmartDetect') <> 'true'::jsonb"
    "  )";
  const char* params[2] = { cam_id.c_str(), kSmartDetectSettings };
  PGresult* res = onvif::pg::ExecParamsWithTimeout(conn, -1, sql, 2, nullptr, params,
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
// read_smart_detect_flags — Postgres side of the featureFlags drift check
// ---------------------------------------------------------------------------

absl::StatusOr<std::map<std::string, std::string>>
read_smart_detect_flags(const std::vector<std::string>& camera_ids,
                        const DbConfig& db) {
  std::map<std::string, std::string> out;
  if (camera_ids.empty()) return out;

  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError("unifi::read_smart_detect_flags: " + pg.error());

  const std::string ids = internal::pg_array(camera_ids);
  const char* params[] = { ids.c_str() };
  PGresult* res = onvif::pg::ExecParamsWithTimeout(pg.conn, 10'000,
      "SELECT id, COALESCE("
      "         \"featureFlags\"::jsonb->'smartDetectTypes', '[]'::jsonb"
      "       )::text "
      "FROM cameras WHERE id = ANY($1::text[])",
      1, nullptr, params, nullptr, nullptr, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    const std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError("unifi::read_smart_detect_flags query: " + err);
  }
  const int n = PQntuples(res);
  for (int i = 0; i < n; ++i) {
    out[PQgetvalue(res, i, 0)] = PQgetvalue(res, i, 1);
  }
  PQclear(res);
  return out;
}

// ---------------------------------------------------------------------------
// AI Port pairing (issue #67)
// ---------------------------------------------------------------------------

// to_jsonb() over the whole row keeps the query valid on Protect versions
// whose cameras table has no isPairedWithAiPort column.
static absl::StatusOr<std::set<std::string>> query_ai_port_paired_ids(
    PGconn* conn) {
  PGresult* res = onvif::pg::ExecWithTimeout(conn, 10'000,
      "SELECT id FROM cameras c "
      "WHERE (to_jsonb(c)->>'isPairedWithAiPort') = 'true'");
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    const std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError("AI Port pairing query: " + err);
  }
  std::set<std::string> out;
  for (int i = 0; i < PQntuples(res); ++i) out.insert(PQgetvalue(res, i, 0));
  PQclear(res);
  return out;
}

absl::StatusOr<std::set<std::string>> load_ai_port_paired_ids(
    const DbConfig& db) {
  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError("unifi::load_ai_port_paired_ids: " + pg.error());
  return query_ai_port_paired_ids(pg.conn);
}

absl::StatusOr<std::vector<std::string>> clear_license_plate_marker(
    const std::vector<std::string>& camera_ids,
    const DbConfig& db,
    CameraChangeLog* log) {
  std::vector<std::string> cleared;
  if (camera_ids.empty()) return cleared;

  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError(
        "unifi::clear_license_plate_marker: " + pg.error());

  const std::string ids = internal::pg_array(camera_ids);
  const char* params[] = { ids.c_str() };
  PGresult* res = onvif::pg::ExecParamsWithTimeout(pg.conn, 10'000,
      "WITH old AS ("
      "  SELECT id, \"featureFlags\"::jsonb->'smartDetectTypes' AS sdt "
      "  FROM cameras c "
      "  WHERE id = ANY($1::text[]) "
      "    AND \"isThirdPartyCamera\" = true "
      "    AND (to_jsonb(c)->>'isPairedWithAiPort') IS DISTINCT FROM 'true' "
      "    AND jsonb_typeof(\"featureFlags\"::jsonb->'smartDetectTypes')"
      "        = 'array' "
      "    AND (\"featureFlags\"::jsonb->'smartDetectTypes') ? 'licensePlate' "
      "  FOR UPDATE) "
      "UPDATE cameras "
      "SET \"featureFlags\" = jsonb_set(\"featureFlags\"::jsonb, "
      "      '{smartDetectTypes}', old.sdt - 'licensePlate')::json, "
      "    \"updatedAt\" = NOW() "
      "FROM old WHERE cameras.id = old.id "
      "RETURNING cameras.id, old.sdt::text, (old.sdt - 'licensePlate')::text",
      1, nullptr, params, nullptr, nullptr, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    const std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError("unifi::clear_license_plate_marker: " + err);
  }
  for (int i = 0; i < PQntuples(res); ++i) {
    cleared.push_back(PQgetvalue(res, i, 0));
    if (log) {
      log->record(cleared.back(), "featureFlags.smartDetectTypes",
                  PQgetvalue(res, i, 1), PQgetvalue(res, i, 2));
    }
  }
  PQclear(res);
  return cleared;
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
      PGresult* sel = onvif::pg::ExecParamsWithTimeout(conn, -1, sel_sql, 1, nullptr, p,
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
      PGresult* upd = onvif::pg::ExecParamsWithTimeout(conn, -1, upd_sql, 2, nullptr, up,
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
  std::string arr = internal::pg_array(ids);
  const char* sql =
    "UPDATE cameras "
    "SET \"smartDetectZones\" = $1::json, "
    "    \"updatedAt\" = NOW() "
    "WHERE id = ANY($2) "
    "  AND (\"smartDetectZones\" IS NULL "
    "       OR \"smartDetectZones\"::jsonb = '[]'::jsonb)";
  const char* params[2] = { kDefaultZone, arr.c_str() };
  PGresult* res = onvif::pg::ExecParamsWithTimeout(conn, -1, sql, 2, nullptr, params,
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
    PGresult* sel = onvif::pg::ExecWithTimeout(pg.conn, -1, sel_sql);
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
  PGresult* res = onvif::pg::ExecParamsWithTimeout(pg.conn, -1, sql, 1, nullptr, params,
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

  // Protect and the AI Port own the settings of paired cameras.
  auto paired_or = query_ai_port_paired_ids(pg.conn);
  if (!paired_or.ok())
    return absl::InternalError(
        "unifi::rollback_camera_changes: " +
        std::string(paired_or.status().message()));
  const std::set<std::string>& paired = *paired_or;

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
      if (paired.count(cam_id)) {
        LOG(INFO) << "[rollback] skipping " << cam_id << " " << column
                  << " (paired with an AI Port)";
        continue;
      }

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
      } else if (column == "smartDetectSettings.objectTypes" ||
                 column == "smartDetectSettings") {
        sql =
          "UPDATE cameras "
          "SET \"smartDetectSettings\" = $1::json,"
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
      PGresult* res = onvif::pg::ExecParamsWithTimeout(pg.conn, -1, sql.c_str(), 2, nullptr, params,
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
      "    \"smartDetectSettings\" = '{\"objectTypes\":[]}'::json,"
      "    \"smartDetectZones\" = '[]'::json,"
      "    \"updatedAt\" = NOW() "
      "WHERE \"isThirdPartyCamera\" = true "
      "  AND \"isAdopted\" = true "
      "  AND (to_jsonb(cameras)->>'isPairedWithAiPort') IS DISTINCT FROM "
      "      'true'";

    PGresult* res = onvif::pg::ExecWithTimeout(pg.conn, -1, sql);
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

// ---------------------------------------------------------------------------
// detect_native_msr_thumbnail_format
// ---------------------------------------------------------------------------

absl::StatusOr<bool> detect_native_msr_thumbnail_format(const DbConfig& db) {
  PgConn pg(db);
  if (!pg.ok())
    return absl::InternalError(
        "detect_native_msr_thumbnail_format: " + pg.error());

  // Find the most recent thumbnailId from a first-party camera event.
  const char* sql =
    "SELECT e.\"thumbnailId\" "
    "FROM events e "
    "JOIN cameras c ON e.\"cameraId\" = c.id "
    "WHERE c.\"isThirdPartyCamera\" = false "
    "  AND c.\"isAdopted\" = true "
    "  AND e.\"thumbnailId\" IS NOT NULL "
    "ORDER BY e.start DESC "
    "LIMIT 1";

  PGresult* res = onvif::pg::ExecWithTimeout(pg.conn, -1, sql);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    return absl::InternalError(
        "detect_native_msr_thumbnail_format query: " + err);
  }

  bool use_msr = false;
  if (PQntuples(res) > 0 && !PQgetisnull(res, 0, 0)) {
    const char* tid = PQgetvalue(res, 0, 0);
    int len = static_cast<int>(std::strlen(tid));
    use_msr = (len != 24);
    LOG(INFO) << "[startup] native thumbnailId sample: " << tid
              << " (len=" << len << ") => "
              << (use_msr ? "MSR format" : "DB format");
  } else {
    LOG(INFO) << "[startup] no native camera events found; "
              << "defaulting to 24-char DB thumbnail IDs";
  }

  PQclear(res);
  return use_msr;
}

}  // namespace unifi
