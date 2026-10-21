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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "upstream_event.hpp"

namespace onvif {
namespace pullpoint {

// ---------------------------------------------------------------------
// Camera identity shared by Protect's requests and the recorder's
// cameras: lower-case host without a trailing dot, plus ":port" unless
// the port is "", "80" or "0".
// ---------------------------------------------------------------------
std::string camera_key(const std::string& host, const std::string& port);
// Same key from CameraConfig::ip ("host" or "host:port").
std::string camera_key_from_ip(const std::string& ip);

// Why the recorder's own subscription to a camera failed.
enum class FailureClass {
  kNone,
  kSlotLimited,   // the camera has no free subscription slot
  kUnreachable,   // transport error (connect, timeout, empty reply)
  kRejected,      // anything else: auth, missing event service, bad reply
};
const char* failure_class_name(FailureClass c);
// @p http_status is 0 for transport errors; @p detail is the fault or
// response text.
FailureClass classify_failure(int http_status, const std::string& detail);

// Time source, replaceable in tests.
class Clock {
 public:
  virtual ~Clock() = default;
  virtual std::chrono::steady_clock::time_point steady_now() const = 0;
  virtual std::chrono::system_clock::time_point system_now() const = 0;
};
Clock* real_clock();

struct HubCamera {
  std::string ip;        // CameraConfig::ip
  std::string mac;       // upper-case hex without colons; may be empty
  bool proxied = false;  // Protect's subscription is shared for it
};

// Thread-safe state of the PullPoint service that shares each camera's
// event subscription with Protect.  The listener's taps feed it, the
// SOAP server asks it what to answer, and it never does network I/O.
class PullPointHub {
 public:
  struct Options {
    // Per-camera history kept across restarts.  Empty keeps it in memory.
    std::string state_path;
    // Limits, overridable in tests.
    std::chrono::seconds up_window{30};
    std::chrono::seconds not_ready_limit{60};
    std::chrono::seconds reserve_after_status{30};
    std::chrono::seconds resubscribe_after_status{10};
    std::chrono::seconds rejected_for{600};
    int rejected_attempts = 3;
    std::chrono::seconds blocked_after{900};
    std::chrono::seconds silent_after{1800};
    std::chrono::hours silent_history{24};
    std::chrono::seconds recover_after{300};
    std::chrono::seconds nudge_interval{30};
    std::chrono::seconds tombstone_ttl{600};
    size_t max_queue_messages = 256;
    size_t max_queue_bytes = 512 * 1024;
    size_t max_cache_entries = 256;
    size_t max_cache_bytes = 1024 * 1024;
    size_t max_pull_points_per_camera = 8;
    size_t max_pull_points_total = 64;
  };

  // Result of a CreatePullPointSubscription.
  struct CreateResult {
    bool accepted = false;
    std::string id;       // 32 lower-case hex digits
    std::string reason;   // why it was declined
    std::chrono::system_clock::time_point termination;
    size_t snapshot = 0;  // current-state messages queued for it
  };
  struct PullResult {
    enum class Status { kOk, kUnknown, kStopped };
    Status status = Status::kOk;
    std::vector<std::shared_ptr<const std::string>> messages;  // fragments
    std::chrono::system_clock::time_point termination;
  };
  struct RenewResult {
    bool ok = false;
    std::string reason;   // why it faulted
    std::chrono::system_clock::time_point termination;
  };

  explicit PullPointHub(Options opts, Clock* clock = nullptr);
  ~PullPointHub();
  PullPointHub(const PullPointHub&) = delete;
  PullPointHub& operator=(const PullPointHub&) = delete;

  // ---- registry ----------------------------------------------------
  // Add or update a camera.  Safe while running (hot-added cameras).
  void register_camera(const HubCamera& cam);
  // Every camera known at start-up is registered.  Until then Creates
  // are declined and unknown subscriptions are kept provisionally.
  void set_ready();
  bool ready() const;
  // Called without the hub lock held.
  void set_nudge_fn(std::function<void(const std::string& ip)> fn);
  void set_resubscribe_fn(std::function<void(const std::string& ip)> fn);

  // ---- the recorder's own subscription (camera worker threads) -------
  void on_upstream_response(const std::string& ip, const std::string& xml);
  void on_upstream_event(const std::string& ip, UpstreamEvent ev,
                         int http_status, const std::string& detail);

  // ---- Protect's requests (server connection threads) --------------
  // @p ttl is the requested lifetime; nullopt means the default.
  CreateResult create(const std::string& key,
                      std::optional<std::chrono::seconds> ttl);
  // Whether Protect should move its direct subscription for @p key to
  // the recorder.  @p reason explains a no.
  bool status_probe(const std::string& key, std::string* reason);
  // Blocks up to @p wait when nothing is queued.
  PullResult pull(const std::string& key, const std::string& id,
                  std::chrono::milliseconds wait, int limit,
                  size_t byte_budget);
  RenewResult renew(const std::string& key, const std::string& id,
                    std::optional<std::chrono::seconds> ttl);
  void unsubscribe(const std::string& key, const std::string& id);
  // SetSynchronizationPoint: queue the current state again.
  bool sync_point(const std::string& key, const std::string& id);

  // ---- housekeeping --------------------------------------------------
  // Expire, evaluate hand-back, run scheduled resubscribes, persist and
  // log the hourly summary.  The hub thread calls it once a second.
  void tick();
  void start();     // start the hub thread
  // Wake every held pull; later pulls return at once until start().
  void wake_all();
  void stop();      // stop the thread, wake held pulls, persist

  // ---- reporting -----------------------------------------------------
  std::string snapshot_json() const;
  // " protect_pp=<n> protect_pull_age=<s>s" for shared cameras, else "".
  std::string heartbeat_suffix(const std::string& ip) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pullpoint
}  // namespace onvif
