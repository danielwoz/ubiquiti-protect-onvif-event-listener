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

#pragma once

#include <libpq-fe.h>

#include <cstdint>
#include <mutex>  // NOLINT(build/c++11)
#include <string>

namespace onvif {
namespace pg {

// Default per-query wall-clock timeout (milliseconds).
//
// 60s is high enough that legitimate slow INSERTs under load (e.g. during
// the 7-day startup backfill, when motion_poller bursts ~50 events through
// in a few seconds and Protect itself is also writing to the same tables)
// don't trip a false timeout, but short enough that a silently-dropped
// TCP connection to the Protect DB stops a thread for at most a minute
// rather than indefinitely (issue #34: motion_poller hung for hours when
// libpq's ppoll(timeout=NULL) waited on a half-closed socket).
//
// Long-running maintenance queries (e.g. the 30-day coalesce_history
// scan) explicitly pass a longer timeout.
constexpr int kDefaultTimeoutMs = 60'000;

// Drop-in replacement for PQexecParams() that returns nullptr if the
// query does not complete within @p timeout_ms wall-clock milliseconds.
//
// Implementation: PQsendQueryParams() + a poll(socket) loop that
// respects the deadline.  On timeout, sends PQcancel and drains any
// straggler results so the connection is reusable for the next call.
//
// Caller still owns the returned PGresult (PQclear it) and must still
// check PQresultStatus() for SQL-level errors.
//
// timeout_ms <= 0 falls back to kDefaultTimeoutMs.
//
// Logs a single ERROR line on timeout/cancel/connection drop with the
// SQL text truncated to 200 chars so journal forensics can identify
// the responsible query without needing per-call-site logging.
PGresult* ExecParamsWithTimeout(PGconn* conn,
                                 int timeout_ms,
                                 const char* sql,
                                 int n_params,
                                 const Oid* param_types,
                                 const char* const* param_values,
                                 const int* param_lengths,
                                 const int* param_formats,
                                 int result_format);

// No-params variant matching PQexec().  Same timeout semantics.
PGresult* ExecWithTimeout(PGconn* conn, int timeout_ms, const char* sql);

// Variant that returns a single binary result row of arbitrary size,
// matching PQexecParams(..., result_format=1).  Exists as a convenience
// only -- ExecParamsWithTimeout with result_format=1 produces the same
// PGresult.

// Timeout for writes that are nice to have but never required.
constexpr int kBestEffortTimeoutMs = 2'000;

// Back-off for an optional write.  After a timeout the write is skipped
// for base_ms, doubling on each further timeout up to max_ms; a success
// resets the back-off.  A timed-out query whose backend ignores the
// cancel keeps running server-side after the connection is reset, so
// retrying on every call would leak one busy backend per attempt.
// Thread-safe.
class BestEffortGate {
 public:
  explicit BestEffortGate(std::string name,
                          int64_t base_ms = 3'600'000,
                          int64_t max_ms = 86'400'000);

  // True if the write should be attempted now.
  bool allowed();
  // Report the outcome of an attempted write.
  void record(bool timed_out);

  // Test seam: milliseconds from a monotonic clock.
  void set_clock_for_testing(int64_t (*now_ms)());

 private:
  int64_t now() const;

  const std::string name_;
  const int64_t base_ms_;
  const int64_t max_ms_;
  std::mutex mu_;
  int64_t skip_until_ms_ = 0;
  int64_t backoff_ms_ = 0;
  int64_t (*clock_)() = nullptr;
};

// Shared gate for smartDetectObjectAreas inserts (third-party recorder and
// first-party motion poller write the same table).
BestEffortGate& AreaInsertGate();

}  // namespace pg
}  // namespace onvif
