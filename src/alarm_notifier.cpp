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

#include "alarm_notifier.hpp"

#include <curl/curl.h>

#include <libpq-fe.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/synchronization/mutex.h"
#include "contention_profiler.hpp"

#include "absl/log/log.h"
#include "pg_util.hpp"
#include "util.hpp"

namespace onvif {

// ============================================================
// JSON parsing helpers (file-local)
// ============================================================
namespace {

// Skip whitespace starting at pos; returns updated pos.
static size_t skip_ws(const std::string& j, size_t pos) {
  while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\n' ||
                            j[pos] == '\r' || j[pos] == '\t'))
    ++pos;
  return pos;
}

// Extract a JSON string value starting at pos (which must point to the opening
// quote).  Returns the unescaped string and advances pos past the closing quote.
// Returns empty string and leaves pos unchanged on failure.
static std::string extract_string(const std::string& j, size_t& pos) {
  if (pos >= j.size() || j[pos] != '"') return {};
  size_t start = pos + 1;
  bool esc = false;
  std::string result;
  for (size_t i = start; i < j.size(); ++i) {
    if (esc) {
      result += j[i];
      esc = false;
      continue;
    }
    if (j[i] == '\\') {
      esc = true;
      continue;
    }
    if (j[i] == '"') {
      pos = i + 1;
      return result;
    }
    result += j[i];
  }
  return {};
}

// Extract an integer value starting at pos.
static uint64_t extract_uint(const std::string& j, size_t pos) {
  uint64_t val = 0;
  while (pos < j.size() && j[pos] >= '0' && j[pos] <= '9') {
    val = val * 10 + static_cast<uint64_t>(j[pos] - '0');
    ++pos;
  }
  return val;
}

// Find the value for a given key in a JSON object.  Scans from pos forward
// looking for "key": and returns the position of the value start.
// Returns std::string::npos if not found before the object closes.
static size_t find_key(const std::string& j,
                       size_t pos,
                       const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  while (pos < j.size()) {
    pos = j.find(needle, pos);
    if (pos == std::string::npos) return pos;
    pos += needle.size();
    pos = skip_ws(j, pos);
    if (pos < j.size() && j[pos] == ':') {
      return skip_ws(j, pos + 1);
    }
  }
  return std::string::npos;
}

// Extract the substring of a JSON array or object starting at pos (which must
// point to '[' or '{').  Returns the matched substring including delimiters.
static std::string extract_balanced(const std::string& j, size_t pos) {
  if (pos >= j.size()) return {};
  char open = j[pos];
  char close = (open == '[') ? ']' : '}';
  int depth = 0;
  bool in_str = false;
  bool esc = false;
  for (size_t i = pos; i < j.size(); ++i) {
    char c = j[i];
    if (esc) {
      esc = false;
      continue;
    }
    if (in_str) {
      if (c == '\\') {
        esc = true;
      } else if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
      continue;
    }
    if (c == open) {
      ++depth;
    } else if (c == close) {
      --depth;
      if (depth == 0) return j.substr(pos, i - pos + 1);
    }
  }
  return {};
}

// Parse "conditions" array: extract each condition.source string.
static std::set<std::string> parse_conditions(const std::string& arr) {
  std::set<std::string> types;
  size_t pos = 0;
  while (pos < arr.size()) {
    size_t p = find_key(arr, pos, "source");
    if (p == std::string::npos) break;
    std::string val = extract_string(arr, p);
    if (!val.empty()) types.insert(val);
    pos = p;
  }
  return types;
}

// Parse "sources" array: extract each device MAC string.
static std::set<std::string> parse_sources(const std::string& arr) {
  std::set<std::string> devs;
  size_t pos = 0;
  while (pos < arr.size()) {
    size_t p = find_key(arr, pos, "device");
    if (p == std::string::npos) break;
    std::string val = extract_string(arr, p);
    if (!val.empty()) devs.insert(val);
    pos = p;
  }
  return devs;
}


}  // namespace

// ============================================================
// AlarmNotifier::parse_automations
// ============================================================

bool AlarmNotifier::should_re_register(long code) {  // NOLINT(runtime/int)
  // Only the UOS "Alarm not found" case is recoverable by re-registering.
  return code == 500;
}

bool AlarmNotifier::uos_unavailable(long code) {  // NOLINT(runtime/int)
  // Only a 404 is evidence the route isn't mounted, which is what Protect
  // does when Global Alarm Manager is off.
  //
  // Deliberately NOT code == 0.  Zero means the request never completed --
  // a connect refusal or timeout -- which is exactly what happens while
  // Protect is restarting, including a restart WedgeHealer itself just
  // triggered.  Latching on that would permanently drop every install to
  // the no-thumbnail path after one transient blip, and tell the user to
  // go change a Protect setting that was never the problem.
  return code == 404;
}

void AlarmNotifier::set_notify_via_uos(bool enabled) {
  absl::MutexLock lk(&mu_);
  notify_via_uos_ = enabled;
}

std::vector<AlarmNotifier::AutomationEntry> AlarmNotifier::parse_automations(
    const std::string& json) {
  std::vector<AutomationEntry> result;

  // Walk through top-level array, extracting each {...} automation object.
  int depth = 0;
  size_t start = std::string::npos;
  bool in_str = false;
  bool escape = false;

  for (size_t i = 0; i < json.size(); ++i) {
    char c = json[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (in_str) {
      if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
      continue;
    }

    if (c == '{') {
      if (depth == 0) start = i;
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0 && start != std::string::npos) {
        const std::string obj = json.substr(start, i - start + 1);
        AutomationEntry entry;

        // Extract "id" string
        size_t p = find_key(obj, 0, "id");
        if (p != std::string::npos)
          entry.id = extract_string(obj, p);

        // Extract "name" string
        p = find_key(obj, 0, "name");
        if (p != std::string::npos)
          entry.name = extract_string(obj, p);

        // Check "enable" boolean
        p = find_key(obj, 0, "enable");
        if (p != std::string::npos && p < obj.size())
          entry.enabled = (obj[p] == 't');

        // Skip deleted automations
        p = find_key(obj, 0, "deleted");
        bool deleted = false;
        if (p != std::string::npos && p < obj.size())
          deleted = (obj[p] == 't');

        // Parse "conditions" array
        p = find_key(obj, 0, "conditions");
        if (p != std::string::npos && p < obj.size() && obj[p] == '[') {
          std::string arr = extract_balanced(obj, p);
          entry.trigger_types = parse_conditions(arr);
        }

        // Parse "sources" array (camera device filters)
        p = find_key(obj, 0, "sources");
        if (p != std::string::npos && p < obj.size() && obj[p] == '[') {
          std::string arr = extract_balanced(obj, p);
          entry.source_devices = parse_sources(arr);
        }

        // Parse "cooldown" object
        p = find_key(obj, 0, "cooldown");
        if (p != std::string::npos && p < obj.size() && obj[p] == '{') {
          std::string cd = extract_balanced(obj, p);
          size_t ep = find_key(cd, 0, "enable");
          if (ep != std::string::npos && ep < cd.size())
            entry.cooldown_enabled = (cd[ep] == 't');
          size_t tp = find_key(cd, 0, "timeout");
          if (tp != std::string::npos)
            entry.cooldown_ms = extract_uint(cd, tp);
        }

        if (!entry.id.empty() && !deleted && !entry.trigger_types.empty()) {
          result.push_back(std::move(entry));
        }
        start = std::string::npos;
      }
    }
  }
  return result;
}

// ============================================================
// HTTP helpers
// ============================================================

size_t AlarmNotifier::write_cb(char* ptr, size_t size, size_t nmemb,
                                void* userdata) {
  auto* buf = static_cast<std::string*>(userdata);
  buf->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string AlarmNotifier::protect_api_get(const std::string& url,
                                            const std::string& user_id) {
  std::string body;
  AlarmNotifier::perform_get(url, user_id, &body);
  return body;
}

long AlarmNotifier::perform_get(const std::string& url,  // NOLINT(runtime/int)
                                 const std::string& user_id,
                                 std::string* body) {
  body->clear();
  CURL* curl = curl_easy_init();
  if (!curl) return 0;

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = curl_slist_append(headers, "X-Source: unifi-os");
  std::string user_hdr = "X-UserId: " + user_id;
  headers = curl_slist_append(headers, user_hdr.c_str());

  curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,        5L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,      body);

  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;  // NOLINT(runtime/int)
  if (rc != CURLE_OK) {
    LOG(ERROR) << "[alarm] GET " << url << " failed: " << curl_easy_strerror(rc);
    body->clear();
  } else {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return http_code;
}

long AlarmNotifier::perform_post(const std::string& url,  // NOLINT(runtime/int)
                                  const std::string& user_id,
                                  const std::string& body,
                                  std::string* response_body) {
  CURL* curl = curl_easy_init();
  if (!curl) return 0;

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = curl_slist_append(headers, "X-Source: unifi-os");
  std::string user_hdr = "X-UserId: " + user_id;
  if (!user_id.empty()) headers = curl_slist_append(headers, user_hdr.c_str());

  auto collect = +[](char* d, size_t s, size_t n, void* out) -> size_t {
    if (out != nullptr) {
      auto* str = static_cast<std::string*>(out);
      if (str->size() < 4096) str->append(d, std::min(s * n, 4096 - str->size()));
    }
    return s * n;
  };

  curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,        5L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(body.size()));  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  collect);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,      response_body);

  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;  // NOLINT(runtime/int)
  if (rc != CURLE_OK) {
    LOG(ERROR) << "[alarm] POST " << url << " failed: "
               << curl_easy_strerror(rc);
  } else {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return http_code;
}

// Categorise a non-2xx Protect API response so the log line names the
// failure class rather than just the numeric code.  Each class points
// at what the operator would actually do about it.  Used by both
// http_get and http_post below (issue #41 follow-on).
static const char* categorise_failure(long code) {  // NOLINT(runtime/int)
  if (code == 401)
    return "authentication failed (user_id token rejected -- pass "
           "--protect_user_id=<UUID> to override, see issue #41)";
  if (code == 403)
    return "authorisation failed (this user_id lacks permission)";
  if (code == 404)
    return "not found (automation may have been deleted -- next refresh "
           "will drop it)";
  if (code == 429)
    return "rate-limited by Protect API";
  if (code >= 500)
    return "Protect API server error (typically transient)";
  return "unexpected status";
}

std::string AlarmNotifier::http_get(const std::string& url) {
  std::string body;
  long code = perform_get(url, user_id_provider_->current(), &body);  // NOLINT(runtime/int)
  if (code == 401 && user_id_provider_->try_refresh()) {
    LOG(INFO) << "[alarm] retrying GET " << url << " after user_id refresh";
    code = perform_get(url, user_id_provider_->current(), &body);  // NOLINT(runtime/int)
  }
  if (code != 200) {
    if (code != 0) {
      LOG(ERROR) << "[alarm] GET " << url << " HTTP " << code
                 << " -- " << categorise_failure(code);
    }
    body.clear();
  }
  return body;
}

bool AlarmNotifier::is_internal_automation_url(const std::string& url) {
  return url.find("/internal/automationManager/") != std::string::npos;
}

long AlarmNotifier::http_post(const std::string& url,  // NOLINT(runtime/int)
                               const std::string& body,
                               std::string* response_body) {
  const bool internal = is_internal_automation_url(url);
  const std::string user_id =
      internal ? std::string() : user_id_provider_->current();
  long code = perform_post(url, user_id, body, response_body);  // NOLINT(runtime/int)
  if (!internal && code == 401 && user_id_provider_->try_refresh()) {
    LOG(INFO) << "[alarm] retrying POST " << url << " after user_id refresh";
    if (response_body) response_body->clear();
    code = perform_post(url, user_id_provider_->current(), body,  // NOLINT(runtime/int)
                        response_body);
  }
  if (code == 0) return code;  // network error already logged
  if (code < 200 || code >= 300) {
    LOG(ERROR) << "[alarm] POST " << url << " HTTP " << code
               << " -- " << categorise_failure(code);
  } else {
    LOG(INFO) << "[alarm] POST " << url << " → " << code;
  }
  return code;
}

// ============================================================
// Automation registration
// ============================================================

long AlarmNotifier::register_automation(  // NOLINT(runtime/int)
    const AutomationEntry& entry) {
  // Build the /change payload understood by the UOS automation manager:
  //
  // {"type":"created","data":[{
  //   "id":           "<automation-uuid>",
  //   "title":        "<name>",
  //   "is_paused":    false,
  //   "triggers_data":[[{"id":"person","is_matched_externally":true}], ...],
  //   "actions_data": [[{"id":"notify"}]]
  // }]}
  //
  // triggers_data is a 2-D array where the outer dimension is OR and the inner
  // is AND.  We emit one inner singleton per trigger type (OR semantics).
  using onvif::util::json_str;

  std::string triggers;
  for (const auto& t : entry.trigger_types) {
    if (!triggers.empty()) triggers += ',';
    triggers += "[{\"id\":";
    triggers += json_str(t);
    triggers += ",\"is_matched_externally\":true}]";
  }

  std::string payload;
  payload.reserve(256);
  payload += "{\"type\":\"created\",\"data\":[{";
  payload += "\"id\":";           payload += json_str(entry.id);
  payload += ",\"title\":";       payload += json_str(entry.name);
  payload += ",\"is_paused\":false";
  payload += ",\"triggers_data\":["; payload += triggers; payload += "]";
  payload += ",\"actions_data\":[[{\"id\":\"notify\"}]]";
  payload += "}]}";

  std::string url;
  {
    absl::MutexLock lk(&mu_);
    url = protect_url_;
  }
  long code = http_post(  // NOLINT(runtime/int)
      url + "/internal/automationManager/external/change", payload);
  if (code >= 200 && code < 300) {
    LOG(INFO) << "[alarm] registered automation " << entry.id
              << " (\"" << entry.name << "\") with UOS automation manager";
  }
  return code;
}

// ============================================================
// Internal notify payload builder
// ============================================================

// static
std::string AlarmNotifier::build_notify_payload(
    const std::string& alarm_id,
    const std::string& event_key,
    const std::string& camera_db_id,
    const std::string& event_id,
    uint64_t           ts_ms,
    const std::string& user_id) {
  using onvif::util::json_str;
  char ts_buf[24];
  std::snprintf(ts_buf, sizeof(ts_buf), "%" PRIu64, ts_ms);

  // Target schema (derived from Protect service.js / defaultUosActionDataSchema):
  //
  // {
  //   "alarm_id": "<automation-uuid>",
  //   "events": [{
  //     "id":    "<event_key>",           // EventKeys enum value e.g. "person"
  //     "scope": { "site_id": "protect" },
  //     "metadata": {
  //       "event": {
  //         "key":       "<event_key>",
  //         "device":    "<camera_db_id>", // camera DB UUID (not MAC)
  //         "eventId":   "<uuid>",         // real events.id → thumbnail anchor
  //         "timestamp": <ms>,
  //         "sourceEvent": {               // ← causes buildEventFromSource to
  //           "cameraId": "<camera_db_id>" //   find live event + thumbnail
  //         }
  //       },
  //       "allEvents": [{ "key":..., "device":..., "eventId":...,
  //                       "timestamp":... }]
  //     }
  //   }],
  //   "data":      { "is_critical": false },
  //   "receivers": [{ "user_id": "<ucore-uuid>", "push": true,
  //                   "email": false, "voip": false }]
  // }
  //
  // The sourceEvent.cameraId field is the key difference from the legacy
  // /api/automations/<id>/run path (which hardcodes "expectedNoEventId").
  // Protect's buildEventFromSource uses cameraId to look up the CameraDecalModel
  // by PK, then findEvent to fetch the event row (with DB fallback), and finally
  // getThumbnail to resolve the snapshot URL attached to the push payload.

  std::string p;
  p.reserve(720);
  p += "{\"alarm_id\":";           p += json_str(alarm_id);
  p += ",\"events\":[{\"id\":";    p += json_str(event_key);
  p += ",\"scope\":{\"site_id\":\"protect\"}";
  p += ",\"metadata\":{\"event\":{";
  p +=   "\"key\":";               p += json_str(event_key);
  p +=   ",\"device\":";           p += json_str(camera_db_id);
  p +=   ",\"eventId\":";          p += json_str(event_id);
  p +=   ",\"timestamp\":";        p += ts_buf;
  // sourceEvent lets Protect build the notification (and thumbnail) without
  // looking the event up.  7.1/7.2 read `camera ?? cameraId`; 7.3 validates
  // it against automationSourceEventSchema, which requires `type`.
  p +=   ",\"sourceEvent\":{\"type\":\"smartDetectZone\"";
  p +=     ",\"camera\":";        p += json_str(camera_db_id);
  p +=     ",\"cameraId\":";      p += json_str(camera_db_id);
  p +=     ",\"start\":";         p += ts_buf;
  p +=     ",\"smartDetectTypes\":["; p += json_str(event_key); p += "]}";
  p += "},\"allEvents\":[{";
  p +=   "\"key\":";               p += json_str(event_key);
  p +=   ",\"device\":";           p += json_str(camera_db_id);
  p +=   ",\"eventId\":";          p += json_str(event_id);
  p +=   ",\"timestamp\":";        p += ts_buf;
  p += "}]}}]";
  p += ",\"data\":{\"is_critical\":false}";
  p += ",\"receivers\":[{\"user_id\":"; p += json_str(user_id);
  p += ",\"push\":true,\"email\":false,\"voip\":false}]}";
  return p;
}

// ============================================================
// History recording via direct DB INSERT
// ============================================================

void AlarmNotifier::record_history(const AutomationEntry& automation,
                                   const std::string& obj_type,
                                   const std::string& camera_mac,
                                   const std::string& event_id,
                                   uint64_t ts_ms) {
  if (db_connstr_.empty()) return;

  PGconn* conn = PQconnectdb(db_connstr_.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    LOG(ERROR) << "[alarm] history DB connect failed: "
               << PQerrorMessage(conn);
    PQfinish(conn);
    return;
  }

  const std::string id = onvif::util::generate_24hex_id();
  char ts_buf[24];
  std::snprintf(ts_buf, sizeof(ts_buf), "%" PRIu64, ts_ms);

  // Build the data JSON matching native Protect format:
  // {"name":"...","status":"ok","trigger":{"key":"person",
  //   "zones":{"line":[],"zone":[1],"loiter":[]},
  //   "device":"MAC","eventId":"uuid","timestamp":ms},
  //  "detectionEventId":"uuid"}
  using onvif::util::json_str;
  std::string data;
  data.reserve(384);
  data += "{\"name\":";
  data += json_str(automation.name);
  data += ",\"status\":\"ok\",\"trigger\":{\"key\":";
  data += json_str(obj_type);
  data += ",\"zones\":{\"line\":[],\"zone\":[1],\"loiter\":[]},\"device\":";
  data += json_str(camera_mac);
  data += ",\"eventId\":";
  data += json_str(event_id);
  data += ",\"timestamp\":";
  data += ts_buf;
  data += "},\"detectionEventId\":";
  data += json_str(event_id);
  data += "}";

  const char* params[4] = {id.c_str(), automation.id.c_str(),
                           ts_buf, data.c_str()};
  PGresult* res = onvif::pg::ExecParamsWithTimeout(
      conn, -1,
      "INSERT INTO \"automationsHistory\" "
      "(id, \"automationId\", timestamp, data, "
      " \"createdAt\", \"updatedAt\", \"usedInBreachDetection\") "
      "VALUES ($1, $2, $3::bigint, $4::jsonb, "
      " now(), now(), false)",
      4, nullptr, params, nullptr, nullptr, 0);

  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    LOG(ERROR) << "[alarm] history INSERT failed: "
               << PQresultErrorMessage(res);
  } else {
    LOG(INFO) << "[alarm] recorded history " << id
              << " for automation " << automation.id;
  }

  PQclear(res);
  PQfinish(conn);
}

// ============================================================
// AlarmNotifier public API
// ============================================================

AlarmNotifier::AlarmNotifier(std::string protect_url,
                             ProtectUserIdProvider* user_id_provider,
                             std::string db_connstr)
    : protect_url_(std::move(protect_url)),
      user_id_provider_(user_id_provider),
      db_connstr_(std::move(db_connstr)) {
  ContentionProfiler::instance().register_mutex(&mu_, "alarm_notifier");
}

void AlarmNotifier::refresh_alarms() {
  // Re-arm the UOS path on every refresh (every ~5 min).  If Global Alarm
  // Manager really is off we will get another 404 on the next notify and
  // latch again, at the cost of one failed request per refresh; but if the
  // 404 was situational -- Protect mid-restart, a partially-initialised
  // automation manager -- thumbnails come back on their own instead of
  // staying off until someone restarts the recorder.
  {
    absl::MutexLock lk(&mu_);
    uos_unavailable_ = false;
  }
  std::string url;
  {
    absl::MutexLock lk(&mu_);
    url = protect_url_;
  }

  // ---- Step 1: build MAC → camera DB UUID map --------------------------------
  // Protect's buildEventFromSource looks up cameras by DB UUID (PK), not MAC.
  // We query the DB directly rather than parsing the /api/cameras JSON response
  // because Protect wraps the camera array in an envelope object whose exact
  // schema varies by version, making JSON parsing fragile.  The DB is the
  // single source of truth that Protect itself reads from.
  if (!db_connstr_.empty()) {
    PGconn* cam_conn = PQconnectdb(db_connstr_.c_str());
    if (PQstatus(cam_conn) == CONNECTION_OK) {
      PGresult* cam_res = onvif::pg::ExecParamsWithTimeout(
          cam_conn, /*timeout_ms=*/-1,
          "SELECT id, mac FROM cameras",
          0, nullptr, nullptr, nullptr, nullptr, 0);
      if (PQresultStatus(cam_res) == PGRES_TUPLES_OK) {
        std::map<std::string, std::string> cam_map;
        const int nrows = PQntuples(cam_res);
        for (int r = 0; r < nrows; ++r) {
          std::string id  = PQgetvalue(cam_res, r, 0);
          std::string mac = PQgetvalue(cam_res, r, 1);
          if (!id.empty() && !mac.empty()) cam_map[mac] = id;
        }
        LOG(INFO) << "[alarm] loaded " << cam_map.size()
                  << " camera(s) from DB";
        absl::MutexLock lk(&mu_);
        mac_to_camera_id_ = std::move(cam_map);
      } else {
        LOG(ERROR) << "[alarm] camera DB query failed: "
                   << PQresultErrorMessage(cam_res);
      }
      PQclear(cam_res);
      PQfinish(cam_conn);
    } else {
      LOG(ERROR) << "[alarm] camera DB connect failed: "
                 << PQerrorMessage(cam_conn);
      PQfinish(cam_conn);
    }
  } else {
    LOG(WARNING) << "[alarm] no db_connstr configured -- camera UUID map "
                 << "unavailable; push notifications will fire without thumbnails";
  }

  // ---- Step 2: load automations ----------------------------------------------
  const std::string response = http_get(url + "/api/automations");
  if (response.empty()) return;

  auto parsed = parse_automations(response);
  if (parsed.empty()) {
    LOG(WARNING) << "[alarm] loaded 0 automations from Protect API -- "
                 << "no automations will fire on detections.  Open Protect "
                 << "-> Alarms and create at least one enabled automation "
                 << "with onvif-recorder cameras in scope.";
  } else {
    LOG(INFO) << "[alarm] loaded " << parsed.size()
              << " automation(s) from Protect API";
  }
  for (const auto& a : parsed) {
    LOG(INFO) << "[alarm]   " << a.id << " enabled=" << a.enabled
              << " types=" << a.trigger_types.size()
              << " sources=" << a.source_devices.size()
              << " cooldown=" << a.cooldown_enabled
              << "/" << a.cooldown_ms << "ms";
  }

  // ---- Step 3: register each enabled automation with the UOS store ----------
  // The in-memory UOS automation manager (automationManager) requires a
  // /change "created" call before /actions/notify will succeed.  This
  // registration is lost on every Protect restart; we redo it here.
  // notify() will also re-register on-demand when it receives HTTP 500.
  // One summary line rather than one error per automation: a rejected
  // registration usually affects all of them for the same reason.
  int failed = 0;
  long last_code = 0;  // NOLINT(runtime/int)
  for (const auto& entry : parsed) {
    if (!entry.enabled) continue;
    const long code = register_automation(entry);  // NOLINT(runtime/int)
    if (code < 200 || code >= 300) {
      ++failed;
      last_code = code;
    }
  }
  if (failed > 0) {
    LOG(ERROR) << "[alarm] failed to register " << failed
               << " automation(s) with the UOS automation manager (HTTP "
               << last_code << "); notifications will use the legacy path";
  }

  absl::MutexLock lk(&mu_);
  automations_  = std::move(parsed);
  last_refresh_ = std::chrono::steady_clock::now();
}

void AlarmNotifier::notify(const std::string& obj_type,
                           const std::string& camera_mac,
                           const std::string& event_id,
                           uint64_t ts_ms) {
  // Refresh automation list if empty or older than 5 minutes.
  bool need_refresh;
  std::string url;
  {
    absl::MutexLock lk(&mu_);
    auto now = std::chrono::steady_clock::now();
    need_refresh = automations_.empty() ||
        (now - last_refresh_) > std::chrono::minutes(5);
    url = protect_url_;
  }
  if (need_refresh) refresh_alarms();

  std::vector<AutomationEntry> automations_copy;
  std::string camera_db_id;
  {
    absl::MutexLock lk(&mu_);
    automations_copy = automations_;
    url = protect_url_;
    // Resolve camera MAC → DB UUID for the thumbnail lookup.
    auto cam_it = mac_to_camera_id_.find(camera_mac);
    if (cam_it != mac_to_camera_id_.end()) {
      camera_db_id = cam_it->second;
    }
  }

  if (camera_db_id.empty()) {
    LOG(WARNING) << "[alarm] no DB UUID found for camera MAC " << camera_mac
                 << " -- thumbnails will be missing; will retry on next "
                 << "refresh_alarms() (every 5 min) or Protect restart";
  }

  for (const auto& automation : automations_copy) {
    if (!automation.enabled) continue;

    // Check if this detection type matches an automation condition.
    if (!automation.trigger_types.count(obj_type)) continue;

    // Check if this camera is in the automation's source list.
    // Empty sources list means all cameras.
    if (!automation.source_devices.empty() &&
        !automation.source_devices.count(camera_mac)) {
      continue;
    }

    // Enforce cooldown: skip if last fire was within the cooldown window.
    if (automation.cooldown_enabled && automation.cooldown_ms > 0) {
      absl::MutexLock lk(&mu_);
      auto it = last_fired_.find(automation.id);
      if (it != last_fired_.end() &&
          (ts_ms - it->second) < automation.cooldown_ms) {
        LOG(INFO) << "[alarm] skipping " << automation.id
                  << " (cooldown " << automation.cooldown_ms << "ms)";
        continue;
      }
    }

    LOG(INFO) << "[alarm] triggering automation " << automation.id
              << " for " << obj_type << " on " << camera_mac
              << " event_id=" << event_id;
    {
      // Use the camera's DB UUID as the device identifier so that
      // buildEventFromSource resolves the live event + thumbnail.
      // Falls back to the MAC if the UUID is not yet known (no thumbnail,
      // but the notification still fires).
      const std::string device_id =
          camera_db_id.empty() ? camera_mac : camera_db_id;

      const std::string notify_url =
          url + "/internal/automationManager/external/actions/notify";
      const std::string payload = build_notify_payload(
          automation.id, obj_type, device_id, event_id, ts_ms,
          user_id_provider_->current());

      bool use_uos;
      {
        absl::MutexLock lk(&mu_);
        use_uos = notify_via_uos_ && !uos_unavailable_;
      }

      long code = 0;  // NOLINT(runtime/int)
      if (use_uos) {
        code = http_post(notify_url, payload);
        if (uos_unavailable(code)) {
          // Protect's Global Alarm Manager is off, so this endpoint does
          // not exist.  Say so once -- with the fix -- then use the legacy
          // path from here on.  Without this the notification would simply
          // vanish on every upgrade where the setting was never enabled.
          LOG(ERROR) << "[alarm] UOS automation manager unavailable (HTTP "
                     << code << "); falling back to the legacy "
                     << "/api/automations/<id>/run path for all "
                     << "notifications. Thumbnails cannot be attached on "
                     << "that path. If Protect Settings -> Alarm Manager "
                     << "is Local, switching it to Global enables them.";
          {
            absl::MutexLock lk(&mu_);
            uos_unavailable_ = true;
          }
          use_uos = false;
        }
      }

      if (!use_uos) {
        // Legacy path: fires the automation but cannot carry a thumbnail,
        // because Protect hardcodes eventId="expectedNoEventId" here.
        std::string resp;
        const long run_code = http_post(  // NOLINT(runtime/int)
            url + "/api/automations/" + automation.id + "/run", "{}", &resp);
        if (run_code == 400 &&
            resp.find("global alarm manager") != std::string::npos) {
          LOG(ERROR) << "[alarm] automation " << automation.id
                     << " not triggered: Protect refuses the legacy run "
                        "path while Global Alarm Manager is on, and the "
                        "UOS automation manager did not accept our "
                        "request either";
        }
      }

      // HTTP 500 ("Alarm not found") means the in-memory UOS registration
      // was evicted, typically because Protect restarted.  Re-register and
      // retry once; the next refresh_alarms() will also re-register.
      if (should_re_register(code)) {
        LOG(WARNING) << "[alarm] automation " << automation.id
                     << " not found in UOS store (HTTP 500); "
                     << "re-registering and retrying";
        register_automation(automation);
        http_post(notify_url, payload);
      }
    }

    // Record in automationsHistory so Protect UI shows hits and history.
    record_history(automation, obj_type, camera_mac, event_id, ts_ms);

    // Update local cooldown tracker.
    {
      absl::MutexLock lk(&mu_);
      last_fired_[automation.id] = ts_ms;
    }
  }
}

}  // namespace onvif
