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
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
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

// Position just past the JSON array or object starting at pos (which must
// point to '[' or '{'); npos if it is not closed.
static size_t balanced_end(const std::string& j, size_t pos) {
  if (pos >= j.size()) return std::string::npos;
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
      if (depth == 0) return i + 1;
    }
  }
  return std::string::npos;
}

// Extract the substring of a JSON array or object starting at pos (which must
// point to '[' or '{').  Returns the matched substring including delimiters.
static std::string extract_balanced(const std::string& j, size_t pos) {
  const size_t end = balanced_end(j, pos);
  if (end == std::string::npos) return {};
  return j.substr(pos, end - pos);
}

// Position just past the JSON value starting at pos; npos if malformed.
static size_t skip_value(const std::string& j, size_t pos) {
  if (pos >= j.size()) return std::string::npos;
  if (j[pos] == '{' || j[pos] == '[') return balanced_end(j, pos);
  if (j[pos] == '"') {
    for (size_t i = pos + 1; i < j.size(); ++i) {
      if (j[i] == '\\') {
        ++i;
      } else if (j[i] == '"') {
        return i + 1;
      }
    }
    return std::string::npos;
  }
  size_t i = pos;  // number, true, false or null
  while (i < j.size() && j[i] != ',' && j[i] != '}' && j[i] != ']' &&
         j[i] != ' ' && j[i] != '\n' && j[i] != '\r' && j[i] != '\t')
    ++i;
  return i == pos ? std::string::npos : i;
}

// Call fn(key, value_pos) for each member of the object at j[pos] itself,
// not of the objects nested in it.
template <typename Fn>
static void for_each_member(const std::string& j, size_t pos, Fn fn) {
  if (pos >= j.size() || j[pos] != '{') return;
  pos = skip_ws(j, pos + 1);
  while (pos < j.size() && j[pos] == '"') {
    const size_t key_pos = pos;
    const std::string key = extract_string(j, pos);
    if (pos == key_pos) return;
    pos = skip_ws(j, pos);
    if (pos >= j.size() || j[pos] != ':') return;
    pos = skip_ws(j, pos + 1);
    const size_t end = skip_value(j, pos);
    if (end == std::string::npos) return;
    fn(key, pos);
    pos = skip_ws(j, end);
    if (pos >= j.size() || j[pos] != ',') return;
    pos = skip_ws(j, pos + 1);
  }
}

// Position of the value of `key` among the members of the object at
// j[pos] itself; npos if it has none.
static size_t member(const std::string& j, size_t pos,
                     const std::string& key) {
  size_t found = std::string::npos;
  for_each_member(j, pos, [&](const std::string& k, size_t value_pos) {
    if (found == std::string::npos && k == key) found = value_pos;
  });
  return found;
}

// Call fn(value_pos) for each element of the array at j[pos].
template <typename Fn>
static void for_each_element(const std::string& j, size_t pos, Fn fn) {
  if (pos >= j.size() || j[pos] != '[') return;
  pos = skip_ws(j, pos + 1);
  while (pos < j.size() && j[pos] != ']') {
    const size_t end = skip_value(j, pos);
    if (end == std::string::npos) return;
    fn(pos);
    pos = skip_ws(j, end);
    if (pos >= j.size() || j[pos] != ',') return;
    pos = skip_ws(j, pos + 1);
  }
}

// String value at j[pos]; empty if it is not a string.
static std::string string_at(const std::string& j, size_t pos) {
  if (pos == std::string::npos) return {};
  return extract_string(j, pos);
}

// Append, in order, the numbers in the array at j[pos] and in the arrays
// directly inside it: [1, 2] and [[8, 0], [17, 30]] both flatten.
static void collect_ints(const std::string& j, size_t pos,
                         std::vector<int>* out) {
  const auto add = [&](size_t p) {
    if (j[p] == '-' || (j[p] >= '0' && j[p] <= '9'))
      out->push_back(static_cast<int>(std::strtol(j.c_str() + p, nullptr, 10)));
  };
  for_each_element(j, pos, [&](size_t p) {
    if (j[p] == '[') {
      for_each_element(j, p, add);
    } else {
      add(p);
    }
  });
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

        // Keys are looked up among the alarm's own members only: its
        // actions nest objects with keys of the same names.
        entry.id = string_at(obj, member(obj, 0, "id"));
        entry.name = string_at(obj, member(obj, 0, "name"));

        // Check "enable" boolean
        size_t p = member(obj, 0, "enable");
        entry.enabled = p != std::string::npos && obj[p] == 't';

        // Skip deleted automations
        p = member(obj, 0, "deleted");
        const bool deleted = p != std::string::npos && obj[p] == 't';

        // Parse "conditions" array
        p = member(obj, 0, "conditions");
        if (p != std::string::npos && obj[p] == '[') {
          std::string arr = extract_balanced(obj, p);
          entry.trigger_types = parse_conditions(arr);
        }

        // Camera filters: [{"device":"<MAC>","type":"include"|"exclude"}].
        // A source without a type counts as included; any other type is
        // ignored, as Protect's filterEventsBySources does.
        for_each_element(obj, member(obj, 0, "sources"), [&](size_t sp) {
          const std::string dev = string_at(obj, member(obj, sp, "device"));
          if (dev.empty()) return;
          const size_t tp = member(obj, sp, "type");
          const std::string type = string_at(obj, tp);
          if (type == "exclude") {
            entry.excluded_devices.insert(dev);
          } else if (tp == std::string::npos || type == "include") {
            entry.source_devices.insert(dev);
          }
        });

        // Parse "cooldown" object
        p = member(obj, 0, "cooldown");
        if (p != std::string::npos && obj[p] == '{') {
          const size_t ep = member(obj, p, "enable");
          entry.cooldown_enabled = ep != std::string::npos && obj[ep] == 't';
          const size_t tp = member(obj, p, "timeout");
          if (tp != std::string::npos)
            entry.cooldown_ms = extract_uint(obj, tp);
        }

        // The alarm's own schedules, and the recipients of its
        // SEND_NOTIFICATION action:
        //   "actions":[{"type":"SEND_NOTIFICATION","metadata":{"receivers":[
        //     {"user":"<protect id>","ucoreUser":"<uuid>",
        //      "channels":["push","email"],"schedules":[...]}]}}]
        p = member(obj, 0, "schedules");
        if (p != std::string::npos) entry.schedules = parse_schedules(obj, p);
        for_each_element(obj, member(obj, 0, "actions"), [&](size_t action) {
          if (string_at(obj, member(obj, action, "type")) !=
              "SEND_NOTIFICATION") {
            return;
          }
          entry.has_notify_action = true;
          const size_t meta = member(obj, action, "metadata");
          for_each_element(obj, member(obj, meta, "receivers"), [&](size_t rp) {
            Receiver r;
            r.user = string_at(obj, member(obj, rp, "user"));
            r.ucore_user = string_at(obj, member(obj, rp, "ucoreUser"));
            for_each_element(obj, member(obj, rp, "channels"), [&](size_t cp) {
              const std::string channel = string_at(obj, cp);
              if (channel == "push") r.push = true;
              if (channel == "email") r.email = true;
            });
            const size_t sp = member(obj, rp, "schedules");
            if (sp != std::string::npos) r.schedules = parse_schedules(obj, sp);
            entry.receivers.push_back(std::move(r));
          });
        });

        if (!entry.id.empty() && !deleted && !entry.trigger_types.empty()) {
          result.push_back(std::move(entry));
        }
        start = std::string::npos;
      }
    }
  }
  return result;
}

std::vector<AlarmNotifier::Schedule> AlarmNotifier::parse_schedules(
    const std::string& json, size_t pos) {
  std::vector<Schedule> schedules;
  for_each_element(json, pos, [&](size_t p) {
    if (json[p] != '{') return;
    // Protect stores each entry as {"schedule": {...}}.
    const size_t inner = member(json, p, "schedule");
    schedules.push_back(
        parse_schedule(json, inner != std::string::npos ? inner : p, 0));
  });
  return schedules;
}

AlarmNotifier::Schedule AlarmNotifier::parse_schedule(const std::string& json,
                                                      size_t pos, int depth) {
  constexpr int kMaxDepth = 16;
  Schedule s;
  if (pos >= json.size() || json[pos] != '{') return s;
  // {"type":"is"|"not","unit":"day_of_week","values":[1,2],
  //  "and":{...},"or":{...}}
  s.negate = string_at(json, member(json, pos, "type")) != "is";
  const std::string unit = string_at(json, member(json, pos, "unit"));
  const size_t values = member(json, pos, "values");
  if (unit == "time_range") {
    s.unit = Schedule::Unit::kTimeRange;  // [[8, 0], [17, 30]]
    collect_ints(json, values, &s.values);
  } else if (unit == "day_of_month") {
    s.unit = Schedule::Unit::kMonthDay;
    collect_ints(json, values, &s.values);
  } else if (unit == "day_of_week") {
    s.unit = Schedule::Unit::kWeekDay;
    collect_ints(json, values, &s.values);
  } else if (unit == "location") {
    s.unit = Schedule::Unit::kLocation;  // ["away"] or ["allAway"]
    size_t first = std::string::npos;
    for_each_element(json, values, [&](size_t p) {
      if (first == std::string::npos) first = p;
    });
    s.user_away = string_at(json, first) == "away";
  }
  if (depth < kMaxDepth) {
    const size_t and_pos = member(json, pos, "and");
    if (and_pos != std::string::npos && json[and_pos] == '{') {
      s.and_then = std::make_shared<const Schedule>(
          parse_schedule(json, and_pos, depth + 1));
    }
    const size_t or_pos = member(json, pos, "or");
    if (or_pos != std::string::npos && json[or_pos] == '{') {
      s.or_else = std::make_shared<const Schedule>(
          parse_schedule(json, or_pos, depth + 1));
    }
  }
  return s;
}

std::map<std::string, std::string> AlarmNotifier::parse_user_ids(
    const std::string& json) {
  std::map<std::string, std::string> ids;
  for_each_element(json, skip_ws(json, 0), [&](size_t p) {
    const std::string id = string_at(json, member(json, p, "id"));
    const std::string ucore = string_at(json, member(json, p, "ucoreUserId"));
    if (!id.empty() && !ucore.empty()) ids[id] = ucore;
  });
  return ids;
}

// ============================================================
// Recipients and schedules
// ============================================================

bool AlarmNotifier::schedule_matches(const Schedule& s,
                                     const ScheduleClock& clock,
                                     bool user_away, bool all_away) {
  const auto has = [&](int v) {
    return std::find(s.values.begin(), s.values.end(), v) != s.values.end();
  };
  bool match = false;
  switch (s.unit) {
    case Schedule::Unit::kMonthDay:
      match = has(clock.month_day);
      break;
    case Schedule::Unit::kWeekDay:
      match = has(clock.week_day);
      break;
    case Schedule::Unit::kTimeRange:
      if (s.values.size() >= 4) {
        const int from = s.values[0] * 60 + s.values[1];
        const int to = s.values[2] * 60 + s.values[3];
        match = clock.minute_of_day >= from && clock.minute_of_day < to;
      }
      break;
    case Schedule::Unit::kLocation:
      match = s.user_away ? user_away : all_away;
      break;
    case Schedule::Unit::kOther:
      break;
  }
  if (s.negate) match = !match;
  if (match && s.and_then)
    match = schedule_matches(*s.and_then, clock, user_away, all_away);
  if (!match && s.or_else)
    match = schedule_matches(*s.or_else, clock, user_away, all_away);
  return match;
}

bool AlarmNotifier::schedules_match(const std::vector<Schedule>& schedules,
                                    const ScheduleClock& clock,
                                    bool user_away, bool all_away) {
  if (schedules.empty()) return true;
  for (const Schedule& s : schedules) {
    if (schedule_matches(s, clock, user_away, all_away)) return true;
  }
  return false;
}

bool AlarmNotifier::covers_camera(const AutomationEntry& automation,
                                  const std::string& mac) {
  if (automation.excluded_devices.count(mac)) return false;
  return automation.source_devices.empty() ||
         automation.source_devices.count(mac) > 0;
}

bool AlarmNotifier::alarm_schedule_open(const AutomationEntry& automation,
                                        const ScheduleClock& clock) {
  // Protect tests the alarm's own schedules with the user taken as at
  // home.  Whether every user is away is only known to Protect, so the
  // alarm counts as open if it matches either way; a match only while
  // everyone is away becomes the recipients' geofence_restriction.
  return schedules_match(automation.schedules, clock, false, false) ||
         schedules_match(automation.schedules, clock, false, true);
}

AlarmNotifier::ScheduleClock AlarmNotifier::local_clock(std::time_t t) {
  std::tm tm{};
  localtime_r(&t, &tm);
  ScheduleClock clock;
  clock.minute_of_day = tm.tm_hour * 60 + tm.tm_min;
  clock.month_day = tm.tm_mday;
  clock.week_day = tm.tm_wday;
  return clock;
}

AlarmNotifier::NotifyPlan AlarmNotifier::plan_notification(
    const AutomationEntry& automation, const ScheduleClock& clock) {
  NotifyPlan plan;
  if (!automation.has_notify_action) {
    plan.skip_reason = "the alarm has no notification action";
    return plan;
  }
  // Whether people are away is known to Protect at delivery time, so each
  // recipient is tested in every (this user away, every user away) state
  // that can occur: every user being away implies this one is.  Protect
  // tests the alarm's own schedules with the user taken as at home.
  static constexpr bool kAway[3][2] = {
      {false, false}, {true, false}, {true, true}};
  bool any_channel = false;
  bool any_user = false;
  for (const Receiver& r : automation.receivers) {
    if (!r.push && !r.email) continue;
    any_channel = true;
    if (r.ucore_user.empty()) {
      plan.unresolved_user = true;
      continue;
    }
    any_user = true;
    bool m[3];
    for (int i = 0; i < 3; ++i) {
      m[i] = schedules_match(automation.schedules, clock, false, kAway[i][1]) &&
             schedules_match(r.schedules, clock, kAway[i][0], kAway[i][1]);
    }
    if (!m[0] && !m[1] && !m[2]) continue;
    NotifyReceiver receiver;
    receiver.user_id = r.ucore_user;
    receiver.push = r.push;
    receiver.email = r.email;
    if (!m[0] && m[1] && m[2]) {
      receiver.geofence = Geofence::kCurrentAdmin;
    } else if (!m[0] && !m[1] && m[2]) {
      receiver.geofence = Geofence::kAllAdmins;
    } else if (!(m[0] && m[1] && m[2])) {
      // Only while at home, or only while this user but not everyone is
      // away: geofence_restriction cannot say either.
      plan.location_approximated = true;
    }
    plan.receivers.push_back(std::move(receiver));
  }
  if (plan.receivers.empty()) {
    plan.skip_reason =
        !any_channel ? "no recipient has push or email on"
        : !any_user  ? "no recipient has a UniFi OS user id"
                     : "the alarm's and recipients' schedules do not match now";
  }
  return plan;
}

bool AlarmNotifier::first_warning(const std::string& key) {
  absl::MutexLock lk(&mu_);
  return warned_.insert(key).second;
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
    const std::vector<NotifyReceiver>& receivers) {
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
  //                   "email": false, "voip": false,
  //                   "geofence_restriction": "current_admin" }, ...]
  // }
  //
  // Protect sends to each receiver whose UniFi OS user it knows, on the
  // channels set true; geofence_restriction "current_admin" holds it back
  // unless that user is away, "all_admins" unless every user is.
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
  p += ",\"receivers\":[";
  for (size_t i = 0; i < receivers.size(); ++i) {
    const NotifyReceiver& r = receivers[i];
    if (i > 0) p += ',';
    p += "{\"user_id\":";          p += json_str(r.user_id);
    p += ",\"push\":";             p += r.push ? "true" : "false";
    p += ",\"email\":";            p += r.email ? "true" : "false";
    p += ",\"voip\":false";
    if (r.geofence == Geofence::kCurrentAdmin) {
      p += ",\"geofence_restriction\":\"current_admin\"";
    } else if (r.geofence == Geofence::kAllAdmins) {
      p += ",\"geofence_restriction\":\"all_admins\"";
    }
    p += '}';
  }
  p += "]}";
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
// Global Alarm Manager (alarms kept by UniFi OS)
// ============================================================

namespace {

constexpr char kObjectTriggerPrefix[] = "protect:objects.";
constexpr size_t kObjectTriggerPrefixLen = sizeof(kObjectTriggerPrefix) - 1;

// Non-negative integer at j[pos], quoted ("10") or not (10); -1 if neither.
static int64_t number_at(const std::string& j, size_t pos) {
  if (pos == std::string::npos || pos >= j.size()) return -1;
  if (j[pos] == '"') ++pos;
  if (pos >= j.size() || j[pos] < '0' || j[pos] > '9') return -1;
  return static_cast<int64_t>(std::strtoll(j.c_str() + pos, nullptr, 10));
}

// At most @p max bytes of @p body on one line, for a log message.
static std::string log_excerpt(const std::string& body, size_t max = 300) {
  std::string out = body.substr(0, max);
  for (char& c : out) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }
  if (body.size() > max) out += "...";
  return out;
}

}  // namespace

AlarmNotifier::Mode AlarmNotifier::decide_mode(std::optional<bool> flag,
                                               Mode previous,
                                               bool uos_has_object_alarms) {
  if (flag.has_value()) return *flag ? Mode::kGlobal : Mode::kLocal;
  if (previous != Mode::kUnknown) return previous;
  return uos_has_object_alarms ? Mode::kGlobal : Mode::kLocal;
}

const char* AlarmNotifier::mode_name(Mode mode) {
  switch (mode) {
    case Mode::kGlobal:
      return "Global Alarm Manager (alarms kept by UniFi OS)";
    case Mode::kLocal:
      return "Local Alarm Manager (Protect automations)";
    case Mode::kUnknown:
      break;
  }
  return "unknown";
}

std::string AlarmNotifier::normalize_mac(const std::string& mac) {
  std::string out;
  out.reserve(mac.size());
  for (const char c : mac) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isxdigit(u)) out += static_cast<char>(std::toupper(u));
  }
  return out;
}

const char* AlarmNotifier::default_scope_key(const std::string& obj_type) {
  return obj_type == "package" ? "scope_all_smart_cameras_with_package"
                               : "scope_all_smart_cameras_with_zones";
}

bool AlarmNotifier::parse_uos_alarms(const std::string& json,
                                     std::vector<UosAlarm>* out) {
  out->clear();
  const size_t top = skip_ws(json, 0);
  if (top >= json.size() || json[top] != '[' ||
      balanced_end(json, top) == std::string::npos) {
    return false;
  }
  for_each_element(json, top, [&](size_t ap) {
    if (json[ap] != '{') return;
    UosAlarm a;
    a.id = string_at(json, member(json, ap, "id"));
    a.title = string_at(json, member(json, ap, "title"));
    const size_t paused = member(json, ap, "is_paused");
    a.paused = paused != std::string::npos && json[paused] == 't';

    // triggers_data is an array of arrays of triggers.  Protect reads it
    // (externalTriggerToConditions) as: every trigger without a
    // precondition_config is a main trigger, and any one main trigger
    // fires the alarm; a trigger with a precondition_config must also have
    // happened within its interval, AND-ed with the main one.  A main
    // trigger's data.count ({"items":{"primaryInput":"10",
    // "secondarySelector":"1 hour"}}) fires only once that many matching
    // detections were seen within the window.
    std::map<std::string, bool> single;  // object type -> fires on one
    std::map<std::string, std::string> count_reason;
    std::string preconditions;
    for_each_element(json, member(json, ap, "triggers_data"), [&](size_t gp) {
      for_each_element(json, gp, [&](size_t tp) {
        if (json[tp] != '{') return;
        const std::string id = string_at(json, member(json, tp, "id"));
        const bool object = id.size() > kObjectTriggerPrefixLen &&
                            id.compare(0, kObjectTriggerPrefixLen,
                                       kObjectTriggerPrefix) == 0;
        if (object) a.has_object_trigger = true;
        const size_t pc = member(json, tp, "precondition_config");
        if (pc != std::string::npos && json[pc] == '{') {
          if (!preconditions.empty()) preconditions += ", ";
          preconditions += id;
          return;
        }
        if (!object) return;
        const std::string type = id.substr(kObjectTriggerPrefixLen);
        const size_t items =
            member(json, member(json, member(json, tp, "data"), "count"),
                   "items");
        const int64_t n =
            number_at(json, member(json, items, "primaryInput"));
        std::string window =
            string_at(json, member(json, items, "secondarySelector"));
        if (window == "custom") {
          const int64_t d =
              number_at(json, member(json, items, "customDuration"));
          const std::string unit =
              string_at(json, member(json, items, "customTimeframe"));
          window = unit.empty() ? std::string()
                                : (d >= 0 ? std::to_string(d) + " " : "") + unit;
        }
        if (n > 1 && !window.empty()) {
          count_reason[type] = "its " + type + " trigger needs " +
                               std::to_string(n) + " detections within " +
                               window;
          single.emplace(type, false);
        } else {
          single[type] = true;
        }
      });
    });
    for (const auto& [type, fires] : single) {
      if (!preconditions.empty()) {
        a.skipped[type] = "its " + type + " trigger fires only together "
                          "with " + preconditions + " within a time window";
      } else if (fires) {
        a.fire_types.insert(type);
      } else {
        a.skipped[type] = count_reason[type];
      }
    }

    // "scope":{"mode":"include"|"exclude","data":{"<key>":[MAC|"ALL_ITEMS"]}}
    for_each_member(
        json, member(json, member(json, ap, "scope"), "data"),
        [&](const std::string& key, size_t /*value*/) {
          if (key != "site_id") a.scope_keys.push_back(key);
        });
    if (!a.id.empty()) out->push_back(std::move(a));
  });
  return true;
}

// static
std::string AlarmNotifier::build_uos_events(
    const std::vector<UosAlarm>& alarms, const std::string& obj_type,
    const std::string& mac, const std::string& camera_id,
    const std::string& event_id, uint64_t ts_ms) {
  using onvif::util::json_str;
  char ts_buf[24];
  std::snprintf(ts_buf, sizeof(ts_buf), "%" PRIu64, ts_ms);
  // Each event names one alarm and is matched against only that alarm:
  //   {"id":"person","alarm_id":"<uos id>",
  //    "scope":{"<scope key>":"<MAC>",...,"site_id":"protect"},
  //    "metadata":{"event":{"key":"person","device":"<MAC>",
  //      "eventId":"<events.id>","timestamp":<ms>,
  //      "sourceEvent":{"type":"smartDetectZone","camera":"<cameras.id>",
  //        "start":<ms>,"smartDetectTypes":["person"]}}}}
  // The scope carries the camera under every key of the alarm's own scope
  // (plus the detection type's usual key), the way Protect files one
  // device under all of its scope names in one scope object; UniFi OS
  // checks it against the alarm's include / exclude list.  With
  // sourceEvent.camera Protect builds the notification and its thumbnail
  // from the event row; without it Protect looks the event id up.
  std::string p = "[";
  for (size_t i = 0; i < alarms.size(); ++i) {
    const UosAlarm& a = alarms[i];
    std::vector<std::string> keys = a.scope_keys;
    const std::string fallback = default_scope_key(obj_type);
    if (std::find(keys.begin(), keys.end(), fallback) == keys.end())
      keys.push_back(fallback);
    if (i > 0) p += ',';
    p += "{\"id\":";             p += json_str(obj_type);
    p += ",\"alarm_id\":";       p += json_str(a.id);
    p += ",\"scope\":{";
    for (const std::string& k : keys) {
      p += json_str(k);
      p += ':';
      p += json_str(mac);
      p += ',';
    }
    p += "\"site_id\":\"protect\"}";
    p += ",\"metadata\":{\"event\":{\"key\":"; p += json_str(obj_type);
    p += ",\"device\":";         p += json_str(mac);
    p += ",\"eventId\":";        p += json_str(event_id);
    p += ",\"timestamp\":";      p += ts_buf;
    p += ",\"sourceEvent\":{\"type\":\"smartDetectZone\"";
    if (!camera_id.empty()) {
      p += ",\"camera\":";       p += json_str(camera_id);
    }
    p += ",\"start\":";          p += ts_buf;
    p += ",\"smartDetectTypes\":["; p += json_str(obj_type); p += "]}}}}";
  }
  p += ']';
  return p;
}

long AlarmNotifier::uos_request(const std::string& url,  // NOLINT(runtime/int)
                                const std::string& body,
                                std::string* response, int* curl_code) {
  response->clear();
  *curl_code = CURLE_OK;
  CURL* curl = curl_easy_init();
  if (!curl) {
    *curl_code = CURLE_FAILED_INIT;
    return 0;
  }
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = curl_slist_append(headers, "X-Source: protect");
  if (!body.empty())
    headers = curl_slist_append(headers, "Content-Type: application/json");

  auto collect = +[](char* d, size_t s, size_t n, void* out) -> size_t {
    constexpr size_t kMaxBody = 8u << 20;
    auto* str = static_cast<std::string*>(out);
    if (str->size() < kMaxBody)
      str->append(d, std::min(s * n, kMaxBody - str->size()));
    return s * n;
  };
  curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,        5L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  collect);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,      response);
  if (!body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,   body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));  // NOLINT(runtime/int)
  }
  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;  // NOLINT(runtime/int)
  if (rc != CURLE_OK) {
    *curl_code = rc;
  } else {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return http_code;
}

bool AlarmNotifier::fetch_uos_alarms(std::vector<UosAlarm>* out) {
  const std::string url = uos_url_ + "/api/v1/alarms";
  std::string body;
  int cc = 0;
  const long code = uos_request(url, "", &body, &cc);  // NOLINT(runtime/int)
  if (code != 200) {
    if (code == 0) {
      LOG(WARNING) << "[alarm] GET " << url << " failed: "
                   << curl_easy_strerror(static_cast<CURLcode>(cc));
    } else {
      LOG(WARNING) << "[alarm] GET " << url << " HTTP " << code << ": "
                   << log_excerpt(body);
    }
    return false;
  }
  if (!parse_uos_alarms(body, out)) {
    LOG(WARNING) << "[alarm] GET " << url << " did not return a JSON "
                 << "array: " << log_excerpt(body);
    return false;
  }
  return true;
}

void AlarmNotifier::store_uos_alarms(std::vector<UosAlarm> alarms) {
  // One summary line when the usable alarms change, not on every refresh.
  std::string summary;
  size_t usable = 0;
  for (const UosAlarm& a : alarms) {
    if (a.fire_types.empty()) continue;
    if (!summary.empty()) summary += ", ";
    summary += "\"" + a.title + "\" (";
    std::string types;
    for (const std::string& t : a.fire_types) {
      if (!types.empty()) types += '/';
      types += t;
    }
    summary += types + (a.paused ? ", paused)" : ")");
    if (!a.paused) ++usable;
  }
  std::vector<std::pair<std::string, std::string>> skipped;
  for (const UosAlarm& a : alarms) {
    for (const auto& [type, reason] : a.skipped)
      skipped.emplace_back(a.id + ":" + type, "\"" + a.title + "\": " + reason);
  }
  bool changed = false;
  {
    absl::MutexLock lk(&mu_);
    changed = !uos_loaded_ || summary != uos_summary_;
    uos_summary_ = summary;
    uos_alarms_ = std::move(alarms);
    uos_loaded_ = true;
  }
  if (changed) {
    LOG(INFO) << "[alarm] " << usable << " UniFi OS alarm(s) with an object "
              << "trigger will be sent detections"
              << (summary.empty() ? std::string() : ": " + summary);
  }
  for (const auto& [key, text] : skipped) {
    if (first_warning("uos-skip:" + key)) {
      LOG(WARNING) << "[alarm] UniFi OS alarm " << text << ", which only "
                   << "Protect's own detection matching evaluates; "
                   << "onvif-recorder does not trigger it";
    }
  }
}

// ============================================================
// AlarmNotifier public API
// ============================================================

AlarmNotifier::AlarmNotifier(std::string protect_url,
                             ProtectUserIdProvider* user_id_provider,
                             std::string db_connstr, std::string uos_url)
    : protect_url_(std::move(protect_url)),
      user_id_provider_(user_id_provider),
      db_connstr_(std::move(db_connstr)),
      uos_url_(std::move(uos_url)) {
  ContentionProfiler::instance().register_mutex(&mu_, "alarm_notifier");
}

std::unique_ptr<AlarmNotifier> AlarmNotifier::create(
    bool trigger_automations, std::string protect_url,
    ProtectUserIdProvider* user_id_provider, std::string db_connstr) {
  if (!trigger_automations) {
    LOG(WARNING) << "[alarm] --trigger_automations=false: detections are "
                    "recorded but no alarms are triggered";
    return nullptr;
  }
  if (user_id_provider == nullptr || user_id_provider->empty()) {
    // WARNING (not INFO) because the failure mode is silent to the user
    // on a Local Alarm Manager console: detections still record, but no
    // automations fire (issue #41).  Global Alarm Manager alarms are
    // handed to UniFi OS, which needs no user id.
    LOG(WARNING) << "[alarm] user ID not found; Local Alarm Manager "
                    "automation triggers disabled (pass "
                    "--protect_user_id=<UUID> to override; see issue #41). "
                    "Global Alarm Manager alarms do not need it.";
  }
  return std::make_unique<AlarmNotifier>(std::move(protect_url),
                                         user_id_provider,
                                         std::move(db_connstr));
}

void AlarmNotifier::set_mode_probe(ModeProbe probe) {
  absl::MutexLock lk(&mu_);
  mode_probe_ = std::move(probe);
}

AlarmNotifier::Mode AlarmNotifier::mode() {
  absl::MutexLock lk(&mu_);
  return mode_;
}

void AlarmNotifier::refresh_alarms() {
  absl::MutexLock refresh(&refresh_mu_);
  refresh_locked();
}

void AlarmNotifier::maybe_refresh() {
  absl::MutexLock refresh(&refresh_mu_);
  bool stale = true;
  {
    absl::MutexLock lk(&mu_);
    const auto age = std::chrono::steady_clock::now() - last_refresh_;
    if (mode_ == Mode::kGlobal) {
      stale = !uos_loaded_ || age > std::chrono::seconds(60);
    } else if (mode_ == Mode::kLocal) {
      stale = automations_.empty() || age > std::chrono::minutes(5);
    }
  }
  if (stale) refresh_locked();
}

void AlarmNotifier::refresh_locked() {
  // Re-arm the UOS path on every refresh (every ~5 min).  If Global Alarm
  // Manager really is off we will get another 404 on the next notify and
  // latch again, at the cost of one failed request per refresh; but if the
  // 404 was situational -- Protect mid-restart, a partially-initialised
  // automation manager -- thumbnails come back on their own instead of
  // staying off until someone restarts the recorder.
  std::string url;
  ModeProbe probe;
  Mode previous = Mode::kUnknown;
  {
    absl::MutexLock lk(&mu_);
    uos_unavailable_ = false;
    url = protect_url_;
    probe = mode_probe_;
    previous = mode_;
  }
  std::optional<bool> db_flag;

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
      // Protect's Global Alarm Manager switch.  A missing key is Local,
      // as in Protect.
      PGresult* ff_res = onvif::pg::ExecParamsWithTimeout(
          cam_conn, /*timeout_ms=*/-1,
          "SELECT \"featureFlags\"->>'useExternalAlarmManager' FROM nvrs "
          "LIMIT 1",
          0, nullptr, nullptr, nullptr, nullptr, 0);
      if (PQresultStatus(ff_res) == PGRES_TUPLES_OK && PQntuples(ff_res) > 0) {
        db_flag = std::string(PQgetvalue(ff_res, 0, 0)) == "true";
      } else if (PQresultStatus(ff_res) != PGRES_TUPLES_OK) {
        LOG(WARNING) << "[alarm] nvrs featureFlags query failed: "
                     << PQresultErrorMessage(ff_res);
      }
      PQclear(ff_res);
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

  // ---- Step 2: Local or Global ----------------------------------------------
  const std::optional<bool> flag = probe ? probe() : db_flag;
  // The UniFi OS list is read whenever the console may be Global: the
  // flag says so, or it is unreadable and the last decision was not Local.
  const bool may_be_global =
      flag.has_value() ? *flag : previous != Mode::kLocal;
  std::vector<UosAlarm> uos;
  const bool uos_ok = may_be_global && fetch_uos_alarms(&uos);
  const bool uos_objects =
      uos_ok && std::any_of(uos.begin(), uos.end(), [](const UosAlarm& a) {
        return a.has_object_trigger;
      });
  const Mode mode = decide_mode(flag, previous, uos_objects);
  {
    absl::MutexLock lk(&mu_);
    mode_ = mode;
  }
  if (mode != previous) {
    const std::string why =
        flag.has_value()
            ? std::string(" (nvrs.featureFlags.useExternalAlarmManager=") +
                  (*flag ? "true)" : "false)")
            : uos_objects ? " (Protect DB unreadable; UniFi OS lists object "
                            "alarms)"
                          : " (Protect DB unreadable; no UniFi OS object "
                            "alarms found)";
    if (previous == Mode::kUnknown) {
      LOG(INFO) << "[alarm] alarm mode: " << mode_name(mode) << why;
    } else {
      LOG(WARNING) << "[alarm] alarm mode changed from " << mode_name(previous)
                   << " to " << mode_name(mode) << why;
    }
  }
  if (mode == Mode::kGlobal) {
    // Global: UniFi OS applies each alarm's cameras, schedule, cooldown,
    // pause and recipients and runs its actions, so only the alarm list is
    // kept.  An unreadable list keeps the last good one.
    if (uos_ok) store_uos_alarms(std::move(uos));
    absl::MutexLock lk(&mu_);
    last_refresh_ = std::chrono::steady_clock::now();
    return;
  }
  refresh_local(url);
}

void AlarmNotifier::refresh_local(const std::string& url) {
  if (user_id_provider_ == nullptr || user_id_provider_->empty()) {
    absl::MutexLock lk(&mu_);
    automations_.clear();
    last_refresh_ = std::chrono::steady_clock::now();
    return;
  }

  // ---- Step 3: load automations ----------------------------------------------
  const std::string response = http_get(url + "/api/automations");
  if (response.empty()) return;

  auto parsed = parse_automations(response);

  // A recipient saved without its UniFi OS user id is found by its Protect
  // user id, the way Protect resolves recipients itself.
  bool missing_ucore = false;
  for (const auto& a : parsed) {
    for (const auto& r : a.receivers) {
      if (r.ucore_user.empty() && !r.user.empty()) missing_ucore = true;
    }
  }
  if (missing_ucore) {
    const auto ucore_ids = parse_user_ids(http_get(url + "/api/users"));
    for (auto& a : parsed) {
      for (auto& r : a.receivers) {
        if (!r.ucore_user.empty()) continue;
        const auto it = ucore_ids.find(r.user);
        if (it != ucore_ids.end()) r.ucore_user = it->second;
      }
    }
  }

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
              << " excluded=" << a.excluded_devices.size()
              << " cooldown=" << a.cooldown_enabled
              << "/" << a.cooldown_ms << "ms"
              << " notify=" << a.has_notify_action
              << " recipients=" << a.receivers.size()
              << " schedules=" << a.schedules.size();
  }

  // ---- Step 4: register each enabled automation with the UOS store ----------
  // The in-memory UOS automation manager (automationManager) requires a
  // /change "created" call before /actions/notify will succeed.  This
  // registration is lost on every Protect restart; we redo it here.
  // notify() will also re-register on-demand when it receives HTTP 500.
  // One summary line rather than one error per automation: a rejected
  // registration usually affects all of them for the same reason.
  int attempted = 0;
  int failed = 0;
  int route_missing = 0;
  long last_code = 0;  // NOLINT(runtime/int)
  for (const auto& entry : parsed) {
    if (!entry.enabled) continue;
    ++attempted;
    const long code = register_automation(entry);  // NOLINT(runtime/int)
    if (code < 200 || code >= 300) {
      ++failed;
      last_code = code;
      if (uos_unavailable(code)) ++route_missing;
    }
  }
  if (failed > 0) {
    LOG(ERROR) << "[alarm] failed to register " << failed
               << " automation(s) with the UOS automation manager (HTTP "
               << last_code << "); notifications will use the legacy path";
  }

  absl::MutexLock lk(&mu_);
  // Every registration answering 404 means the automation manager routes
  // are absent, so the legacy path is used from the first detection on.
  // notify() makes no UOS call for an alarm with nobody to notify, so it
  // would not otherwise find out before the legacy path is needed.
  if (attempted > 0 && route_missing == attempted) uos_unavailable_ = true;
  automations_  = std::move(parsed);
  last_refresh_ = std::chrono::steady_clock::now();
}

void AlarmNotifier::notify(const std::string& obj_type,
                           const std::string& camera_mac,
                           const std::string& event_id,
                           uint64_t ts_ms) {
  // The alarm list is re-read when missing or older than 60 s (Global)
  // or 5 min (Local); the mode is decided again on each read.
  maybe_refresh();
  if (mode() == Mode::kGlobal) {
    notify_global(obj_type, normalize_mac(camera_mac), event_id, ts_ms);
  } else {
    notify_local(obj_type, camera_mac, event_id, ts_ms);
  }
}

void AlarmNotifier::notify_global(const std::string& obj_type,
                                  const std::string& mac,
                                  const std::string& event_id,
                                  uint64_t ts_ms) {
  std::vector<UosAlarm> selected;
  std::string camera_id;
  bool loaded = false;
  {
    absl::MutexLock lk(&mu_);
    loaded = uos_loaded_;
    for (const UosAlarm& a : uos_alarms_) {
      if (!a.paused && a.fire_types.count(obj_type)) selected.push_back(a);
    }
    const auto it = mac_to_camera_id_.find(mac);
    if (it != mac_to_camera_id_.end()) camera_id = it->second;
  }
  const std::string what =
      obj_type + " on " + mac + " (event " + event_id + ")";
  if (!loaded) {
    LOG(ERROR) << "[alarm] " << what << " not sent: the UniFi OS alarm "
               << "list could not be read";
    return;
  }
  if (selected.empty()) {
    LOG(INFO) << "[alarm] " << what << ": no UniFi OS alarm fires on a "
              << "single " << obj_type << " detection, nothing sent";
    return;
  }
  if (camera_id.empty() && first_warning("camera-id:" + mac)) {
    LOG(WARNING) << "[alarm] no Protect camera id for MAC " << mac
                 << "; alarms are sent without it, so Protect looks the "
                 << "event up by id for the notification thumbnail";
  }
  std::string titles;
  for (const UosAlarm& a : selected) {
    if (!titles.empty()) titles += ", ";
    titles += "\"" + a.title + "\"";
  }
  const std::string payload =
      build_uos_events(selected, obj_type, mac, camera_id, event_id, ts_ms);
  const std::string url = uos_url_ + "/api/v1/alarms/events";

  // UniFi OS answers 202 before it runs the alarms and does not drop a
  // repeated event, so the request is repeated only when the connection
  // was never made; a timeout or a 5xx may already have run them.
  std::string resp;
  int cc = 0;
  long code = uos_request(url, payload, &resp, &cc);  // NOLINT(runtime/int)
  if (code == 0 && cc == CURLE_COULDNT_CONNECT) {
    LOG(WARNING) << "[alarm] " << what << ": could not connect to " << url
                 << "; retrying once in " << retry_delay_ms_ << " ms";
    std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms_));
    code = uos_request(url, payload, &resp, &cc);
  }
  if (code == 0) {
    LOG(ERROR) << "[alarm] " << what << " not handed to UniFi OS for "
               << titles << ": "
               << curl_easy_strerror(static_cast<CURLcode>(cc))
               << (cc == CURLE_COULDNT_CONNECT
                       ? " (after one retry); dropped"
                       : "; not retried, as UniFi OS may have run them");
  } else if (code >= 200 && code < 300) {
    LOG(INFO) << "[alarm] " << what << " handed to UniFi OS for " << titles
              << " (HTTP " << code << "); which alarms matched is logged "
              << "in /data/unifi-core/logs/uos-agent.log";
  } else if (code >= 500) {
    LOG(ERROR) << "[alarm] " << what << ": UniFi OS answered HTTP " << code
               << " for " << titles << "; not retried, as it may have run "
               << "them: " << log_excerpt(resp);
  } else {
    LOG(ERROR) << "[alarm] " << what << ": UniFi OS rejected the alarm "
               << "events for " << titles << " with HTTP " << code << ": "
               << log_excerpt(resp);
  }
}

void AlarmNotifier::notify_local(const std::string& obj_type,
                                 const std::string& camera_mac,
                                 const std::string& event_id,
                                 uint64_t ts_ms) {
  if (user_id_provider_ == nullptr || user_id_provider_->empty()) {
    if (first_warning("no-user-id")) {
      LOG(WARNING) << "[alarm] Local Alarm Manager automations are not "
                      "triggered: no Protect user id (see issue #41)";
    }
    return;
  }
  std::string url;
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

    // Check the automation's include / exclude camera lists.
    if (!covers_camera(automation, camera_mac)) continue;

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

    bool use_uos;
    {
      absl::MutexLock lk(&mu_);
      use_uos = !uos_unavailable_;
    }
    const ScheduleClock clock = local_clock(std::time(nullptr));

    // Outside its own schedule a Protect alarm does not fire at all: no
    // actions, no history row, and its cooldown does not start, so a hit
    // just before the schedule opens cannot suppress one just after.
    if (use_uos && !alarm_schedule_open(automation, clock)) {
      LOG(INFO) << "[alarm] automation " << automation.id
                << ": outside the alarm's schedule, not fired";
      continue;
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
      std::string payload;

      long code = 0;  // NOLINT(runtime/int)
      if (use_uos) {
        // The route notifies exactly the receivers named, so they come
        // from the alarm's own recipients, channels and schedules.
        const NotifyPlan plan = plan_notification(automation, clock);
        if (plan.location_approximated &&
            first_warning("location:" + automation.id)) {
          LOG(WARNING) << "[alarm] automation " << automation.id << " (\""
                       << automation.name << "\"): a recipient's schedule "
                       << "depends on who is home in a way Protect's notify "
                       << "route cannot check; notifying that recipient "
                       << "whether or not they are away";
        }
        if (plan.unresolved_user && first_warning("user:" + automation.id)) {
          LOG(WARNING) << "[alarm] automation " << automation.id << " (\""
                       << automation.name << "\"): a recipient could not "
                       << "be matched to a UniFi OS user; not notifying "
                       << "that recipient";
        }
        if (plan.receivers.empty()) {
          LOG(INFO) << "[alarm] automation " << automation.id
                    << ": no notification sent -- " << plan.skip_reason;
        } else {
          payload = build_notify_payload(automation.id, obj_type, device_id,
                                         event_id, ts_ms, plan.receivers);
          code = http_post(notify_url, payload);
        }
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
