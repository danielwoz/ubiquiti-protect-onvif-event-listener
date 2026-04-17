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

struct MHD_Daemon;

namespace onvif {

class LogRing;

/// Minimal HTTP server that serves the LogRing contents as an HTML page.
///
/// Listens on 127.0.0.1:@p port (loopback only).  The nginx reverse proxy
/// on the UniFi OS router forwards authenticated requests from
/// /onvif/events/log to this server.
///
/// Thread-safe: the server runs on libmicrohttpd's internal thread pool.
class LogServer {
 public:
  /// Start the server.  Returns false on failure.
  bool start(const LogRing* ring, uint16_t port = 7890);

  /// Stop the server and release the listening socket.
  void stop();

  ~LogServer();

  LogServer() = default;
  LogServer(const LogServer&)            = delete;
  LogServer& operator=(const LogServer&) = delete;

 private:
  ::MHD_Daemon* daemon_{nullptr};
  const LogRing* ring_{nullptr};
};

}  // namespace onvif
