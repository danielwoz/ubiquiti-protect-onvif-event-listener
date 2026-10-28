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

/**
 * test_alarm_notifier.cpp
 *
 * White-box tests for AlarmNotifier via a friend struct declared in
 * alarm_notifier.hpp.
 *
 * Local Alarm Manager: the JSON shapes seen from Protect's API --
 * enable/disable, condition sources, include and exclude device filters,
 * cooldown, "deleted" entries being dropped, and the SEND_NOTIFICATION
 * recipients, channels and schedules that decide who the UOS notify call
 * goes to.  Fake Protect APIs check that notify() skips the call when
 * nobody is to be notified but still records history and the cooldown,
 * and still runs the alarm by the legacy path when the automation manager
 * routes are absent.
 *
 * Global Alarm Manager: the Local / Global decision, the UniFi OS alarm
 * list (a synthetic list shaped like a live one), which alarms a detection
 * is sent to, the exact POST /api/v1/alarms/events body, and how answers
 * and connection failures are handled, against a fake uos-agent.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "alarm_notifier.hpp"
#include "camera_emulators.hpp"
#include "onvif_camera_emulator.hpp"
#include "protect_user_id_provider.hpp"

namespace onvif {

// Test seam declared as `friend` in AlarmNotifier so we can reach the private
// static parser without exposing it to the public API.
struct AlarmNotifierTest {
  using Entry = AlarmNotifier::AutomationEntry;
  using Clock = AlarmNotifier::ScheduleClock;
  using Plan = AlarmNotifier::NotifyPlan;
  using Geofence = AlarmNotifier::Geofence;

  static std::vector<Entry> parse(const std::string& json) {
    return AlarmNotifier::parse_automations(json);
  }
  static bool should_re_register(long code) {  // NOLINT(runtime/int)
    return AlarmNotifier::should_re_register(code);
  }
  static bool uos_unavailable(long code) {  // NOLINT(runtime/int)
    return AlarmNotifier::uos_unavailable(code);
  }
  static Plan plan(const Entry& entry, const Clock& clock) {
    return AlarmNotifier::plan_notification(entry, clock);
  }
  static bool alarm_open(const Entry& entry, const Clock& clock) {
    return AlarmNotifier::alarm_schedule_open(entry, clock);
  }
  static std::string payload(const Entry& entry, const Clock& clock) {
    return AlarmNotifier::build_notify_payload(
        entry.id, "person", "cam-uuid", "evt-1", 1700000000000ULL,
        plan(entry, clock).receivers);
  }
  static Clock local_clock(std::time_t t) {
    return AlarmNotifier::local_clock(t);
  }
  static std::map<std::string, std::string> user_ids(const std::string& json) {
    return AlarmNotifier::parse_user_ids(json);
  }
  static std::map<std::string, uint64_t> last_fired(AlarmNotifier* n) {
    absl::MutexLock lk(&n->mu_);
    return n->last_fired_;
  }
  static bool covers(const Entry& entry, const std::string& mac) {
    return AlarmNotifier::covers_camera(entry, mac);
  }

  // Global Alarm Manager.
  using UosAlarm = AlarmNotifier::UosAlarm;
  using Mode = AlarmNotifier::Mode;
  static Mode decide(std::optional<bool> flag, Mode previous,
                     std::optional<bool> objects) {
    return AlarmNotifier::decide_mode(flag, previous, objects);
  }
  static bool parse_uos(const std::string& json, std::vector<UosAlarm>* out) {
    return AlarmNotifier::parse_uos_alarms(json, out);
  }
  static std::string uos_events(const std::vector<UosAlarm>& alarms,
                                const std::string& type,
                                const std::string& mac,
                                const std::string& camera_id,
                                const std::string& event_id, uint64_t ts) {
    return AlarmNotifier::build_uos_events(alarms, type, mac, camera_id,
                                           event_id, ts);
  }
  static std::string normalize_mac(const std::string& mac) {
    return AlarmNotifier::normalize_mac(mac);
  }
  static void set_camera_id(AlarmNotifier* n, const std::string& mac,
                            const std::string& id) {
    absl::MutexLock lk(&n->mu_);
    n->mac_to_camera_id_[mac] = id;
  }
  static void set_retry_delay_ms(AlarmNotifier* n, int ms) {
    n->retry_delay_ms_ = ms;
  }
  static void set_uos_timeout_ms(AlarmNotifier* n, int ms) {
    n->uos_timeout_ms_ = ms;
  }
  // POST /api/v1/alarms/events attempts, connection failures included.
  static int uos_posts(AlarmNotifier* n) { return n->uos_posts_.load(); }
  // Make the last refresh (and the Local list) look @p by older.
  static void age(AlarmNotifier* n, std::chrono::seconds by) {
    absl::MutexLock lk(&n->mu_);
    n->last_attempt_ -= by;
    n->local_loaded_at_ -= by;
  }
  static absl::Mutex* refresh_mu(AlarmNotifier* n) { return &n->refresh_mu_; }
  static void set_uos_url(AlarmNotifier* n, const std::string& url) {
    n->uos_url_ = url;
  }
};

}  // namespace onvif

using onvif::AlarmNotifierTest;
using Clock = AlarmNotifierTest::Clock;
using Geofence = AlarmNotifierTest::Geofence;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* label) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << label << '\n';
  }
}

static bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// Wall-clock fields for schedule tests; week_day 0 = Sunday.
static Clock at(int week_day, int hour, int minute, int month_day = 15) {
  Clock c;
  c.week_day = week_day;
  c.minute_of_day = hour * 60 + minute;
  c.month_day = month_day;
  return c;
}

constexpr int kSunday = 0;
constexpr int kMonday = 1;
constexpr int kSaturday = 6;

constexpr char kOwner[] = "00000000-0000-4000-8000-0000000000a1";

// A UniFi OS URL nothing listens on, for tests of the Local path.
constexpr char kNoUos[] = "http://127.0.0.1:1";

// Mode probes: the console's useExternalAlarmManager.
static std::optional<bool> local_console() { return false; }
static std::optional<bool> global_console() { return true; }
static std::optional<bool> unreadable_flag() { return std::nullopt; }
constexpr char kOwnerPk[] = "0000000000000000000000a1";

// One SEND_NOTIFICATION recipient, in Protect's key order.
static std::string receiver(const std::string& user, const std::string& ucore,
                            const std::string& channels,
                            const std::string& schedules = "[]") {
  std::string r = R"({"user":")" + user + R"(","channels":)" + channels +
                  R"(,"schedules":)" + schedules;
  if (!ucore.empty()) r += R"(,"ucoreUser":")" + ucore + "\"";
  return r + "}";
}

// A person alarm as GET /api/automations returns it, with the given
// actions and alarm-level schedules.
static std::string alarm_with_actions(const std::string& id,
                                      const std::string& actions,
                                      const std::string& schedules = "[]") {
  return R"({"name":"Person )" + id +
         R"(","enable":true,"isCreatedBySystem":false,"editable":true,)"
         R"("sources":[],"conditions":[{"condition":{"type":"is",)"
         R"("source":"person"}}],"historyConditions":[],"schedules":)" +
         schedules + R"(,"actions":)" + actions +
         R"(,"userId":"0000000000000000000000a1","status":{)"
         R"("lastExecutedAt":null,"lastDetectionEventId":null,"total":0},)"
         R"("cooldown":{"enable":false,"timeout":600000,"lastExecutedAt":0},)"
         R"("id":")" + id +
         R"(","deleted":false,"createdAt":1771856985737,)"
         R"("isBlockedByArmMode":false,"usedInArmMode":false,)"
         R"("isExternal":false})";
}

static std::string alarm(const std::string& id, const std::string& receivers,
                         const std::string& schedules = "[]") {
  return alarm_with_actions(
      id,
      R"([{"type":"SEND_NOTIFICATION","metadata":{"receivers":)" + receivers +
          R"(},"order":-1}])",
      schedules);
}

// Parse one alarm; an empty Entry with id "" if it did not parse.
static AlarmNotifierTest::Entry parse_one(const std::string& alarm_json) {
  auto v = AlarmNotifierTest::parse("[" + alarm_json + "]");
  return v.size() == 1 ? v[0] : AlarmNotifierTest::Entry{};
}

// Protect schedule entries.
static const char kWeekdayOfficeHours[] =
    R"([{"schedule":{"type":"is","unit":"day_of_week","values":[1,2,3,4,5],)"
    R"("and":{"type":"is","unit":"time_range","values":[[8,0],[17,0]]}}}])";
static const char kWeekends[] =
    R"([{"schedule":{"type":"is","unit":"day_of_week","values":[0,6]}}])";

// ---------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------

static void test_parse_basics() {
  // Empty array parses to zero entries.
  {
    auto v = AlarmNotifierTest::parse("[]");
    check(v.empty(), "empty array -> 0 entries");
  }

  // Single enabled automation with one trigger and one source device.
  {
    const std::string json = R"([{
      "id": "a1",
      "name": "Porch person",
      "enable": true,
      "conditions": [{"source":"person"}],
      "sources":    [{"device":"AABBCCDDEEFF"}],
      "cooldown":   {"enable": true, "timeout": 15000}
    }])";
    auto v = AlarmNotifierTest::parse(json);
    check(v.size() == 1, "one automation parsed");
    if (v.size() == 1) {
      check(v[0].id == "a1", "id extracted");
      check(v[0].name == "Porch person", "name extracted");
      check(v[0].enabled, "enabled=true parsed");
      check(v[0].trigger_types.count("person") == 1, "trigger person present");
      check(v[0].trigger_types.size() == 1, "trigger_types size 1");
      check(v[0].source_devices.count("AABBCCDDEEFF") == 1, "source MAC present");
      check(v[0].cooldown_enabled, "cooldown_enabled true");
      check(v[0].cooldown_ms == 15000, "cooldown_ms 15000");
      check(!v[0].has_notify_action, "no actions -> no notify action");
    }
  }

  // HTTP 500 should trigger re-registration, other codes should not.
  {
    check(AlarmNotifierTest::should_re_register(500),
          "HTTP 500 triggers re-registration");
    check(!AlarmNotifierTest::should_re_register(204),
          "HTTP 204 does not re-register");
    check(!AlarmNotifierTest::should_re_register(401),
          "HTTP 401 does not re-register");
  }

  // Global Alarm Manager off => 404 (route absent) or 0 (request never
  // completed) means fall back to the legacy path.  A 500 is the
  // re-register case and must NOT be mistaken for "unavailable", or we
  // would permanently downgrade on a transient eviction.
  {
    check(AlarmNotifierTest::uos_unavailable(404),
          "HTTP 404 means UOS unavailable");
    check(!AlarmNotifierTest::uos_unavailable(0),
          "a failed request is transient, NOT proof the route is absent "
          "(Protect restarting must not permanently disable thumbnails)");
    check(!AlarmNotifierTest::uos_unavailable(500),
          "HTTP 500 is re-register, not unavailable");
    check(!AlarmNotifierTest::uos_unavailable(204),
          "HTTP 204 is success, not unavailable");
  }

  // Deleted entries are dropped.
  {
    const std::string json = R"([
      {"id":"keep","enable":true,"conditions":[{"source":"vehicle"}]},
      {"id":"drop","enable":true,"deleted":true,"conditions":[{"source":"person"}]}
    ])";
    auto v = AlarmNotifierTest::parse(json);
    check(v.size() == 1, "deleted entry dropped");
    if (!v.empty()) {
      check(v[0].id == "keep", "surviving id == 'keep'");
    }
  }

  // Entries with no trigger_types are dropped (no-op alarm would never fire).
  {
    const std::string json =
        R"([{"id":"bad","enable":true,"conditions":[]}])";
    auto v = AlarmNotifierTest::parse(json);
    check(v.empty(), "no-trigger entry dropped");
  }

  // Empty sources array means "all cameras".
  {
    const std::string json = R"([{
      "id":"all",
      "enable":true,
      "conditions":[{"source":"person"}],
      "sources":[]
    }])";
    auto v = AlarmNotifierTest::parse(json);
    check(v.size() == 1, "empty sources parses");
    if (!v.empty()) {
      check(v[0].source_devices.empty(),
            "source_devices empty means all cameras");
    }
  }

  // Multiple trigger types collected into the set.
  {
    const std::string json = R"([{
      "id":"multi",
      "enable":true,
      "conditions":[
        {"source":"person"},
        {"source":"vehicle"},
        {"source":"animal"}
      ]
    }])";
    auto v = AlarmNotifierTest::parse(json);
    check(v.size() == 1 && v[0].trigger_types.size() == 3,
          "three trigger types collected");
  }

  // Disabled automation is still returned (caller filters on enabled).
  {
    const std::string json = R"([{
      "id":"off",
      "enable":false,
      "conditions":[{"source":"person"}]
    }])";
    auto v = AlarmNotifierTest::parse(json);
    check(v.size() == 1 && !v[0].enabled, "disabled retained, enable=false");
  }

  // Garbage input returns empty.
  {
    auto v = AlarmNotifierTest::parse("this is not json");
    check(v.empty(), "garbage -> empty");
  }

  // Escaped quotes inside strings don't fool the depth tracker.
  {
    const std::string json =
        R"([{"id":"esc","name":"hi \"there\"","enable":true,)"
        R"("conditions":[{"source":"person"}]}])";
    auto v = AlarmNotifierTest::parse(json);
    check(v.size() == 1, "escaped quotes in name don't confuse parser");
    if (!v.empty()) {
      check(v[0].id == "esc", "escaped-name id extracted");
    }
  }
}

// The default "Person" alarm as Protect 7.3 returns it, push turned off.
static void test_parse_real_alarm_push_off() {
  const std::string json = R"([{"name":"Person","enable":true,)"
      R"("isCreatedBySystem":false,"editable":true,"sources":[],)"
      R"("conditions":[{"condition":{"type":"is","source":"person"}}],)"
      R"("historyConditions":[],"schedules":[],"actions":[{"type":)"
      R"("SEND_NOTIFICATION","metadata":{"receivers":[{"user":)"
      R"("0000000000000000000000a1","channels":[],"schedules":[],)"
      R"("ucoreUser":"00000000-0000-4000-8000-0000000000a1"}]},"order":-1}],)"
      R"("userId":"0000000000000000000000a1","status":{"lastExecutedAt":null,)"
      R"("lastDetectionEventId":null,"total":0},"cooldown":{"enable":false,)"
      R"("timeout":600000,"lastExecutedAt":0},"id":"0000000000000000000000c3",)"
      R"("deleted":false,"createdAt":1771856985737,"isBlockedByArmMode":false,)"
      R"("usedInArmMode":false,"isExternal":false}])";
  auto v = AlarmNotifierTest::parse(json);
  check(v.size() == 1, "real Person alarm parsed");
  if (v.size() != 1) return;
  const auto& a = v[0];
  check(a.id == "0000000000000000000000c3", "real alarm id is its own id");
  check(a.trigger_types.count("person") == 1, "real alarm trigger person");
  check(!a.cooldown_enabled && a.cooldown_ms == 600000,
        "real alarm cooldown off, 600000 ms");
  check(a.has_notify_action, "real alarm has SEND_NOTIFICATION");
  check(a.schedules.empty(), "real alarm has no alarm-level schedule");
  check(a.receivers.size() == 1, "real alarm has one recipient");
  if (a.receivers.size() == 1) {
    check(a.receivers[0].user == kOwnerPk, "recipient Protect user id");
    check(a.receivers[0].ucore_user == kOwner, "recipient ucoreUser");
    check(!a.receivers[0].push && !a.receivers[0].email,
          "channels [] -> push and email off");
    check(a.receivers[0].schedules.empty(), "recipient has no schedule");
  }
  const auto plan = AlarmNotifierTest::plan(a, at(kMonday, 12, 0));
  check(plan.receivers.empty(), "channels [] -> no receivers");
  check(contains(plan.skip_reason, "push or email"),
        "channels [] -> skip reason names the channels");
}

static void test_channels() {
  // ["push","email"], as the Smoke Alarm default has it.
  {
    const auto a = parse_one(alarm(
        "smoke", "[" + receiver(kOwnerPk, kOwner, R"(["push","email"])") + "]"));
    const auto plan = AlarmNotifierTest::plan(a, at(kMonday, 12, 0));
    check(plan.receivers.size() == 1, "push+email -> one receiver");
    if (plan.receivers.size() == 1) {
      check(plan.receivers[0].user_id == kOwner, "user_id is ucoreUser");
      check(plan.receivers[0].push && plan.receivers[0].email,
            "push+email -> both channels");
      check(plan.receivers[0].geofence == Geofence::kNone, "no geofence");
    }
    const std::string p = AlarmNotifierTest::payload(a, at(kMonday, 12, 0));
    check(contains(p, R"("receivers":[{"user_id":"00000000-0000-4000-8000-)"
                      R"(0000000000a1","push":true,"email":true,"voip":false}]})"),
          "push+email payload receiver");
    check(contains(p, R"("alarm_id":"smoke")"), "payload alarm_id");
    check(!contains(p, "geofence_restriction"), "no geofence in payload");
  }

  // ["push"] only, as the Ring default has it.
  {
    const auto a = parse_one(
        alarm("ring", "[" + receiver(kOwnerPk, kOwner, R"(["push"])") + "]"));
    const std::string p = AlarmNotifierTest::payload(a, at(kMonday, 12, 0));
    check(contains(p, R"("push":true,"email":false,"voip":false)"),
          "push-only payload receiver");
  }

  // ["email"] only.
  {
    const auto a = parse_one(
        alarm("mail", "[" + receiver(kOwnerPk, kOwner, R"(["email"])") + "]"));
    const std::string p = AlarmNotifierTest::payload(a, at(kMonday, 12, 0));
    check(contains(p, R"("push":false,"email":true,"voip":false)"),
          "email-only payload receiver");
  }

  // Several recipients: those with push or email are sent, in order; a
  // recipient with no channel, or only voip, is left out.
  {
    const auto a = parse_one(alarm(
        "multi",
        "[" + receiver("pk-a", "ucore-a", R"(["push"])") + "," +
            receiver("pk-b", "ucore-b", "[]") + "," +
            receiver("pk-c", "ucore-c", R"(["email","push"])") + "," +
            receiver("pk-d", "ucore-d", R"(["voip"])") + "]"));
    check(a.receivers.size() == 4, "four recipients parsed");
    const auto plan = AlarmNotifierTest::plan(a, at(kMonday, 12, 0));
    check(plan.receivers.size() == 2, "two recipients with push/email");
    if (plan.receivers.size() == 2) {
      check(plan.receivers[0].user_id == "ucore-a" &&
                plan.receivers[0].push && !plan.receivers[0].email,
            "first receiver push only");
      check(plan.receivers[1].user_id == "ucore-c" &&
                plan.receivers[1].push && plan.receivers[1].email,
            "second receiver push and email");
    }
    const std::string p = AlarmNotifierTest::payload(a, at(kMonday, 12, 0));
    check(contains(p, R"("receivers":[{"user_id":"ucore-a","push":true,)"
                      R"("email":false,"voip":false},{"user_id":"ucore-c",)"
                      R"("push":true,"email":true,"voip":false}]})"),
          "two receivers in payload");
    check(!contains(p, "ucore-b") && !contains(p, "ucore-d"),
          "channel-less recipients not in payload");
  }

  // No recipients at all.
  {
    const auto a = parse_one(alarm("nobody", "[]"));
    check(a.has_notify_action, "empty receivers still a notify action");
    const auto plan = AlarmNotifierTest::plan(a, at(kMonday, 12, 0));
    check(plan.receivers.empty(), "no recipients -> no receivers");
  }
}

static void test_no_notify_action() {
  // Only a speaker action, whose metadata has an "id" ahead of the
  // alarm's own: the alarm id must still be the alarm's.
  {
    const auto a = parse_one(alarm_with_actions(
        "speaker-only",
        R"([{"type":"PLAY_SPEAKER","metadata":{"id":"speaker-1",)"
        R"("name":"Porch speaker","enable":true,"volume":80},"order":0}])"));
    check(a.id == "speaker-only", "nested action id not taken as alarm id");
    check(a.name == "Person speaker-only", "alarm name, not action name");
    check(a.enabled, "alarm enable read from the alarm");
    check(!a.has_notify_action, "speaker-only alarm has no notify action");
    check(a.receivers.empty(), "speaker-only alarm has no recipients");
    const auto plan = AlarmNotifierTest::plan(a, at(kMonday, 12, 0));
    check(plan.receivers.empty(), "no notify action -> no receivers");
    check(contains(plan.skip_reason, "no notification action"),
          "no notify action -> skip reason");
  }

  // No actions at all.
  {
    const auto a = parse_one(alarm_with_actions("no-actions", "[]"));
    check(!a.id.empty() && !a.has_notify_action,
          "actions [] -> no notify action");
  }

  // A notify action after another action is still found.
  {
    const auto a = parse_one(alarm_with_actions(
        "two-actions",
        R"([{"type":"PLAY_SIREN","metadata":{"duration":5},"order":0},)"
        R"({"type":"SEND_NOTIFICATION","metadata":{"receivers":[)" +
            receiver(kOwnerPk, kOwner, R"(["push"])") +
            R"(]},"order":1}])"));
    check(a.has_notify_action && a.receivers.size() == 1,
          "notify action after a siren action parsed");
  }
}

// ---------------------------------------------------------------------
// Schedules
// ---------------------------------------------------------------------

static void test_alarm_schedule() {
  const auto a = parse_one(alarm(
      "office", "[" + receiver(kOwnerPk, kOwner, R"(["push"])") + "]",
      kWeekdayOfficeHours));
  check(a.schedules.size() == 1, "alarm-level schedule parsed");
  check(!AlarmNotifierTest::plan(a, at(kMonday, 9, 0)).receivers.empty(),
        "alarm schedule: Monday 09:00 notifies");
  check(!AlarmNotifierTest::plan(a, at(kMonday, 8, 0)).receivers.empty(),
        "alarm schedule: start minute is inside");
  check(AlarmNotifierTest::plan(a, at(kMonday, 17, 0)).receivers.empty(),
        "alarm schedule: end minute is outside");
  check(AlarmNotifierTest::plan(a, at(kMonday, 7, 59)).receivers.empty(),
        "alarm schedule: before start is outside");
  const auto weekend = AlarmNotifierTest::plan(a, at(kSaturday, 9, 0));
  check(weekend.receivers.empty(), "alarm schedule: Saturday is outside");
  check(contains(weekend.skip_reason, "schedules"),
        "outside schedule -> skip reason names schedules");

  // Whether the alarm fires at all: only its own schedules count, not
  // the recipients'.
  check(AlarmNotifierTest::alarm_open(a, at(kMonday, 9, 0)),
        "alarm open inside its schedule");
  check(!AlarmNotifierTest::alarm_open(a, at(kMonday, 17, 0)) &&
            !AlarmNotifierTest::alarm_open(a, at(kSaturday, 9, 0)),
        "alarm closed outside its schedule");
  const auto unscheduled = parse_one(alarm(
      "unscheduled", "[" + receiver("pk", "ucore", R"(["push"])", kWeekends) +
                         "]"));
  check(AlarmNotifierTest::alarm_open(unscheduled, at(kMonday, 9, 0)),
        "alarm without a schedule is open whatever its recipients' say");
  // An alarm-level "everyone away" may hold now: open, and the recipients
  // get all_admins.  An alarm-level "I am away" never holds.
  const auto all_away = parse_one(alarm(
      "open-all-away", "[" + receiver("pk", "ucore", R"(["push"])") + "]",
      R"([{"schedule":{"type":"is","unit":"location","values":["allAway"]}}])"));
  check(AlarmNotifierTest::alarm_open(all_away, at(kMonday, 9, 0)),
        "alarm-level allAway is open");
  const auto away = parse_one(alarm(
      "closed-away", "[" + receiver("pk", "ucore", R"(["push"])") + "]",
      R"([{"schedule":{"type":"is","unit":"location","values":["away"]}}])"));
  check(!AlarmNotifierTest::alarm_open(away, at(kMonday, 9, 0)),
        "alarm-level away is closed");
}

static void test_receiver_schedule() {
  const auto a = parse_one(alarm(
      "split",
      "[" + receiver("pk-a", "ucore-weekend", R"(["push"])", kWeekends) + "," +
          receiver("pk-b", "ucore-always", R"(["push"])") + "]"));
  check(a.receivers.size() == 2 && a.receivers[0].schedules.size() == 1,
        "recipient schedule parsed");
  const auto monday = AlarmNotifierTest::plan(a, at(kMonday, 12, 0));
  check(monday.receivers.size() == 1 &&
            monday.receivers[0].user_id == "ucore-always",
        "recipient schedule: Monday only the always recipient");
  const auto sunday = AlarmNotifierTest::plan(a, at(kSunday, 12, 0));
  check(sunday.receivers.size() == 2, "recipient schedule: Sunday both");

  // Every recipient out of schedule -> nothing to send.
  const auto b = parse_one(alarm(
      "weekend-only",
      "[" + receiver("pk-a", "ucore-a", R"(["push"])", kWeekends) + "]"));
  check(AlarmNotifierTest::plan(b, at(kMonday, 12, 0)).receivers.empty(),
        "only recipient out of schedule -> no receivers");

  // The alarm's schedule and the recipient's must both match.
  const auto c = parse_one(alarm(
      "both",
      "[" + receiver("pk-a", "ucore-a", R"(["push"])", kWeekends) + "]",
      kWeekdayOfficeHours));
  check(AlarmNotifierTest::plan(c, at(kMonday, 9, 0)).receivers.empty() &&
            AlarmNotifierTest::plan(c, at(kSunday, 9, 0)).receivers.empty(),
        "alarm and recipient schedules are both required");
}

// Protect's checkScheduleConditions rules, one unit at a time.
static void test_schedule_rules() {
  const auto matches = [](const std::string& schedules, const Clock& clock) {
    const auto a = parse_one(alarm(
        "rule", "[" + receiver("pk", "ucore", R"(["push"])") + "]",
        schedules));
    return !AlarmNotifierTest::plan(a, clock).receivers.empty();
  };

  check(matches("[]", at(kMonday, 3, 0)), "no schedule always matches");
  check(!matches(R"([{"schedule":{"type":"is","unit":"never","values":[]}}])",
                 at(kMonday, 12, 0)),
        "never never matches");
  check(matches(R"([{"schedule":{"type":"not","unit":"day_of_week",)"
                R"("values":[0,6]}}])",
                at(kMonday, 12, 0)) &&
            !matches(R"([{"schedule":{"type":"not","unit":"day_of_week",)"
                     R"("values":[0,6]}}])",
                     at(kSunday, 12, 0)),
        "type not inverts the unit");
  check(matches(R"([{"schedule":{"type":"is","unit":"day_of_month",)"
                R"("values":[1,15]}}])",
                at(kMonday, 12, 0, 15)) &&
            !matches(R"([{"schedule":{"type":"is","unit":"day_of_month",)"
                     R"("values":[1,15]}}])",
                     at(kMonday, 12, 0, 16)),
        "day_of_month matches the date");
  // Several schedules: any one matching is enough.
  const std::string nights =
      R"([{"schedule":{"type":"is","unit":"time_range","values":[[22,0],[23,59]]}},)"
      R"({"schedule":{"type":"is","unit":"time_range","values":[[0,0],[6,0]]}}])";
  check(matches(nights, at(kMonday, 23, 0)) && matches(nights, at(kMonday, 5, 0)) &&
            !matches(nights, at(kMonday, 12, 0)),
        "schedules are OR-ed");
  // "or" is applied after "and": (weekday && 08-17) || saturday.
  const std::string chained =
      R"([{"schedule":{"type":"is","unit":"day_of_week","values":[1,2,3,4,5],)"
      R"("and":{"type":"is","unit":"time_range","values":[[8,0],[17,0]]},)"
      R"("or":{"type":"is","unit":"day_of_week","values":[6]}}}])";
  check(matches(chained, at(kMonday, 9, 0)), "and-chain inside matches");
  check(!matches(chained, at(kMonday, 20, 0)), "and-chain outside fails");
  check(matches(chained, at(kSaturday, 20, 0)), "or-chain rescues");
  check(!matches(chained, at(kSunday, 9, 0)), "neither branch matches");
}

static void test_location_schedules() {
  const std::string away =
      R"([{"schedule":{"type":"is","unit":"location","values":["away"]}}])";
  const std::string all_away =
      R"([{"schedule":{"type":"is","unit":"location","values":["allAway"]}}])";
  const std::string at_home =
      R"([{"schedule":{"type":"not","unit":"location","values":["away"]}}])";

  // "When I am away" -> current_admin.
  {
    const auto a = parse_one(alarm(
        "away", "[" + receiver("pk", "ucore", R"(["push"])", away) + "]"));
    const auto plan = AlarmNotifierTest::plan(a, at(kMonday, 12, 0));
    check(plan.receivers.size() == 1 &&
              plan.receivers[0].geofence == Geofence::kCurrentAdmin,
          "location away -> current_admin");
    check(!plan.location_approximated, "location away is exact");
    check(contains(AlarmNotifierTest::payload(a, at(kMonday, 12, 0)),
                   R"("voip":false,"geofence_restriction":"current_admin"})"),
          "current_admin in payload");
  }

  // "When everyone is away" -> all_admins.
  {
    const auto a = parse_one(alarm(
        "all-away",
        "[" + receiver("pk", "ucore", R"(["push"])", all_away) + "]"));
    const auto plan = AlarmNotifierTest::plan(a, at(kMonday, 12, 0));
    check(plan.receivers.size() == 1 &&
              plan.receivers[0].geofence == Geofence::kAllAdmins,
          "location allAway -> all_admins");
    check(contains(AlarmNotifierTest::payload(a, at(kMonday, 12, 0)),
                   R"("geofence_restriction":"all_admins")"),
          "all_admins in payload");
  }

  // Office hours, or whenever I am away: inside office hours there is no
  // restriction; outside them it applies.
  {
    const std::string office_or_away =
        R"([{"schedule":{"type":"is","unit":"day_of_week","values":[1,2,3,4,5],)"
        R"("and":{"type":"is","unit":"time_range","values":[[8,0],[17,0]]}}},)"
        R"({"schedule":{"type":"is","unit":"location","values":["away"]}}])";
    const auto a = parse_one(alarm(
        "office-or-away",
        "[" + receiver("pk", "ucore", R"(["push"])", office_or_away) + "]"));
    const auto in = AlarmNotifierTest::plan(a, at(kMonday, 9, 0));
    check(in.receivers.size() == 1 &&
              in.receivers[0].geofence == Geofence::kNone,
          "office-or-away inside office hours: unrestricted");
    const auto out = AlarmNotifierTest::plan(a, at(kMonday, 20, 0));
    check(out.receivers.size() == 1 &&
              out.receivers[0].geofence == Geofence::kCurrentAdmin,
          "office-or-away outside office hours: current_admin");
  }

  // "Only while at home" cannot be expressed: notified regardless, flagged.
  {
    const auto a = parse_one(alarm(
        "home", "[" + receiver("pk", "ucore", R"(["push"])", at_home) + "]"));
    const auto plan = AlarmNotifierTest::plan(a, at(kMonday, 12, 0));
    check(plan.receivers.size() == 1 &&
              plan.receivers[0].geofence == Geofence::kNone,
          "location not away -> notified without restriction");
    check(plan.location_approximated, "location not away is flagged");
  }

  // An alarm-level "everyone away" schedule restricts every recipient.
  {
    const auto a = parse_one(alarm(
        "armed-away",
        "[" + receiver("pk-a", "ucore-a", R"(["push"])") + "," +
            receiver("pk-b", "ucore-b", R"(["email"])") + "]",
        all_away));
    const auto plan = AlarmNotifierTest::plan(a, at(kMonday, 12, 0));
    check(plan.receivers.size() == 2 &&
              plan.receivers[0].geofence == Geofence::kAllAdmins &&
              plan.receivers[1].geofence == Geofence::kAllAdmins,
          "alarm-level allAway -> all_admins on every recipient");
  }

  // Protect tests the alarm's own schedules with the user taken as at
  // home, so an alarm-level "I am away" never matches.
  {
    const auto a = parse_one(alarm(
        "alarm-away", "[" + receiver("pk", "ucore", R"(["push"])") + "]",
        away));
    check(AlarmNotifierTest::plan(a, at(kMonday, 12, 0)).receivers.empty(),
          "alarm-level away never matches");
  }
}

// ---------------------------------------------------------------------
// Recipients without a UniFi OS user id
// ---------------------------------------------------------------------

static void test_unresolved_users() {
  const auto a = parse_one(
      alarm("no-ucore", "[" + receiver(kOwnerPk, "", R"(["push"])") + "]"));
  check(a.receivers.size() == 1 && a.receivers[0].ucore_user.empty() &&
            a.receivers[0].user == kOwnerPk,
        "recipient without ucoreUser parsed");
  const auto plan = AlarmNotifierTest::plan(a, at(kMonday, 12, 0));
  check(plan.receivers.empty() && plan.unresolved_user,
        "recipient without UniFi OS user id is left out and flagged");
  check(contains(plan.skip_reason, "UniFi OS user"),
        "skip reason names the missing user");

  // GET /api/users maps Protect user ids to UniFi OS user ids.
  const auto ids = AlarmNotifierTest::user_ids(
      R"([{"permissions":["nvr:read:*"],"lastLoginIp":null,"isOwner":false,)"
      R"("location":{"isAway":true},"id":"0000000000000000000000b2",)"
      R"("ucoreUserId":"00000000-0000-4000-8000-0000000000b2"},)"
      R"({"isOwner":true,"id":"0000000000000000000000a1","settings":{"id":"x"},)"
      R"("ucoreUserId":"00000000-0000-4000-8000-0000000000a1"},)"
      R"({"id":"no-ucore"}])");
  check(ids.size() == 2, "two users with UniFi OS ids");
  check(ids.count(kOwnerPk) == 1 && ids.at(kOwnerPk) == kOwner,
        "owner mapped to its UniFi OS id");
  check(ids.count("0000000000000000000000b2") == 1 &&
            ids.at("0000000000000000000000b2") ==
                "00000000-0000-4000-8000-0000000000b2",
        "second user mapped");
  check(AlarmNotifierTest::user_ids("not json").empty(), "garbage users");
}

static void test_local_clock() {
  setenv("TZ", "UTC", 1);
  tzset();
  const Clock epoch = AlarmNotifierTest::local_clock(0);
  check(epoch.week_day == 4 && epoch.month_day == 1 &&
            epoch.minute_of_day == 0,
        "1970-01-01 00:00 UTC is Thursday the 1st, minute 0");
  // 2023-11-14 22:13:20 UTC, a Tuesday.
  const Clock t = AlarmNotifierTest::local_clock(1700000000);
  check(t.week_day == 2 && t.month_day == 14 &&
            t.minute_of_day == 22 * 60 + 13,
        "1700000000 is Tuesday the 14th, 22:13");
}

// ---------------------------------------------------------------------
// notify() against the fake Protect API (UosEmulator)
// ---------------------------------------------------------------------

class CaptureSink : public absl::LogSink {
 public:
  void Send(const absl::LogEntry& entry) override {
    std::lock_guard<std::mutex> lk(mu_);
    lines_.emplace_back(entry.text_message());
  }
  size_t count(const std::string& needle) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const std::string& l : lines_) {
      if (l.find(needle) != std::string::npos) ++n;
    }
    return n;
  }

 private:
  std::mutex mu_;
  std::vector<std::string> lines_;
};

static void test_notify_skips_but_records() {
  const std::string never =
      R"([{"schedule":{"type":"is","unit":"never","values":[]}}])";
  // "never" has a recipient out of schedule: the alarm fires, nobody is
  // notified.  "closed" is out of its own schedule: it does not fire.
  const std::string automations =
      "[" +
      alarm("push-on", "[" + receiver(kOwnerPk, kOwner, R"(["push"])") + "]") +
      "," + alarm("push-off", "[" + receiver(kOwnerPk, kOwner, "[]") + "]") +
      "," +
      alarm_with_actions(
          "no-notify",
          R"([{"type":"PLAY_SPEAKER","metadata":{"volume":80},"order":0}])") +
      "," +
      alarm("never",
            "[" + receiver(kOwnerPk, kOwner, R"(["push"])", never) + "]") +
      "," +
      alarm("closed", "[" + receiver(kOwnerPk, kOwner, R"(["push"])") + "]",
            never) +
      "]";
  UosEmulator protect;
  protect.set_alarms_json(automations);
  protect.start();

  CaptureSink sink;
  absl::AddLogSink(&sink);
  {
    onvif::ProtectUserIdProvider provider("owner-user-id", "");
    // An unreachable database: each history row is attempted and logged
    // as a failed connect, which is what this test counts.
    onvif::AlarmNotifier notifier(
        protect.base_url(), &provider,
        "host=/nonexistent-onvif-recorder-test port=1 connect_timeout=1",
        kNoUos);
    notifier.set_mode_probe(local_console);
    notifier.refresh_alarms();
    notifier.notify("person", "AABBCCDDEEFF", "evt-1", 1700000000000ULL);

    check(protect.register_bodies().size() == 5,
          "every enabled alarm registered");
    const auto notifies = protect.notify_bodies();
    check(notifies.size() == 1, "one notify call, for the push-on alarm");
    if (notifies.size() == 1) {
      const std::string& body = notifies[0];
      check(contains(body, R"("alarm_id":"push-on")"), "notify is push-on");
      check(contains(body, R"("receivers":[{"user_id":"00000000-0000-4000-)"
                           R"(8000-0000000000a1","push":true,"email":false,)"
                           R"("voip":false}]})"),
            "notify names the alarm's recipient");
      check(!contains(body, "owner-user-id"),
            "notify does not use the X-UserId user");
    }
    check(protect.posted_events().empty(), "legacy run path not used");
    check(sink.count("history DB connect failed") == 4,
          "history recorded for the four alarms that fired, sent or not");
    const auto fired = AlarmNotifierTest::last_fired(&notifier);
    check(fired.size() == 4 && fired.count("push-off") == 1 &&
              fired.count("no-notify") == 1 && fired.count("never") == 1 &&
              fired.at("push-off") == 1700000000000ULL,
          "cooldown updated for skipped alarms too");
    check(fired.count("closed") == 0,
          "an alarm outside its own schedule starts no cooldown");
    check(sink.count("automation closed: outside the alarm's schedule") == 1,
          "an alarm outside its own schedule is logged as not fired");
  }
  absl::RemoveLogSink(&sink);
}

// A Protect whose automation manager routes are absent: only the alarm
// list and the legacy /api/automations/<id>/run path answer.
class NoAlarmManagerProtect : public OnvifCameraEmulator {
 public:
  explicit NoAlarmManagerProtect(std::string alarms)
      : OnvifCameraEmulator("no-alarm-manager"), alarms_(std::move(alarms)) {}
  ~NoAlarmManagerProtect() override { stop(); }

  std::vector<std::string> paths() {
    std::lock_guard<std::mutex> lk(mu_);
    return paths_;
  }

 protected:
  std::pair<int, std::string> handle(const std::string& path,
                                     const std::string& /*soap_action*/,
                                     const std::string& /*body*/) override {
    std::lock_guard<std::mutex> lk(mu_);
    paths_.push_back(path);
    if (path == "/api/automations") return {200, alarms_};
    if (path.rfind("/api/automations/", 0) == 0) return {200, "{}"};
    return {404, ""};
  }

 private:
  std::mutex mu_;
  std::string alarms_;
  std::vector<std::string> paths_;
};

// The legacy path runs the whole alarm (sirens, lights, ...), so it is
// used even for an alarm with nobody to notify once registration shows
// the automation manager routes are absent.
static void test_legacy_path_without_alarm_manager() {
  NoAlarmManagerProtect protect(
      "[" +
      alarm_with_actions(
          "siren-only",
          R"([{"type":"PLAY_SIREN","metadata":{"duration":5},"order":0}])") +
      "]");
  protect.start();
  onvif::ProtectUserIdProvider provider("owner-user-id", "");
  {
    onvif::AlarmNotifier notifier(
        "http://127.0.0.1:" + std::to_string(protect.port()), &provider, "",
        kNoUos);
    notifier.set_mode_probe(local_console);
    notifier.refresh_alarms();
    notifier.notify("person", "AABBCCDDEEFF", "evt-2", 1700000000000ULL);
  }
  size_t runs = 0;
  size_t notifies = 0;
  for (const std::string& p : protect.paths()) {
    if (p == "/api/automations/siren-only/run") ++runs;
    if (contains(p, "/actions/notify")) ++notifies;
  }
  check(runs == 1, "no alarm manager: legacy path runs a siren-only alarm");
  check(notifies == 0, "no alarm manager: no UOS notify call");
}

// A fake Protect that also answers GET /api/users.
class UsersProtect : public UosEmulator {
 public:
  explicit UsersProtect(std::string users) : users_(std::move(users)) {}
  ~UsersProtect() override { stop(); }

  int users_requests() {
    std::lock_guard<std::mutex> lk(users_mu_);
    return users_requests_;
  }

 protected:
  std::pair<int, std::string> handle(const std::string& path,
                                     const std::string& soap_action,
                                     const std::string& body) override {
    if (path == "/api/users") {
      std::lock_guard<std::mutex> lk(users_mu_);
      ++users_requests_;
      return {200, users_};
    }
    return UosEmulator::handle(path, soap_action, body);
  }

 private:
  std::mutex users_mu_;
  std::string users_;
  int users_requests_ = 0;
};

// A recipient saved without its UniFi OS user id is notified under the id
// GET /api/users gives its Protect user; the user list is fetched only
// when such a recipient exists.
static void test_recipient_resolved_through_users() {
  const std::string users =
      R"([{"id":"0000000000000000000000b2",)"
      R"("ucoreUserId":"00000000-0000-4000-8000-0000000000b2"},)"
      R"({"id":"0000000000000000000000a1","isOwner":true,)"
      R"("ucoreUserId":"00000000-0000-4000-8000-0000000000a1"}])";
  onvif::ProtectUserIdProvider provider("owner-user-id", "");
  {
    UsersProtect protect(users);
    protect.set_alarms_json(
        "[" +
        alarm("no-ucore", "[" + receiver(kOwnerPk, "", R"(["push"])") + "]") +
        "]");
    protect.start();
    {
      onvif::AlarmNotifier notifier(protect.base_url(), &provider, "", kNoUos);
      notifier.set_mode_probe(local_console);
      notifier.refresh_alarms();
      notifier.notify("person", "AABBCCDDEEFF", "evt-3", 1700000000000ULL);
    }
    check(protect.users_requests() == 1,
          "recipient without ucoreUser: user list fetched once");
    const auto bodies = protect.notify_bodies();
    check(bodies.size() == 1 &&
              contains(bodies[0], R"("receivers":[{"user_id":")" +
                                      std::string(kOwner) + R"(","push":true)"),
          "recipient without ucoreUser notified under its UniFi OS id");
  }
  {
    UsersProtect protect(users);
    protect.set_alarms_json(
        "[" +
        alarm("known", "[" + receiver(kOwnerPk, kOwner, R"(["push"])") + "]") +
        "]");
    protect.start();
    {
      onvif::AlarmNotifier notifier(protect.base_url(), &provider, "", kNoUos);
      notifier.set_mode_probe(local_console);
      notifier.refresh_alarms();
      notifier.notify("person", "AABBCCDDEEFF", "evt-4", 1700000000000ULL);
    }
    check(protect.users_requests() == 0,
          "every recipient has ucoreUser: user list not fetched");
    check(protect.notify_bodies().size() == 1,
          "every recipient has ucoreUser: notified");
  }
}

// ---------------------------------------------------------------------
// Local Alarm Manager: include / exclude sources
// ---------------------------------------------------------------------

// "exclude" sources leave those cameras out and cover every other one;
// an alarm can mix both, as Protect's filterEventsBySources allows.
static void test_local_exclude_sources() {
  const auto with_sources = [](const std::string& id,
                               const std::string& sources) {
    return R"({"id":")" + id + R"(","name":")" + id +
           R"(","enable":true,"conditions":[{"condition":{"type":"is",)"
           R"("source":"person"}}],"sources":)" + sources + "}";
  };
  const auto exclude = parse_one(with_sources(
      "exclude", R"([{"device":"AABBCCDDEEFF","type":"exclude"}])"));
  check(exclude.source_devices.empty() &&
            exclude.excluded_devices.count("AABBCCDDEEFF") == 1,
        "exclude source parsed as excluded, not included");
  check(!AlarmNotifierTest::covers(exclude, "AABBCCDDEEFF"),
        "exclude: the excluded camera is not covered");
  check(AlarmNotifierTest::covers(exclude, "024D4F434B01"),
        "exclude: every other camera is covered");

  const auto include = parse_one(with_sources(
      "include", R"([{"device":"AABBCCDDEEFF","type":"include"}])"));
  check(AlarmNotifierTest::covers(include, "AABBCCDDEEFF") &&
            !AlarmNotifierTest::covers(include, "024D4F434B01"),
        "include: only the included camera is covered");

  const auto mixed = parse_one(with_sources(
      "mixed", R"([{"device":"AABBCCDDEEFF","type":"include"},)"
               R"({"device":"024D4F434B01","type":"include"},)"
               R"({"device":"024D4F434B01","type":"exclude"}])"));
  check(AlarmNotifierTest::covers(mixed, "AABBCCDDEEFF") &&
            !AlarmNotifierTest::covers(mixed, "024D4F434B01"),
        "mixed: an excluded camera stays out even when included");

  const auto unknown = parse_one(with_sources(
      "unknown", R"([{"device":"AABBCCDDEEFF","type":"other"}])"));
  check(unknown.source_devices.empty() && unknown.excluded_devices.empty(),
        "a source of an unknown type is ignored");

  // End to end on a Local console: the excluded camera fires nothing.
  UosEmulator protect;
  protect.set_alarms_json(
      R"([{"id":"not-gate","name":"Not the gate","enable":true,)"
      R"("conditions":[{"condition":{"type":"is","source":"person"}}],)"
      R"("sources":[{"device":"AABBCCDDEEFF","type":"exclude"}],)"
      R"("actions":[{"type":"SEND_NOTIFICATION","metadata":{"receivers":[)" +
      receiver(kOwnerPk, kOwner, R"(["push"])") + R"(]},"order":-1}]}])");
  protect.start();
  onvif::ProtectUserIdProvider provider("owner-user-id", "");
  {
    onvif::AlarmNotifier notifier(protect.base_url(), &provider, "", kNoUos);
    notifier.set_mode_probe(local_console);
    notifier.refresh_alarms();
    notifier.notify("person", "AABBCCDDEEFF", "evt-x1", 1700000000000ULL);
    check(protect.notify_bodies().empty(),
          "Local: excluded camera does not fire the alarm");
    notifier.notify("person", "024D4F434B01", "evt-x2", 1700000100000ULL);
    const auto bodies = protect.notify_bodies();
    check(bodies.size() == 1 && contains(bodies[0], R"("alarm_id":"not-gate")"),
          "Local: a camera not excluded fires the alarm");
  }
}

// ---------------------------------------------------------------------
// Global Alarm Manager
// ---------------------------------------------------------------------

constexpr char kMockMac[] = "024D4F434B01";
constexpr char kMockCameraId[] = "0000000000000000000000d4";

// One alarm as GET /api/v1/alarms returns it.  @p triggers is the
// triggers_data value; @p scope the scope value ("null" for none).
static std::string uos_alarm(const std::string& id, const std::string& title,
                             const std::string& triggers,
                             const std::string& scope,
                             const std::string& extra = "") {
  return R"({"id":")" + id + R"(","title":")" + title +
         R"(","triggers_data":)" + triggers +
         R"(,"actions_data":[[{"id":"protect:notify","target_ids":null,)"
         R"("data":{"is_critical":false,"receivers":[")" + std::string(kOwner) +
         R"("]}}]],"scope":)" + scope + extra +
         R"(,"created_by":")" + std::string(kOwner) +
         R"(","created_at":"2026-09-14T15:52:20Z"})";
}

// One trigger as uos-agent stores it.
static std::string trig(const std::string& id, const std::string& data = "null",
                        const std::string& precondition = "null") {
  const bool object = id.rfind("protect:objects.", 0) == 0;
  return R"({"id":")" + id + R"(","precondition_config":)" + precondition +
         R"(,"data":)" + data + R"(,"is_matched_externally":)" +
         (object ? "true" : "false") + "}";
}

static std::string count_data(const std::string& n,
                              const std::string& selector) {
  return R"({"count":{"items":{"primaryInput":")" + n +
         R"(","secondarySelector":")" + selector + R"("}}})";
}

static const char kZonesAll[] =
    R"({"mode":"include","data":{"scope_all_smart_cameras_with_zones":)"
    R"(["ALL_ITEMS"]}})";
static const char kZonesMock[] =
    R"({"mode":"include","data":{"scope_all_smart_cameras_with_zones":)"
    R"(["024D4F434B01"]}})";

// A synthetic alarm list shaped like a live Protect 7.3 / UniFi OS 6.0
// one: system alarms with no scope, the default object alarms, a paused
// alarm, schedules and cooldowns, and the trigger shapes Protect writes
// for "count within" and for a trigger AND-ed with another.
static std::string synthetic_alarm_list() {
  const std::string person = "[[" + trig("protect:objects.person") + "]]";
  return "[" +
         uos_alarm("a-admin", "Admin Access",
                   "[[" + trig("protect:system.adminAccess") + "]]", "null") +
         "," +
         uos_alarm("a-person", "Person", person, kZonesAll) + "," +
         uos_alarm("a-vehicle", "Vehicle",
                   "[[" + trig("protect:objects.vehicle") + "]]", kZonesAll,
                   R"(,"suppression":{"type":"cooldown","duration":120})") +
         "," +
         uos_alarm(
             "a-package", "Package",
             "[[" + trig("protect:objects.package") + "]]",
             R"({"mode":"include","data":{)"
             R"("scope_all_smart_cameras_with_package":["ALL_ITEMS"]}})",
             R"(,"restriction":{"type":"schedule","days":{"sun":{"type":)"
             R"("intervals","entries":[{"start":{"hour":3,"minute":0},)"
             R"("end":{"hour":3,"minute":1}}]}}})") +
         "," +
         uos_alarm("a-paused", "Paused person", person, kZonesMock,
                   R"(,"is_paused":true)") +
         "," +
         uos_alarm("a-mock", "Mock camera person", person, kZonesMock) + "," +
         uos_alarm("a-exclude", "Not the mock camera", person,
                   R"({"mode":"exclude","data":{)"
                   R"("scope_all_smart_cameras_with_zones":)"
                   R"(["024D4F434B01"]}})") +
         "," +
         uos_alarm("a-count3", "Three people within an hour",
                   "[[" + trig("protect:objects.person",
                               count_data("3", "1 hour")) +
                       "]]",
                   kZonesAll) +
         "," +
         uos_alarm("a-count1", "One person within a day",
                   "[[" + trig("protect:objects.person",
                               count_data("1", "1 day")) +
                       "]]",
                   kZonesAll) +
         "," +
         uos_alarm(
             "a-custom", "Two animals in 5 minutes",
             "[[" +
                 trig("protect:objects.animal",
                      R"({"count":{"items":{"primaryInput":"2",)"
                      R"("secondarySelector":"custom","customDuration":"5",)"
                      R"("customTimeframe":"minutes"}}})") +
                 "]]",
             kZonesAll) +
         "," +
         uos_alarm("a-and", "Person after a vehicle",
                   "[[" + trig("protect:objects.person") + "],[" +
                       trig("protect:objects.vehicle", "null",
                            R"({"interval":"60"})") +
                       "]]",
                   kZonesAll) +
         "," +
         uos_alarm("a-or", "Person or vehicle",
                   "[[" + trig("protect:objects.person") + "," +
                       trig("protect:objects.vehicle") + "]]",
                   kZonesAll) +
         "," +
         uos_alarm("a-motion", "Motion",
                   "[[" + trig("protect:activity.motionCamera") + "]]",
                   R"({"mode":"include","data":{"scope_all_motion_cameras":)"
                   R"(["ALL_ITEMS"]}})") +
         "," +
         uos_alarm("a-noscope", "Person anywhere", person, "null") + "," +
         uos_alarm("a-twokeys", "Person, two scope keys", person,
                   R"({"mode":"include","data":{)"
                   R"("scope_all_smart_cameras_with_zones":["024D4F434B01"],)"
                   R"("scope_all_smart_cameras":["ALL_ITEMS"]}})") +
         "]";
}

static const AlarmNotifierTest::UosAlarm* find_alarm(
    const std::vector<AlarmNotifierTest::UosAlarm>& alarms,
    const std::string& id) {
  for (const auto& a : alarms) {
    if (a.id == id) return &a;
  }
  return nullptr;
}

static void test_mode_decision() {
  using Mode = AlarmNotifierTest::Mode;
  const auto decide = AlarmNotifierTest::decide;
  check(decide(true, Mode::kUnknown, false) == Mode::kGlobal,
        "flag true -> Global");
  check(decide(false, Mode::kGlobal, true) == Mode::kLocal,
        "flag false -> Local, whatever UniFi OS lists");
  check(decide(std::nullopt, Mode::kGlobal, false) == Mode::kGlobal,
        "flag unreadable keeps Global");
  check(decide(std::nullopt, Mode::kLocal, true) == Mode::kLocal,
        "flag unreadable keeps Local");
  check(decide(std::nullopt, Mode::kUnknown, true) == Mode::kGlobal,
        "first refresh, flag unreadable, object alarms in UniFi OS -> Global");
  check(decide(std::nullopt, Mode::kUnknown, false) == Mode::kLocal,
        "first refresh, flag unreadable, no object alarms -> Local");
  check(decide(std::nullopt, Mode::kUnknown, std::nullopt) == Mode::kUnknown,
        "first refresh, flag and UniFi OS list unreadable -> undecided");
  check(decide(std::nullopt, Mode::kGlobal, std::nullopt) == Mode::kGlobal &&
            decide(std::nullopt, Mode::kLocal, std::nullopt) == Mode::kLocal,
        "nothing readable later: previous decision kept");
  check(decide(true, Mode::kUnknown, std::nullopt) == Mode::kGlobal,
        "flag readable, list unreadable -> flag decides");
}

static void test_uos_alarm_parsing() {
  std::vector<AlarmNotifierTest::UosAlarm> alarms;
  check(AlarmNotifierTest::parse_uos(synthetic_alarm_list(), &alarms),
        "synthetic alarm list parses");
  check(alarms.size() == 15, "every alarm with an id is kept");
  const auto* admin = find_alarm(alarms, "a-admin");
  check(admin && !admin->has_object_trigger && admin->fire_types.empty() &&
            admin->scope_keys.empty(),
        "system alarm: no object trigger, no scope");
  const auto* person = find_alarm(alarms, "a-person");
  check(person && person->title == "Person" && !person->paused &&
            person->fire_types == std::set<std::string>{"person"} &&
            person->scope_keys ==
                std::vector<std::string>{"scope_all_smart_cameras_with_zones"},
        "Person alarm: fires on person, zones scope key");
  const auto* package = find_alarm(alarms, "a-package");
  check(package && package->fire_types.count("package") == 1 &&
            package->scope_keys == std::vector<std::string>{
                                       "scope_all_smart_cameras_with_package"},
        "Package alarm: package scope key; its schedule is not evaluated");
  const auto* paused = find_alarm(alarms, "a-paused");
  check(paused && paused->paused, "is_paused parsed");
  const auto* count3 = find_alarm(alarms, "a-count3");
  check(count3 && count3->fire_types.empty() &&
            count3->skipped.count("person") == 1 &&
            contains(count3->skipped.at("person"), "3 detections within 1 hour"),
        "count of 3 within an hour: not sent, reason names the count");
  const auto* count1 = find_alarm(alarms, "a-count1");
  check(count1 && count1->fire_types.count("person") == 1 &&
            count1->skipped.empty(),
        "count of 1 fires on one detection");
  const auto* custom = find_alarm(alarms, "a-custom");
  check(custom && custom->fire_types.empty() &&
            custom->skipped.count("animal") == 1 &&
            contains(custom->skipped.at("animal"),
                     "2 detections within 5 minutes"),
        "custom count window: not sent, reason names the window");
  const auto* and_alarm = find_alarm(alarms, "a-and");
  check(and_alarm && and_alarm->fire_types.empty() &&
            and_alarm->skipped.count("person") == 1 &&
            contains(and_alarm->skipped.at("person"),
                     "protect:objects.vehicle"),
        "trigger AND-ed with a precondition: not sent, names the other");
  const auto* or_alarm = find_alarm(alarms, "a-or");
  check(or_alarm &&
            or_alarm->fire_types ==
                std::set<std::string>({"person", "vehicle"}),
        "two main triggers in one group: either fires it");
  const auto* motion = find_alarm(alarms, "a-motion");
  check(motion && !motion->has_object_trigger && motion->fire_types.empty(),
        "motion alarm has no object trigger");
  const auto* two = find_alarm(alarms, "a-twokeys");
  check(two && two->scope_keys.size() == 2, "both scope keys kept");

  check(person && !person->zone_scoped && two && !two->zone_scoped,
        "MAC and ALL_ITEMS scopes are not zone scopes");

  // A count without a window is not applied by Protect: fires on one.
  // site_id inside scope.data is not a scope key.  A "<MAC>:<key>=<id>"
  // value limits the alarm to a zone or button.
  std::vector<AlarmNotifierTest::UosAlarm> extra;
  check(AlarmNotifierTest::parse_uos(
            "[" +
                uos_alarm("a-nowindow", "Five people, no window",
                          "[[" + trig("protect:objects.person",
                                      R"({"count":{"items":)"
                                      R"({"primaryInput":"5"}}})") +
                              "]]",
                          kZonesAll) +
                "," +
                uos_alarm("a-site", "Site in scope",
                          "[[" + trig("protect:objects.person") + "]]",
                          R"({"mode":"include","data":{"site_id":"protect",)"
                          R"("scope_all_smart_cameras_with_zones":)"
                          R"(["024D4F434B01"]}})") +
                "," +
                uos_alarm("a-zone", "Driveway zone",
                          "[[" + trig("protect:objects.vehicle") + "]]",
                          R"({"mode":"include","data":{)"
                          R"("scope_all_smart_cameras_with_zones":)"
                          R"(["024D4F434B01:zone=1"]}})") +
                "]",
            &extra),
        "extra alarm shapes parse");
  const auto* nowindow = find_alarm(extra, "a-nowindow");
  check(nowindow && nowindow->fire_types.count("person") == 1 &&
            nowindow->skipped.empty(),
        "count without a window fires on one detection");
  const auto* site = find_alarm(extra, "a-site");
  check(site && site->scope_keys == std::vector<std::string>{
                                        "scope_all_smart_cameras_with_zones"},
        "site_id in scope.data is not a scope key");
  if (site) {
    const std::string body = AlarmNotifierTest::uos_events(
        {*site}, "person", kMockMac, kMockCameraId, "e", 1);
    check(body.find("site_id") == body.rfind("site_id"),
          "site_id appears once in the event scope");
  }
  const auto* zone = find_alarm(extra, "a-zone");
  check(zone && zone->zone_scoped && zone->fire_types.count("vehicle") == 1,
        "zone-scoped alarm detected and still sent");

  std::vector<AlarmNotifierTest::UosAlarm> none;
  check(!AlarmNotifierTest::parse_uos(R"({"error":"x"})", &none),
        "an object is not an alarm list");
  check(!AlarmNotifierTest::parse_uos("[{", &none), "truncated list rejected");
  check(AlarmNotifierTest::parse_uos("[]", &none) && none.empty(),
        "empty list parses");
  check(AlarmNotifierTest::normalize_mac("02:4d:4f:43:4b:01") == kMockMac &&
            AlarmNotifierTest::normalize_mac("02-4D-4F-43-4B-01") == kMockMac,
        "MAC normalised to upper-case hex without separators");
}

static void test_uos_payload_golden() {
  AlarmNotifierTest::UosAlarm a;
  a.id = "01a1816a-0000-7000-8000-000000000001";
  a.title = "Mock camera person";
  a.scope_keys = {"scope_all_smart_cameras_with_zones"};
  check(AlarmNotifierTest::uos_events({a}, "person", kMockMac, kMockCameraId,
                                      "evt-1", 1793173174455ULL) ==
            R"([{"id":"person","alarm_id":"01a1816a-0000-7000-8000-)"
            R"(000000000001","scope":{"scope_all_smart_cameras_with_zones":)"
            R"("024D4F434B01","site_id":"protect"},"metadata":{"event":{)"
            R"("key":"person","device":"024D4F434B01","eventId":"evt-1",)"
            R"("timestamp":1793173174455,"sourceEvent":{"type":)"
            R"("smartDetectZone","camera":"0000000000000000000000d4",)"
            R"("start":1793173174455,"smartDetectTypes":["person"]}}}}])",
        "golden POST /api/v1/alarms/events body");

  // No camera id: sourceEvent is left out; Protect looks up eventId.
  const std::string no_cam = AlarmNotifierTest::uos_events(
      {a}, "person", kMockMac, "", "evt-2", 1793173174455ULL);
  check(no_cam ==
            R"([{"id":"person","alarm_id":"01a1816a-0000-7000-8000-)"
            R"(000000000001","scope":{"scope_all_smart_cameras_with_zones":)"
            R"("024D4F434B01","site_id":"protect"},"metadata":{"event":{)"
            R"("key":"person","device":"024D4F434B01","eventId":"evt-2",)"
            R"("timestamp":1793173174455}}}])",
        "unknown camera id: no sourceEvent");

  // No scope: the detection type's usual key.  Package uses its own.
  AlarmNotifierTest::UosAlarm bare;
  bare.id = "bare";
  check(contains(AlarmNotifierTest::uos_events({bare}, "vehicle", kMockMac, "",
                                               "e", 1),
                 R"("scope":{"scope_all_smart_cameras_with_zones":)"
                 R"("024D4F434B01","site_id":"protect"})"),
        "no scope data: zones key");
  check(contains(AlarmNotifierTest::uos_events({bare}, "package", kMockMac, "",
                                               "e", 1),
                 R"("scope":{"scope_all_smart_cameras_with_package":)"
                 R"("024D4F434B01","site_id":"protect"})"),
        "no scope data, package: package key");

  // Only the alarm's own scope keys, all in one scope object; the usual
  // key is not added when the alarm has keys of its own.
  AlarmNotifierTest::UosAlarm odd;
  odd.id = "odd";
  odd.scope_keys = {"scope_all_smart_cameras"};
  check(contains(AlarmNotifierTest::uos_events({odd}, "person", kMockMac, "",
                                               "e", 1),
                 R"("scope":{"scope_all_smart_cameras":"024D4F434B01",)"
                 R"("site_id":"protect"})"),
        "alarm's own scope key only");
  odd.scope_keys = {"scope_all_smart_cameras",
                    "scope_all_smart_cameras_with_zones"};
  check(contains(AlarmNotifierTest::uos_events({odd}, "person", kMockMac, "",
                                               "e", 1),
                 R"("scope":{"scope_all_smart_cameras":"024D4F434B01",)"
                 R"("scope_all_smart_cameras_with_zones":"024D4F434B01",)"
                 R"("site_id":"protect"})"),
        "two scope keys: one scope object holding both");

  // Two alarms: two events, one per alarm id.
  const std::string two = AlarmNotifierTest::uos_events(
      {a, bare}, "person", kMockMac, kMockCameraId, "evt-3", 5);
  check(two.front() == '[' && two.back() == ']' &&
            contains(two, R"("alarm_id":"01a1816a-)") &&
            contains(two, R"(}}}},{"id":"person","alarm_id":"bare")"),
        "one event per alarm in one array");
}

// A fake uos-agent: GET /api/v1/alarms and POST /api/v1/alarms/events,
// both requiring X-Source: protect, as the real one does.
class FakeUosAgent : public OnvifCameraEmulator {
 public:
  FakeUosAgent() : OnvifCameraEmulator("uos-agent") {}
  ~FakeUosAgent() override { stop(); }

  std::string base_url() const {
    return "http://127.0.0.1:" + std::to_string(port());
  }
  void set_alarms(const std::string& json, int status = 200) {
    std::lock_guard<std::mutex> lk(mu_);
    alarms_ = json;
    alarms_status_ = status;
  }
  void set_events_answer(int status, const std::string& body) {
    std::lock_guard<std::mutex> lk(mu_);
    events_status_ = status;
    events_body_ = body;
  }
  // Answer each events POST only after @p ms (recorded on arrival).
  void set_events_delay_ms(int ms) {
    std::lock_guard<std::mutex> lk(mu_);
    events_delay_ms_ = ms;
  }
  std::vector<std::string> events() {
    std::lock_guard<std::mutex> lk(mu_);
    return events_;
  }
  int list_requests() {
    std::lock_guard<std::mutex> lk(mu_);
    return list_requests_;
  }
  int bad_requests() {
    std::lock_guard<std::mutex> lk(mu_);
    return bad_requests_;
  }

 protected:
  std::pair<int, std::string> handle(const std::string& path,
                                     const std::string& /*soap_action*/,
                                     const std::string& body) override {
    std::unique_lock<std::mutex> lk(mu_);
    if (request_header("X-Source") != "protect") {
      ++bad_requests_;
      return {400, R"({"error":"missing X-Source"})"};
    }
    if (path == "/api/v1/alarms") {
      ++list_requests_;
      return {alarms_status_, alarms_};
    }
    if (path == "/api/v1/alarms/events") {
      if (request_header("Content-Type") != "application/json") {
        ++bad_requests_;
        return {415, ""};
      }
      events_.push_back(body);
      if (events_delay_ms_ > 0) {
        const int delay = events_delay_ms_;
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        lk.lock();
      }
      return {events_status_, events_body_};
    }
    return {404, ""};
  }

 private:
  std::mutex mu_;
  std::string alarms_ = "[]";
  int alarms_status_ = 200;
  int events_status_ = 202;
  int events_delay_ms_ = 0;
  std::string events_body_;
  std::vector<std::string> events_;
  int list_requests_ = 0;
  int bad_requests_ = 0;
};

// Alarm ids named in a POST /api/v1/alarms/events body, in order.
static std::vector<std::string> alarm_ids(const std::string& body) {
  std::vector<std::string> ids;
  const std::string key = R"("alarm_id":")";
  for (size_t p = body.find(key); p != std::string::npos;
       p = body.find(key, p)) {
    p += key.size();
    ids.push_back(body.substr(p, body.find('"', p) - p));
  }
  return ids;
}

// A Global console: detections go to UniFi OS, one POST per detection
// naming only the alarms that fire on that detection by itself; nothing
// goes to Protect's automation routes and no history row is written.
static void test_global_notify() {
  FakeUosAgent uos;
  uos.set_alarms(synthetic_alarm_list());
  uos.start();
  NoAlarmManagerProtect protect("[]");
  protect.start();
  CaptureSink sink;
  absl::AddLogSink(&sink);
  onvif::ProtectUserIdProvider provider("owner-user-id", "");
  {
    onvif::AlarmNotifier notifier(
        "http://127.0.0.1:" + std::to_string(protect.port()), &provider,
        "host=/nonexistent-onvif-recorder-test port=1 connect_timeout=1",
        uos.base_url());
    notifier.set_mode_probe(global_console);
    AlarmNotifierTest::set_camera_id(&notifier, kMockMac, kMockCameraId);
    notifier.refresh_alarms();
    check(notifier.mode() == AlarmNotifierTest::Mode::kGlobal,
          "probe true -> Global");
    check(sink.count("alarm mode: Global Alarm Manager") == 1,
          "the mode is logged at start");

    notifier.notify("person", "02:4d:4f:43:4b:01", "evt-g1",
                    1793173174455ULL);
    auto events = uos.events();
    check(events.size() == 1, "one POST for one detection");
    if (events.size() == 1) {
      check(alarm_ids(events[0]) ==
                std::vector<std::string>({"a-person", "a-mock", "a-exclude",
                                          "a-count1", "a-or", "a-noscope",
                                          "a-twokeys"}),
            "person: every unpaused alarm that fires on one person, and "
            "no scope, schedule or exclude check of our own");
      check(contains(events[0], R"("device":"024D4F434B01")") &&
                contains(events[0], R"("camera":"0000000000000000000000d4")") &&
                contains(events[0], R"("eventId":"evt-g1")"),
            "event carries the normalised MAC, camera id and event id");
    }
    check(sink.count("handed to UniFi OS for \"Person\", \"Mock camera "
                     "person\"") == 1,
          "INFO names the alarms sent");
    check(sink.count("uos-agent.log") >= 1,
          "the log points at uos-agent.log");

    notifier.notify("vehicle", kMockMac, "evt-g2", 1793173180000ULL);
    events = uos.events();
    check(events.size() == 2 &&
              alarm_ids(events[1]) ==
                  std::vector<std::string>({"a-vehicle", "a-or"}),
          "vehicle: Vehicle and Person-or-vehicle, not the AND alarm");

    notifier.notify("animal", kMockMac, "evt-g3", 1793173190000ULL);
    check(uos.events().size() == 2,
          "animal: only a count alarm has an animal trigger, nothing sent");
    check(sink.count("no UniFi OS alarm fires on a single animal "
                     "detection") == 1,
          "nothing to send is logged");

    notifier.notify("package", kMockMac, "evt-g4", 1793173200000ULL);
    events = uos.events();
    check(events.size() == 3 &&
              contains(events[2], R"("scope":{"scope_all_smart_cameras_)"
                                  R"(with_package":"024D4F434B01",)"),
          "package: package scope key");

    // The list is read again only once it is a minute old.
    check(uos.list_requests() == 1, "alarm list read once within a minute");
    notifier.refresh_alarms();
    check(sink.count("UniFi OS alarm \"Three people within an hour\": its "
                     "person trigger needs 3 detections") == 1 &&
              sink.count("UniFi OS alarm \"Person after a vehicle\"") == 1 &&
              sink.count("UniFi OS alarm \"Two animals in 5 minutes\"") == 1,
          "each skipped alarm warned once, across refreshes");
  }
  absl::RemoveLogSink(&sink);
  check(uos.bad_requests() == 0,
        "every request carried X-Source: protect and JSON content type");
  bool protect_used = false;
  for (const std::string& p : protect.paths()) {
    if (contains(p, "/api/automations") || contains(p, "/internal/"))
      protect_used = true;
  }
  check(!protect_used, "Global: no Protect automation reads, registrations "
                       "or notify calls");
  check(sink.count("history DB connect failed") == 0,
        "Global: no automationsHistory insert attempted");
}

// What the notifier does with each answer to the events POST.
static void test_global_errors() {
  CaptureSink sink;
  absl::AddLogSink(&sink);
  onvif::ProtectUserIdProvider provider("owner-user-id", "");
  const std::string list =
      "[" + uos_alarm("a-person", "Person",
                      "[[" + trig("protect:objects.person") + "]]",
                      kZonesAll) +
      "]";

  // 400: logged with the body, not retried.
  {
    FakeUosAgent uos;
    uos.set_alarms(list);
    uos.set_events_answer(400, "{\"error\":\"bad scope\nfield\"}");
    uos.start();
    onvif::AlarmNotifier notifier("http://127.0.0.1:1", &provider, "",
                                  uos.base_url());
    notifier.set_mode_probe(global_console);
    notifier.refresh_alarms();
    notifier.notify("person", kMockMac, "evt-e1", 1);
    check(uos.events().size() == 1 &&
              AlarmNotifierTest::uos_posts(&notifier) == 1,
          "HTTP 400: sent once, not retried");
    check(sink.count("rejected the alarm events for \"Person\" with HTTP "
                     "400: {\"error\":\"bad scope field\"}") == 1,
          "HTTP 400: logged with the body on one line");
  }
  // 500: logged, not retried (UniFi OS may have run the alarm).
  {
    FakeUosAgent uos;
    uos.set_alarms(list);
    uos.set_events_answer(500, "boom");
    uos.start();
    onvif::AlarmNotifier notifier("http://127.0.0.1:1", &provider, "",
                                  uos.base_url());
    notifier.set_mode_probe(global_console);
    notifier.refresh_alarms();
    notifier.notify("person", kMockMac, "evt-e2", 1);
    check(uos.events().size() == 1 &&
              AlarmNotifierTest::uos_posts(&notifier) == 1,
          "HTTP 500: sent once, not retried");
    check(sink.count("answered HTTP 500") == 1, "HTTP 500 logged");
  }
  // Connection refused: one retry, then dropped.  The alarm list stays
  // loaded from before uos-agent went away.
  {
    FakeUosAgent uos;
    uos.set_alarms(list);
    uos.start();
    onvif::AlarmNotifier notifier("http://127.0.0.1:1", &provider, "",
                                  uos.base_url());
    notifier.set_mode_probe(global_console);
    AlarmNotifierTest::set_retry_delay_ms(&notifier, 50);
    notifier.refresh_alarms();
    uos.stop();
    const auto start = std::chrono::steady_clock::now();
    notifier.notify("person", kMockMac, "evt-e3", 1);
    const auto took = std::chrono::steady_clock::now() - start;
    check(sink.count("evt-e3): could not connect to") == 1,
          "connection refused: one retry announced");
    check(AlarmNotifierTest::uos_posts(&notifier) == 2,
          "connection refused: exactly two attempts (one retry)");
    check(sink.count("evt-e3) not handed to UniFi OS") == 1 &&
              sink.count("(after one retry); dropped") == 1,
          "connection refused twice: dropped with an error");
    check(took >= std::chrono::milliseconds(50),
          "the retry waits the retry delay");
  }
  // Timeout: UniFi OS may already be running the alarms, so no retry.
  {
    FakeUosAgent uos;
    uos.set_alarms(list);
    uos.set_events_delay_ms(800);
    uos.start();
    onvif::AlarmNotifier notifier("http://127.0.0.1:1", &provider, "",
                                  uos.base_url());
    notifier.set_mode_probe(global_console);
    AlarmNotifierTest::set_retry_delay_ms(&notifier, 0);
    notifier.refresh_alarms();
    AlarmNotifierTest::set_uos_timeout_ms(&notifier, 200);
    notifier.notify("person", kMockMac, "evt-e6", 1);
    check(uos.events().size() == 1 &&
              AlarmNotifierTest::uos_posts(&notifier) == 1,
          "timeout: sent once, not retried");
    check(sink.count("evt-e6) not handed to UniFi OS") == 1 &&
              sink.count("evt-e6): could not connect") == 0 &&
              sink.count("not retried, as UniFi OS may have run them") == 1,
          "timeout: logged as not retried");
  }
  // Refused, then the retry times out: the log says it was retried.  The
  // socket is bound but refuses connections until listen(), and then
  // never answers.
  {
    FakeUosAgent uos;
    uos.set_alarms(list);
    uos.start();
    onvif::AlarmNotifier notifier("http://127.0.0.1:1", &provider, "",
                                  uos.base_url());
    notifier.set_mode_probe(global_console);
    notifier.refresh_alarms();
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    const bool bound =
        fd >= 0 &&
        ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0;
    check(bound, "silent socket bound");
    if (bound) {
      AlarmNotifierTest::set_uos_url(
          &notifier, "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)));
      AlarmNotifierTest::set_uos_timeout_ms(&notifier, 200);
      AlarmNotifierTest::set_retry_delay_ms(&notifier, 300);
      std::thread opener([fd]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ::listen(fd, 4);
      });
      notifier.notify("person", kMockMac, "evt-e7", 1);
      opener.join();
      check(AlarmNotifierTest::uos_posts(&notifier) == 2,
            "refused then timeout: two attempts, no third");
      check(sink.count("evt-e7) not handed to UniFi OS for \"Person\": "
                       "Timeout was reached (after one retry); not retried, "
                       "as UniFi OS may have run them") == 1,
            "refused then timeout: the error names the retry");
    }
    if (fd >= 0) ::close(fd);
  }
  // An unreadable list keeps the last good one.
  {
    FakeUosAgent uos;
    uos.set_alarms(list);
    uos.start();
    onvif::AlarmNotifier notifier("http://127.0.0.1:1", &provider, "",
                                  uos.base_url());
    notifier.set_mode_probe(global_console);
    notifier.refresh_alarms();
    uos.set_alarms(R"({"error":"down"})", 503);
    notifier.refresh_alarms();
    notifier.notify("person", kMockMac, "evt-e4", 1);
    const auto events = uos.events();
    check(events.size() == 1 &&
              alarm_ids(events[0]) == std::vector<std::string>{"a-person"},
          "list read failed: the last good list is used");
  }
  // No list ever read: nothing sent, an error says why.
  {
    FakeUosAgent uos;
    uos.set_alarms("", 500);
    uos.start();
    onvif::AlarmNotifier notifier("http://127.0.0.1:1", &provider, "",
                                  uos.base_url());
    notifier.set_mode_probe(global_console);
    notifier.refresh_alarms();
    notifier.notify("person", kMockMac, "evt-e5", 1);
    check(uos.events().empty() &&
              sink.count("evt-e5) not sent: the UniFi OS alarm list could "
                         "not be read") == 1,
          "no list: nothing sent, error logged");
  }
  absl::RemoveLogSink(&sink);
}

// Mode decided from the alarm list when the flag cannot be read, and
// followed at run time when the console moves from Local to Global.
static void test_mode_at_run_time() {
  CaptureSink sink;
  absl::AddLogSink(&sink);
  onvif::ProtectUserIdProvider provider("owner-user-id", "");
  const std::string objects =
      "[" + uos_alarm("a-person", "Person",
                      "[[" + trig("protect:objects.person") + "]]",
                      kZonesAll) +
      "]";
  const std::string system_only =
      "[" + uos_alarm("a-admin", "Admin Access",
                      "[[" + trig("protect:system.adminAccess") + "]]",
                      "null") +
      "]";
  {
    FakeUosAgent uos;
    uos.set_alarms(objects);
    uos.start();
    onvif::AlarmNotifier notifier("http://127.0.0.1:1", &provider, "",
                                  uos.base_url());
    notifier.set_mode_probe(unreadable_flag);
    notifier.refresh_alarms();
    check(notifier.mode() == AlarmNotifierTest::Mode::kGlobal,
          "flag unreadable, UniFi OS lists object alarms -> Global");
    // Later the list has no object alarms and the flag is still
    // unreadable: the decision stands.
    uos.set_alarms(system_only);
    notifier.refresh_alarms();
    check(notifier.mode() == AlarmNotifierTest::Mode::kGlobal,
          "flag unreadable later: previous Global kept");
  }
  {
    FakeUosAgent uos;
    uos.set_alarms(system_only);
    uos.start();
    onvif::AlarmNotifier notifier("http://127.0.0.1:1", &provider, "",
                                  uos.base_url());
    notifier.set_mode_probe(unreadable_flag);
    notifier.refresh_alarms();
    check(notifier.mode() == AlarmNotifierTest::Mode::kLocal,
          "flag unreadable, no object alarms in UniFi OS -> Local");
  }
  {
    // Neither source answers: undecided, nothing sent anywhere, Protect's
    // automations not read; decided once the UniFi OS list answers.
    FakeUosAgent uos;
    uos.set_alarms("nope", 404);
    uos.start();
    NoAlarmManagerProtect protect("[]");
    protect.start();
    onvif::AlarmNotifier notifier(
        "http://127.0.0.1:" + std::to_string(protect.port()), &provider, "",
        uos.base_url());
    notifier.set_mode_probe(unreadable_flag);
    notifier.refresh_alarms();
    check(notifier.mode() == AlarmNotifierTest::Mode::kUnknown,
          "flag unreadable, no UniFi OS alarm list -> undecided");
    notifier.notify("person", kMockMac, "evt-u1", 1);
    check(uos.events().empty() && protect.paths().empty(),
          "undecided: nothing sent, no Protect automation calls");
    check(sink.count("evt-u1) not sent: the alarm mode is not known") == 1,
          "undecided: the dropped detection is logged");
    check(sink.count("alarm mode not decided yet") == 1,
          "undecided: warned once");
    check(uos.list_requests() == 1,
          "undecided: not re-read within 10 s");
    uos.set_alarms(objects);
    AlarmNotifierTest::age(&notifier, std::chrono::seconds(11));
    notifier.notify("person", kMockMac, "evt-u2", 1);
    check(notifier.mode() == AlarmNotifierTest::Mode::kGlobal &&
              uos.events().size() == 1,
          "undecided: re-read after 10 s, then Global and sent");
  }
  {
    // Local, then the user migrates to Global.
    UosEmulator protect;
    protect.set_alarms_json(
        "[" +
        alarm("local-person",
              "[" + receiver(kOwnerPk, kOwner, R"(["push"])") + "]") +
        "]");
    protect.start();
    FakeUosAgent uos;
    uos.set_alarms(objects);
    uos.start();
    auto flag = std::make_shared<std::atomic<bool>>(false);
    onvif::AlarmNotifier notifier(protect.base_url(), &provider, "",
                                  uos.base_url());
    notifier.set_mode_probe(
        [flag]() -> std::optional<bool> { return flag->load(); });
    notifier.refresh_alarms();
    check(uos.list_requests() == 0, "Local: UniFi OS list not read");
    notifier.notify("person", kMockMac, "evt-m1", 1700000000000ULL);
    check(protect.notify_bodies().size() == 1 && uos.events().empty(),
          "Local: the legacy notify path is used");
    flag->store(true);
    notifier.refresh_alarms();
    check(notifier.mode() == AlarmNotifierTest::Mode::kGlobal,
          "flag flipped -> Global");
    check(sink.count("alarm mode changed from Local Alarm Manager") == 1,
          "the change is logged once");
    notifier.notify("person", kMockMac, "evt-m2", 1700000100000ULL);
    check(uos.events().size() == 1 && protect.notify_bodies().size() == 1,
          "Global: UniFi OS gets the detection, Protect's notify does not");
  }
  {
    // The move to Global is followed at the first detection once the last
    // check is a minute old, without refresh_alarms(); Protect's own list
    // is re-read only every 5 minutes meanwhile.
    NoAlarmManagerProtect protect(
        "[" +
        alarm("local-person",
              "[" + receiver(kOwnerPk, kOwner, R"(["push"])") + "]") +
        "]");
    protect.start();
    FakeUosAgent uos;
    uos.set_alarms(objects);
    uos.start();
    auto flag = std::make_shared<std::atomic<bool>>(false);
    auto probes = std::make_shared<std::atomic<int>>(0);
    onvif::AlarmNotifier notifier(
        "http://127.0.0.1:" + std::to_string(protect.port()), &provider, "",
        uos.base_url());
    notifier.set_mode_probe([flag, probes]() -> std::optional<bool> {
      ++*probes;
      return flag->load();
    });
    notifier.refresh_alarms();
    const auto list_reads = [&protect]() {
      int n = 0;
      for (const std::string& p : protect.paths()) {
        if (p == "/api/automations") ++n;
      }
      return n;
    };
    notifier.notify("person", kMockMac, "evt-c1", 1700000000000ULL);
    check(*probes == 1 && list_reads() == 1,
          "Local: no refresh within the minute");
    AlarmNotifierTest::age(&notifier, std::chrono::seconds(61));
    notifier.notify("person", kMockMac, "evt-c2", 1700000100000ULL);
    check(*probes == 2 && list_reads() == 1,
          "Local: mode checked after a minute; automations not re-read");
    AlarmNotifierTest::age(&notifier, std::chrono::seconds(301));
    notifier.notify("person", kMockMac, "evt-c3", 1700000200000ULL);
    check(*probes == 3 && list_reads() == 2,
          "Local: automations re-read after 5 minutes");
    flag->store(true);
    AlarmNotifierTest::age(&notifier, std::chrono::seconds(61));
    notifier.notify("person", kMockMac, "evt-c4", 1700000300000ULL);
    check(notifier.mode() == AlarmNotifierTest::Mode::kGlobal &&
              uos.events().size() == 1 && list_reads() == 2,
          "Local -> Global followed at the next detection after a minute");
  }
  absl::RemoveLogSink(&sink);
}

// Detections never wait for another thread's refresh, and a failing
// refresh is not repeated on every detection.
static void test_refresh_does_not_block() {
  onvif::ProtectUserIdProvider provider("owner-user-id", "");
  const std::string list =
      "[" + uos_alarm("a-person", "Person",
                      "[[" + trig("protect:objects.person") + "]]",
                      kZonesAll) +
      "]";
  {
    FakeUosAgent uos;
    uos.set_alarms(list);
    uos.start();
    onvif::AlarmNotifier notifier("http://127.0.0.1:1", &provider, "",
                                  uos.base_url());
    notifier.set_mode_probe(global_console);
    notifier.refresh_alarms();
    AlarmNotifierTest::age(&notifier, std::chrono::seconds(61));
    // Another thread is in the middle of a refresh.
    absl::Notification held;
    absl::Notification release;
    std::thread refresher([&]() {
      absl::MutexLock lk(AlarmNotifierTest::refresh_mu(&notifier));
      held.Notify();
      release.WaitForNotification();
    });
    held.WaitForNotification();
    const auto start = std::chrono::steady_clock::now();
    notifier.notify("person", kMockMac, "evt-b1", 1);
    const auto took = std::chrono::steady_clock::now() - start;
    release.Notify();
    refresher.join();
    check(took < std::chrono::seconds(2) && uos.events().size() == 1,
          "a detection during another refresh is sent with the loaded list");
    check(uos.list_requests() == 1,
          "the detection did not refresh while another thread was");
  }
  {
    // Global, list never readable: one read per 10 s, not per detection.
    FakeUosAgent uos;
    uos.set_alarms("", 500);
    uos.start();
    auto probes = std::make_shared<std::atomic<int>>(0);
    onvif::AlarmNotifier notifier("http://127.0.0.1:1", &provider, "",
                                  uos.base_url());
    notifier.set_mode_probe([probes]() -> std::optional<bool> {
      ++*probes;
      return true;
    });
    notifier.refresh_alarms();
    for (int i = 0; i < 5; ++i) notifier.notify("person", kMockMac, "e", 1);
    check(uos.list_requests() == 1 && *probes == 1,
          "unreadable list: not re-read on every detection");
    AlarmNotifierTest::age(&notifier, std::chrono::seconds(11));
    notifier.notify("person", kMockMac, "e", 1);
    check(uos.list_requests() == 2, "unreadable list: re-read after 10 s");
  }
  {
    // Local console without a Protect user id: no refresh per detection.
    onvif::ProtectUserIdProvider no_user("", "");
    auto probes = std::make_shared<std::atomic<int>>(0);
    onvif::AlarmNotifier notifier("http://127.0.0.1:1", &no_user, "", kNoUos);
    notifier.set_mode_probe([probes]() -> std::optional<bool> {
      ++*probes;
      return false;
    });
    notifier.refresh_alarms();
    for (int i = 0; i < 5; ++i) notifier.notify("person", kMockMac, "e", 1);
    check(*probes == 1, "Local without a user id: one refresh, not five");
  }
}

// --trigger_automations=false: no notifier, so nothing is read or sent.
static void test_trigger_automations_off() {
  FakeUosAgent uos;
  uos.set_alarms(synthetic_alarm_list());
  uos.start();
  onvif::ProtectUserIdProvider provider("owner-user-id", "");
  auto off = onvif::AlarmNotifier::create(false, "http://127.0.0.1:1",
                                          &provider, "");
  check(off == nullptr, "trigger_automations=false: no notifier");
  check(uos.list_requests() == 0 && uos.events().empty(),
        "trigger_automations=false: nothing read or sent");
  onvif::ProtectUserIdProvider no_user("", "");
  auto on = onvif::AlarmNotifier::create(true, "http://127.0.0.1:1",
                                         &no_user, "");
  check(on != nullptr,
        "no Protect user id: a notifier still exists for Global alarms");
}

int main() {
  test_parse_basics();
  test_parse_real_alarm_push_off();
  test_channels();
  test_no_notify_action();
  test_alarm_schedule();
  test_receiver_schedule();
  test_schedule_rules();
  test_location_schedules();
  test_unresolved_users();
  test_local_clock();
  test_notify_skips_but_records();
  test_legacy_path_without_alarm_manager();
  test_recipient_resolved_through_users();
  test_local_exclude_sources();
  test_mode_decision();
  test_uos_alarm_parsing();
  test_uos_payload_golden();
  test_global_notify();
  test_global_errors();
  test_mode_at_run_time();
  test_refresh_does_not_block();
  test_trigger_automations_off();

  std::cerr << "PASS " << g_pass << " / FAIL " << g_fail << '\n';
  return g_fail == 0 ? 0 : 1;
}
