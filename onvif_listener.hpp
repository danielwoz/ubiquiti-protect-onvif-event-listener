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
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "jpeg_crop.hpp"

namespace onvif {

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
    int retry_interval_sec{10};      ///< Seconds to wait before retrying a failed subscription
    ///< Pause and reset after this many consecutive failures (0 = unlimited)
    int max_consecutive_failures{0};
    ///< After hitting max_consecutive_failures, wait this long before resetting
    ///< the failure counter and retrying. Default: 3600 s (1 hour).
    int failure_window_sec{3600};
};

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
class OnvifListener {
 public:
    OnvifListener();
    ~OnvifListener();

    OnvifListener(const OnvifListener&)            = delete;
    OnvifListener& operator=(const OnvifListener&) = delete;

    /// Register a camera before calling run().
    void add_camera(const CameraConfig& cfg);

    /// Enable raw HTTP recording. Every SOAP request and its response are
    /// written as JSON Lines to @p path (one object per exchange). Must be
    /// called before run(). Disabled when not called.
    ///
    /// Each line contains:
    ///   timestamp, camera_ip, url, soap_action,
    ///   request  (full SOAP XML string),
    ///   response_status (HTTP code), response (full SOAP XML string)
    void enable_raw_recording(const std::string& path);

    /// Spawn one thread per camera. Invoke cb for every received event
    /// (from the camera's own thread -- cb must be thread-safe).
    /// Blocks until stop() is called and all threads have joined.
    void run(EventCallback cb);

    /// Signal all camera threads to stop. Safe to call from any thread
    /// or signal handler; run() returns once all threads have joined.
    void stop();

 private:
    std::atomic<bool>         running_{false};
    std::vector<CameraConfig> cameras_;
    std::string               raw_path_;   // empty = raw recording disabled
};

}  // namespace onvif
