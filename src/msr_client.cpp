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

#include "msr_client.hpp"

#include <curl/curl.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "absl/log/log.h"

namespace onvif {

namespace {

// ============================================================
// Minimal protobuf wire format encoder (fields we need)
// ============================================================

void append_varint(std::string* out, uint64_t v) {
  while (v >= 0x80) {
    out->push_back(static_cast<char>((v & 0x7fU) | 0x80U));
    v >>= 7;
  }
  out->push_back(static_cast<char>(v));
}

void append_tag(std::string* out, uint32_t field_num, uint32_t wire_type) {
  append_varint(out,
                (static_cast<uint64_t>(field_num) << 3) | wire_type);
}

// Append a length-delimited (wire type 2) field: tag + varint(len) + bytes.
void append_len_delim(std::string* out, uint32_t field_num,
                      const char* data, std::size_t len) {
  append_tag(out, field_num, 2);
  append_varint(out, static_cast<uint64_t>(len));
  out->append(data, len);
}

// ============================================================
// Minimal protobuf wire format decoder (fields we need)
// ============================================================

// Reads a varint at *pos and advances.  Returns false on EOF / overlong.
bool read_varint(const uint8_t* buf, std::size_t len, std::size_t* pos,
                 uint64_t* out) {
  uint64_t result = 0;
  int shift = 0;
  while (*pos < len) {
    uint8_t b = buf[(*pos)++];
    result |= static_cast<uint64_t>(b & 0x7fU) << shift;
    if ((b & 0x80U) == 0) {
      *out = result;
      return true;
    }
    shift += 7;
    if (shift > 63) return false;
  }
  return false;
}

// ============================================================
// libcurl callbacks
// ============================================================

struct CurlCtx {
  std::string body;
  int grpc_status = -1;
  std::string grpc_message;
};

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb,
                     void* userdata) {
  auto* ctx = static_cast<CurlCtx*>(userdata);
  std::size_t n = size * nmemb;
  ctx->body.append(ptr, n);
  return n;
}

bool starts_with_ci(const char* line, std::size_t line_len,
                    const char* prefix) {
  std::size_t plen = std::strlen(prefix);
  if (line_len < plen) return false;
  for (std::size_t i = 0; i < plen; ++i) {
    if (std::tolower(static_cast<unsigned char>(line[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

std::size_t header_cb(char* buf, std::size_t size, std::size_t nitems,
                      void* userdata) {
  auto* ctx = static_cast<CurlCtx*>(userdata);
  std::size_t n = size * nitems;
  // gRPC status is delivered in HTTP/2 trailers, which libcurl surfaces via
  // this callback just like headers.  Look for "grpc-status:" (case-insensitive).
  if (starts_with_ci(buf, n, "grpc-status:")) {
    const char* v = buf + std::strlen("grpc-status:");
    std::size_t vlen = n - std::strlen("grpc-status:");
    while (vlen > 0 && (*v == ' ' || *v == '\t')) {
      ++v;
      --vlen;
    }
    ctx->grpc_status = std::atoi(v);
  } else if (starts_with_ci(buf, n, "grpc-message:")) {
    const char* v = buf + std::strlen("grpc-message:");
    std::size_t vlen = n - std::strlen("grpc-message:");
    while (vlen > 0 && (*v == ' ' || *v == '\t')) {
      ++v;
      --vlen;
    }
    ctx->grpc_message.assign(v, vlen);
    while (!ctx->grpc_message.empty() &&
           (ctx->grpc_message.back() == '\r' ||
            ctx->grpc_message.back() == '\n')) {
      ctx->grpc_message.pop_back();
    }
  }
  return n;
}

}  // namespace

// ============================================================
// Internal helpers (also exposed for tests)
// ============================================================
namespace msr_client_internal {

std::string build_store_request(const std::string& mac,
                                const void* jpeg, std::size_t jpeg_len) {
  // StoreSnapshotParameters { mac:1 (string), snapshot:2 (bytes) }
  std::string params;
  append_len_delim(&params, 1, mac.data(), mac.size());
  append_len_delim(&params, 2,
                   static_cast<const char*>(jpeg), jpeg_len);
  // StoreSnapshotsRequest { parameters:1 repeated StoreSnapshotParameters }
  std::string req;
  append_len_delim(&req, 1, params.data(), params.size());
  return req;
}

std::string parse_store_response(const void* msg, std::size_t msg_len) {
  const auto* buf = static_cast<const uint8_t*>(msg);
  std::size_t pos = 0;
  // StoreSnapshotsResponse { results:1 repeated StoreSnapshotResult }
  while (pos < msg_len) {
    uint64_t tag;
    if (!read_varint(buf, msg_len, &pos, &tag)) return "";
    uint32_t field = static_cast<uint32_t>(tag >> 3);
    uint32_t wtype = static_cast<uint32_t>(tag & 7U);
    if (wtype != 2) return "";  // unexpected; spec says all fields are msgs
    uint64_t flen;
    if (!read_varint(buf, msg_len, &pos, &flen) ||
        pos + flen > msg_len) {
      return "";
    }
    if (field == 1) {
      // StoreSnapshotResult { id:1 string (optional), size:2 Size (optional) }
      std::size_t rpos = pos;
      std::size_t rend = pos + static_cast<std::size_t>(flen);
      while (rpos < rend) {
        uint64_t rtag;
        if (!read_varint(buf, msg_len, &rpos, &rtag)) return "";
        uint32_t rfield = static_cast<uint32_t>(rtag >> 3);
        uint32_t rwtype = static_cast<uint32_t>(rtag & 7U);
        if (rwtype != 2) return "";
        uint64_t rslen;
        if (!read_varint(buf, msg_len, &rpos, &rslen) ||
            rpos + rslen > rend) {
          return "";
        }
        if (rfield == 1) {
          return std::string(
              reinterpret_cast<const char*>(buf + rpos),
              static_cast<std::size_t>(rslen));
        }
        rpos += static_cast<std::size_t>(rslen);
      }
    }
    pos += static_cast<std::size_t>(flen);
  }
  return "";
}

}  // namespace msr_client_internal

// ============================================================
// MsrClient
// ============================================================

MsrClient::MsrClient(std::string url) : url_(std::move(url)) {}

void MsrClient::set_serialize(bool on) {
  serialize_.store(on);
}

void MsrClient::set_suspend_cooldown(int sec) {
  cooldown_ms_.store(sec > 0 ? sec * 1000 : 0);
}

void MsrClient::set_clock_for_testing(std::int64_t (*now_ms)()) {
  clock_fn_ = now_ms;
}

void MsrClient::set_perform_fn_for_testing(MsrClient::PerformFn fn) {
  perform_fn_ = fn;
}

void MsrClient::set_suspended_for_testing(bool s) {
  suspended_.store(s);
}

void MsrClient::set_last_failure_for_testing(std::int64_t ms) {
  last_failure_ms_.store(ms);
}

std::int64_t MsrClient::perform_count_for_testing() const {
  return perform_count_.load();
}

namespace {

std::int64_t monotonic_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

std::string MsrClient::StoreSnapshot(const std::string& mac,
                                     const void* jpeg,
                                     std::size_t jpeg_len) {
  return StoreSnapshot(mac, jpeg, jpeg_len, nullptr);
}

std::string MsrClient::StoreSnapshot(const std::string& mac,
                                     const void* jpeg,
                                     std::size_t jpeg_len,
                                     Outcome* outcome) {
  Outcome dummy;
  Outcome& out = outcome ? *outcome : dummy;
  out = Outcome::kSkipped;
  if (url_.empty() || mac.empty() || jpeg == nullptr || jpeg_len == 0) {
    return "";
  }

  // Cool-down skip: if we're suspended and the cool-down window has not
  // elapsed since the last failure, don't even try.  Avoids piling up
  // doomed curl handles when MSR is overloaded.
  const int cooldown_ms = cooldown_ms_.load();
  if (cooldown_ms > 0 && suspended_.load()) {
    const std::int64_t now =
        clock_fn_ ? clock_fn_() : monotonic_ms();
    const std::int64_t last_fail = last_failure_ms_.load();
    if (now - last_fail < cooldown_ms) {
      return "";
    }
    // Cool-down has elapsed — fall through and probe MSR with one call.
  }

  // Single-flight: when enabled, only one StoreSnapshot runs at a time
  // across the whole process.  This caps MSR-bound traffic from us at
  // 1 in-flight, which is what MSR (and Protect, sharing the same
  // service) needs to stay responsive on a busy router.
  std::unique_lock<std::mutex> serialize_lk;
  if (serialize_.load()) {
    serialize_lk = std::unique_lock<std::mutex>(single_flight_mu_);
  }

  perform_count_.fetch_add(1);

  // Test hook: short-circuit before any libcurl work.  Connection-state
  // bookkeeping still runs so cool-down behaviour can be exercised.
  if (perform_fn_ != nullptr) {
    std::string id = perform_fn_(
        mac,
        std::string(static_cast<const char*>(jpeg), jpeg_len));
    constexpr int kSuspendThreshold = 5;
    if (id == kRejectedForTesting) {
      out = Outcome::kRejected;
      return "";
    }
    if (id.empty()) {
      out = Outcome::kTransport;
      last_failure_ms_.store(clock_fn_ ? clock_fn_() : monotonic_ms());
      int n = consecutive_failures_.fetch_add(1) + 1;
      if (n == kSuspendThreshold && !suspended_.exchange(true)) {
        LOG(WARNING) << "[msr] forwarding suspended after " << n
                     << " consecutive failures to " << url_;
      }
      return "";
    }
    consecutive_failures_.store(0);
    if (suspended_.exchange(false)) {
      LOG(INFO) << "[msr] forwarding resumed: " << url_;
    } else if (!seen_first_success_.exchange(true)) {
      LOG(INFO) << "[msr] connected to " << url_;
    }
    out = Outcome::kOk;
    return id;
  }

  std::string body =
      msr_client_internal::build_store_request(mac, jpeg, jpeg_len);

  // gRPC length-prefixed frame: [compressed_flag:1][length:4 BE][message]
  std::string framed;
  framed.reserve(5 + body.size());
  framed.push_back(0);  // not compressed
  uint32_t n = static_cast<uint32_t>(body.size());
  framed.push_back(static_cast<char>((n >> 24) & 0xffU));
  framed.push_back(static_cast<char>((n >> 16) & 0xffU));
  framed.push_back(static_cast<char>((n >> 8) & 0xffU));
  framed.push_back(static_cast<char>(n & 0xffU));
  framed.append(body);

  out = Outcome::kTransport;
  CURL* curl = curl_easy_init();
  if (!curl) return "";

  std::string full_url =
      url_ +
      "/unifi.media_server.recording.v1.RecordingAPI/StoreSnapshots";

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/grpc");
  headers = curl_slist_append(headers, "TE: trailers");
  headers = curl_slist_append(
      headers, "User-Agent: onvif-recorder-grpc/1");
  // Tell curl to actually send an Expect: header override (gRPC forbids).
  headers = curl_slist_append(headers, "Expect:");

  CurlCtx ctx;
  curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, framed.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(framed.size()));  // NOLINT(runtime/int)
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                   CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);  // NOLINT(runtime/int)

  CURLcode rc = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  // Connection-state diagnostics.  Five consecutive failures emit a single
  // "suspended" warning; any subsequent success emits a single "resumed"
  // (or first-success "connected") line and resets the counter.
  constexpr int kSuspendThreshold = 5;
  auto note_failure = [this]() {
    last_failure_ms_.store(clock_fn_ ? clock_fn_() : monotonic_ms());
    int n = consecutive_failures_.fetch_add(1) + 1;
    if (n == kSuspendThreshold && !suspended_.exchange(true)) {
      LOG(WARNING) << "[msr] forwarding suspended after " << n
                   << " consecutive failures to " << url_;
    }
  };
  auto note_success = [this]() {
    consecutive_failures_.store(0);
    if (suspended_.exchange(false)) {
      LOG(INFO) << "[msr] forwarding resumed: " << url_;
    } else if (!seen_first_success_.exchange(true)) {
      LOG(INFO) << "[msr] connected to " << url_;
    }
  };

  if (rc != CURLE_OK) {
    LOG(WARNING) << "MSR StoreSnapshots curl error: "
                 << curl_easy_strerror(rc) << " url=" << full_url;
    note_failure();
    return "";
  }
  if (ctx.grpc_status != 0) {
    out = Outcome::kGrpc;
    LOG(WARNING) << "MSR StoreSnapshots gRPC error: status="
                 << ctx.grpc_status << " msg=" << ctx.grpc_message;
    note_failure();
    return "";
  }
  if (ctx.body.size() < 5) {
    LOG(WARNING) << "MSR StoreSnapshots short body: "
                 << ctx.body.size() << " bytes";
    note_failure();
    return "";
  }
  // Skip the 5-byte gRPC length prefix on the inner message.
  const auto* msg =
      reinterpret_cast<const uint8_t*>(ctx.body.data()) + 5;
  std::size_t msg_len = ctx.body.size() - 5;
  std::string id = msr_client_internal::parse_store_response(msg, msg_len);
  if (id.empty()) {
    // A well-formed reply without an id: MSR is up but stored nothing for
    // this camera, typically because it is not recording it.
    out = Outcome::kRejected;
    LOG(WARNING) << "MSR StoreSnapshots: no snapshot stored for " << mac
                 << " (" << msg_len << " byte reply)";
    return "";
  }
  note_success();
  out = Outcome::kOk;
  return id;
}

}  // namespace onvif
