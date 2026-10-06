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
 * test_msr_client.cpp
 *
 * Wire-format tests for build_store_request / parse_store_response.
 * Does not call out to MSR — the actual gRPC transport is covered
 * only by integration testing against a live router.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "msr_client.hpp"

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

// Minimal protobuf varint encoder used for constructing test inputs.
static void append_varint(std::string* out, uint64_t v) {
  while (v >= 0x80) {
    out->push_back(static_cast<char>((v & 0x7fU) | 0x80U));
    v >>= 7;
  }
  out->push_back(static_cast<char>(v));
}

static void append_tag(std::string* out, uint32_t fnum, uint32_t wtype) {
  append_varint(out, (static_cast<uint64_t>(fnum) << 3) | wtype);
}

static void append_len(std::string* out, uint32_t fnum,
                       const std::string& data) {
  append_tag(out, fnum, 2);
  append_varint(out, data.size());
  out->append(data);
}

// ---------------------------------------------------------------
// build_store_request
// ---------------------------------------------------------------
static void test_build_store_request_shape() {
  // StoreSnapshotsRequest wraps a single parameters message containing
  // mac (field 1 string) + snapshot (field 2 bytes).  Verify round-trip.
  const std::string mac = "FC5F49CA68D4";
  const std::string jpeg = "\xff\xd8\xff\xe0HELLO\xff\xd9";
  std::string req = onvif::msr_client_internal::build_store_request(
      mac, jpeg.data(), jpeg.size());

  // First tag must be field=1, wtype=2 (parameters).
  check(!req.empty(), "build_store_request non-empty");
  check(static_cast<uint8_t>(req[0]) == ((1U << 3) | 2U),
        "outer field tag = 0x0a");

  // Manually parse: skip outer tag+length, parse inner submessage.
  std::size_t pos = 1;
  // Outer length varint — small enough to be one byte for our sizes.
  uint8_t outer_len = static_cast<uint8_t>(req[pos++]);
  check(outer_len == req.size() - 2,
        "outer length matches remaining bytes");

  // Inner: field 1 (mac) string.
  check(static_cast<uint8_t>(req[pos]) == ((1U << 3) | 2U),
        "mac field tag = 0x0a");
  ++pos;
  uint8_t mac_len = static_cast<uint8_t>(req[pos++]);
  check(mac_len == mac.size(), "mac length matches");
  check(req.substr(pos, mac.size()) == mac, "mac bytes match");
  pos += mac_len;

  // Inner: field 2 (snapshot) bytes.
  check(static_cast<uint8_t>(req[pos]) == ((2U << 3) | 2U),
        "snapshot field tag = 0x12");
  ++pos;
  uint8_t jpg_len = static_cast<uint8_t>(req[pos++]);
  check(jpg_len == jpeg.size(), "jpeg length matches");
  check(req.substr(pos, jpeg.size()) == jpeg, "jpeg bytes match");
}

static void test_build_store_request_large_jpeg() {
  // Verify varint length encoding for >= 128 byte payloads (2-byte varint).
  const std::string mac = "AABBCCDDEEFF";
  std::string jpeg(10000, '\x00');  // 10 KB of zeros
  std::string req = onvif::msr_client_internal::build_store_request(
      mac, jpeg.data(), jpeg.size());
  // Request wraps params wraps mac+snapshot.  Total size
  // should be > 10000 and < 10064.
  check(req.size() > jpeg.size(), "request includes payload");
  check(req.size() < jpeg.size() + 64, "request overhead is small");
}

static void test_build_store_request_empty_inputs() {
  // Empty mac and empty jpeg still produce a valid shape (two zero-length
  // length-delimited fields inside the parameters submessage).
  std::string req = onvif::msr_client_internal::build_store_request(
      "", "", 0);
  // Expected bytes:
  //   outer tag (0a) + outer len (04) +
  //   inner-mac tag (0a) + inner-mac len (00) +
  //   inner-snap tag (12) + inner-snap len (00)
  const uint8_t kExpected[] = {0x0a, 0x04, 0x0a, 0x00, 0x12, 0x00};
  check(req.size() == sizeof(kExpected), "empty-input req length is 6");
  if (req.size() == sizeof(kExpected)) {
    check(std::memcmp(req.data(), kExpected, sizeof(kExpected)) == 0,
          "empty-input req bytes match expected");
  }
}

// ---------------------------------------------------------------
// parse_store_response
// ---------------------------------------------------------------
static std::string make_response_with_id(const std::string& id) {
  // StoreSnapshotResult { id:1 string }
  std::string result;
  append_len(&result, 1, id);
  // StoreSnapshotsResponse { results:1 repeated StoreSnapshotResult }
  std::string resp;
  append_len(&resp, 1, result);
  return resp;
}

static void test_parse_store_response_basic() {
  std::string resp = make_response_with_id("FC5F49CA68D4-1776861156551");
  std::string id = onvif::msr_client_internal::parse_store_response(
      resp.data(), resp.size());
  check(id == "FC5F49CA68D4-1776861156551",
        "parse_store_response: basic id extracted");
}

static void test_parse_store_response_empty_response() {
  std::string id = onvif::msr_client_internal::parse_store_response("", 0);
  check(id.empty(), "parse_store_response: empty body -> empty id");
}

static void test_parse_store_response_no_results() {
  // Well-formed response with no results field.  Just append an unrelated
  // varint field so the decoder has something to walk past — but walk past
  // must still terminate cleanly.  Use a field we don't care about at
  // wire-type 2 with zero length.
  std::string resp;
  append_len(&resp, 99, "");
  std::string id = onvif::msr_client_internal::parse_store_response(
      resp.data(), resp.size());
  check(id.empty(), "parse_store_response: no results -> empty id");
}

static void test_parse_store_response_result_missing_id() {
  // results present, but the StoreSnapshotResult only has field 2 (size).
  std::string size_msg;
  append_varint(&size_msg, (1U << 3) | 0);  // field 1 width (varint)
  append_varint(&size_msg, 640);
  std::string result;
  append_len(&result, 2, size_msg);
  std::string resp;
  append_len(&resp, 1, result);
  std::string id = onvif::msr_client_internal::parse_store_response(
      resp.data(), resp.size());
  check(id.empty(),
        "parse_store_response: result without id -> empty id");
}

static void test_parse_store_response_truncated_length() {
  // A results field claiming length 10 but only 2 bytes of payload.
  std::string resp;
  append_tag(&resp, 1, 2);
  append_varint(&resp, 10);
  resp.append(2, '\x00');
  std::string id = onvif::msr_client_internal::parse_store_response(
      resp.data(), resp.size());
  check(id.empty(),
        "parse_store_response: truncated length -> empty id");
}

// ---------------------------------------------------------------
// Roundtrip
// ---------------------------------------------------------------
static void test_roundtrip_empty_api_does_not_crash() {
  onvif::MsrClient client("");
  // Empty URL short-circuits without ever calling curl.
  std::string id = client.StoreSnapshot("FC5F49CA68D4", "X", 1);
  check(id.empty(), "empty URL returns empty id");
}

// ---------------------------------------------------------------
// Single-flight serialisation
// ---------------------------------------------------------------
//
// Test perform-fn that holds the mutex long enough for the test to
// observe overlap.  Increments g_concurrent on entry, decrements on
// exit, tracks the maximum observed concurrent count globally so the
// callback can be a plain C function pointer.
namespace serialize_test {
static std::atomic<int> in_flight{0};
static std::atomic<int> peak_concurrent{0};
static std::atomic<int> calls_seen{0};

static std::string slow_perform(const std::string& /*mac*/,
                                const std::string& /*jpeg*/) {
  int now = in_flight.fetch_add(1) + 1;
  // Track the peak so we can assert it stayed at 1 with serialize on.
  int prev = peak_concurrent.load();
  while (now > prev && !peak_concurrent.compare_exchange_weak(prev, now)) {
    /* retry */
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  in_flight.fetch_sub(1);
  calls_seen.fetch_add(1);
  return "ok-id";
}
}  // namespace serialize_test

static void run_concurrent_calls(onvif::MsrClient* client, int n_threads) {
  serialize_test::in_flight.store(0);
  serialize_test::peak_concurrent.store(0);
  serialize_test::calls_seen.store(0);
  std::vector<std::thread> ts;
  ts.reserve(n_threads);
  for (int i = 0; i < n_threads; ++i) {
    ts.emplace_back([client]() {
      // Non-empty url + mac + jpeg so we don't hit the empty-input
      // short-circuit.
      client->StoreSnapshot("AABBCCDDEEFF", "JPEG", 4);
    });
  }
  for (auto& t : ts) t.join();
}

static void test_serialize_caps_concurrency_to_one() {
  onvif::MsrClient client("http://127.0.0.1:1");  // url non-empty; perform-fn replaces curl
  client.set_perform_fn_for_testing(&serialize_test::slow_perform);
  client.set_serialize(true);

  run_concurrent_calls(&client, 8);

  check(serialize_test::calls_seen.load() == 8,
        "serialize: all 8 calls completed");
  check(serialize_test::peak_concurrent.load() == 1,
        "serialize: peak concurrent calls capped at 1");
}

static void test_serialize_off_allows_concurrency() {
  onvif::MsrClient client("http://127.0.0.1:1");
  client.set_perform_fn_for_testing(&serialize_test::slow_perform);
  client.set_serialize(false);

  run_concurrent_calls(&client, 8);

  check(serialize_test::calls_seen.load() == 8,
        "no-serialize: all 8 calls completed");
  check(serialize_test::peak_concurrent.load() > 1,
        "no-serialize: observed concurrent calls > 1");
}

// ---------------------------------------------------------------
// Suspension cool-down
// ---------------------------------------------------------------
namespace cooldown_test {
static std::atomic<int> calls_seen{0};
static std::string ok_perform(const std::string& /*mac*/,
                              const std::string& /*jpeg*/) {
  calls_seen.fetch_add(1);
  return "ok-id";
}
static std::int64_t fake_now_ms = 0;
static std::int64_t clock() { return fake_now_ms; }
}  // namespace cooldown_test

static void test_cooldown_skips_calls_when_suspended_within_window() {
  onvif::MsrClient client("http://127.0.0.1:1");
  client.set_perform_fn_for_testing(&cooldown_test::ok_perform);
  client.set_clock_for_testing(&cooldown_test::clock);
  client.set_suspend_cooldown(10);  // 10s

  cooldown_test::fake_now_ms = 1'000'000;
  client.set_last_failure_for_testing(cooldown_test::fake_now_ms);
  client.set_suspended_for_testing(true);

  cooldown_test::calls_seen.store(0);
  // 5 seconds after last failure: still inside cool-down → must skip.
  cooldown_test::fake_now_ms = 1'005'000;
  std::string id = client.StoreSnapshot("AABBCCDDEEFF", "X", 1);
  check(id.empty(), "cooldown: returns empty inside window");
  check(cooldown_test::calls_seen.load() == 0,
        "cooldown: perform-fn NOT invoked inside window");
}

static void test_cooldown_allows_probe_after_window_elapses() {
  onvif::MsrClient client("http://127.0.0.1:1");
  client.set_perform_fn_for_testing(&cooldown_test::ok_perform);
  client.set_clock_for_testing(&cooldown_test::clock);
  client.set_suspend_cooldown(10);

  cooldown_test::fake_now_ms = 2'000'000;
  client.set_last_failure_for_testing(cooldown_test::fake_now_ms);
  client.set_suspended_for_testing(true);

  cooldown_test::calls_seen.store(0);
  // 11 seconds after last failure: cool-down elapsed → probe allowed.
  cooldown_test::fake_now_ms = 2'011'000;
  std::string id = client.StoreSnapshot("AABBCCDDEEFF", "X", 1);
  check(id == "ok-id", "cooldown: probe call returns id after window");
  check(cooldown_test::calls_seen.load() == 1,
        "cooldown: perform-fn invoked exactly once after window");
}

static void test_cooldown_disabled_always_attempts() {
  onvif::MsrClient client("http://127.0.0.1:1");
  client.set_perform_fn_for_testing(&cooldown_test::ok_perform);
  client.set_clock_for_testing(&cooldown_test::clock);
  // cooldown=0 (default) — suspension is not honoured at the call site.
  client.set_suspended_for_testing(true);
  client.set_last_failure_for_testing(0);

  cooldown_test::calls_seen.store(0);
  cooldown_test::fake_now_ms = 100;
  std::string id = client.StoreSnapshot("AABBCCDDEEFF", "X", 1);
  check(id == "ok-id", "cooldown=0: call still attempted while suspended");
  check(cooldown_test::calls_seen.load() == 1,
        "cooldown=0: perform-fn invoked");
}

namespace rejected_test {
static std::string rejecting_perform(const std::string& /*mac*/,
                                     const std::string& /*jpeg*/) {
  return onvif::MsrClient::kRejectedForTesting;
}
static std::string failing_perform(const std::string& /*mac*/,
                                   const std::string& /*jpeg*/) {
  return "";
}
}  // namespace rejected_test

// MSR refusing one camera (e.g. not recording it) is reported as kRejected
// and must not suspend forwarding for every other camera.
static void test_rejection_reported_and_does_not_suspend() {
  onvif::MsrClient client("http://127.0.0.1:1");
  client.set_perform_fn_for_testing(&rejected_test::rejecting_perform);
  client.set_clock_for_testing(&cooldown_test::clock);
  client.set_suspend_cooldown(30);
  cooldown_test::fake_now_ms = 3'000'000;

  onvif::MsrClient::Outcome out = onvif::MsrClient::Outcome::kOk;
  for (int i = 0; i < 6; ++i) {
    std::string id = client.StoreSnapshot("AABBCCDDEEFF", "X", 1, &out);
    check(id.empty(), "rejected: no id returned");
  }
  check(out == onvif::MsrClient::Outcome::kRejected,
        "rejected: outcome is kRejected");

  client.set_perform_fn_for_testing(&cooldown_test::ok_perform);
  cooldown_test::calls_seen.store(0);
  std::string id = client.StoreSnapshot("112233445566", "X", 1, &out);
  check(id == "ok-id" && out == onvif::MsrClient::Outcome::kOk,
        "rejected: another camera still forwarded");
  check(cooldown_test::calls_seen.load() == 1,
        "rejected: forwarding not suspended");
}

static void test_transport_failures_still_suspend() {
  onvif::MsrClient client("http://127.0.0.1:1");
  client.set_perform_fn_for_testing(&rejected_test::failing_perform);
  client.set_clock_for_testing(&cooldown_test::clock);
  client.set_suspend_cooldown(30);
  cooldown_test::fake_now_ms = 4'000'000;

  onvif::MsrClient::Outcome out = onvif::MsrClient::Outcome::kOk;
  for (int i = 0; i < 5; ++i)
    client.StoreSnapshot("AABBCCDDEEFF", "X", 1, &out);
  check(out == onvif::MsrClient::Outcome::kTransport,
        "transport: outcome is kTransport");

  client.set_perform_fn_for_testing(&cooldown_test::ok_perform);
  cooldown_test::calls_seen.store(0);
  client.StoreSnapshot("112233445566", "X", 1, &out);
  check(cooldown_test::calls_seen.load() == 0 &&
            out == onvif::MsrClient::Outcome::kSkipped,
        "transport: five failures suspend forwarding");
}

int main() {
  test_build_store_request_shape();
  test_build_store_request_large_jpeg();
  test_build_store_request_empty_inputs();
  test_parse_store_response_basic();
  test_parse_store_response_empty_response();
  test_parse_store_response_no_results();
  test_parse_store_response_result_missing_id();
  test_parse_store_response_truncated_length();
  test_roundtrip_empty_api_does_not_crash();
  test_serialize_caps_concurrency_to_one();
  test_serialize_off_allows_concurrency();
  test_cooldown_skips_calls_when_suspended_within_window();
  test_cooldown_allows_probe_after_window_elapses();
  test_cooldown_disabled_always_attempts();
  test_rejection_reported_and_does_not_suspend();
  test_transport_failures_still_suspend();

  std::cerr << "\nResult: " << g_pass << " passed, "
            << g_fail << " failed\n";
  return g_fail == 0 ? 0 : 1;
}
