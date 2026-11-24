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

#include "ntp_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

namespace onvif {
namespace ntp {

namespace {

void PutU32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value >> 24);
  out[1] = static_cast<uint8_t>(value >> 16);
  out[2] = static_cast<uint8_t>(value >> 8);
  out[3] = static_cast<uint8_t>(value);
}

void PutU64(uint8_t* out, uint64_t value) {
  PutU32(out, static_cast<uint32_t>(value >> 32));
  PutU32(out + 4, static_cast<uint32_t>(value));
}

}  // namespace

uint64_t EncodeTimestamp(int64_t unix_ns) {
  constexpr int64_t kEpochOffsetSec = 2208988800LL;  // 1900-01-01 to 1970-01-01
  int64_t seconds = unix_ns / 1000000000LL;
  int64_t nanos = unix_ns % 1000000000LL;
  if (nanos < 0) {
    nanos += 1000000000LL;
    --seconds;
  }
  const int64_t ntp_seconds = seconds + kEpochOffsetSec;
  if (ntp_seconds < 0) return 0;
  const uint64_t whole = static_cast<uint64_t>(ntp_seconds);
  const uint64_t fraction =
      (static_cast<uint64_t>(nanos) << 32) / 1000000000ULL;
  return (whole << 32) | fraction;
}

bool BuildReply(const uint8_t* request, size_t length,
                int64_t received_ns, int64_t sent_ns,
                std::array<uint8_t, kPacketSize>* reply) {
  if (request == nullptr || reply == nullptr || length < kPacketSize)
    return false;
  const uint8_t version = static_cast<uint8_t>((request[0] >> 3) & 0x07);
  const uint8_t mode = static_cast<uint8_t>(request[0] & 0x07);
  // Version 0 is reserved; NTPv1..v4 share the packet layout.  Answer a
  // client (3) with a server reply (4) and a symmetric active peer (1)
  // with a symmetric passive reply (2); everything else (broadcast,
  // already-a-reply, control, private) is not ours to answer.
  if (version == 0 || version > 4) return false;
  if (mode != 3 && mode != 1) return false;

  std::array<uint8_t, kPacketSize> r {};
  r[0] = static_cast<uint8_t>((version << 3) | (mode == 3 ? 4 : 2));
  r[1] = 3;                          // stratum
  r[2] = request[2] == 0 ? 6 : request[2];  // poll interval
  r[3] = 0xEC;                       // precision, 2^-20 s
  PutU32(r.data() + 8, 0x0000028F);  // root dispersion, 10 ms
  r[12] = 'L';
  r[13] = 'O';
  r[14] = 'C';
  r[15] = 'L';
  PutU64(r.data() + 16, EncodeTimestamp(received_ns));
  std::memcpy(r.data() + 24, request + 40, 8);  // origin = client transmit
  PutU64(r.data() + 32, EncodeTimestamp(received_ns));
  PutU64(r.data() + 40, EncodeTimestamp(sent_ns));
  *reply = r;
  return true;
}

}  // namespace ntp

namespace {

// Current Unix time in nanoseconds.
int64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL +
         static_cast<int64_t>(ts.tv_nsec);
}

bool ParseBind(const std::string& spec, struct sockaddr_in* addr,
               std::string* error) {
  const size_t colon = spec.rfind(':');
  if (colon == std::string::npos) {
    *error = "expected address:port, got \"" + spec + "\"";
    return false;
  }
  const std::string host = spec.substr(0, colon);
  const std::string port = spec.substr(colon + 1);
  if (port.empty()) {
    *error = "missing port in \"" + spec + "\"";
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const unsigned long parsed =  // NOLINT(runtime/int)
      std::strtoul(port.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0' || parsed > 65535) {
    *error = "invalid port in \"" + spec + "\"";
    return false;
  }
  std::memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET;
  addr->sin_port = htons(static_cast<uint16_t>(parsed));
  if (host.empty() || host == "*") {
    addr->sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (inet_pton(AF_INET, host.c_str(), &addr->sin_addr) != 1) {
    *error = "not an IPv4 address: \"" + host + "\"";
    return false;
  }
  return true;
}

std::string FormatAddress(const struct sockaddr_in& addr) {
  char ip[INET_ADDRSTRLEN] = {};
  if (inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip)) == nullptr)
    return std::string();
  return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

}  // namespace

NtpServer::~NtpServer() { stop(); }

bool NtpServer::start(const std::string& bind, std::string* error) {
  if (running_.load(std::memory_order_acquire)) {
    if (error != nullptr) *error = "already running";
    return false;
  }
  struct sockaddr_in addr {};
  std::string parse_error;
  if (!ParseBind(bind, &addr, &parse_error)) {
    if (error != nullptr) *error = parse_error;
    return false;
  }

  const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    if (error != nullptr) *error = std::string("socket: ") +
                                   std::strerror(errno);
    return false;
  }
  if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr),
             sizeof(addr)) != 0) {
    if (error != nullptr) *error = "bind " + bind + ": " +
                                   std::strerror(errno);
    close(fd);
    return false;
  }

  struct sockaddr_in local {};
  socklen_t local_len = sizeof(local);
  if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&local),
                  &local_len) == 0) {
    std::lock_guard<std::mutex> lock(mu_);
    bound_ = FormatAddress(local);
  }

  fd_ = fd;
  running_.store(true, std::memory_order_release);
  // The build disables exceptions: a thread that cannot be created is
  // fatal, as it is for the recorder's other worker threads.
  thread_ = std::thread(&NtpServer::run, this);
  return true;
}

void NtpServer::stop() {
  running_.store(false, std::memory_order_release);
  if (thread_.joinable()) thread_.join();
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

std::string NtpServer::bound() const {
  std::lock_guard<std::mutex> lock(mu_);
  return bound_;
}

int64_t NtpServer::last_request_unix_ms() const {
  const int64_t ns = last_request_ns_.load(std::memory_order_relaxed);
  return ns <= 0 ? 0 : ns / 1000000LL;
}

void NtpServer::run() {
  std::array<uint8_t, 512> request {};
  while (running_.load(std::memory_order_acquire)) {
    struct pollfd pfd {};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    const int ready = poll(&pfd, 1, 200);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) continue;
    if ((pfd.revents & POLLIN) == 0) {
      if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) break;
      continue;
    }

    struct sockaddr_in peer {};
    socklen_t peer_len = sizeof(peer);
    const ssize_t n = recvfrom(
        fd_, request.data(), request.size(), 0,
        reinterpret_cast<struct sockaddr*>(&peer), &peer_len);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }

    const int64_t received_ns = NowNs();
    std::array<uint8_t, ntp::kPacketSize> reply {};
    if (!ntp::BuildReply(request.data(), static_cast<size_t>(n), received_ns,
                         NowNs(), &reply)) {
      continue;
    }
    const ssize_t sent = sendto(
        fd_, reply.data(), reply.size(), 0,
        reinterpret_cast<const struct sockaddr*>(&peer), peer_len);
    if (sent == static_cast<ssize_t>(reply.size())) {
      requests_.fetch_add(1, std::memory_order_relaxed);
      last_request_ns_.store(received_ns, std::memory_order_relaxed);
    }
  }
}

}  // namespace onvif
