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

#include <cstdint>
#include <memory>

namespace onvif {
namespace pullpoint {

class PullPointHub;

// Loopback ONVIF PullPoint service that answers Protect's event
// subscription requests for shared cameras from the PullPointHub.
// Requests reach it through the onvif-recorder patch in Protect's
// service.js, under kPathPrefix.
class PullPointServer {
 public:
  static constexpr uint16_t kDefaultPort = 7892;
  static constexpr const char* kPathPrefix = "/onvif-recorder/pullpoint/";

  explicit PullPointServer(PullPointHub* hub);
  ~PullPointServer();  // stop()
  PullPointServer(const PullPointServer&) = delete;
  PullPointServer& operator=(const PullPointServer&) = delete;

  // Listen on 127.0.0.1:@p port (0 picks a free port).  Retries the bind
  // @p attempts times, 500 ms apart.  Returns false if it never binds.
  bool start(uint16_t port, int attempts = 1);
  // Release held pulls, then close the listener.  Idempotent.
  void stop();
  bool running() const;
  uint16_t port() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pullpoint
}  // namespace onvif
