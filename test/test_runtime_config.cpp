// Copyright 2026 Daniel W
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Unit tests for runtime_config.  We declare a couple of toy ABSL_FLAGs
// whose names match real schema entries; LoadFromFile then mutates them
// via absl::FindCommandLineFlag.  Each test resets them up-front so they
// run in any order.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/status/status.h"

#include "runtime_config.hpp"

// Use real flag names from the schema so FindCommandLineFlag can look
// them up.  These declarations must come BEFORE the test body so absl
// has registered them by main().
ABSL_FLAG(bool, verbose, false, "test");
ABSL_FLAG(bool, detect_override, false, "test");
ABSL_FLAG(int32_t, coalesce_window_sec, 30, "test");
ABSL_FLAG(std::string, first_party_cameras, "", "test");
ABSL_FLAG(std::string, protect_event_proxy_cameras, "", "test");

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& msg) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << msg << "\n";
  }
}

static std::string tmp_path(const char* suffix) {
  const char* d = std::getenv("TEST_TMPDIR");
  std::string base = d ? d : "/tmp";
  return base + "/runtime_config_test." + suffix;
}

static bool write(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::trunc);
  if (!f.is_open()) return false;
  f << content;
  return f.good();
}

static void reset_flags() {
  absl::SetFlag(&FLAGS_verbose, false);
  absl::SetFlag(&FLAGS_detect_override, false);
  absl::SetFlag(&FLAGS_coalesce_window_sec, 30);
  absl::SetFlag(&FLAGS_first_party_cameras, std::string());
  absl::SetFlag(&FLAGS_protect_event_proxy_cameras, std::string());
}

// ---------------------------------------------------------------
// Test: empty values do not override.
// ---------------------------------------------------------------
static void test_empty_values() {
  reset_flags();
  const std::string path = tmp_path("empty");
  check(write(path,
              R"({"verbose":"","detect_override":"","coalesce_window_sec":""})"),
        "write empty config");
  int n = runtime_config::LoadFromFile(path);
  check(n == 0, "no overrides applied for empty values");
  check(!absl::GetFlag(FLAGS_verbose), "verbose unchanged");
  check(!absl::GetFlag(FLAGS_detect_override), "detect_override unchanged");
  check(absl::GetFlag(FLAGS_coalesce_window_sec) == 30,
        "coalesce_window_sec unchanged");
}

// ---------------------------------------------------------------
// Test: typed values override.
// ---------------------------------------------------------------
static void test_typed_overrides() {
  reset_flags();
  const std::string path = tmp_path("typed");
  check(write(path,
        R"({"verbose":"true","detect_override":"true",)"
        R"("coalesce_window_sec":"120",)"
        R"("first_party_cameras":"id1,id2"})"),
        "write typed config");
  int n = runtime_config::LoadFromFile(path);
  check(n >= 4, "expected >=4 overrides applied (got " +
                    std::to_string(n) + ")");
  check(absl::GetFlag(FLAGS_verbose), "verbose set to true");
  check(absl::GetFlag(FLAGS_detect_override), "detect_override set to true");
  check(absl::GetFlag(FLAGS_coalesce_window_sec) == 120,
        "coalesce_window_sec set to 120");
  check(absl::GetFlag(FLAGS_first_party_cameras) == "id1,id2",
        "first_party_cameras set to CSV");
}

// ---------------------------------------------------------------
// Test: invalid type for a flag is logged and skipped, not fatal.
// ---------------------------------------------------------------
static void test_invalid_value() {
  reset_flags();
  const std::string path = tmp_path("invalid");
  // "banana" is not a valid bool; should be skipped, leaving flag at default.
  check(write(path,
              R"({"verbose":"banana","coalesce_window_sec":"60"})"),
        "write invalid+valid config");
  int n = runtime_config::LoadFromFile(path);
  check(n == 1, "exactly 1 override applied (the int)");
  check(!absl::GetFlag(FLAGS_verbose), "verbose unchanged after rejection");
  check(absl::GetFlag(FLAGS_coalesce_window_sec) == 60,
        "valid int still applied");
}

// ---------------------------------------------------------------
// Test: missing file is a no-op.
// ---------------------------------------------------------------
static void test_missing_file() {
  reset_flags();
  int n = runtime_config::LoadFromFile("/nonexistent/path.json");
  check(n == 0, "missing file -> 0 overrides");
}

// ---------------------------------------------------------------
// Test: ReadFromFile + WriteToFile round-trip every schema key with
// empty defaults when the file is fresh.
// ---------------------------------------------------------------
static void test_write_then_read() {
  const std::string path = tmp_path("roundtrip");
  std::map<std::string, std::string> values;
  values["verbose"]              = "true";
  values["coalesce_window_sec"]  = "45";
  values["first_party_cameras"]  = "abc,def";
  // Intentionally leave others absent -- WriteToFile should fill empties.
  auto s = runtime_config::WriteToFile(path, values);
  check(s.ok(), std::string("write ok: ") + std::string(s.message()));

  auto round = runtime_config::ReadFromFile(path);
  check(round["verbose"] == "true", "read verbose");
  check(round["coalesce_window_sec"] == "45", "read coalesce");
  check(round["first_party_cameras"] == "abc,def", "read first_party_cameras");
  // A schema entry not specified in `values` is written as empty.
  check(round.count("detect_override") == 1 &&
            round["detect_override"].empty(),
        "absent key written as empty");
}

// ---------------------------------------------------------------
// Test: schema is non-empty, all entries have non-null name + group.
// ---------------------------------------------------------------
static void test_schema_sanity() {
  const auto& s = runtime_config::Schema();
  check(!s.empty(), "schema not empty");
  for (const auto& e : s) {
    check(e.name != nullptr && std::string(e.name) != "", "name set");
    check(e.group != nullptr && std::string(e.group) != "", "group set");
    check(e.description != nullptr, "description set");
  }
}

// ---------------------------------------------------------------
// Test: protect_event_proxy_cameras is a String entry in the Cameras
// group, and a saved MAC list overrides the flag.
// ---------------------------------------------------------------
static void test_event_proxy_entry() {
  const runtime_config::Entry* entry = nullptr;
  for (const auto& e : runtime_config::Schema()) {
    if (std::string(e.name) == "protect_event_proxy_cameras") entry = &e;
  }
  check(entry != nullptr, "schema contains protect_event_proxy_cameras");
  if (entry == nullptr) return;
  check(entry->type == runtime_config::Type::String,
        "protect_event_proxy_cameras is a String");
  check(std::string(entry->group) == "Cameras",
        "protect_event_proxy_cameras is in the Cameras group");
  check(std::string(entry->description).find("restarts Protect") !=
            std::string::npos,
        "protect_event_proxy_cameras help mentions the Protect restart");

  reset_flags();
  const std::string path = tmp_path("event_proxy");
  check(write(path,
              R"({"protect_event_proxy_cameras":"F0:00:00:FD:AE:D9,aabbccddeeff"})"),
        "write event proxy config");
  check(runtime_config::LoadFromFile(path) == 1,
        "event proxy MAC list applied");
  check(absl::GetFlag(FLAGS_protect_event_proxy_cameras) ==
            "F0:00:00:FD:AE:D9,aabbccddeeff",
        "protect_event_proxy_cameras set to the saved CSV");
}

// ---------------------------------------------------------------
// Test: object types.  default_object_type takes the four Protect types
// plus motion; a camera_object_types override takes the four types only.
// ---------------------------------------------------------------
static void test_validate_object_type() {
  using runtime_config::ValidateObjectType;
  for (const char* t : {"person", "vehicle", "animal", "package"}) {
    check(ValidateObjectType(t, /*allow_motion=*/true).ok(),
          std::string(t) + " is a valid default_object_type");
    check(ValidateObjectType(t, /*allow_motion=*/false).ok(),
          std::string(t) + " is a valid camera override");
  }
  check(ValidateObjectType("motion", /*allow_motion=*/true).ok(),
        "motion is a valid default_object_type");
  check(ValidateObjectType(" Vehicle ", /*allow_motion=*/false).ok(),
        "case and surrounding spaces are ignored");
  check(runtime_config::NormalizeObjectType("  MoTion\t") == "motion",
        "NormalizeObjectType lower-cases and trims");

  const absl::Status motion_override =
      ValidateObjectType("motion", /*allow_motion=*/false);
  check(!motion_override.ok(), "motion is not a valid camera override");
  check(std::string(motion_override.message()).find("default_object_type") !=
            std::string::npos,
        "the camera override error says where motion applies: " +
            std::string(motion_override.message()));

  const absl::Status dog = ValidateObjectType("dog", /*allow_motion=*/true);
  check(!dog.ok(), "dog is rejected");
  check(dog.code() == absl::StatusCode::kInvalidArgument,
        "unknown type is InvalidArgument");
  check(std::string(dog.message()).find(
            "person, vehicle, animal, package, motion") != std::string::npos,
        "the error names the valid values: " + std::string(dog.message()));
  check(!ValidateObjectType("", /*allow_motion=*/true).ok(),
        "an empty type is not an object type");
  check(!ValidateObjectType("human", /*allow_motion=*/true).ok(),
        "only the documented names are accepted");
}

// ---------------------------------------------------------------
// Test: ValidateValue checks the object-type settings and passes every
// other setting through.
// ---------------------------------------------------------------
static void test_validate_value() {
  using runtime_config::ValidateValue;
  check(ValidateValue("default_object_type", "motion").ok(),
        "default_object_type=motion accepted");
  check(ValidateValue("default_object_type", "animal").ok(),
        "default_object_type=animal accepted");
  check(ValidateValue("default_object_type", "").ok(),
        "clearing default_object_type accepted");
  const absl::Status bad_default =
      ValidateValue("default_object_type", "cat");
  check(!bad_default.ok(), "default_object_type=cat rejected");
  check(std::string(bad_default.message()).find("\"cat\"") !=
            std::string::npos,
        "the error names the rejected value: " +
            std::string(bad_default.message()));

  check(ValidateValue("camera_object_types",
                      "192.168.1.108=animal,192.168.1.109=Package").ok(),
        "valid camera overrides accepted");
  check(ValidateValue("camera_object_types",
                      "192.168.1.108=animal, ,192.168.1.110=,").ok(),
        "empty entries and ip= (no override) accepted");
  const absl::Status motion_cam =
      ValidateValue("camera_object_types",
                    "192.168.1.108=animal,192.168.1.109=motion");
  check(!motion_cam.ok(), "a camera override of motion is rejected");
  check(std::string(motion_cam.message()).find("192.168.1.109") !=
            std::string::npos,
        "the error names the camera: " + std::string(motion_cam.message()));
  check(!ValidateValue("camera_object_types", "192.168.1.108=dog").ok(),
        "an unknown camera override is rejected");
  check(!ValidateValue("camera_object_types", "192.168.1.108").ok(),
        "an entry without = is rejected");
  check(!ValidateValue("camera_object_types", "=animal").ok(),
        "an entry without an ip is rejected");

  check(ValidateValue("first_party_fallback_class", "anything").ok(),
        "other settings are not checked here");
  check(ValidateValue("verbose", "true").ok(), "bool settings pass through");
}

static void test_for_each_ip_value() {
  std::vector<std::pair<std::string, std::string>> got;
  runtime_config::ForEachIpValue(
      " 192.168.1.108=animal, 192.168.1.109 = package ,,bad, =x,"
      "192.168.1.110=",
      [&](const std::string& ip, const std::string& v) {
        got.emplace_back(ip, v);
      });
  check(got.size() == 3, "ForEachIpValue: three usable pairs");
  check(got.size() > 1 && got[0].first == "192.168.1.108" &&
            got[0].second == "animal",
        "ForEachIpValue: leading space trimmed");
  check(got.size() > 1 && got[1].first == "192.168.1.109" &&
            got[1].second == "package",
        "ForEachIpValue: spaces around '=' and after ',' trimmed");
  check(got.size() > 2 && got[2].first == "192.168.1.110" &&
            got[2].second.empty(),
        "ForEachIpValue: ip= gives an empty value");
}

int main(int /*argc*/, char* argv[]) {
  // ParseCommandLine is not strictly required for SetFlag/GetFlag, but
  // calling it ensures any platform-specific absl init runs.
  (void)argv;

  test_schema_sanity();
  test_empty_values();
  test_typed_overrides();
  test_invalid_value();
  test_missing_file();
  test_write_then_read();
  test_event_proxy_entry();
  test_validate_object_type();
  test_validate_value();
  test_for_each_ip_value();

  std::cout << "test_runtime_config: " << g_pass << " passed, "
            << g_fail << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
