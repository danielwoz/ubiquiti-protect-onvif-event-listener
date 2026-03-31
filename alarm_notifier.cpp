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

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"

namespace onvif {

// ============================================================
// JSON parsing helpers (file-local)
// ============================================================
namespace {

// Extract the first "id":"<value>" string value from a JSON fragment where
// the value is UUID-shaped (36 chars: 8-4-4-4-12 with hyphens).  Returns
// empty string if no such value is found.
static std::string extract_uuid_id(const std::string& json) {
  const std::string needle = "\"id\":\"";
  size_t pos = 0;
  while (true) {
    pos = json.find(needle, pos);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return {};
    const std::string val = json.substr(pos, end - pos);
    if (val.size() == 36) return val;  // UUID has 8-4-4-4-12 = 36 chars
    pos = end + 1;
  }
}

}  // namespace

// ============================================================
// AlarmNotifier::parse_alarms
// ============================================================

// Parse a UOS alarm list JSON array.  Tracks brace depth to extract each
// top-level alarm object as a substring, then pulls out the alarm UUID and
// determines whether the alarm has person/vehicle smart-detect triggers.
std::vector<AlarmNotifier::AlarmEntry> AlarmNotifier::parse_alarms(
    const std::string& json) {
  std::vector<AlarmEntry> result;
  int    depth  = 0;
  size_t start  = std::string::npos;
  bool   in_str = false;
  bool   escape = false;

  for (size_t i = 0; i < json.size(); ++i) {
    const char c = json[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (in_str) {
      if (c == '\\') escape = true;
      else if (c == '"') in_str = false;
      continue;
    }
    if (c == '"') {
      in_str = true;
      continue;
    }

    if (c == '{') {
      if (depth == 0) start = i;  // entering a top-level alarm object
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0 && start != std::string::npos) {
        const std::string alarm = json.substr(start, i - start + 1);
        AlarmEntry entry;
        entry.id = extract_uuid_id(alarm);
        if (entry.id.size() == 36) {
          for (const char* t : {"person", "vehicle", "animal", "package"}) {
            if (alarm.find(std::string("smartDetectType:") + t) != std::string::npos)
              entry.trigger_types.insert(t);
          }
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

  // Discard response body.
  auto discard = +[](char*, size_t s, size_t n, void*) -> size_t { return s * n; };

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
    }
  } else {
    LOG(ERROR) << "[alarm] POST " << url << " failed: " << curl_easy_strerror(rc);
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
}

// ============================================================
// AlarmNotifier public API
// ============================================================

AlarmNotifier::AlarmNotifier(std::string uos_base_url)
    : uos_base_url_(std::move(uos_base_url)) {}

void AlarmNotifier::set_base_url(const std::string& url) {
  std::lock_guard<std::mutex> lk(mu_);
  if (uos_base_url_ == url) return;
  uos_base_url_ = url;
  alarms_.clear();
  last_refresh_ = {};
}

void AlarmNotifier::refresh_alarms() {
  std::string url;
  {
    std::lock_guard<std::mutex> lk(mu_);
    url = uos_base_url_;
  }
  const std::string response = http_get(url + "/api/v1/alarms");
  if (response.empty()) return;

  auto parsed = parse_alarms(response);
  LOG(INFO) << "[alarm] loaded " << parsed.size() << " alarm(s) from UOS";

  std::lock_guard<std::mutex> lk(mu_);
  alarms_       = std::move(parsed);
  last_refresh_ = std::chrono::steady_clock::now();
}

void AlarmNotifier::notify(const std::string& obj_type,
                           const std::string& camera_mac,
                           const std::string& event_id,
                           uint64_t ts_ms) {
  // Refresh alarm list if empty or older than 5 minutes.
  bool need_refresh;
  std::string url;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto now = std::chrono::steady_clock::now();
    need_refresh = alarms_.empty() ||
        (now - last_refresh_) > std::chrono::minutes(5);
    url = uos_base_url_;
  }
  if (need_refresh) refresh_alarms();

  const std::string event_key = "smartDetectType:" + obj_type;

  std::vector<AlarmEntry> alarms_copy;
  {
    std::lock_guard<std::mutex> lk(mu_);
    alarms_copy = alarms_;
    url = uos_base_url_;  // re-snapshot after potential refresh
  }

  for (const auto& alarm : alarms_copy) {
    if (!alarm.trigger_types.count(obj_type)) continue;

    char ts_buf[24];
    std::snprintf(ts_buf, sizeof(ts_buf), "%" PRIu64, ts_ms);

    // Build POST body: array with one trigger entry per alarm.
    //   [{
    //     "id":       "<event_key>",
    //     "alarm_id": "<alarm_id>",
    //     "scope":    { scope_name: camera_mac, ..., "site_id": "protect" },
    //     "metadata": { "event": { key, device, id, start }, "allEvents": [] }
    //   }]
    std::string body;
    body.reserve(512);
    body += "[{\"id\":\"";
    body += event_key;
    body += "\",\"alarm_id\":\"";
    body += alarm.id;
    body += "\",\"scope\":{";
    body += "\"scope_all_smart_cameras_with_zones\":\"";
    body += camera_mac;
    body += "\",\"scope_all_smart_cameras\":\"";
    body += camera_mac;
    body += "\",\"scope_all_cameras\":\"";
    body += camera_mac;
    body += "\",\"site_id\":\"protect\"";
    body += "},\"metadata\":{\"event\":{";
    body += "\"key\":\"";
    body += event_key;
    body += "\",\"device\":\"";
    body += camera_mac;
    body += "\",\"id\":\"";
    body += event_id;
    body += "\",\"start\":";
    body += ts_buf;
    body += "},\"allEvents\":[]}}]";

    http_post(url + "/api/v1/alarms/events", body);
  }
}

}  // namespace onvif
