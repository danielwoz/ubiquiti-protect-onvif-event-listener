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

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "onvif_camera_emulator.hpp"

// ============================================================
// RecordedSession -- responses loaded from a raw JSONL log file
// ============================================================

/// One HTTP exchange loaded from the raw recording.
struct RecordedExchange {
  int         status;
  std::string response;  // raw SOAP XML (JSON-unescaped)
};

/// All exchanges for one camera, partitioned by SOAP action.
struct RecordedSession {
  std::vector<RecordedExchange> create_sub;  ///< CreatePullPointSubscription
  std::vector<RecordedExchange> pull;        ///< PullMessages
  std::vector<RecordedExchange> renew;       ///< Renew

  /// Parse all entries from @p jsonl_path (one camera per file).
  static RecordedSession from_jsonl(const std::string& jsonl_path);
};

// ============================================================
// HikvisionCompatibleEmulator -- Hikvision-compatible camera
// (subscribes on first attempt, emits Initialized + Changed events)
// ============================================================
class HikvisionCompatibleEmulator : public OnvifCameraEmulator {
 public:
  explicit HikvisionCompatibleEmulator(const std::string& jsonl_path);

 protected:
  std::pair<int, std::string> handle(
    const std::string& path,
    const std::string& soap_action,
    const std::string& body) override;

 private:
  RecordedSession session_;
  std::size_t     create_idx_{0};
  std::size_t     pull_idx_{0};
  std::size_t     renew_idx_{0};
  std::mutex      mu_;
};

// ============================================================
// CellMotionCameraEmulator -- Amcrest / Lorex / Dahua basic-motion camera
// (subscribes immediately, emits CellMotionDetector/Motion + MotionAlarm topics)
// ============================================================
class CellMotionCameraEmulator : public OnvifCameraEmulator {
 public:
  explicit CellMotionCameraEmulator(const std::string& jsonl_path);

 protected:
  std::pair<int, std::string> handle(
    const std::string& path,
    const std::string& soap_action,
    const std::string& body) override;

 private:
  RecordedSession session_;
  std::size_t     create_idx_{0};
  std::size_t     pull_idx_{0};
  std::size_t     renew_idx_{0};
  std::mutex      mu_;
};

// ============================================================
// ThinginoCameraEmulator -- Wyze cam with Thingino open-source firmware
// (ONVIF event service endpoint absent; always returns HTTP 404)
// ============================================================
class ThinginoCameraEmulator : public OnvifCameraEmulator {
 public:
  explicit ThinginoCameraEmulator(const std::string& jsonl_path);

 protected:
  std::pair<int, std::string> handle(
    const std::string& path,
    const std::string& soap_action,
    const std::string& body) override;

 private:
  RecordedSession session_;
  std::size_t     create_idx_{0};
  std::mutex      mu_;
};

// ============================================================
// Html404CameraEmulator -- camera whose web server returns HTTP 200
// with an HTML "404 File Not Found" body for all ONVIF endpoints.
// Exercises the XML-parse-failure / graceful-give-up code path.
// ============================================================
class Html404CameraEmulator : public OnvifCameraEmulator {
 public:
  explicit Html404CameraEmulator(const std::string& jsonl_path);

 protected:
  std::pair<int, std::string> handle(
    const std::string& path,
    const std::string& soap_action,
    const std::string& body) override;

 private:
  RecordedSession session_;
  std::size_t     create_idx_{0};
  std::mutex      mu_;
};

// ============================================================
// DahuaSD4A425DBEmulator -- Dahua DH-SD4A425DB-HNY PTZ camera
// (returns 400 x N then 200 for subscribe, motion/alarm topics)
// ============================================================
class DahuaSD4A425DBEmulator : public OnvifCameraEmulator {
 public:
  explicit DahuaSD4A425DBEmulator(const std::string& jsonl_path);

 protected:
  std::pair<int, std::string> handle(
    const std::string& path,
    const std::string& soap_action,
    const std::string& body) override;

 private:
  RecordedSession session_;
  std::size_t     create_idx_{0};
  std::size_t     pull_idx_{0};
  std::mutex      mu_;
};

// ============================================================
// UosEmulator -- fake UOS external automation manager (port 11010 role)
//
// Accepts:
//   GET  /api/v1/alarms         → returns the configured alarm list JSON
//   POST /api/v1/alarms/events  → records the request body, returns "{}"
//
// Used by test_detection_recorder to verify that AlarmNotifier sends the
// correct requests to UOS when a person or vehicle detection is recorded.
// ============================================================
class UosEmulator : public OnvifCameraEmulator {
 public:
  UosEmulator();

  /// Replace the alarm list JSON returned by GET /api/v1/alarms.
  /// Call before start() or while running; thread-safe.
  void set_alarms_json(const std::string& json);

  /// Returns every POST body received at /api/v1/alarms/events.
  std::vector<std::string> posted_events() const;

  /// Base URL of this server, e.g. "http://127.0.0.1:54321".
  std::string base_url() const;

 protected:
  std::pair<int, std::string> handle(
    const std::string& path,
    const std::string& soap_action,
    const std::string& body) override;

 private:
  mutable std::mutex       mu_;
  std::string              alarms_json_{"[]"};
  std::vector<std::string> posted_;
};
