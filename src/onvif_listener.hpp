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
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "absl/synchronization/mutex.h"
#include "jpeg_crop.hpp"

namespace onvif {

// Process-global, always-on capture of recent ONVIF SOAP exchanges.  The
// in-memory ring keeps the most recent 50 000 entries, with each entry
// individually zstd-compressed (level 3) so the in-RAM footprint stays
// small even for verbose SOAP XML — we typically see ~5x compression,
// so the 64 MiB compressed cap holds roughly 300+ MiB of raw captures.
// /api/diagnostic_dump decompresses the entire ring on demand.
// --raw_log additionally tees every (uncompressed) exchange to a JSONL
// file for unbounded long-form captures.  Thread-safe.
class RawSink {
 public:
  static RawSink& instance();

  // Tee subsequent exchanges to @p path in append mode.  Failure to open
  // is logged and ignored; the ring stays active either way.  Idempotent:
  // calling with an empty string disables the file tee.
  void enable_disk(const std::string& path);

  // Append one full request/response exchange.  Called from camera
  // worker threads.
  void record(const std::string& camera_ip,
              const std::string& url,
              const std::string& soap_action,
              const std::string& request,
              int64_t            response_status,
              const std::string& response);

  // Concatenate every entry currently in the ring as JSONL, decompressing
  // each entry on the fly.  Each entry is one self-contained JSON object
  // on its own line.  Used by /api/diagnostic_dump and any test seam
  // that wants to inspect recent wire activity.
  std::string snapshot() const;

  // Test seams: how many entries are in the ring; how many bytes are
  // currently held in compressed form.
  size_t entry_count() const;
  size_t compressed_bytes() const;

 private:
  RawSink();

  static constexpr size_t kMaxEntries  = 50000;
  // 64 MiB cap on COMPRESSED ring size.  At a typical 5x ratio for SOAP
  // XML this corresponds to ~300 MiB of raw captures — comfortably more
  // than the 50k entry cap can fill on its own at typical exchange size.
  static constexpr size_t kMaxBytes    = 64 * 1024 * 1024;
  // zstd compression level: 3 is the library's default — ~500 MB/s
  // compress on a single core, ratio comparable to gzip-1.
  static constexpr int    kZstdLevel   = 3;

  mutable absl::Mutex     mu_;
  std::deque<std::string> ring_;          // each element: zstd-compressed JSONL line (no \n)
  size_t                  ring_bytes_{0};  // sum of compressed sizes
  std::ofstream           disk_;
};

// ---------------------------------------------------------------
// Event delivered to the caller via EventCallback
// ---------------------------------------------------------------
struct OnvifEvent {
    std::string camera_ip;
    std::string camera_user;
    std::string topic;          // e.g. "tns1:RuleEngine/CellMotionDetector/Motion"
    std::string event_time;     // Camera-reported UTC timestamp
    std::string property_op;   // "Initialized", "Changed", or "Deleted"
    std::map<std::string, std::string> source;
    std::map<std::string, std::string> data;
    // Bounding box of detected object in normalised [0,1] coordinates.
    // Set only for cameras that include <tt:BoundingBox> in their ONVIF analytics
    // events (e.g. tns1:VideoAnalytics/ObjectDetector).  Most cameras do not.
    std::optional<jpeg_crop::BoundingBox> bbox;
    // Non-empty when the camera advertised an alarm service in GetServices
    // (e.g. managed by UniFi Protect).  Used as a presence gate: DetectionRecorder
    // skips alarm notification when this is empty.  The actual host:port used for
    // alarm POSTs is always the configured --uos_url, not this value.
    std::string alarm_url;
};

// ---------------------------------------------------------------
// Camera credentials / address
// ---------------------------------------------------------------
struct CameraConfig {
    std::string id;           ///< UUID from the cameras table (empty = not registered)
    std::string mac;          ///< MAC address, uppercase no colons e.g. "FC5F49CA68D4"
    std::string ip;           ///< Host or host:port, e.g. "192.168.1.108" or "192.168.1.108:8080"
    std::string user;
    std::string password;
    std::string snapshot_url;  ///< Optional HTTP URL used to capture a snapshot image

    /// Returns "http://host" or "http://host:port".
    /// All HTTP URL construction must go through this method so that port
    /// handling is centralised here rather than scattered at each call site.
    std::string http_base() const { return "http://" + ip; }
    int retry_interval_sec{10};      ///< Seconds to wait before retrying a failed subscription
    ///< Pause and reset after this many consecutive failures (0 = unlimited)
    int max_consecutive_failures{0};
    ///< Backoff after a run of max_consecutive_failures failures.  Starts at
    ///< base, doubles per consecutive pause, clamps at max, and resets to base
    ///< the moment a subscription succeeds.
    ///
    ///< This used to sleep out the remainder of a 1 h "failure window" measured
    ///< from the first failure in the streak.  With 3 failures at a 10 s retry
    ///< interval the streak burned ~30 s, so every pause slept ~3570 s: a brief
    ///< network blip cost a full hour of blindness on that camera.  Field-
    ///< observed on the dev router as a nightly all-camera outage (57-75 min,
    ///< two nights running) whenever the 04:17 backup, apt-daily and e2scrub
    ///< overlapped and starved curl into timing out.
    int retry_backoff_base_sec{30};
    int retry_backoff_max_sec{300};
};

// Pure helper visible for testing.  Given the backoff just slept, return the
// next one: double it, clamped to [base_sec, max_sec].  Degenerate input
// (non-positive current or base, max below base) collapses to a sane value
// rather than a zero-length or negative sleep.
int next_backoff_sec(int current_sec, int base_sec, int max_sec);

using EventCallback = std::function<void(const OnvifEvent&)>;

// ---------------------------------------------------------------
// Library lifecycle -- call once from main() around all listeners
// ---------------------------------------------------------------
void global_init();
void global_cleanup();

// ---------------------------------------------------------------
// OnvifListener
//
// Manages WS-PullPoint subscriptions for one or more cameras.
// Not copyable or movable.
//
// Typical usage:
//
//   onvif::global_init();
//   {
//       onvif::OnvifListener listener;
//       listener.add_camera({"192.168.1.108", "admin", "secret"});
//       listener.add_camera({"192.168.1.109", "user",  "secret"});
//
//       // signal handler calls listener.stop()
//       listener.run([](const onvif::OnvifEvent& ev) {
//           // called from camera thread; must be thread-safe
//           std::cout << ev.topic << '\n';
//       });
//   }
//   onvif::global_cleanup();
// ---------------------------------------------------------------
// Diagnostic snapshot for one camera, published by CameraWorker after each
// pull tick and mirrored to the admin UI via /api/camera_health.  Fields
// here are additive-only -- callers deserialise by name, so appending new
// members is a compatible change.
struct CameraHealth {
  std::string ip;
  uint64_t events_received_total{0};
  uint64_t last_event_ms{0};       // 0 = never
  uint64_t last_renew_ms{0};       // 0 = never
  uint64_t subscribed_at_ms{0};    // 0 = not subscribed
  bool     subscribed{false};
};

class OnvifListener {
 public:
    OnvifListener();
    ~OnvifListener();

    OnvifListener(const OnvifListener&)            = delete;
    OnvifListener& operator=(const OnvifListener&) = delete;

    /// Register a camera before calling run().
    void add_camera(const CameraConfig& cfg);

    /// Register a camera while run() is already executing.  Safe to call
    /// from any thread.  The new camera is picked up on the next tick of
    /// run()'s supervisor loop (within ~250ms) and a subscription worker
    /// thread is spawned for it.  Intended for hot-adding cameras that
    /// appear in Protect's database after startup.
    void add_camera_live(const CameraConfig& cfg);

    /// Enable raw HTTP recording to disk.  Every SOAP request and its
    /// response are appended as JSON Lines to @p path.  The in-memory
    /// ring buffer (RawSink::instance()) is always active regardless of
    /// this call; this only adds an on-disk tee for long-form captures.
    /// Must be called before run().  Pass an empty string to disable.
    void enable_raw_recording(const std::string& path);

    /// Spawn one thread per camera. Invoke cb for every received event
    /// (from the camera's own thread -- cb must be thread-safe).
    /// Blocks until stop() is called and all threads have exited (or a
    /// 30-second shutdown deadline expires, at which point stuck threads
    /// are detached so the process can exit cleanly).
    void run(EventCallback cb);

    /// Signal all camera threads to stop. Safe to call from any thread
    /// or signal handler; run() returns once all threads have joined
    /// or the shutdown deadline has been reached.
    void stop();

    /// Snapshot of per-camera health from the most recent publish tick
    /// of each CameraWorker.  Safe to call concurrently with run() from
    /// any thread (typically the admin-server HTTP handler thread).
    std::vector<CameraHealth> healths() const;

 private:
    std::atomic<bool>         running_{false};
    // Set by stop().  Survives a stop() that arrives before run() starts,
    // which run() would otherwise overwrite by setting running_ = true.
    std::atomic<bool>         stop_requested_{false};
    std::vector<CameraConfig> cameras_;

    // Queue of cameras added via add_camera_live() after run() started.
    // Drained by run()'s supervisor loop under pending_mutex_.
    absl::Mutex               pending_mutex_;
    std::vector<CameraConfig> pending_cameras_;

    // Published by every CameraWorker after each pull tick.  Read by
    // healths() from arbitrary threads.  Keyed by camera IP so a hot-added
    // camera slots in without disturbing the others.
    mutable absl::Mutex       healths_mutex_;
    std::map<std::string, CameraHealth>
                              healths_ ABSL_GUARDED_BY(healths_mutex_);

    // Called by CameraWorker to push its latest snapshot into healths_.
    void publish_health(const CameraHealth& h);
};

}  // namespace onvif
