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
 * test_camera_change_log.cpp
 *
 * Tests for the CameraChangeLog write/read roundtrip, concurrent writes,
 * empty file handling, and malformed line recovery.
 */

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "camera_change_log.hpp"

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

// Unique temp path per test.
static std::string temp_path(const char* suffix) {
  const char* tmpdir = std::getenv("TEST_TMPDIR");
  std::string dir = tmpdir ? tmpdir : "/tmp";
  return dir + "/test_camera_change_log_" + suffix + ".jsonl";
}

// ---------------------------------------------------------------
// Test 1: Write + read_all round-trip
// ---------------------------------------------------------------
static void test_roundtrip() {
  std::string path = temp_path("roundtrip");
  std::remove(path.c_str());

  {
    auto result = unifi::CameraChangeLog::Create(path);
    check(result.ok(), "Create succeeds");
    auto& log = *result;

    log->record("cam-001", "featureFlags.smartDetectTypes",
                "[]", "[\"person\",\"vehicle\"]");
    log->record("cam-002", "smartDetectSettings.objectTypes",
                "[]", "[\"person\"]");
    log->record("cam-001", "smartDetectZones",
                "[]", "[{\"name\":\"Default\"}]");
  }

  auto records = unifi::CameraChangeLog::read_all(path);
  check(records.size() == 3, "read_all returns 3 records");

  check(records[0].camera_id == "cam-001", "record 0 camera_id");
  check(records[0].column == "featureFlags.smartDetectTypes",
        "record 0 column");
  check(records[0].old_value == "[]", "record 0 old_value");
  check(records[0].new_value == "[\"person\",\"vehicle\"]",
        "record 0 new_value");
  check(!records[0].timestamp.empty(), "record 0 has timestamp");

  check(records[1].camera_id == "cam-002", "record 1 camera_id");
  check(records[2].camera_id == "cam-001", "record 2 camera_id");
  check(records[2].column == "smartDetectZones", "record 2 column");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------
// Test 2: Empty and non-existent files
// ---------------------------------------------------------------
static void test_empty_file() {
  std::string path = temp_path("empty");
  std::remove(path.c_str());

  // Non-existent file: read_all returns empty.
  auto records = unifi::CameraChangeLog::read_all(path);
  check(records.empty(), "read_all on non-existent returns empty");

  // Empty file: read_all returns empty.
  { std::ofstream f(path); }  // create empty
  records = unifi::CameraChangeLog::read_all(path);
  check(records.empty(), "read_all on empty file returns empty");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------
// Test 3: Malformed lines are skipped
// ---------------------------------------------------------------
static void test_malformed_lines() {
  std::string path = temp_path("malformed");
  std::remove(path.c_str());

  {
    std::ofstream f(path);
    // Valid line.
    f << "{\"ts\":\"2026-04-08T12:00:00.000Z\",\"camera_id\":\"cam-001\","
         "\"col\":\"featureFlags\",\"old\":\"[]\",\"new\":\"[1]\"}\n";
    // Blank line.
    f << "\n";
    // Not JSON (no opening brace).
    f << "garbage line\n";
    // Missing camera_id: should be skipped.
    f << "{\"ts\":\"2026-04-08T12:00:01.000Z\",\"col\":\"x\","
         "\"old\":\"a\",\"new\":\"b\"}\n";
    // Another valid line.
    f << "{\"ts\":\"2026-04-08T12:00:02.000Z\",\"camera_id\":\"cam-002\","
         "\"col\":\"smartDetectZones\",\"old\":\"[]\",\"new\":\"[{}]\"}\n";
  }

  auto records = unifi::CameraChangeLog::read_all(path);
  check(records.size() == 2, "malformed: 2 valid records");
  check(records[0].camera_id == "cam-001", "malformed: first record ok");
  check(records[1].camera_id == "cam-002", "malformed: second record ok");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------
// Test 4: Concurrent writes from multiple threads
// ---------------------------------------------------------------
static void test_concurrent_writes() {
  std::string path = temp_path("concurrent");
  std::remove(path.c_str());

  constexpr int kThreads = 4;
  constexpr int kRecordsPerThread = 50;

  {
    auto result = unifi::CameraChangeLog::Create(path);
    check(result.ok(), "concurrent: Create succeeds");
    auto& log = *result;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&log, t]() {
        for (int i = 0; i < kRecordsPerThread; ++i) {
          std::string cam = "cam-" + std::to_string(t);
          log->record(cam, "col", "old", "new");
        }
      });
    }
    for (auto& th : threads) th.join();
  }

  auto records = unifi::CameraChangeLog::read_all(path);
  check(records.size() == kThreads * kRecordsPerThread,
        "concurrent: all records written");

  // Every record should have a non-empty camera_id and timestamp.
  bool all_ok = true;
  for (const auto& r : records) {
    if (r.camera_id.empty() || r.timestamp.empty()) {
      all_ok = false;
      break;
    }
  }
  check(all_ok, "concurrent: all records valid");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------
// Test 5: Values with special chars (quotes, backslashes)
// ---------------------------------------------------------------
static void test_special_chars() {
  std::string path = temp_path("special");
  std::remove(path.c_str());

  {
    auto result = unifi::CameraChangeLog::Create(path);
    check(result.ok(), "special: Create succeeds");
    auto& log = *result;

    log->record("cam-001", "col-\"test\"",
                "old\\value", "new\nvalue");
  }

  auto records = unifi::CameraChangeLog::read_all(path);
  check(records.size() == 1, "special: 1 record");
  check(records[0].column == "col-\"test\"", "special: column with quotes");
  check(records[0].old_value == "old\\value",
        "special: old_value with backslash");
  check(records[0].new_value == "new\nvalue",
        "special: new_value with newline");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------
// Test 6: Create on non-writable path
// ---------------------------------------------------------------
static void test_bad_path() {
  auto result = unifi::CameraChangeLog::Create(
      "/nonexistent/directory/log.jsonl");
  check(!result.ok(), "bad_path: Create fails on bad directory");
}

// ---------------------------------------------------------------
// Test 7: Append mode (re-open existing file)
// ---------------------------------------------------------------
static void test_append() {
  std::string path = temp_path("append");
  std::remove(path.c_str());

  // First open: write 2 records.
  {
    auto log = *unifi::CameraChangeLog::Create(path);
    log->record("cam-001", "col", "a", "b");
    log->record("cam-002", "col", "c", "d");
  }

  // Second open: write 1 more record.
  {
    auto log = *unifi::CameraChangeLog::Create(path);
    log->record("cam-003", "col", "e", "f");
  }

  auto records = unifi::CameraChangeLog::read_all(path);
  check(records.size() == 3, "append: 3 records total");
  check(records[0].camera_id == "cam-001", "append: first record preserved");
  check(records[2].camera_id == "cam-003", "append: third record added");

  std::remove(path.c_str());
}

int main() {
  test_roundtrip();
  test_empty_file();
  test_malformed_lines();
  test_concurrent_writes();
  test_special_chars();
  test_bad_path();
  test_append();

  std::cout << "test_camera_change_log: "
            << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
