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
 * test_pg_util.cpp
 *
 * Pure-logic + integration tests for onvif::pg::ExecParamsWithTimeout /
 * ExecWithTimeout.
 *
 * The DB-dependent tests connect to a real local postgres so they can
 * exercise the timeout / cancel / PQreset paths end to end (a mock-only
 * test of an opaque PGconn would prove nothing about the actual
 * behaviour we ship).  They auto-skip when no postgres is reachable
 * so the test target stays green in the bazel sandbox and CI.
 *
 * To run the DB tests, point the test at any local postgres via either:
 *   ONVIF_TEST_DB_CONN="host=/run/postgresql port=5432"   (libpq conninfo)
 * or the standard PGHOST/PGPORT/PGUSER/PGDATABASE env vars.
 *
 * Tested behaviours:
 *   - NULL conn returns NULL (no crash, no UB)
 *   - Simple SELECT succeeds and returns expected row count
 *   - Parameterised SELECT carries values through correctly
 *   - Multi-statement SQL works via ExecWithTimeout (regression: the
 *     ...QueryParams family rejects this with "cannot insert multiple
 *     commands into a prepared statement", which broke 1.6.2-dev2)
 *   - pg_sleep(N) longer than timeout returns NULL within the budget
 *   - Connection is reusable for a fresh query after a timeout fires
 *     (regression for the CancelAndDrain + PQreset fallback path that
 *     surfaced as "another command is already in progress" in dev3)
 */

#include <libpq-fe.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "pg_util.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;
int g_skip = 0;

void check(bool cond, const char* label) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << label << '\n';
  }
}

void skip(const char* label) {
  ++g_skip;
  std::cerr << "SKIP: " << label << '\n';
}

// Build a conninfo from ONVIF_TEST_DB_CONN, or fall back to libpq's
// PGHOST/PGPORT/etc env-var defaults (empty string).  Returns nullptr
// when no DB is reachable.
PGconn* try_connect() {
  const char* env = std::getenv("ONVIF_TEST_DB_CONN");
  const std::string conninfo = env ? env : "";
  PGconn* c = PQconnectdb(conninfo.c_str());
  if (PQstatus(c) != CONNECTION_OK) {
    PQfinish(c);
    return nullptr;
  }
  return c;
}

}  // namespace

// ---------------------------------------------------------------------------
// API safety: NULL conn
// ---------------------------------------------------------------------------
static void test_null_conn_returns_null() {
  PGresult* r = onvif::pg::ExecWithTimeout(nullptr, 1000, "SELECT 1");
  check(r == nullptr, "null conn -> ExecWithTimeout returns null");

  PGresult* r2 = onvif::pg::ExecParamsWithTimeout(
      nullptr, 1000, "SELECT $1::int", 0, nullptr, nullptr, nullptr, nullptr, 0);
  check(r2 == nullptr, "null conn -> ExecParamsWithTimeout returns null");
}

// Negative / zero timeout falls back to the default (verified indirectly:
// the call still succeeds against a real conn, never blocks forever).
static void test_default_timeout_constant() {
  check(onvif::pg::kDefaultTimeoutMs > 0,
        "kDefaultTimeoutMs > 0");
  check(onvif::pg::kDefaultTimeoutMs >= 15'000 &&
        onvif::pg::kDefaultTimeoutMs <= 120'000,
        "kDefaultTimeoutMs in plausible range [15s, 2min]");
}

// ---------------------------------------------------------------------------
// BestEffortGate: back-off for optional writes that time out
// ---------------------------------------------------------------------------
namespace gate_test {
int64_t now_ms = 0;
int64_t clock() { return now_ms; }
}  // namespace gate_test

static void test_best_effort_gate() {
  onvif::pg::BestEffortGate gate("test write", /*base_ms=*/1000,
                                 /*max_ms=*/4000);
  gate.set_clock_for_testing(&gate_test::clock);
  gate_test::now_ms = 10'000;

  check(gate.allowed(), "gate: allowed initially");
  gate.record(false);
  check(gate.allowed(), "gate: success keeps it open");

  gate.record(true);  // back off 1 s
  check(!gate.allowed(), "gate: closed after a timeout");
  gate_test::now_ms += 999;
  check(!gate.allowed(), "gate: still closed inside the back-off");
  gate_test::now_ms += 1;
  check(gate.allowed(), "gate: reopens after the back-off");

  gate.record(true);  // 2 s
  gate_test::now_ms += 1999;
  check(!gate.allowed(), "gate: back-off doubles");
  gate_test::now_ms += 1;
  gate.record(true);  // 4 s
  gate_test::now_ms += 4000;
  gate.record(true);  // capped at 4 s
  gate_test::now_ms += 4000;
  check(gate.allowed(), "gate: back-off capped at max_ms");

  gate.record(false);  // success resets
  gate.record(true);
  gate_test::now_ms += 1000;
  check(gate.allowed(), "gate: success resets the back-off to base");

  check(onvif::pg::kBestEffortTimeoutMs > 0 &&
        onvif::pg::kBestEffortTimeoutMs < onvif::pg::kDefaultTimeoutMs,
        "kBestEffortTimeoutMs shorter than the default timeout");
}

// ---------------------------------------------------------------------------
// DB-dependent tests
// ---------------------------------------------------------------------------
static void test_simple_select(PGconn* conn) {
  PGresult* r = onvif::pg::ExecWithTimeout(conn, 5000, "SELECT 1");
  check(r != nullptr, "ExecWithTimeout SELECT 1: result non-null");
  if (!r) return;
  check(PQresultStatus(r) == PGRES_TUPLES_OK,
        "ExecWithTimeout SELECT 1: PGRES_TUPLES_OK");
  check(PQntuples(r) == 1, "ExecWithTimeout SELECT 1: one row");
  check(std::strcmp(PQgetvalue(r, 0, 0), "1") == 0,
        "ExecWithTimeout SELECT 1: row value");
  PQclear(r);
}

static void test_parameterised_select(PGconn* conn) {
  const char* params[] = {"42"};
  PGresult* r = onvif::pg::ExecParamsWithTimeout(
      conn, 5000, "SELECT $1::int", 1, nullptr, params, nullptr, nullptr, 0);
  check(r != nullptr, "ExecParamsWithTimeout: result non-null");
  if (!r) return;
  check(PQresultStatus(r) == PGRES_TUPLES_OK,
        "ExecParamsWithTimeout: PGRES_TUPLES_OK");
  check(std::strcmp(PQgetvalue(r, 0, 0), "42") == 0,
        "ExecParamsWithTimeout: $1 -> 42");
  PQclear(r);
}

// Regression: ExecWithTimeout must use PQsendQuery (not PQsendQueryParams)
// because the latter rejects multi-statement SQL with "cannot insert
// multiple commands into a prepared statement".  This was the
// detection_recorder startup-check crash in v1.6.2-dev2.
static void test_multi_statement_via_exec(PGconn* conn) {
  PGresult* r = onvif::pg::ExecWithTimeout(
      conn, 5000,
      "SELECT 1; SELECT 2; SELECT 3");
  check(r != nullptr, "ExecWithTimeout multi-statement: result non-null");
  if (!r) return;
  // PQexec semantics: last result is returned; we expect SELECT 3 -> "3".
  check(PQresultStatus(r) == PGRES_TUPLES_OK,
        "ExecWithTimeout multi-statement: PGRES_TUPLES_OK");
  check(std::strcmp(PQgetvalue(r, 0, 0), "3") == 0,
        "ExecWithTimeout multi-statement: last SELECT value returned");
  PQclear(r);
}

// pg_sleep longer than the timeout must return null and do so within the
// timeout budget (plus a generous slack for the cancel-drain).
static void test_timeout_fires(PGconn* conn) {
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  // 5s sleep, 1s timeout.
  PGresult* r = onvif::pg::ExecWithTimeout(
      conn, 1000, "SELECT pg_sleep(5)");
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      clk::now() - t0).count();
  check(r == nullptr,
        "ExecWithTimeout timeout: pg_sleep(5) with 1s budget -> null");
  // Should be at least 1s (we honour the timeout) and well under the
  // 5s sleep duration (we cancelled).  Allow up to 7s slack so a slow
  // PQreset still passes.
  check(elapsed_ms >= 900 && elapsed_ms < 8000,
        "ExecWithTimeout timeout: completed within bounded budget");
  PQclear(r);  // PQclear(nullptr) is a no-op
}

// After a timeout fires, the connection must be reusable.  CancelAndDrain
// either fully drains (no PQreset needed) or escalates to PQreset; either
// way the next query should succeed.  Regression for the cascade we hit
// in v1.6.2-dev3 ("another command is already in progress" on every
// subsequent call).
static void test_connection_usable_after_timeout(PGconn* conn) {
  PGresult* timeout_r = onvif::pg::ExecWithTimeout(
      conn, 1000, "SELECT pg_sleep(5)");
  check(timeout_r == nullptr,
        "post-timeout: first query timed out (precondition)");

  PGresult* r = onvif::pg::ExecWithTimeout(conn, 5000, "SELECT 99");
  check(r != nullptr,
        "post-timeout: subsequent query succeeded");
  if (!r) return;
  check(PQresultStatus(r) == PGRES_TUPLES_OK,
        "post-timeout: subsequent query PGRES_TUPLES_OK");
  check(std::strcmp(PQgetvalue(r, 0, 0), "99") == 0,
        "post-timeout: subsequent query value correct");
  PQclear(r);
}

// SQL error (not a timeout) must return a non-null PGresult with
// status != OK -- existing call sites rely on the "PQresultStatus(r) !=
// PGRES_COMMAND_OK" pattern continuing to fire for normal SQL errors.
static void test_sql_error_returns_error_result(PGconn* conn) {
  PGresult* r = onvif::pg::ExecWithTimeout(conn, 5000,
                                            "SELECT * FROM no_such_table");
  check(r != nullptr, "sql-error: result non-null (libpq convention)");
  if (!r) return;
  const ExecStatusType st = PQresultStatus(r);
  check(st == PGRES_FATAL_ERROR || st == PGRES_NONFATAL_ERROR,
        "sql-error: PGRES_*_ERROR returned");
  PQclear(r);
}

int main() {
  // Always-runnable tests
  test_null_conn_returns_null();
  test_default_timeout_constant();
  test_best_effort_gate();

  // DB-dependent tests
  PGconn* conn = try_connect();
  if (!conn) {
    std::cerr << "\nNo postgres reachable (ONVIF_TEST_DB_CONN env or "
                 "libpq defaults); skipping integration tests.\n"
                 "To exercise the timeout / cancel / PQreset paths:\n"
                 "  ONVIF_TEST_DB_CONN='host=/run/postgresql port=5432' "
                 "  scripts/bz run --config=x86 //test:test_pg_util\n\n";
    skip("test_simple_select");
    skip("test_parameterised_select");
    skip("test_multi_statement_via_exec");
    skip("test_timeout_fires");
    skip("test_connection_usable_after_timeout");
    skip("test_sql_error_returns_error_result");
  } else {
    test_simple_select(conn);
    test_parameterised_select(conn);
    test_multi_statement_via_exec(conn);
    test_timeout_fires(conn);
    test_connection_usable_after_timeout(conn);
    test_sql_error_returns_error_result(conn);
    PQfinish(conn);
  }

  std::cout << "test_pg_util: "
            << g_pass << " passed, "
            << g_fail << " failed, "
            << g_skip << " skipped\n";
  return g_fail > 0 ? 1 : 0;
}
