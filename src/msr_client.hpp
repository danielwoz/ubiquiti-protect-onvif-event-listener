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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace onvif {

/**
 * MsrClient — minimal gRPC client for UniFi Media Server Recording (MSR).
 *
 * Talks HTTP/2 cleartext ("prior knowledge") to MSR's unary RPC service on
 * 127.0.0.1:7700 via libcurl+nghttp2.  The only method we use is
 * unifi.media_server.recording.v1.RecordingAPI.StoreSnapshots, which causes
 * MSR to persist a JPEG as a native UBV thumbnail owned by ms:unifi-streaming
 * and returns an id (e.g. "FC5F49CA68D4-1776861156551") that UniFi Protect's
 * UI serves via MSP TCP on port 7701 — indistinguishable from first-party
 * camera thumbnails.
 *
 * Thread-safe: StoreSnapshot() uses a fresh curl handle per call.  When
 * single-flight is enabled (set_serialize(true)) calls are also globally
 * serialised by a mutex so MSR sees at most one in-flight StoreSnapshots
 * from this process — preventing us from saturating MSR's worker pool and
 * indirectly stalling Protect's own video-segment queries.
 */
class MsrClient {
 public:
  // url is the MSR base URL, e.g. "http://127.0.0.1:7700".
  explicit MsrClient(std::string url);

  // Calls RecordingAPI.StoreSnapshots with a single parameters entry
  // { mac, snapshot=jpeg }.  Returns the native snapshot id on success,
  // empty string on failure (network, gRPC error, or malformed response).
  std::string StoreSnapshot(const std::string& mac,
                            const void* jpeg, std::size_t jpeg_len);

  // Why a StoreSnapshot call did or did not return an id.
  enum class Outcome {
    kOk,
    kSkipped,    // bad arguments, or suspended and still cooling down
    kTransport,  // curl / HTTP / framing failure
    kGrpc,       // MSR answered with a non-zero grpc-status
    kRejected,   // MSR answered OK but stored nothing for this camera
                 // (e.g. it is not recording it); MSR itself is healthy
  };

  // As above, reporting the outcome.  kRejected does not count towards
  // suspension: one camera MSR will not record must not pause forwarding
  // for every other camera.
  std::string StoreSnapshot(const std::string& mac,
                            const void* jpeg, std::size_t jpeg_len,
                            Outcome* outcome);

  // Value a PerformFn returns to simulate kRejected.
  static constexpr const char* kRejectedForTesting = "\x01rejected";

  // When true, only one StoreSnapshot call may execute at a time across
  // all threads; concurrent callers block on a mutex.  Default false
  // (backwards-compatible: every caller runs concurrently).
  void set_serialize(bool on);

  // When > 0 and the client has entered the "suspended" state (5 consecutive
  // failures), subsequent calls return "" immediately without contacting MSR
  // until @p sec seconds have elapsed since the last failure.  After the
  // cool-down a single probe call is allowed; success clears suspension,
  // failure restarts the cool-down.  Default 0 (cool-down disabled — every
  // call attempts MSR even while suspended).
  void set_suspend_cooldown(int sec);

  // Test hook: override the wall-clock source used for cool-down checks.
  // Returns milliseconds since some epoch.  Default uses CLOCK_MONOTONIC.
  void set_clock_for_testing(std::int64_t (*now_ms)());

  // Test hook: replaces the curl/network call.  When set, StoreSnapshot
  // calls @p fn(mac, jpeg) after the mutex + cool-down gates and treats
  // its return as the gRPC response id (empty = failure).  Test fixtures
  // use this to exercise serialisation and cool-down without standing
  // up an MSR fake.  Pass nullptr to clear.
  using PerformFn = std::string (*)(
      const std::string& mac, const std::string& jpeg);
  void set_perform_fn_for_testing(PerformFn fn);

  // Test hook: force the suspended state so tests can verify the cool-down
  // skip without making 5 real failures.
  void set_suspended_for_testing(bool s);

  // Test hook: directly seed the last-failure timestamp (ms).
  void set_last_failure_for_testing(std::int64_t ms);

  // Test observability: number of times a real network/perform call was
  // attempted (i.e. mutex acquired and not skipped by cool-down).
  std::int64_t perform_count_for_testing() const;

 private:
  std::string url_;

  // Single-flight mutex: held for the entire StoreSnapshot call (curl
  // perform + response parse) when serialize_ is true.  Recursive locking
  // is impossible — one camera thread, one acquisition.
  std::mutex single_flight_mu_;
  std::atomic<bool> serialize_{false};

  // Cool-down: when > 0 ms and suspended_ is true, skip calls until
  // last_failure_ms_ + cooldown_ms_ has elapsed.
  std::atomic<int>  cooldown_ms_{0};
  std::atomic<std::int64_t> last_failure_ms_{0};

  // Test hook: see set_clock_for_testing().
  std::int64_t (*clock_fn_)() = nullptr;

  // Test hook: see set_perform_fn_for_testing().
  PerformFn perform_fn_{nullptr};
  std::atomic<std::int64_t> perform_count_{0};

  // Connection-state diagnostics: one log line on first successful store
  // ("connected to <url>"), one on transition into the "suspended" state
  // after kSuspendThreshold consecutive failures.  Reset on next success.
  std::atomic<bool> seen_first_success_{false};
  std::atomic<int>  consecutive_failures_{0};
  std::atomic<bool> suspended_{false};
};

namespace msr_client_internal {

// Exposed for unit tests: build a serialised StoreSnapshotsRequest protobuf
// (not yet wrapped in the 5-byte gRPC length-prefixed frame).
std::string build_store_request(const std::string& mac,
                                const void* jpeg, std::size_t jpeg_len);

// Exposed for unit tests: parse the inner StoreSnapshotsResponse message
// (after stripping the 5-byte gRPC prefix) and return the first result's id.
// Returns empty string on any parse error or if id is missing.
std::string parse_store_response(const void* msg, std::size_t msg_len);

}  // namespace msr_client_internal

}  // namespace onvif
