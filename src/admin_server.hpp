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
#include <functional>
#include <memory>
#include <mutex>  // NOLINT(build/c++11)
#include <string>
#include <vector>

#include "onvif_listener.hpp"
#include "unifi_camera_config.hpp"

struct MHD_Daemon;

namespace onvif {

class LogRing;
class ProtectUserIdProvider;

/// Minimal HTTP server that exposes an admin page + JSON API for managing the
/// onvif-recorder Debian package: force-check-for-updates, switch APT channel,
/// toggle auto-update, uninstall.
///
/// Listens on 127.0.0.1:@p port (loopback only).  The nginx reverse proxy on
/// the UniFi OS router forwards authenticated requests from /onvif/admin/ to
/// this server.
///
/// All mutating endpoints shell out to `systemctl` / `apt-get`, so the server
/// must run as root (same as the recorder binary does today).
class AdminServer {
 public:
  /// Start the server.  Returns false on failure.
  ///
  /// @p channel_file is the path to /etc/onvif-recorder/channel (may be empty
  /// for testing to disable channel switching).
  /// @p version is the version string reported by /api/status.
  /// Pass @p port == 0 to let the OS pick an ephemeral port (tests);
  /// the chosen port is then available via port().
  using GetOnvifHealthsFn =
      std::function<std::vector<onvif::CameraHealth>()>;

  bool start(const std::string& version,
             const std::string& channel_file,
             uint16_t port = 7891,
             const std::string& config_path = "/etc/onvif-recorder/config.json",
             const unifi::DbConfig& db = unifi::DbConfig{},
             const std::string& protect_url = "",
             ProtectUserIdProvider* protect_user_id_provider = nullptr,
             const std::string& event_log_path = "",
             GetOnvifHealthsFn get_onvif_healths = {});

  /// Attach the shared LogRing so /api/recent_errors can serve the last
  /// N error lines to the admin page.  Must be called before start().
  /// The ring must outlive the AdminServer.
  void set_log_ring(const LogRing* ring) { log_ring_ = ring; }

  /// Returns the Protect event-sharing status as a JSON object
  /// (mode, port, bound, patch, protect_restart_pending, hub, and
  /// "bind_failed":true when the event-sharing port could not be bound).
  using EventProxyStatusFn = std::function<std::string()>;

  /// Set the source of GET /api/event_proxy and of event_proxy.json in
  /// the diagnostic dump.  Without one, or when it returns an empty
  /// string, both report {"mode":"off"}.  Callable before or after
  /// start(), from any thread.
  void set_event_proxy_status_fn(EventProxyStatusFn fn);

  /// Returns what became of the user's local service.js patches
  /// (--local_patches) as a JSON object (path, error, entries).
  using LocalPatchesStatusFn = std::function<std::string()>;

  /// Set the source of "local_patches" in GET /api/status.  Without one,
  /// or when it returns an empty string, the key is left out.  Callable
  /// before or after start(), from any thread.
  void set_local_patches_status_fn(LocalPatchesStatusFn fn);

  /// Return the port the server is listening on. Only meaningful after a
  /// successful start().  Useful when start() was called with port=0.
  uint16_t port() const { return port_; }

  /// Stop the server and release the listening socket.
  void stop();

  ~AdminServer();

  AdminServer() = default;
  AdminServer(const AdminServer&)            = delete;
  AdminServer& operator=(const AdminServer&) = delete;

 private:
  // The provider's JSON, or an empty string without one.
  std::string event_proxy_status() const;
  std::string local_patches_status() const;

  ::MHD_Daemon* daemon_{nullptr};
  // State the request handler reads; set while the daemon runs.
  std::shared_ptr<void> ctx_;
  const LogRing* log_ring_{nullptr};
  std::string version_;
  std::string channel_file_;
  std::string config_path_;
  unifi::DbConfig db_;
  std::string protect_url_;
  ProtectUserIdProvider* protect_user_id_provider_{nullptr};
  std::string event_log_path_;
  GetOnvifHealthsFn get_onvif_healths_;
  mutable std::mutex event_proxy_mu_;
  EventProxyStatusFn event_proxy_fn_;  // guarded by event_proxy_mu_
  mutable std::mutex local_patches_mu_;
  LocalPatchesStatusFn local_patches_fn_;  // guarded by local_patches_mu_
  uint16_t port_{0};
};

/// Body of GET /api/camera_health for @p rows.  Each camera's tickbox
/// state (via Protect, Enabled, Share with Protect) comes from the
/// config file at @p config_path; a missing file leaves every box at its
/// default.
std::string camera_health_json(const std::vector<unifi::CameraHealth>& rows,
                               const std::string& config_path,
                               uint64_t now_ms);

/// Replace the query string of each named URL-valued key in a flat JSON
/// object with "?[REDACTED]".  Used on config.json in the diagnostic dump.
std::string redact_url_queries(std::string json,
                               const std::vector<std::string>& keys);

}  // namespace onvif
