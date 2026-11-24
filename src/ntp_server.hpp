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

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace onvif {

namespace ntp {

// NTP packet size, in bytes (RFC 5905, section 7.3).
constexpr size_t kPacketSize = 48;

// Fill @p reply with a server response to the client request in @p request.
// @p received_ns and @p sent_ns are Unix times in nanoseconds at which the
// request arrived and the reply is built.  Returns false for packets the
// server must not answer: shorter than @p kPacketSize, an unsupported
// version, or a mode other than client (3) or symmetric active (1).
bool BuildReply(const uint8_t* request, size_t length,
                int64_t received_ns, int64_t sent_ns,
                std::array<uint8_t, kPacketSize>* reply);

// Encode a Unix time in nanoseconds as an NTP 64-bit timestamp: seconds
// since 1900 in the high half and a fraction of a second in the low half.
uint64_t EncodeTimestamp(int64_t unix_ns);

}  // namespace ntp

// Serves the system clock over NTPv4 on the local network, so cameras that
// check ONVIF WS-Security timestamps against their own clock have a time
// source to sync to.  One UDP socket, one thread; a bind failure only
// disables the server.
class NtpServer {
 public:
  NtpServer() = default;
  ~NtpServer();

  NtpServer(const NtpServer&) = delete;
  NtpServer& operator=(const NtpServer&) = delete;

  // Bind @p bind ("address:port"; an empty address means all interfaces,
  // port 0 picks a free port) and start answering requests.  Returns false
  // and sets @p error when the socket cannot be bound.
  bool start(const std::string& bind, std::string* error);

  // Stop answering and close the socket.  Safe to call more than once.
  void stop();

  bool running() const { return running_.load(std::memory_order_acquire); }

  // The "address:port" actually bound; empty before start.
  std::string bound() const;

  // Replies sent so far, and the Unix time of the last request in
  // milliseconds (0 before the first).
  uint64_t requests() const {
    return requests_.load(std::memory_order_relaxed);
  }
  int64_t last_request_unix_ms() const;

 private:
  void run();

  int fd_{-1};
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> requests_{0};
  std::atomic<int64_t> last_request_ns_{0};
  mutable std::mutex mu_;
  std::string bound_;
};

}  // namespace onvif
