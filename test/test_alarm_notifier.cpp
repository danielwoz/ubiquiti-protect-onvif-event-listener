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
 * alarm_notifier.hpp.  Covers the JSON shapes we have actually seen from
 * the Protect API: enable/disable, condition sources, device filters,
 * cooldown, "deleted" entries being dropped, and the SEND_NOTIFICATION
 * recipients, channels and schedules that decide who the UOS notify call
 * goes to.  Fake Protect APIs check that notify() skips the call when
 * nobody is to be notified but still records history and the cooldown,
 * and still runs the alarm by the legacy path when the automation manager
 * routes are absent.
 */

#include <time.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/synchronization/mutex.h"
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

constexpr char kOwner[] = "21f4450a-3b42-4be4-9d05-852776e70e77";
constexpr char kOwnerPk[] = "699bf3d301fa4a03e400040b";

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
         R"(,"userId":"699bf3d301fa4a03e400040b","status":{)"
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
      "sources":    [{"device":"FC5F49CA68D4"}],
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
      check(v[0].source_devices.count("FC5F49CA68D4") == 1, "source MAC present");
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
      R"("699bf3d301fa4a03e400040b","channels":[],"schedules":[],)"
      R"("ucoreUser":"21f4450a-3b42-4be4-9d05-852776e70e77"}]},"order":-1}],)"
      R"("userId":"699bf3d301fa4a03e400040b","status":{"lastExecutedAt":null,)"
      R"("lastDetectionEventId":null,"total":0},"cooldown":{"enable":false,)"
      R"("timeout":600000,"lastExecutedAt":0},"id":"699bf3d902e14a03e400042c",)"
      R"("deleted":false,"createdAt":1771856985737,"isBlockedByArmMode":false,)"
      R"("usedInArmMode":false,"isExternal":false}])";
  auto v = AlarmNotifierTest::parse(json);
  check(v.size() == 1, "real Person alarm parsed");
  if (v.size() != 1) return;
  const auto& a = v[0];
  check(a.id == "699bf3d902e14a03e400042c", "real alarm id is its own id");
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
    check(contains(p, R"("receivers":[{"user_id":"21f4450a-3b42-4be4-9d05-)"
                      R"(852776e70e77","push":true,"email":true,"voip":false}]})"),
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
      R"("location":{"isAway":true},"id":"6aae34e6025eea03e40084e1",)"
      R"("ucoreUserId":"bae7433e-c596-453a-90e4-a4befe35d460"},)"
      R"({"isOwner":true,"id":"699bf3d301fa4a03e400040b","settings":{"id":"x"},)"
      R"("ucoreUserId":"21f4450a-3b42-4be4-9d05-852776e70e77"},)"
      R"({"id":"no-ucore"}])");
  check(ids.size() == 2, "two users with UniFi OS ids");
  check(ids.count(kOwnerPk) == 1 && ids.at(kOwnerPk) == kOwner,
        "owner mapped to its UniFi OS id");
  check(ids.count("6aae34e6025eea03e40084e1") == 1 &&
            ids.at("6aae34e6025eea03e40084e1") ==
                "bae7433e-c596-453a-90e4-a4befe35d460",
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
  const std::string automations =
      "[" +
      alarm("push-on", "[" + receiver(kOwnerPk, kOwner, R"(["push"])") + "]") +
      "," + alarm("push-off", "[" + receiver(kOwnerPk, kOwner, "[]") + "]") +
      "," +
      alarm_with_actions(
          "no-notify",
          R"([{"type":"PLAY_SPEAKER","metadata":{"volume":80},"order":0}])") +
      "," +
      alarm("never", "[" + receiver(kOwnerPk, kOwner, R"(["push"])") + "]",
            R"([{"schedule":{"type":"is","unit":"never","values":[]}}])") +
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
        "host=/nonexistent-onvif-recorder-test port=1 connect_timeout=1");
    notifier.refresh_alarms();
    notifier.notify("person", "AABBCCDDEEFF", "evt-1", 1700000000000ULL);

    check(protect.register_bodies().size() == 4,
          "every enabled alarm registered");
    const auto notifies = protect.notify_bodies();
    check(notifies.size() == 1, "one notify call, for the push-on alarm");
    if (notifies.size() == 1) {
      const std::string& body = notifies[0];
      check(contains(body, R"("alarm_id":"push-on")"), "notify is push-on");
      check(contains(body, R"("receivers":[{"user_id":"21f4450a-3b42-4be4-)"
                           R"(9d05-852776e70e77","push":true,"email":false,)"
                           R"("voip":false}]})"),
            "notify names the alarm's recipient");
      check(!contains(body, "owner-user-id"),
            "notify does not use the X-UserId user");
    }
    check(protect.posted_events().empty(), "legacy run path not used");
    check(sink.count("history DB connect failed") == 4,
          "history recorded for all four alarms, sent or not");
    const auto fired = AlarmNotifierTest::last_fired(&notifier);
    check(fired.size() == 4 && fired.count("push-off") == 1 &&
              fired.count("no-notify") == 1 && fired.count("never") == 1 &&
              fired.at("push-off") == 1700000000000ULL,
          "cooldown updated for skipped alarms too");
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
        "http://127.0.0.1:" + std::to_string(protect.port()), &provider);
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

  std::cerr << "PASS " << g_pass << " / FAIL " << g_fail << '\n';
  return g_fail == 0 ? 0 : 1;
}
