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

#include "pg_util.hpp"

#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "absl/log/log.h"
#include "pg_stats.hpp"

namespace onvif {
namespace pg {

namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

// Send a best-effort cancel for the current query and drain any
// straggling results so the PGconn ends up in an idle state ready for
// the next caller.  Safe to call after a timeout, a poll error, or a
// PQconsumeInput failure.
//
// If after the drain budget the connection is still mid-query (libpq
// would reject the next PQsendQuery with "another command is already
// in progress"), forcibly PQreset() it.  PQreset is a full close+reopen
// against a local Unix socket; on a typical Dream Router it returns in
// a handful of milliseconds, well within the next caller's tolerance.
void CancelAndDrain(PGconn* conn) {
  PGcancel* cancel = PQgetCancel(conn);
  if (cancel) {
    char errbuf[256] = {0};
    // PQcancel sends a separate cancel request and returns quickly; it
    // does NOT wait for the server's ACK on the original connection.
    PQcancel(cancel, errbuf, sizeof(errbuf));
    PQfreeCancel(cancel);
  }
  // Drain whatever the server flushes back.  5s budget is plenty for
  // the server to deliver the canceled-query error response over a
  // healthy connection; on a truly dead socket the per-poll wait still
  // bounds the per-iteration time.
  const auto drain_deadline = steady_clock::now() + milliseconds(5000);
  bool drained = false;
  while (steady_clock::now() < drain_deadline) {
    if (PQconsumeInput(conn) == 0) break;  // connection broken
    while (PQisBusy(conn) == 0) {
      PGresult* r = PQgetResult(conn);
      if (!r) {
        // PQgetResult returned NULL -> the query is fully terminated
        // and the connection is idle again.
        drained = true;
        break;
      }
      PQclear(r);
    }
    if (drained) return;
    const int sock = PQsocket(conn);
    if (sock < 0) break;
    struct pollfd pfd = {sock, POLLIN, 0};
    // 1s per poll: enough time for a slow cancel-ack to arrive, short
    // enough that a totally-dead connection still exits in 5s total.
    const int prc = poll(&pfd, 1, 1000);
    if (prc < 0 && errno != EINTR) break;
  }
  if (drained) return;
  // Connection is stuck mid-query; force-reset it so the next caller
  // can submit a fresh query.  PQreset closes + reopens the socket;
  // any per-session state (prepared statements, search_path, etc.) is
  // lost, but the recorder doesn't depend on session state.
  LOG(WARNING) << "[pg] connection stuck after cancel; calling PQreset";
  PQreset(conn);
  if (PQstatus(conn) != CONNECTION_OK) {
    LOG(ERROR) << "[pg] PQreset failed: " << PQerrorMessage(conn);
  }
}

std::string TruncateSql(const char* sql) {
  if (!sql) return "<null>";
  std::string s(sql);
  if (s.size() > 200) {
    s.resize(200);
    s += "…";
  }
  // Collapse newlines/tabs to spaces so the log line stays single-line.
  for (char& c : s) {
    if (c == '\n' || c == '\t' || c == '\r') c = ' ';
  }
  return s;
}

}  // namespace

namespace {

// Wait for the in-flight query (already submitted via PQsendQuery or
// PQsendQueryParams) and return the last non-null PGresult.  Returns
// nullptr on timeout, PQconsumeInput failure, poll() error, or invalid
// socket; in all those cases the connection is left drained.
PGresult* WaitForResult(PGconn* conn, int timeout_ms, const char* sql) {
  if (timeout_ms <= 0) timeout_ms = kDefaultTimeoutMs;
  const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
  PGresult* last_result = nullptr;
  while (true) {
    if (PQconsumeInput(conn) == 0) {
      LOG(ERROR) << "[pg] PQconsumeInput failed: " << PQerrorMessage(conn)
                 << " sql=" << TruncateSql(sql);
      if (last_result) PQclear(last_result);
      CancelAndDrain(conn);
      return nullptr;
    }
    while (PQisBusy(conn) == 0) {
      PGresult* r = PQgetResult(conn);
      if (!r) {
        // End of results -- query is complete.  Return the final non-
        // null PGresult (matches PQexec semantics: intermediates from
        // multi-statement strings are discarded; caller gets the last).
        return last_result;
      }
      if (last_result) PQclear(last_result);
      last_result = r;
    }
    const int sock = PQsocket(conn);
    if (sock < 0) {
      LOG(ERROR) << "[pg] PQsocket invalid mid-query"
                 << " sql=" << TruncateSql(sql);
      if (last_result) PQclear(last_result);
      CancelAndDrain(conn);
      return nullptr;
    }
    const auto now = steady_clock::now();
    if (now >= deadline) {
      LOG(ERROR) << "[pg] query timeout after " << timeout_ms << "ms"
                 << " sql=" << TruncateSql(sql);
      if (last_result) PQclear(last_result);
      CancelAndDrain(conn);
      return nullptr;
    }
    const int remaining_ms = static_cast<int>(
        std::chrono::duration_cast<milliseconds>(deadline - now).count());
    struct pollfd pfd = {sock, POLLIN, 0};
    const int prc = poll(&pfd, 1, remaining_ms);
    if (prc < 0) {
      if (errno == EINTR) continue;
      LOG(ERROR) << "[pg] poll() failed: " << std::strerror(errno)
                 << " sql=" << TruncateSql(sql);
      if (last_result) PQclear(last_result);
      CancelAndDrain(conn);
      return nullptr;
    }
    // prc == 0 -> timeout elapsed; loop checks deadline.
    // prc > 0  -> socket readable, loop reads via PQconsumeInput.
  }
}

}  // namespace

PGresult* ExecParamsWithTimeout(PGconn* conn,
                                 int timeout_ms,
                                 const char* sql,
                                 int n_params,
                                 const Oid* param_types,
                                 const char* const* param_values,
                                 const int* param_lengths,
                                 const int* param_formats,
                                 int result_format) {
  if (!conn) return nullptr;
  const auto t0 = std::chrono::steady_clock::now();
  if (PQsendQueryParams(conn, sql, n_params, param_types, param_values,
                        param_lengths, param_formats, result_format) == 0) {
    LOG(ERROR) << "[pg] PQsendQueryParams failed: "
               << PQerrorMessage(conn)
               << " sql=" << TruncateSql(sql);
    return nullptr;
  }
  PGresult* res = WaitForResult(conn, timeout_ms, sql);
  const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - t0).count();
  RecordQueryStats(sql, us, /*timed_out=*/res == nullptr);
  return res;
}

PGresult* ExecWithTimeout(PGconn* conn, int timeout_ms, const char* sql) {
  // Route through PQsendQuery (not PQsendQueryParams) so multi-statement
  // SQL still works -- detection_recorder's startup table check submits
  // four `SELECT 1 FROM <t> LIMIT 0;` statements in one call, and the
  // ...QueryParams family rejects that with "cannot insert multiple
  // commands into a prepared statement".
  if (!conn) return nullptr;
  const auto t0 = std::chrono::steady_clock::now();
  if (PQsendQuery(conn, sql) == 0) {
    LOG(ERROR) << "[pg] PQsendQuery failed: " << PQerrorMessage(conn)
               << " sql=" << TruncateSql(sql);
    return nullptr;
  }
  PGresult* res = WaitForResult(conn, timeout_ms, sql);
  const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - t0).count();
  RecordQueryStats(sql, us, /*timed_out=*/res == nullptr);
  return res;
}

BestEffortGate::BestEffortGate(std::string name, int64_t base_ms,
                               int64_t max_ms)
    : name_(std::move(name)), base_ms_(base_ms), max_ms_(max_ms) {}

int64_t BestEffortGate::now() const {
  if (clock_ != nullptr) return clock_();
  return std::chrono::duration_cast<milliseconds>(
             steady_clock::now().time_since_epoch()).count();
}

bool BestEffortGate::allowed() {
  std::lock_guard<std::mutex> lk(mu_);
  return now() >= skip_until_ms_;
}

void BestEffortGate::record(bool timed_out) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!timed_out) {
    backoff_ms_ = 0;
    return;
  }
  backoff_ms_ = backoff_ms_ == 0 ? base_ms_ : std::min(backoff_ms_ * 2, max_ms_);
  skip_until_ms_ = now() + backoff_ms_;
  LOG(ERROR) << "[pg] " << name_ << " timed out; skipping it for "
             << backoff_ms_ / 60'000 << " min.  The database is not "
             << "completing this write -- check for stuck backends in "
             << "pg_stat_activity.";
}

void BestEffortGate::set_clock_for_testing(int64_t (*now_ms)()) {
  std::lock_guard<std::mutex> lk(mu_);
  clock_ = now_ms;
}

BestEffortGate& AreaInsertGate() {
  static BestEffortGate gate("smartDetectObjectAreas insert");
  return gate;
}

}  // namespace pg
}  // namespace onvif
