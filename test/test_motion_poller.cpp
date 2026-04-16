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
 * (smart_detect_types_json, build_sdr_payload).  No PostgreSQL required.
 */

#include <cstdint>
#include <iostream>
#include <string>

#include "motion_poller.hpp"

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
  std::string j = build_sdr_payload(ts, "vehicle");

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

  // Fixed structural fields.
  check(contains(j, "\"confidence\":75"),
        "build_sdr_payload: confidence=75");
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
  std::string p = build_sdr_payload(1000, "person");
  std::string a = build_sdr_payload(1000, "animal");
  check(contains(p, "\"objectType\":\"person\""),
        "build_sdr_payload: objectType=person");
  check(contains(a, "\"objectType\":\"animal\""),
        "build_sdr_payload: objectType=animal");
  // Same ts → only objectType differs.
  check(p != a, "build_sdr_payload: object type changes output");
}

int main() {
  test_smart_detect_types_known();
  test_smart_detect_types_person_default();
  test_build_sdr_payload_fields();
  test_build_sdr_payload_object_type_varies();

  std::cout << "test_motion_poller: "
            << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
