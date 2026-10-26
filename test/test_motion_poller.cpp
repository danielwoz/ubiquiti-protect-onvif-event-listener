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
 * test_motion_poller.cpp
 *
 * Pure-logic tests for helpers exposed via onvif::motion_poller_internal
 * (smart_detect_types_json, build_sdr_payload, wait_for_connection, ...).
 * No PostgreSQL required; the LISTEN-restore test also runs against one
 * when ONVIF_TEST_DB_CONN is set and is skipped otherwise.
 */

#include <libpq-fe.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include "motion_poller.hpp"
#include "object_detect.hpp"

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

// ---------------------------------------------------------------
// smart_detect_types_json
// ---------------------------------------------------------------
static void test_smart_detect_types_known() {
  using onvif::motion_poller_internal::smart_detect_types_json;
  check(smart_detect_types_json("vehicle") == "[\"vehicle\"]",
        "smart_detect_types_json: vehicle");
  check(smart_detect_types_json("animal") == "[\"animal\"]",
        "smart_detect_types_json: animal");
  check(smart_detect_types_json("package") == "[\"package\"]",
        "smart_detect_types_json: package");
}

static void test_smart_detect_types_person_default() {
  using onvif::motion_poller_internal::smart_detect_types_json;
  check(smart_detect_types_json("person") == "[\"person\"]",
        "smart_detect_types_json: person maps explicitly");
  check(smart_detect_types_json("unknown") == "[\"person\"]",
        "smart_detect_types_json: unknown falls back to person");
  check(smart_detect_types_json("") == "[\"person\"]",
        "smart_detect_types_json: empty falls back to person");
}

// ---------------------------------------------------------------
// build_sdr_payload
// ---------------------------------------------------------------

// Helper: does @p haystack contain @p needle?
static bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

static void test_build_sdr_payload_fields() {
  using onvif::motion_poller_internal::build_sdr_payload;
  const uint64_t ts = 1712345678901ULL;
  std::string j = build_sdr_payload(ts, "vehicle", 87);

  // Timestamp fields should all carry the ts value.
  check(contains(j, "\"clockStream\":1712345678901"),
        "build_sdr_payload: clockStream ts");
  check(contains(j, "\"clockWall\":1712345678901"),
        "build_sdr_payload: clockWall ts");
  check(contains(j, "\"firstShownTimeMs\":1712345678901"),
        "build_sdr_payload: firstShownTimeMs ts");
  check(contains(j, "\"idleSinceTimeMs\":1712345678901"),
        "build_sdr_payload: idleSinceTimeMs ts");
  check(contains(j, "\"timestamp\":1712345678901"),
        "build_sdr_payload: timestamp ts");

  // Object type should be embedded verbatim in a quoted JSON string.
  check(contains(j, "\"objectType\":\"vehicle\""),
        "build_sdr_payload: objectType=vehicle");

  // Confidence is now caller-supplied (model output scaled 0-100).
  check(contains(j, "\"confidence\":87"),
        "build_sdr_payload: confidence=87 (caller-supplied)");
  check(contains(j, "\"clockStreamRate\":1000"),
        "build_sdr_payload: clockStreamRate=1000");
  check(contains(j, "\"edgeType\":\"none\""),
        "build_sdr_payload: edgeType=none");

  // Must be well-formed-ish: starts with { and ends with }.
  check(!j.empty() && j.front() == '{' && j.back() == '}',
        "build_sdr_payload: outer braces present");
}

static void test_build_sdr_payload_object_type_varies() {
  using onvif::motion_poller_internal::build_sdr_payload;
  std::string p = build_sdr_payload(1000, "person",  50);
  std::string a = build_sdr_payload(1000, "animal",  50);
  std::string k = build_sdr_payload(1000, "package", 50);
  check(contains(p, "\"objectType\":\"person\""),
        "build_sdr_payload: objectType=person");
  check(contains(a, "\"objectType\":\"animal\""),
        "build_sdr_payload: objectType=animal");
  check(contains(k, "\"objectType\":\"package\""),
        "build_sdr_payload: objectType=package");
  // Same ts + confidence → only objectType differs.
  check(p != a && a != k && p != k,
        "build_sdr_payload: object type changes output");
}

// ---------------------------------------------------------------
// build_sdo_attributes
// ---------------------------------------------------------------
static void test_build_sdo_attributes_fields() {
  using onvif::motion_poller_internal::build_sdo_attributes;
  std::string j = build_sdo_attributes("vehicle", 87);

  // Object type and confidence should appear verbatim.
  check(contains(j, "\"objectType\":\"vehicle\""),
        "build_sdo_attributes: objectType=vehicle");
  check(contains(j, "\"confidence\":87"),
        "build_sdo_attributes: confidence=87");

  // Schema fields the iOS app expects to be present (as null sentinels).
  check(contains(j, "\"associatedFaceTrackerID\":null"),
        "build_sdo_attributes: associatedFaceTrackerID null");
  check(contains(j, "\"faceEmbed\":null"),
        "build_sdo_attributes: faceEmbed null");
  check(contains(j, "\"matchedId\":null"),
        "build_sdo_attributes: matchedId null");
  check(contains(j, "\"trackerId\":1"),
        "build_sdo_attributes: trackerId=1");
  check(contains(j, "\"vehicleType\":null"),
        "build_sdo_attributes: vehicleType null");
  // zone MUST be a non-empty array: iOS reads attributes.zone[0]
  // unchecked and crashes on []. Default to [1] to match native events.
  check(contains(j, "\"zone\":[1]"),
        "build_sdo_attributes: zone defaults to [1] (iOS crash guard)");

  check(!j.empty() && j.front() == '{' && j.back() == '}',
        "build_sdo_attributes: outer braces present");
}

static void test_build_sdo_attributes_object_type_varies() {
  using onvif::motion_poller_internal::build_sdo_attributes;
  std::string p = build_sdo_attributes("person",  80);
  std::string a = build_sdo_attributes("animal",  80);
  std::string k = build_sdo_attributes("package", 80);
  check(contains(p, "\"objectType\":\"person\""),
        "build_sdo_attributes: person");
  check(contains(a, "\"objectType\":\"animal\""),
        "build_sdo_attributes: animal");
  check(contains(k, "\"objectType\":\"package\""),
        "build_sdo_attributes: package");
}

// ---------------------------------------------------------------
// build_sdt_payload (smartDetectTracks)
// ---------------------------------------------------------------
static void test_build_sdt_payload_fields() {
  using onvif::motion_poller_internal::build_sdt_payload;
  const uint64_t start = 1712345678901ULL;
  const uint64_t end   = 1712345681901ULL;  // +3s
  std::string j = build_sdt_payload(start, end, "person", 92);

  // Outer wrapper is a JSON array of one entry.
  check(!j.empty() && j.front() == '[' && j.back() == ']',
        "build_sdt_payload: outer array brackets");

  // Object type and confidence carry through.
  check(contains(j, "\"objectType\":\"person\""),
        "build_sdt_payload: objectType");
  check(contains(j, "\"confidence\":92"),
        "build_sdt_payload: confidence");

  // Timestamps and duration (seconds).
  check(contains(j, "\"firstShownTimeMs\":1712345678901"),
        "build_sdt_payload: firstShownTimeMs");
  check(contains(j, "\"timestamp\":1712345681901"),
        "build_sdt_payload: timestamp = end_ms");
  check(contains(j, "\"duration\":3"),
        "build_sdt_payload: duration in seconds");

  // Sentinel coord and stable defaults.
  check(contains(j, "\"coord\":[-1.0,-1.0,-1.0,-1.0]"),
        "build_sdt_payload: coord sentinel");
  check(contains(j, "\"id\":\"1\""),
        "build_sdt_payload: id");
  check(contains(j, "\"zones\":[]"),
        "build_sdt_payload: zones empty array");
}

static void test_build_sdt_payload_zero_duration() {
  using onvif::motion_poller_internal::build_sdt_payload;
  // end_ms <= start_ms → duration clamps to 0.
  std::string j = build_sdt_payload(1000, 1000, "person", 50);
  check(contains(j, "\"duration\":0"),
        "build_sdt_payload: zero-duration when end <= start");
  std::string j2 = build_sdt_payload(2000, 1500, "person", 50);
  check(contains(j2, "\"duration\":0"),
        "build_sdt_payload: zero-duration when end < start");
}

// ---------------------------------------------------------------
// object_detect::detection_type coverage for the package classes
// added alongside animal support (see object_detect.hpp).  Lives
// here rather than its own file because motion_poller is the only
// consumer of the mapping.
// ---------------------------------------------------------------
static void test_detection_type_coco_mapping() {
  using object_detect::detection_type;
  using object_detect::is_security_relevant;
  // Sanity: existing classes still map.
  check(detection_type(0)  == "person",  "COCO 0  -> person");
  check(detection_type(2)  == "vehicle", "COCO 2  -> vehicle");
  check(detection_type(16) == "animal",  "COCO 16 -> animal");
  // New: backpack/handbag/suitcase -> package.
  check(detection_type(24) == "package", "COCO 24 (backpack) -> package");
  check(detection_type(26) == "package", "COCO 26 (handbag)  -> package");
  check(detection_type(28) == "package", "COCO 28 (suitcase) -> package");
  // Unrelated class falls back to person.
  check(detection_type(99) == "person",  "COCO 99 (unknown) -> person fallback");
  // is_security_relevant must include the new classes.
  check(is_security_relevant(24), "is_security_relevant(24) backpack");
  check(is_security_relevant(26), "is_security_relevant(26) handbag");
  check(is_security_relevant(28), "is_security_relevant(28) suitcase");
  // And reject unrelated COCO classes.
  check(!is_security_relevant(50), "is_security_relevant(50) excluded");
}

// ---------------------------------------------------------------
// select_best_candidate_index (time-travel sampling + baseline)
// ---------------------------------------------------------------

using onvif::motion_poller_internal::Candidate;
using onvif::motion_poller_internal::select_best_candidate_index;

// No baseline + single candidate -> that candidate wins.
static void test_select_no_baseline_single() {
  std::vector<Candidate> evs = {{"person", 74}};
  check(select_best_candidate_index({}, evs) == 0,
        "select: no baseline + single person -> idx 0");
}

// Empty event list -> -1 (no winner).
static void test_select_empty_events() {
  check(select_best_candidate_index({}, {}) == -1,
        "select: empty events -> -1");
  check(select_best_candidate_index({"vehicle"}, {}) == -1,
        "select: empty events with baseline -> -1");
}

// Highest confidence wins across multiple candidates.
static void test_select_picks_highest_confidence() {
  std::vector<Candidate> evs = {
      {"person", 50},   // idx 0
      {"person", 85},   // idx 1 — highest
      {"person", 60},   // idx 2
  };
  check(select_best_candidate_index({}, evs) == 1,
        "select: 50/85/60 -> idx 1 (highest)");
}

// Tie-break on equal confidence: earliest index wins.
static void test_select_ties_break_to_earliest_index() {
  std::vector<Candidate> evs = {
      {"person", 70},   // idx 0
      {"vehicle", 70},  // idx 1 — same conf
  };
  check(select_best_candidate_index({}, evs) == 0,
        "select: equal conf -> earliest idx wins");
}

// THE PARKED-CAR CASE: vehicle in baseline AND in event -> suppressed.
static void test_select_parked_car_suppressed() {
  std::vector<Candidate> evs = {
      {"vehicle", 80},  // parked car still there
  };
  check(select_best_candidate_index({"vehicle"}, evs) == -1,
        "select: parked car suppressed (vehicle in baseline)");
}

// Mixed case: vehicle baseline, vehicle + person in event -> person wins.
static void test_select_person_through_parked_car() {
  std::vector<Candidate> evs = {
      {"vehicle", 90},  // the parked car -> suppressed
      {"person",  55},  // the actual person walking past
  };
  check(select_best_candidate_index({"vehicle"}, evs) == 1,
        "select: person wins through parked car");
}

// All event classes in baseline -> -1 (the "always-there" no-op).
static void test_select_all_classes_in_baseline() {
  std::vector<Candidate> evs = {
      {"vehicle", 80},
      {"animal",  70},
  };
  check(select_best_candidate_index({"vehicle", "animal"}, evs) == -1,
        "select: all classes in baseline -> no winner");
}

// Multi-frame realism: same person classified twice with different confidences
// across frames -> the higher-conf frame wins.
static void test_select_same_class_two_frames() {
  std::vector<Candidate> evs = {
      // frame 0: weak detection
      {"person", 42},
      // frame 1: nothing classified
      // frame 2: strong detection -- person is mid-frame now
      {"person", 78},
  };
  check(select_best_candidate_index({}, evs) == 1,
        "select: stronger detection in later frame wins");
}

// ---------------------------------------------------------------------------
// wait_for_connection: the poll loop's wait while the database is down
// ---------------------------------------------------------------------------

namespace wait_test {
int resets = 0;
int succeed_on = 0;
bool up = false;
bool fake_up(PGconn*) { return up; }
bool fake_reset(PGconn*, std::string* error) {
  if (++resets >= succeed_on) {
    up = true;
    return true;
  }
  *error = "FATAL: the database system is starting up";
  return false;
}
}  // namespace wait_test

// A Postgres restart: the third attempt gets through.  The wait returns
// once connected, having slept out the back-off between attempts rather
// than retrying in a tight loop.
static void test_wait_for_connection_until_up() {
  onvif::pg::ReconnectGate gate("wait_test", /*base_ms=*/20, /*max_ms=*/40);
  gate.set_conn_ops_for_testing({&wait_test::fake_up, &wait_test::fake_reset});
  wait_test::resets = 0;
  wait_test::succeed_on = 3;
  wait_test::up = false;
  std::atomic<bool> running{true};
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok = onvif::motion_poller_internal::wait_for_connection(
      nullptr, &gate, running);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
  check(ok, "wait: returns true once the connection is back");
  check(wait_test::resets == 3, "wait: one attempt per back-off");
  check(ms >= 55, "wait: sleeps out the back-off between attempts");
}

// The database stays down: a real libpq connection to a server that is
// not there.  The wait keeps to the back-off and returns false promptly
// when the poller is stopped.
static void test_wait_for_connection_stops() {
  PGconn* conn = PQconnectdb(
      "host=/nonexistent-onvif-recorder-test port=1 connect_timeout=1");
  onvif::pg::ReconnectGate gate("wait_test_down", /*base_ms=*/100,
                                /*max_ms=*/200);
  std::atomic<bool> running{true};
  std::atomic<bool> result{true};
  std::thread waiter([&] {
    result = onvif::motion_poller_internal::wait_for_connection(
        conn, &gate, running);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  const auto t0 = std::chrono::steady_clock::now();
  running = false;
  waiter.join();
  const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
  check(!result.load(), "wait: returns false when stopped while down");
  check(stop_ms < 500, "wait: notices stop() within its sleep slice");
  // Attempts at 0, 100, 300, 500, 700, 900 ms: about six in one second.
  check(gate.attempts() >= 4 && gate.attempts() <= 8,
        ("wait: attempts bounded by the back-off, got " +
         std::to_string(gate.attempts())).c_str());
  PQfinish(conn);
}

// ---------------------------------------------------------------------------
// ensure_listening: push mode's LISTEN follows the connection's session
// ---------------------------------------------------------------------------

// A connection that is down is left for the next call, not reported as a
// refused LISTEN (which would drop push mode for the rest of the run).
static void test_ensure_listening_while_down() {
  PGconn* conn = PQconnectdb(
      "host=/nonexistent-onvif-recorder-test port=1 connect_timeout=1");
  int pid = 1234;
  check(onvif::motion_poller_internal::ensure_listening(conn, &pid),
        "listen: down connection is retried later");
  check(pid == 1234, "listen: down connection leaves the session id alone");
  PQfinish(conn);
}

static bool listening(PGconn* conn) {
  PGresult* r = PQexec(conn,
      "SELECT count(*) FROM pg_listening_channels() c"
      " WHERE c = 'onvif_recorder_motion'");
  const bool on = PQresultStatus(r) == PGRES_TUPLES_OK &&
                  std::string(PQgetvalue(r, 0, 0)) == "1";
  PQclear(r);
  return on;
}

// Against a real PostgreSQL (ONVIF_TEST_DB_CONN or libpq's PG* defaults;
// skipped when none is reachable): the LISTEN comes back after the
// session is re-opened, whether the reconnect gate or pg_util's
// cancel-and-drain re-opened it.
static void test_ensure_listening_follows_session() {
  const char* env = std::getenv("ONVIF_TEST_DB_CONN");
  PGconn* conn = PQconnectdb(env ? env : "");
  if (PQstatus(conn) != CONNECTION_OK) {
    std::cerr << "SKIP: ensure_listening against a database "
                 "(set ONVIF_TEST_DB_CONN)\n";
    PQfinish(conn);
    return;
  }
  using onvif::motion_poller_internal::ensure_listening;
  int pid = 0;
  check(ensure_listening(conn, &pid) && pid == PQbackendPID(conn),
        "listen db: first LISTEN records the session");
  check(listening(conn), "listen db: channel listened");

  // Server ends the session; the reconnect gate re-opens it.
  onvif::pg::ReconnectGate gate("listen_test");
  PQclear(PQexec(conn, "SELECT pg_terminate_backend(pg_backend_pid())"));
  check(ensure_listening(conn, &pid), "listen db: down session deferred");
  check(gate.ensure(conn), "listen db: gate re-opened the session");
  check(!listening(conn), "listen db: new session starts without LISTEN");
  check(ensure_listening(conn, &pid) && pid == PQbackendPID(conn),
        "listen db: LISTEN issued on the new session");
  check(listening(conn), "listen db: channel listened again (gate)");

  // Session ends under a query: pg_util resets the connection itself.
  PQclear(PQexec(conn, "SELECT pg_terminate_backend(pg_backend_pid())"));
  PQclear(onvif::pg::ExecWithTimeout(conn, 5000, "SELECT 1"));
  if (PQstatus(conn) != CONNECTION_OK) gate.ensure(conn);
  check(PQstatus(conn) == CONNECTION_OK, "listen db: session re-opened");
  check(ensure_listening(conn, &pid) && pid == PQbackendPID(conn),
        "listen db: LISTEN issued after pg_util's reset");
  check(listening(conn), "listen db: channel listened again (pg_util)");
  PQfinish(conn);
}

int main() {
  test_smart_detect_types_known();
  test_smart_detect_types_person_default();
  test_build_sdr_payload_fields();
  test_build_sdr_payload_object_type_varies();
  test_build_sdo_attributes_fields();
  test_build_sdo_attributes_object_type_varies();
  test_build_sdt_payload_fields();
  test_build_sdt_payload_zero_duration();
  test_detection_type_coco_mapping();

  test_select_no_baseline_single();
  test_select_empty_events();
  test_select_picks_highest_confidence();
  test_select_ties_break_to_earliest_index();
  test_select_parked_car_suppressed();
  test_select_person_through_parked_car();
  test_select_all_classes_in_baseline();
  test_select_same_class_two_frames();

  test_wait_for_connection_until_up();
  test_wait_for_connection_stops();
  test_ensure_listening_while_down();
  test_ensure_listening_follows_session();

  std::cout << "test_motion_poller: "
            << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
