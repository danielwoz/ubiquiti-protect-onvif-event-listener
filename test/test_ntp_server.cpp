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

// Packet encode/decode checks plus a loopback request against a running
// server on an ephemeral port.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>  // NOLINT(build/c++11)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ntp_server.hpp"

namespace {

#define CHECK(expr) do { \
  if (!(expr)) { \
    std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                 __FILE__, __LINE__, #expr); \
    std::exit(1); \
  } \
} while (0)

constexpr uint64_t kNtpEpochOffsetSec = 2208988800ULL;

uint64_t ReadU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

int64_t NtpToUnixNs(uint64_t ts) {
  const uint64_t secs = ts >> 32;
  const uint64_t frac = ts & 0xFFFFFFFFULL;
  const int64_t unix_sec = static_cast<int64_t>(secs) -
                          static_cast<int64_t>(kNtpEpochOffsetSec);
  const int64_t nanos =
      static_cast<int64_t>((frac * 1000000000ULL) >> 32);
  return unix_sec * 1000000000LL + nanos;
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::array<uint8_t, onvif::ntp::kPacketSize> MakeRequest(uint8_t version,
                                                         uint8_t mode) {
  std::array<uint8_t, onvif::ntp::kPacketSize> req {};
  req[0] = static_cast<uint8_t>((version << 3) | mode);
  req[2] = 6;  // poll interval
  // Client transmit timestamp, echoed back as the reply's origin.
  req[40] = 0x11;
  req[41] = 0x22;
  req[42] = 0x33;
  req[43] = 0x44;
  req[44] = 0x55;
  req[45] = 0x66;
  req[46] = 0x77;
  req[47] = 0x88;
  return req;
}

uint16_t BoundPort(const std::string& bound) {
  const size_t colon = bound.rfind(':');
  CHECK(colon != std::string::npos);
  return static_cast<uint16_t>(
      std::strtoul(bound.substr(colon + 1).c_str(), nullptr, 10));
}

void test_encode_epoch_and_fraction() {
  CHECK(onvif::ntp::EncodeTimestamp(0) == (kNtpEpochOffsetSec << 32));
  CHECK((onvif::ntp::EncodeTimestamp(500000000LL) & 0xFFFFFFFFULL) ==
        0x80000000ULL);
  const int64_t unix_ns = 1700000000LL * 1000000000LL;
  CHECK(onvif::ntp::EncodeTimestamp(unix_ns) ==
        ((1700000000ULL + kNtpEpochOffsetSec) << 32));
}

void test_reply_fields() {
  const auto req = MakeRequest(4, 3);
  const int64_t received_ns = 1700000000LL * 1000000000LL + 250000000LL;
  const int64_t sent_ns = received_ns + 1000000LL;
  std::array<uint8_t, onvif::ntp::kPacketSize> reply {};
  CHECK(onvif::ntp::BuildReply(req.data(), req.size(), received_ns, sent_ns,
                               &reply));

  CHECK(((reply[0] >> 6) & 0x03) == 0);  // no leap-second warning
  CHECK(((reply[0] >> 3) & 0x07) == 4);  // same version as the request
  CHECK((reply[0] & 0x07) == 4);         // server reply
  CHECK(reply[1] == 3);                  // stratum
  CHECK(reply[2] == 6);                  // poll copied from the request
  CHECK(reply[3] == 0xEC);               // precision -20
  CHECK(std::memcmp(reply.data() + 12, "LOCL", 4) == 0);
  CHECK(ReadU64(reply.data() + 24) == ReadU64(req.data() + 40));
  CHECK(ReadU64(reply.data() + 16) ==
        onvif::ntp::EncodeTimestamp(received_ns));
  CHECK(ReadU64(reply.data() + 32) ==
        onvif::ntp::EncodeTimestamp(received_ns));
  CHECK(ReadU64(reply.data() + 40) == onvif::ntp::EncodeTimestamp(sent_ns));
}

void test_reply_versions_and_modes() {
  std::array<uint8_t, onvif::ntp::kPacketSize> reply {};
  const auto v3 = MakeRequest(3, 3);
  CHECK(onvif::ntp::BuildReply(v3.data(), v3.size(), 0, 0, &reply));
  CHECK(((reply[0] >> 3) & 0x07) == 3);

  const auto active = MakeRequest(4, 1);
  CHECK(onvif::ntp::BuildReply(active.data(), active.size(), 0, 0, &reply));
  CHECK((reply[0] & 0x07) == 2);  // symmetric passive

  const auto server_reply = MakeRequest(4, 4);
  CHECK(!onvif::ntp::BuildReply(server_reply.data(), server_reply.size(),
                                0, 0, &reply));
  const auto broadcast = MakeRequest(4, 5);
  CHECK(!onvif::ntp::BuildReply(broadcast.data(), broadcast.size(), 0, 0,
                                &reply));
  const auto version_zero = MakeRequest(0, 3);
  CHECK(!onvif::ntp::BuildReply(version_zero.data(), version_zero.size(),
                                0, 0, &reply));
  CHECK(!onvif::ntp::BuildReply(server_reply.data(), 8, 0, 0, &reply));
}

bool ReceiveWithTimeout(int fd, std::array<uint8_t, 512>* buf, int timeout_ms) {
  struct pollfd pfd {};
  pfd.fd = fd;
  pfd.events = POLLIN;
  const int ready = poll(&pfd, 1, timeout_ms);
  if (ready <= 0) return false;
  const ssize_t n = recvfrom(fd, buf->data(), buf->size(), 0, nullptr,
                             nullptr);
  return n > 0;
}

void test_socket_replies() {
  onvif::NtpServer server;
  std::string error;
  CHECK(server.start("127.0.0.1:0", &error));
  CHECK(server.running());
  const uint16_t port = BoundPort(server.bound());

  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(fd >= 0);
  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  CHECK(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);

  const auto req = MakeRequest(4, 3);
  CHECK(sendto(fd, req.data(), req.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&addr),
               sizeof(addr)) == static_cast<ssize_t>(req.size()));
  std::array<uint8_t, 512> reply {};
  CHECK(ReceiveWithTimeout(fd, &reply, 2000));
  CHECK((reply[0] & 0x07) == 4);
  CHECK(ReadU64(reply.data() + 24) == ReadU64(req.data() + 40));
  const int64_t sent_ns = NtpToUnixNs(ReadU64(reply.data() + 40));
  CHECK(sent_ns > NowNs() - 5000000000LL);
  CHECK(sent_ns < NowNs() + 5000000000LL);
  CHECK(server.requests() >= 1);
  CHECK(server.last_request_unix_ms() > 0);

  // A packet the server does not answer produces no reply.
  const auto mode_reply = MakeRequest(4, 4);
  CHECK(sendto(fd, mode_reply.data(), mode_reply.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&addr),
               sizeof(addr)) == static_cast<ssize_t>(mode_reply.size()));
  CHECK(!ReceiveWithTimeout(fd, &reply, 300));

  close(fd);
  server.stop();
  CHECK(!server.running());
  server.stop();  // idempotent
}

void test_bind_errors() {
  onvif::NtpServer server;
  std::string error;
  CHECK(!server.start("127.0.0.1:notaport", &error));
  CHECK(!error.empty());
  CHECK(!server.start("not-an-ip:123", &error));
  CHECK(!error.empty());
  CHECK(!server.start("127.0.0.1", &error));
  CHECK(!error.empty());
}

}  // namespace

int main() {
  test_encode_epoch_and_fraction();
  test_reply_fields();
  test_reply_versions_and_modes();
  test_socket_replies();
  test_bind_errors();
  std::printf("ntp_server tests passed\n");
  return 0;
}
