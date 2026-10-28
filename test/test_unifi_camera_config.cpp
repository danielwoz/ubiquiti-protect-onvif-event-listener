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
 * test_unifi_camera_config.cpp
 *
 * Pure-logic tests for the static helpers exposed via
 * unifi::internal (build_connstr, json_get, pg_array).  These do not
 * require a running PostgreSQL instance.
 */

#include <iostream>
#include <string>
#include <vector>

#include "unifi_camera_config.hpp"

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
// build_connstr
// ---------------------------------------------------------------
static void test_build_connstr_default() {
  unifi::DbConfig db;  // defaults
  std::string s = unifi::internal::build_connstr(db);
  check(s == "host=127.0.0.1 port=5433 dbname=unifi-protect user=postgres",
        "build_connstr: default config, no password");
}

static void test_build_connstr_with_password() {
  unifi::DbConfig db;
  db.password = "secret";
  std::string s = unifi::internal::build_connstr(db);
  check(s ==
        "host=127.0.0.1 port=5433 dbname=unifi-protect user=postgres"
        " password=secret",
        "build_connstr: password appended");
}

static void test_build_connstr_custom() {
  unifi::DbConfig db;
  db.host = "db.example.com";
  db.port = 5432;
  db.dbname = "mydb";
  db.user = "alice";
  std::string s = unifi::internal::build_connstr(db);
  check(s == "host=db.example.com port=5432 dbname=mydb user=alice",
        "build_connstr: custom host/port/db/user");
}

// ---------------------------------------------------------------
// json_get
// ---------------------------------------------------------------
static void test_json_get_string() {
  std::string j = R"({"username":"admin","password":"hunter2"})";
  check(unifi::internal::json_get(j, "username") == "admin",
        "json_get: string value");
  check(unifi::internal::json_get(j, "password") == "hunter2",
        "json_get: second string value");
}

static void test_json_get_missing() {
  std::string j = R"({"a":"1"})";
  check(unifi::internal::json_get(j, "b").empty(),
        "json_get: missing key returns empty");
}

static void test_json_get_null() {
  std::string j = R"({"username":"admin","snapshotUrl":null})";
  check(unifi::internal::json_get(j, "snapshotUrl").empty(),
        "json_get: null value returns empty");
  check(unifi::internal::json_get(j, "username") == "admin",
        "json_get: sibling of null still works");
}

static void test_json_get_bare_token() {
  std::string j = R"({"port":554,"hasAudio":true})";
  check(unifi::internal::json_get(j, "port") == "554",
        "json_get: numeric bare token");
  check(unifi::internal::json_get(j, "hasAudio") == "true",
        "json_get: boolean bare token");
}

static void test_json_get_escapes() {
  // Password containing an escaped quote and a backslash.
  std::string j = R"({"password":"ab\"c\\d"})";
  check(unifi::internal::json_get(j, "password") == "ab\"c\\d",
        "json_get: escaped quote and backslash");
}

static void test_json_get_special_chars() {
  // Values can contain newlines / tabs / forward slashes.
  std::string j = R"({"url":"http:\/\/x","note":"a\nb\tc"})";
  check(unifi::internal::json_get(j, "url") == "http://x",
        "json_get: escaped forward slash");
  check(unifi::internal::json_get(j, "note") == "a\nb\tc",
        "json_get: escaped newline and tab");
}

// ---------------------------------------------------------------
// pg_array
// ---------------------------------------------------------------
static void test_pg_array_empty() {
  std::vector<std::string> ids;
  check(unifi::internal::pg_array(ids) == "{}",
        "pg_array: empty list");
}

static void test_pg_array_single() {
  std::vector<std::string> ids = {"id-1"};
  check(unifi::internal::pg_array(ids) == "{\"id-1\"}",
        "pg_array: single element");
}

static void test_pg_array_multiple() {
  std::vector<std::string> ids = {"a", "b", "c"};
  check(unifi::internal::pg_array(ids) == "{\"a\",\"b\",\"c\"}",
        "pg_array: multiple elements");
}

// ---------------------------------------------------------------
// maybe_rewrite_dahua_snapshot_url (issue #32)
// ---------------------------------------------------------------
static void test_dahua_rewrite_basic() {
  // Exact-type "Dahua" + /onvif/snapshot path -> rewrites to /cgi-bin/...
  const std::string in  = "http://192.168.1.24/onvif/snapshot?channel=1&subtype=0";
  const std::string out = unifi::internal::maybe_rewrite_dahua_snapshot_url(
      "Dahua", in);
  check(out == "http://192.168.1.24/cgi-bin/snapshot.cgi",
        "dahua-rewrite: bare 'Dahua' + /onvif/snapshot path");
}

static void test_dahua_rewrite_model_substring() {
  // "Dahua DH-IPC-..." should also match.
  const std::string in  = "http://10.0.0.50:8080/onvif/snapshot";
  const std::string out = unifi::internal::maybe_rewrite_dahua_snapshot_url(
      "Dahua DH-IPC-HFW3549T1-ZAS-PV", in);
  check(out == "http://10.0.0.50:8080/cgi-bin/snapshot.cgi",
        "dahua-rewrite: model-string substring + host:port preserved");
}

static void test_dahua_rewrite_amcrest_alias() {
  // Amcrest = Dahua OEM; same path bug, same fix.
  const std::string in  = "http://172.16.0.5/onvif/snapshot?channel=1";
  const std::string out = unifi::internal::maybe_rewrite_dahua_snapshot_url(
      "Amcrest IP4M-1051", in);
  check(out == "http://172.16.0.5/cgi-bin/snapshot.cgi",
        "dahua-rewrite: Amcrest alias triggers rewrite");
}

static void test_dahua_rewrite_case_insensitive() {
  // Type matching is case-insensitive.
  const std::string in  = "http://192.168.1.1/onvif/snapshot";
  const std::string out = unifi::internal::maybe_rewrite_dahua_snapshot_url(
      "dAhUa whatever", in);
  check(out == "http://192.168.1.1/cgi-bin/snapshot.cgi",
        "dahua-rewrite: case-insensitive type match");
}

static void test_dahua_rewrite_non_dahua_passthrough() {
  // Non-Dahua cameras pass through unchanged even when they advertise the
  // same path -- belt-and-suspenders: we only rewrite known-broken vendors.
  const std::string in  = "http://192.168.1.10/onvif/snapshot?channel=1";
  const std::string out = unifi::internal::maybe_rewrite_dahua_snapshot_url(
      "Hikvision DS-2CD2", in);
  check(out == in, "dahua-rewrite: non-Dahua type passes through");
}

static void test_dahua_rewrite_non_matching_path() {
  // Right vendor, wrong path -> passes through.  E.g. a Dahua that for once
  // advertises the cgi-bin URL correctly, or a non-broken sub-path.
  const std::string in  = "http://192.168.1.24/cgi-bin/snapshot.cgi";
  const std::string out = unifi::internal::maybe_rewrite_dahua_snapshot_url(
      "Dahua", in);
  check(out == in, "dahua-rewrite: already-correct URL passes through");
}

static void test_dahua_rewrite_false_positive_guard() {
  // "/onvif/snapshots-archive" must NOT match /onvif/snapshot prefix.
  const std::string in  = "http://192.168.1.1/onvif/snapshots-archive/index";
  const std::string out = unifi::internal::maybe_rewrite_dahua_snapshot_url(
      "Dahua", in);
  check(out == in,
        "dahua-rewrite: /onvif/snapshots-* does not match the bad path");
}

static void test_dahua_rewrite_malformed_url() {
  // No scheme://host pattern -> pass through, don't crash.
  const std::string in  = "not-a-url";
  const std::string out = unifi::internal::maybe_rewrite_dahua_snapshot_url(
      "Dahua", in);
  check(out == in, "dahua-rewrite: malformed URL passes through");
}

static void test_dahua_rewrite_empty_inputs() {
  check(unifi::internal::maybe_rewrite_dahua_snapshot_url("", "").empty(),
        "dahua-rewrite: empty type + empty url");
  check(unifi::internal::maybe_rewrite_dahua_snapshot_url(
            "Dahua", "").empty(),
        "dahua-rewrite: empty url passes through");
  check(unifi::internal::maybe_rewrite_dahua_snapshot_url(
            "", "http://x/onvif/snapshot") == "http://x/onvif/snapshot",
        "dahua-rewrite: empty type does not match");
}

// ---------------------------------------------------------------
// LoggedLines
// ---------------------------------------------------------------
static void test_logged_lines_change_only() {
  unifi::internal::LoggedLines logged;
  const std::vector<std::string> streams = {"cam ch0: 1920x1080 h264", "cam ch1: 640x360 h264"};
  check(logged.changed("a", streams), "logged-lines: first load logs");
  check(!logged.changed("a", streams), "logged-lines: same lines on a reload stay quiet");
  check(logged.changed("b", streams), "logged-lines: each camera is tracked on its own");
  const std::vector<std::string> new_bitrate = {"cam ch0: 1920x1080 h264 4096kbps",
                                                "cam ch1: 640x360 h264"};
  check(logged.changed("a", new_bitrate), "logged-lines: a changed line logs again");
  check(!logged.changed("a", new_bitrate), "logged-lines: and then stays quiet");
  check(logged.changed("a", {}), "logged-lines: losing every line is a change");
  check(!logged.changed("a", {}), "logged-lines: no lines twice stays quiet");
}

static void test_logged_lines_retain() {
  unifi::internal::LoggedLines logged;
  const std::vector<std::string> lines = {"cam ch0: 1920x1080 h264"};
  logged.changed("a", lines);
  logged.changed("b", lines);
  logged.retain({"b"});
  check(logged.changed("a", lines), "logged-lines: a camera that comes back logs again");
  check(!logged.changed("b", lines), "logged-lines: a retained camera stays quiet");
}

int main() {
  test_build_connstr_default();
  test_build_connstr_with_password();
  test_build_connstr_custom();

  test_json_get_string();
  test_json_get_missing();
  test_json_get_null();
  test_json_get_bare_token();
  test_json_get_escapes();
  test_json_get_special_chars();

  test_pg_array_empty();
  test_pg_array_single();
  test_pg_array_multiple();

  test_dahua_rewrite_basic();
  test_dahua_rewrite_model_substring();
  test_dahua_rewrite_amcrest_alias();
  test_dahua_rewrite_case_insensitive();
  test_dahua_rewrite_non_dahua_passthrough();
  test_dahua_rewrite_non_matching_path();
  test_dahua_rewrite_false_positive_guard();
  test_dahua_rewrite_malformed_url();
  test_dahua_rewrite_empty_inputs();

  test_logged_lines_change_only();
  test_logged_lines_retain();

  std::cout << "test_unifi_camera_config: "
            << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
