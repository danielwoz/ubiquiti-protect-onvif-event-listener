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

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"

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

// Generate a 24-char lowercase hex ID (12 random bytes).
static std::string generate_24hex_id() {
  static std::random_device rd;
  thread_local std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;
  uint64_t a = dis(gen);
  uint64_t b = dis(gen);
  char buf[25];
  std::snprintf(buf, sizeof(buf), "%016" PRIx64 "%08" PRIx64,
                static_cast<uint64_t>(a),
                static_cast<uint64_t>(b & 0xFFFFFFFFull));
  return std::string(buf, 24);
}

// Escape a string for use inside a JSON string value.
static std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

// ============================================================
// AlarmNotifier::parse_automations
// ============================================================

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

std::string AlarmNotifier::http_get(const std::string& url) {
  std::string buf;
  CURL* curl = curl_easy_init();
  if (!curl) return buf;

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = curl_slist_append(headers, "X-Source: unifi-os");
  std::string user_hdr = "X-UserId: " + user_id_;
  headers = curl_slist_append(headers, user_hdr.c_str());

  curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,        5L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);

  const CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    LOG(ERROR) << "[alarm] GET " << url << " failed: " << curl_easy_strerror(rc);
    buf.clear();
  } else {
    long http_code = 0;  // NOLINT(runtime/int)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
      LOG(ERROR) << "[alarm] GET " << url << " HTTP " << http_code;
      buf.clear();
    }
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return buf;
}

void AlarmNotifier::http_post(const std::string& url, const std::string& body) {
  CURL* curl = curl_easy_init();
  if (!curl) return;

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = curl_slist_append(headers, "X-Source: unifi-os");
  std::string user_hdr = "X-UserId: " + user_id_;
  headers = curl_slist_append(headers, user_hdr.c_str());

  // Discard response body.
  auto discard = +[](char*, size_t s, size_t n, void*) -> size_t {
    return s * n;
  };

  curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,        5L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(body.size()));  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  discard);

  const CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK) {
    long http_code = 0;  // NOLINT(runtime/int)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code < 200 || http_code >= 300) {
      LOG(ERROR) << "[alarm] POST " << url << " HTTP " << http_code;
    } else {
      LOG(INFO) << "[alarm] POST " << url << " → " << http_code;
    }
  } else {
    LOG(ERROR) << "[alarm] POST " << url << " failed: "
               << curl_easy_strerror(rc);
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
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

  const std::string id = generate_24hex_id();
  char ts_buf[24];
  std::snprintf(ts_buf, sizeof(ts_buf), "%" PRIu64, ts_ms);

  // Build the data JSON matching native Protect format:
  // {"name":"...","status":"ok","trigger":{"key":"person",
  //   "zones":{"line":[],"zone":[1],"loiter":[]},
  //   "device":"MAC","eventId":"uuid","timestamp":ms},
  //  "detectionEventId":"uuid"}
  std::string data;
  data.reserve(384);
  data += "{\"name\":\"";
  data += json_escape(automation.name);
  data += "\",\"status\":\"ok\",\"trigger\":{\"key\":\"";
  data += json_escape(obj_type);
  data += "\",\"zones\":{\"line\":[],\"zone\":[1],\"loiter\":[]},\"device\":\"";
  data += json_escape(camera_mac);
  data += "\",\"eventId\":\"";
  data += json_escape(event_id);
  data += "\",\"timestamp\":";
  data += ts_buf;
  data += "},\"detectionEventId\":\"";
  data += json_escape(event_id);
  data += "\"}";

  const char* params[4] = {id.c_str(), automation.id.c_str(),
                           ts_buf, data.c_str()};
  PGresult* res = PQexecParams(
      conn,
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
                             std::string user_id,
                             std::string db_connstr)
    : protect_url_(std::move(protect_url)),
      user_id_(std::move(user_id)),
      db_connstr_(std::move(db_connstr)) {}  // NOLINT(whitespace/indent_namespace)

void AlarmNotifier::refresh_alarms() {
  std::string url;
  {
    std::lock_guard<std::mutex> lk(mu_);
    url = protect_url_;
  }
  const std::string response = http_get(url + "/api/automations");
  if (response.empty()) return;

  auto parsed = parse_automations(response);
  LOG(INFO) << "[alarm] loaded " << parsed.size()
            << " automation(s) from Protect API";
  for (const auto& a : parsed) {
    LOG(INFO) << "[alarm]   " << a.id << " enabled=" << a.enabled
              << " types=" << a.trigger_types.size()
              << " sources=" << a.source_devices.size()
              << " cooldown=" << a.cooldown_enabled
              << "/" << a.cooldown_ms << "ms";
  }

  std::lock_guard<std::mutex> lk(mu_);
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
    std::lock_guard<std::mutex> lk(mu_);
    auto now = std::chrono::steady_clock::now();
    need_refresh = automations_.empty() ||
        (now - last_refresh_) > std::chrono::minutes(5);
    url = protect_url_;
  }
  if (need_refresh) refresh_alarms();

  std::vector<AutomationEntry> automations_copy;
  {
    std::lock_guard<std::mutex> lk(mu_);
    automations_copy = automations_;
    url = protect_url_;
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
      std::lock_guard<std::mutex> lk(mu_);
      auto it = last_fired_.find(automation.id);
      if (it != last_fired_.end() &&
          (ts_ms - it->second) < automation.cooldown_ms) {
        LOG(INFO) << "[alarm] skipping " << automation.id
                  << " (cooldown " << automation.cooldown_ms << "ms)";
        continue;
      }
    }

    LOG(INFO) << "[alarm] triggering automation " << automation.id
              << " for " << obj_type << " on " << camera_mac;
    http_post(url + "/api/automations/" + automation.id + "/run", "{}");

    // Record in automationsHistory so Protect UI shows hits and history.
    record_history(automation, obj_type, camera_mac, event_id, ts_ms);

    // Update local cooldown tracker.
    {
      std::lock_guard<std::mutex> lk(mu_);
      last_fired_[automation.id] = ts_ms;
    }
  }
}

}  // namespace onvif
