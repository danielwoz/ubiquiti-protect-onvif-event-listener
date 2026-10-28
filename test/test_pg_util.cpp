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
 *   - ReconnectGate re-opens a connection the server closed while it was
 *     idle before the next query is sent, and leaves a healthy one (and
 *     its queued notifications) alone; run against a minimal in-process
 *     server, so these always run
 */

#include <arpa/inet.h>
#include <libpq-fe.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>  // NOLINT(build/c++11)
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
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
// ReconnectGate: back-off and log throttling for a lost connection
// ---------------------------------------------------------------------------
namespace {

class CaptureSink : public absl::LogSink {
 public:
  void Send(const absl::LogEntry& entry) override {
    std::lock_guard<std::mutex> lk(mu_);
    lines_.emplace_back(entry.text_message());
    errors_.push_back(entry.log_severity() >= absl::LogSeverity::kError);
  }
  int count(const std::string& needle) { return count_where(needle, false); }
  // Lines at ERROR or above: what the journal shows without --verbose.
  int count_errors(const std::string& needle) {
    return count_where(needle, true);
  }

 private:
  int count_where(const std::string& needle, bool errors_only) {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (std::size_t i = 0; i < lines_.size(); ++i)
      if ((!errors_only || errors_[i]) &&
          lines_[i].find(needle) != std::string::npos)
        ++n;
    return n;
  }

  std::mutex mu_;
  std::vector<std::string> lines_;
  std::vector<bool> errors_;
};

namespace rc_test {
int64_t now_ms = 0;
bool up = true;
bool reset_ok = false;
int resets = 0;
int64_t clock() { return now_ms; }
bool fake_up(PGconn*) { return up; }
bool fake_reset(PGconn*, std::string* error) {
  ++resets;
  if (reset_ok) {
    up = true;
    return true;
  }
  *error = "FATAL: the database system is shutting down";
  return false;
}
void install(onvif::pg::ReconnectGate* gate) {
  gate->set_clock_for_testing(&clock);
  gate->set_conn_ops_for_testing({&fake_up, &fake_reset});
}
}  // namespace rc_test

}  // namespace

static void test_reconnect_gate_backoff() {
  CaptureSink sink;
  absl::AddLogSink(&sink);
  onvif::pg::ReconnectGate gate("rc_test", /*base_ms=*/1000, /*max_ms=*/8000,
                                /*summary_ms=*/60'000);
  rc_test::install(&gate);
  rc_test::now_ms = 100'000;
  rc_test::up = true;
  rc_test::reset_ok = false;
  rc_test::resets = 0;

  check(gate.ensure(nullptr), "reconnect: usable while up");
  check(rc_test::resets == 0, "reconnect: no reset while up");
  check(gate.ms_until_next_attempt() == 0, "reconnect: nothing pending while up");

  // The database goes away: the first attempt is immediate, later ones
  // wait out a back-off that doubles up to max_ms.
  rc_test::up = false;
  check(!gate.ensure(nullptr), "reconnect: not usable while down");
  check(rc_test::resets == 1, "reconnect: first attempt is immediate");
  check(!gate.ensure(nullptr) && rc_test::resets == 1,
        "reconnect: no attempt inside the back-off");
  check(gate.ms_until_next_attempt() == 1000, "reconnect: back-off starts at base");
  rc_test::now_ms += 999;
  gate.ensure(nullptr);
  check(rc_test::resets == 1, "reconnect: still waiting 1 ms before the back-off ends");
  rc_test::now_ms += 1;
  gate.ensure(nullptr);
  check(rc_test::resets == 2, "reconnect: attempt once the back-off ends");
  check(gate.ms_until_next_attempt() == 2000, "reconnect: back-off doubles");
  rc_test::now_ms += 2000;
  gate.ensure(nullptr);
  rc_test::now_ms += 4000;
  gate.ensure(nullptr);
  check(gate.ms_until_next_attempt() == 8000, "reconnect: back-off reaches max");
  rc_test::now_ms += 8000;
  gate.ensure(nullptr);
  check(gate.ms_until_next_attempt() == 8000, "reconnect: back-off capped at max");
  check(rc_test::resets == 5, "reconnect: five attempts so far");

  // A caller calling ensure() every 10 ms through a two-minute outage gets
  // one attempt per back-off, a single loss/failure line, and a summary
  // per minute.
  for (int i = 0; i < 12'000; ++i) {
    rc_test::now_ms += 10;
    gate.ensure(nullptr);
  }
  check(rc_test::resets >= 18 && rc_test::resets <= 21,
        ("reconnect: attempts bounded by the back-off, got " +
         std::to_string(rc_test::resets)).c_str());
  check(sink.count("[rc_test] connection lost") == 1,
        "reconnect: the loss is logged once");
  check(sink.count("[rc_test] reconnect failed: FATAL: the database system is "
                   "shutting down") == 1,
        "reconnect: the first failure is logged once, with the error");
  const int summaries = sink.count("[rc_test] still disconnected after");
  check(summaries == 2,
        ("reconnect: one summary per minute, got " +
         std::to_string(summaries)).c_str());
  check(sink.count_errors("[rc_test] reconnect failed") == 1 &&
            sink.count_errors("[rc_test] still disconnected after") == 2,
        "reconnect: the first failure and the summaries are logged at ERROR, "
        "so a lasting outage stays in the journal without --verbose");

  // Recovery: logged once with the outage length, and the back-off starts
  // over for the next outage.
  rc_test::reset_ok = true;
  rc_test::now_ms += gate.ms_until_next_attempt();
  check(gate.ensure(nullptr), "reconnect: usable after a successful reset");
  check(sink.count("[rc_test] reconnected after") == 1,
        "reconnect: recovery logged with the outage length");
  check(gate.ms_until_next_attempt() == 0, "reconnect: nothing pending once up");

  rc_test::up = false;
  rc_test::reset_ok = false;
  const int before = rc_test::resets;
  check(!gate.ensure(nullptr), "reconnect: second outage detected");
  check(rc_test::resets == before + 1,
        "reconnect: second outage starts with an immediate attempt");
  check(gate.ms_until_next_attempt() == 1000,
        "reconnect: back-off reset to base after recovering");
  check(sink.count("[rc_test] connection lost") == 2,
        "reconnect: each outage logs its loss");

  // The connection comes back without the gate's reset (pg_util re-opens
  // it after a failed query): the outage ends there, and the next one is
  // logged and backed off from scratch.
  rc_test::now_ms += 5000;
  gate.ensure(nullptr);  // second failed attempt of this outage
  rc_test::up = true;
  check(gate.ensure(nullptr), "reconnect: usable when re-opened elsewhere");
  check(sink.count("[rc_test] reconnected after") == 2,
        "reconnect: recovery elsewhere ends the outage");
  check(gate.ms_until_next_attempt() == 0,
        "reconnect: nothing pending after recovery elsewhere");
  rc_test::up = false;
  const int before_third = rc_test::resets;
  check(!gate.ensure(nullptr), "reconnect: third outage detected");
  check(rc_test::resets == before_third + 1,
        "reconnect: third outage starts with an immediate attempt");
  check(sink.count("[rc_test] connection lost") == 3,
        "reconnect: third outage logs its loss");
  check(sink.count("[rc_test] reconnect failed") == 3,
        "reconnect: third outage logs its first failure");
  absl::RemoveLogSink(&sink);
}

// Production defaults: 1 s doubling to 30 s.
static void test_reconnect_gate_defaults() {
  onvif::pg::ReconnectGate gate("rc_defaults");
  rc_test::install(&gate);
  rc_test::now_ms = 500'000;
  rc_test::up = false;
  rc_test::reset_ok = false;
  gate.ensure(nullptr);
  check(gate.ms_until_next_attempt() == 1000, "reconnect defaults: base 1 s");
  for (int i = 0; i < 10; ++i) {
    rc_test::now_ms += gate.ms_until_next_attempt();
    gate.ensure(nullptr);
  }
  check(gate.ms_until_next_attempt() == 30'000, "reconnect defaults: max 30 s");
}

// The real libpq path against a server that is not there: attempts are
// spaced by the back-off however often ensure() is called.
static void test_reconnect_gate_unreachable_server() {
  PGconn* c = PQconnectdb(
      "host=/nonexistent-onvif-recorder-test port=1 connect_timeout=1");
  check(PQstatus(c) == CONNECTION_BAD, "unreachable: connection is bad");
  onvif::pg::ReconnectGate gate("rc_unreachable", /*base_ms=*/60'000,
                                /*max_ms=*/60'000);
  check(!gate.ensure(c), "unreachable: not usable");
  for (int i = 0; i < 100; ++i) gate.ensure(c);
  check(gate.attempts() == 1,
        ("unreachable: one reset inside the back-off, got " +
         std::to_string(gate.attempts())).c_str());
  PQfinish(c);
}

// ---------------------------------------------------------------------------
// ReconnectGate against a server that ends an idle session
// ---------------------------------------------------------------------------
namespace fake_pg {

// What the server does with the first session once it is ready.  Later
// sessions (the reconnect) are always kept open.
enum class First { kKeepOpen, kClose, kFatalThenClose, kNotify };

void put_u32(std::string* out, uint32_t v) {
  const uint32_t be = htonl(v);
  out->append(reinterpret_cast<const char*>(&be), sizeof(be));
}

// One backend message: type byte, length (including itself), body.
std::string message(char type, const std::string& body) {
  std::string m(1, type);
  put_u32(&m, static_cast<uint32_t>(body.size() + 4));
  return m + body;
}

bool read_exact(int fd, char* buf, size_t n) {
  while (n > 0) {
    const ssize_t r = ::recv(fd, buf, n, 0);
    if (r <= 0) return false;
    buf += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

void send_all(int fd, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    const ssize_t w = ::send(fd, data.data() + off, data.size() - off,
                             MSG_NOSIGNAL);
    if (w <= 0) return;
    off += static_cast<size_t>(w);
  }
}

// Minimal Postgres server on 127.0.0.1: reads the startup message,
// answers AuthenticationOk, a few ParameterStatus messages,
// BackendKeyData and ReadyForQuery, then treats the first session as
// `first` says.
class Server {
 public:
  explicit Server(First first) : first_(first) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(listen_fd_, 4);
    socklen_t len = sizeof(addr);
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { run(); });
  }
  ~Server() {
    stop_ = true;
    thread_.join();
    for (int fd : open_) ::close(fd);
    ::close(listen_fd_);
  }

  std::string conninfo() const {
    return "host=127.0.0.1 port=" + std::to_string(port_) +
           " user=test dbname=test sslmode=disable gssencmode=disable"
           " connect_timeout=2";
  }
  int sessions() const { return sessions_.load(); }
  // Wait until the first session's `first` action has been carried out.
  bool wait_first_done() const {
    for (int i = 0; i < 200 && !first_done_.load(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return first_done_.load();
  }

 private:
  void run() {
    while (!stop_) {
      pollfd pfd = {listen_fd_, POLLIN, 0};
      if (::poll(&pfd, 1, 50) <= 0) continue;
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) continue;
      if (!handshake(fd)) {
        ::close(fd);
        continue;
      }
      const bool first = sessions_.fetch_add(1) == 0;
      if (first && first_ == First::kFatalThenClose) {
        std::string err;
        err += 'S'; err += "FATAL"; err += '\0';
        err += 'V'; err += "FATAL"; err += '\0';
        err += 'C'; err += "57P01"; err += '\0';
        err += 'M';
        err += "terminating connection due to administrator command";
        err += '\0';
        err += '\0';
        send_all(fd, message('E', err));
      }
      if (first && (first_ == First::kClose ||
                    first_ == First::kFatalThenClose)) {
        ::close(fd);
      } else {
        if (first && first_ == First::kNotify) {
          std::string body;
          put_u32(&body, 4242);
          body += "cameras_changed";
          body += '\0';
          body += "payload";
          body += '\0';
          send_all(fd, message('A', body));
        }
        open_.push_back(fd);
      }
      if (first) first_done_ = true;
    }
  }

  // Startup message, then the replies a trust-authenticated server sends.
  static bool handshake(int fd) {
    char len_buf[4];
    if (!read_exact(fd, len_buf, 4)) return false;
    uint32_t len = 0;
    std::memcpy(&len, len_buf, 4);
    len = ntohl(len);
    if (len < 8 || len > 10000) return false;
    std::string rest(len - 4, '\0');
    if (!read_exact(fd, &rest[0], rest.size())) return false;
    std::string out;
    std::string auth;
    put_u32(&auth, 0);
    out += message('R', auth);
    for (const char* kv : {"server_version\0" "16.0", "client_encoding\0" "UTF8",
                           "standard_conforming_strings\0" "on"}) {
      const std::string key(kv);
      const std::string value(kv + key.size() + 1);
      out += message('S', key + '\0' + value + '\0');
    }
    std::string key_data;
    put_u32(&key_data, 4242);
    put_u32(&key_data, 99);
    out += message('K', key_data);
    out += message('Z', "I");
    send_all(fd, out);
    return true;
  }

  const First first_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<int> sessions_{0};
  std::atomic<bool> first_done_{false};
  std::vector<int> open_;  // run() thread only, then the destructor
};

// Wait until the client socket has something to read (data or EOF),
// without reading it.
void wait_readable(PGconn* c) {
  pollfd pfd = {PQsocket(c), POLLIN, 0};
  ::poll(&pfd, 1, 1000);
}

}  // namespace fake_pg

// The server ends an idle session, as a Postgres restart does.  libpq
// still reports CONNECTION_OK; the gate must notice and reconnect before
// the caller's next query, instead of letting that query fail.
static void test_reconnect_gate_idle_close(fake_pg::First mode,
                                           const char* name) {
  CaptureSink sink;
  absl::AddLogSink(&sink);
  fake_pg::Server server(mode);
  PGconn* c = PQconnectdb(server.conninfo().c_str());
  const std::string label = std::string("idle close (") + name + "): ";
  check(PQstatus(c) == CONNECTION_OK, (label + "connected").c_str());
  check(server.wait_first_done(), (label + "server ended the session").c_str());
  fake_pg::wait_readable(c);
  check(PQstatus(c) == CONNECTION_OK,
        (label + "libpq has not noticed yet").c_str());
  onvif::pg::ReconnectGate gate("rc_idle");
  check(gate.ensure(c), (label + "usable after ensure()").c_str());
  check(gate.attempts() == 1, (label + "one reconnect").c_str());
  check(server.sessions() == 2, (label + "a new session was opened").c_str());
  check(PQstatus(c) == CONNECTION_OK, (label + "connection is up").c_str());
  check(sink.count("connection lost") == 1,
        (label + "the loss is logged once").c_str());
  check(gate.ensure(c) && gate.attempts() == 1,
        (label + "no further reconnect").c_str());
  absl::RemoveLogSink(&sink);
  PQfinish(c);
}

// A healthy idle connection is left alone, and a notification that is
// waiting on it is still delivered by PQnotifies() afterwards.
static void test_reconnect_gate_idle_healthy() {
  {
    fake_pg::Server server(fake_pg::First::kKeepOpen);
    PGconn* c = PQconnectdb(server.conninfo().c_str());
    check(PQstatus(c) == CONNECTION_OK, "idle healthy: connected");
    check(server.wait_first_done(), "idle healthy: server ready");
    onvif::pg::ReconnectGate gate("rc_healthy");
    for (int i = 0; i < 5; ++i) gate.ensure(c);
    check(gate.ensure(c), "idle healthy: usable");
    check(gate.attempts() == 0, "idle healthy: no reconnect");
    check(server.sessions() == 1, "idle healthy: one session");
    PQfinish(c);
  }
  {
    fake_pg::Server server(fake_pg::First::kNotify);
    PGconn* c = PQconnectdb(server.conninfo().c_str());
    check(server.wait_first_done(), "idle notify: notification sent");
    fake_pg::wait_readable(c);
    onvif::pg::ReconnectGate gate("rc_notify");
    check(gate.ensure(c), "idle notify: usable");
    check(gate.attempts() == 0, "idle notify: no reconnect");
    PGnotify* n = PQnotifies(c);
    check(n != nullptr && std::strcmp(n->relname, "cameras_changed") == 0,
          "idle notify: notification still queued");
    if (n) PQfreemem(n);
    PQfinish(c);
  }
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

// The server ends the session (as a Postgres restart does); the gate
// re-opens it on the next ensure().
static void test_reconnect_gate_recovers(PGconn* conn) {
  onvif::pg::ReconnectGate gate("rc_db");
  check(gate.ensure(conn), "db reconnect: usable at start");
  const int pid = PQbackendPID(conn);
  // Plain PQexec: ExecWithTimeout would re-open the connection itself.
  PQclear(PQexec(conn, "SELECT pg_terminate_backend(pg_backend_pid())"));
  check(PQstatus(conn) == CONNECTION_BAD, "db reconnect: session ended");
  check(gate.ensure(conn), "db reconnect: re-opened");
  check(gate.attempts() == 1, "db reconnect: one attempt");
  check(PQbackendPID(conn) != pid, "db reconnect: new server session");
  PGresult* r = onvif::pg::ExecWithTimeout(conn, 5000, "SELECT 7");
  check(r != nullptr && PQresultStatus(r) == PGRES_TUPLES_OK &&
            std::strcmp(PQgetvalue(r, 0, 0), "7") == 0,
        "db reconnect: query works on the re-opened connection");
  PQclear(r);
}

int main() {
  // Always-runnable tests
  test_null_conn_returns_null();
  test_default_timeout_constant();
  test_best_effort_gate();
  test_reconnect_gate_backoff();
  test_reconnect_gate_defaults();
  test_reconnect_gate_unreachable_server();
  test_reconnect_gate_idle_healthy();
  test_reconnect_gate_idle_close(fake_pg::First::kClose, "EOF");
  test_reconnect_gate_idle_close(fake_pg::First::kFatalThenClose,
                                 "FATAL, then EOF");

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
    skip("test_reconnect_gate_recovers");
  } else {
    test_simple_select(conn);
    test_parameterised_select(conn);
    test_multi_statement_via_exec(conn);
    test_timeout_fires(conn);
    test_connection_usable_after_timeout(conn);
    test_sql_error_returns_error_result(conn);
    test_reconnect_gate_recovers(conn);
    PQfinish(conn);
  }

  std::cout << "test_pg_util: "
            << g_pass << " passed, "
            << g_fail << " failed, "
            << g_skip << " skipped\n";
  return g_fail > 0 ? 1 : 0;
}
