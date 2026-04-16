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

  std::cout << "test_unifi_camera_config: "
            << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
