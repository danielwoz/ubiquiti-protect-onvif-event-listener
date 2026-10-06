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
// GetServices response builder
//
// Builds a minimal GetServices response advertising the events service.
// If alarm_url is non-empty, also advertises an alarm service entry so
// that the ONVIF listener's discover_services() can extract it.
// Pass the result through rewrite_urls() to replace real_ip with the
// local emulator address (the alarm_url is already the correct address).
// ============================================================
std::string make_get_services_response(const std::string& real_ip,
                                        const std::string& alarm_url = "");

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
// HikvisionPullPointNotAuthorizedEmulator -- Hikvision DS-2CD2387G2P-LSU/SL
// (and other Hikvision firmware) where GetServices succeeds with the web-UI
// admin user, but CreatePullPointSubscription returns HTTP 400 with a SOAP
// fault `env:Sender` / `ter:NotAuthorized` because event subscription is
// gated on a separate dedicated ONVIF user that the operator hasn't
// configured (or has set to Operator/User instead of Administrator).
// Reproduces the configuration class reported in issue #20.
// ============================================================
class HikvisionPullPointNotAuthorizedEmulator : public OnvifCameraEmulator {
 public:
  explicit HikvisionPullPointNotAuthorizedEmulator(
      const std::string& jsonl_path);

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
// AxisReferenceParamsEmulator -- synthetic Axis-style camera
//
// Exercises the WS-Addressing ReferenceParameters forwarding path.
// All ONVIF operations are served by a single /onvif/services endpoint,
// matching the behaviour of real Axis cameras.  The emulator:
//
//   GetServices             → advertises /onvif/services as event XAddr
//   CreatePullPointSub      → returns a SubscriptionReference whose
//                             Address is /onvif/services and whose
//                             ReferenceParameters carry a fixed UUID token
//   PullMessages / Renew    → return HTTP 400 when the token is absent from
//                             the request body (i.e. not forwarded by the
//                             listener); return 200 + events when present
//
// The test passes only if the listener correctly threads the token through
// every subsequent PullMessages and Renew call.
// ============================================================
class AxisReferenceParamsEmulator : public OnvifCameraEmulator {
 public:
  AxisReferenceParamsEmulator();

  /// UUID token that must appear in PullMessages / Renew request bodies.
  const std::string& token() const { return token_; }

 protected:
  std::pair<int, std::string> handle(
    const std::string& path,
    const std::string& soap_action,
    const std::string& body) override;

 private:
  const std::string token_;
  std::mutex        mu_;
  bool              subscribed_{false};
};

// ============================================================
// ReolinkCameraEmulator -- Reolink-style cameras that return malformed
// GetServices XML (undeclared tad: namespace prefix in Capabilities).
// Uses the raw GetServices response from the JSONL recording.
//
// Events use tns1:RuleEngine/MyRuleDetector/{PeopleDetect,VehicleDetect,...}
// with data["State"] = "true"/"false".
// ============================================================
class ReolinkCameraEmulator : public OnvifCameraEmulator {
 public:
  explicit ReolinkCameraEmulator(const std::string& jsonl_path);

 protected:
  std::pair<int, std::string> handle(
    const std::string& path,
    const std::string& soap_action,
    const std::string& body) override;

 private:
  RecordedSession session_;
  std::string     raw_get_services_;  ///< verbatim GetServices response
  std::size_t     create_idx_{0};
  std::size_t     pull_idx_{0};
  std::size_t     renew_idx_{0};
  std::mutex      mu_;
};

// ============================================================
// ReplayCameraEmulator -- generic JSONL session replay.
//
// Clamps CreatePullPointSubscription/Renew at their last recorded
// response and cycles PullMessages forever.  That is all a captured
// field session needs, so both cameras added from user dumps share it
// rather than each getting a byte-identical subclass:
//
//   dahua_ivs_linecross_20260726.jsonl  -- IVS line crossing and field
//     detection, both carrying ONVIF ClassTypes, plus a firmware-mangled
//     "tns1:RuleEngine/tt:CellMotionDetector" topic and a spread of
//     RecordingConfig/Media/Device housekeeping that must stay silent.
//   reolink_ai_doorbell_20260725.jsonl  -- the whole MyRuleDetector
//     family from one device: People / Vehicle / Face / DogCat /
//     Visitor / Package, covering all four classes Protect models.
// ============================================================
class ReplayCameraEmulator : public OnvifCameraEmulator {
 public:
  explicit ReplayCameraEmulator(const std::string& jsonl_path);

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

  /// Replace the automations JSON returned by GET /api/automations.
  /// Call before start() or while running; thread-safe.
  void set_alarms_json(const std::string& json);

  /// Returns the URL path of every POST received at /api/automations/*/run.
  std::vector<std::string> posted_events() const;

  /// Bodies POSTed to the UOS external automation manager.  Without
  /// these the recorder's UOS path 404s, silently latches its legacy
  /// fallback, and every test quietly exercises the OLD notification
  /// path instead of the feature under test.
  std::vector<std::string> notify_bodies() const;
  std::vector<std::string> register_bodies() const;
  /// Make /actions/notify answer with @p code (default 200).  Lets a
  /// test drive the 500 re-register retry and the 404 fallback.
  void set_notify_status(int code);
  /// Make /api/automations/{id}/run answer 400 the way Protect does when
  /// Global Alarm Manager is on.
  void set_global_alarm_manager(bool on);
  /// Requests to /internal/ routes rejected because they carried
  /// X-UserId (Protect 7.3 answers those with 404).
  int rejected_internal_with_user() const;

  /// Every notification the recorder sent, by whichever path it chose --
  /// the legacy /api/automations/{id}/run URL or the UOS notify body.
  /// Both embed the automation id, so tests can assert WHICH automation
  /// fired without coupling to the transport.
  std::vector<std::string> triggered_automations() const;

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
  std::vector<std::string> notify_bodies_;
  std::vector<std::string> register_bodies_;
  int                      notify_status_{200};
  bool                     global_alarm_manager_{false};
  int                      rejected_internal_with_user_{0};
};
