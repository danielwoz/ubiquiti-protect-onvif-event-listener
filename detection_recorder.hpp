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

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "object_detect.hpp"
#include "onvif_listener.hpp"

namespace onvif {

class AlarmNotifier;  // forward declaration; full definition in alarm_notifier.hpp

/**
 * DetectionRecorder
 *
 * Translates raw ONVIF events into human/vehicle detections and persists them
 * to a PostgreSQL database using a schema that mirrors the UniFi Protect event
 * tables, making third-party camera data structurally identical to data
 * produced by native Ubiquiti cameras.
 *
 * Supported ONVIF event formats
 * ------------------------------
 * AI events (suppress basic motion from the same camera once seen):
 *   tns1:RuleEngine/FieldDetector/ObjectsInside  (Hikvision IVS)
 *     source["Rule"]   = "Human" | "Vehicle"
 *     data["IsInside"] = "true"  | "false"
 *
 *   tns1:UserAlarm/IVA/HumanShapeDetect  (Hikvision knockoff / Dahua)
 *     data["State"]    = "true"  | "false"   (always maps to "person")
 *
 *   tns1:VehicleAlarm/IVB/VehicleDetect  (Hikvision knockoff / Dahua)
 *     data["State"]    = "true"  | "false"   (always maps to "vehicle")
 *
 *   tns1:RuleEngine/MyRuleDetector/PeopleDetect  (Reolink)
 *     data["State"]    = "true"  | "false"   (maps to "person")
 *
 *   tns1:RuleEngine/MyRuleDetector/VehicleDetect  (Reolink)
 *     data["State"]    = "true"  | "false"   (maps to "vehicle")
 *
 * Basic motion events (suppressed when camera emits AI events):
 *   tns1:RuleEngine/CellMotionDetector/Motion  (Amcrest, Lorex, UNVR etc.)
 *     data["IsMotion"] = "true"  | "false"   (maps to "person")
 *
 * Fallback motion (suppressed when camera emits CellMotionDetector or AI):
 *   tns1:VideoSource/MotionAlarm
 *     data["State"]    = "true"  | "false"   (maps to "person")
 *
 * Detection type mapping
 * ----------------------
 *   ONVIF "human"   -> smartDetectTypes ["person"],  smartDetectObjects.type "person"
 *   ONVIF "vehicle" -> smartDetectTypes ["vehicle"], smartDetectObjects.type "vehicle"
 *
 *   Generic motion events (CellMotionDetector, VideoSource/MotionAlarm) use the
 *   configured default_object_type (default "person") unless NanoDet-M is
 *   enabled (--detect / --detect_override), in which case the COCO class
 *   returned by the detector overrides the type (person / vehicle / animal).
 *   Per-camera overrides (set_camera_object_type) take priority over NCNN.
 *   Valid object types: person, vehicle, animal, package.
 *
 * Backend selection
 * -----------------
 *   conn is a libpq conninfo string
 *   (e.g. "host=localhost dbname=unifi user=protect")
 *
 * Database schema (mirrors UniFi Protect, tables must already exist)
 * -----------------------------------------------------------------------
 *
 *   events (
 *     id TEXT PK,                            -- UUID v4
 *     type TEXT,                             -- 'smartDetectZone'
 *     start INTEGER/BIGINT,                  -- ms since Unix epoch
 *     end INTEGER/BIGINT,                    -- ms since Unix epoch; NULL while active
 *     cameraId TEXT,                         -- camera IP address
 *     score INTEGER DEFAULT 0,
 *     smartDetectTypes TEXT DEFAULT '[]',    -- JSON array, e.g. '["person"]'
 *     metadata TEXT DEFAULT '{}',            -- JSON object
 *     locked INTEGER DEFAULT 0,
 *     thumbnailId TEXT,                      -- '<cameraIP>-<start_ms>'
 *     thumbnailFullfovId TEXT,
 *     packageThumbnailId TEXT,
 *     packageThumbnailFullfovId TEXT,
 *     deletedAt TEXT,
 *     deletionType TEXT,
 *     userId TEXT,
 *     partitionId TEXT,
 *     createdAt TEXT NOT NULL,               -- ISO-8601 UTC
 *     updatedAt TEXT NOT NULL                -- ISO-8601 UTC
 *   )
 *
 *   smartDetectObjects (
 *     id TEXT PK,                            -- UUID v4
 *     eventId TEXT NOT NULL,                 -- -> events.id
 *     thumbnailId TEXT,                      -- '<cameraIP>-<start_ms>'
 *     cameraId TEXT NOT NULL,                -- camera IP address
 *     type TEXT NOT NULL,                    -- 'person' or 'vehicle'
 *     attributes TEXT DEFAULT '{}',          -- JSON: {"confidence":0}
 *     smartDetectObjectGroupId TEXT,
 *     detectedAt INTEGER/BIGINT NOT NULL,    -- ms since Unix epoch
 *     metadata TEXT DEFAULT '{}',            -- JSON object
 *     createdAt TEXT NOT NULL,
 *     updatedAt TEXT NOT NULL
 *   )
 *
 * Thread safety
 * -------------
 * on_event() is fully thread-safe; the OnvifListener may call it from
 * multiple camera threads simultaneously.
 * set_snapshot() must be called before the listener starts.
 */
class DetectionRecorder {
 public:
  /// Factory: connects to PostgreSQL, verifies schema. Returns error on failure.
  static absl::StatusOr<std::unique_ptr<DetectionRecorder>> Create(
      const std::string& conn);

  ~DetectionRecorder();

  DetectionRecorder(const DetectionRecorder&)            = delete;
  DetectionRecorder& operator=(const DetectionRecorder&) = delete;
  DetectionRecorder(DetectionRecorder&&)                 = delete;
  DetectionRecorder& operator=(DetectionRecorder&&)      = delete;

  /// Register snapshot credentials for a camera. Must be called before run().
  void set_snapshot(const CameraConfig& cam);

  /// Set the directory where per-camera UBV thumbnail files are written.
  /// Each camera gets its own file: <dir>/<camera_ip>_thumbnails.ubv
  /// If not called, UBV files are not written (snapshots are still fetched
  /// for the thumbnailId reference if a snapshot URL is configured).
  /// Must be called before run().
  void set_ubv_dir(const std::string& dir);

  /// Set pre/post buffer padding applied to stored timestamps.
  /// start is stored as (detection_time - pre_sec*1000).
  /// end   is stored as (detection_end_time + post_sec*1000).
  /// Defaults: 2 s pre, 2 s post. Must be called before run().
  void set_buffer(uint32_t pre_sec, uint32_t post_sec);

  /// Merge consecutive detections from the same camera into one event if the
  /// new detection starts within @p sec seconds of the previous one ending.
  /// Coalescing re-uses the existing event row instead of creating a new one.
  /// Pass 0 to disable. Default: 30 s.
  void set_coalesce_window(uint32_t sec);

  /// Drop new detection events from a camera once it has produced more than
  /// @p n events in the last rolling hour.  Pass 0 for unlimited. Default: 10.
  void set_max_events_per_hour(uint32_t n);

  /// Process one ONVIF event. Ignores events that are not human/vehicle
  /// detections. Thread-safe.
  void on_event(const OnvifEvent& ev);

  /// Scan the last @p days of ended events in the database and merge
  /// consecutive detections from the same camera with the same type if the
  /// gap between them is within the configured coalesce window.
  /// Returns the number of event rows deleted (merged away).
  /// Intended to be called once at startup, before run(). No-op if
  /// coalesce_window_sec is 0.
  int coalesce_history(int days = 30);

  /// Delete orphaned rows from all dependent tables (smartDetectRaws,
  /// thumbnails, smartDetectObjects, detectionLabels) in a single startup
  /// sweep.  A row is orphaned when its parent event no longer exists.
  /// smartDetectRaws uses a timestamp-range match (no eventId column);
  /// the others match directly on eventId.  thumbnails and smartDetectObjects
  /// are additionally filtered to third-party cameras; detectionLabels has no
  /// cameraId column so all orphaned rows are removed.
  /// Returns the total number of rows deleted across all tables.
  int purge_orphaned_rows();

  /// Delete stuck-open events (end IS NULL) from third-party cameras whose
  /// start timestamp is older than @p older_than_ms milliseconds.  These are
  /// left behind when the service is restarted mid-detection or when a camera
  /// fires rapid "started" events without matching "ended" events.  Dependent
  /// rows (smartDetectRaws, thumbnails, smartDetectObjects, detectionLabels)
  /// are removed first.  Returns the number of event rows deleted.
  int purge_stale_open_events(uint64_t older_than_ms = 300000);

  /// Set the object detector used to locate subjects for thumbnail cropping.
  /// If not called (or set to nullptr), falls back to the smart-crop heuristic.
  /// The detector must outlive the DetectionRecorder.
  void set_detector(const object_detect::ObjectDetector* detector);

  /// Set the alarm notifier used to trigger UniFi Protect security alarms on
  /// detection events.  If not called (or set to nullptr), alarms are not
  /// notified.  The notifier must outlive the DetectionRecorder.
  /// Must be called before run().
  void set_alarm_notifier(AlarmNotifier* notifier);

  /// Set the object type reported for generic motion events (CellMotionDetector,
  /// VideoSource/MotionAlarm) where the camera does not specify a type.
  /// Valid values: "person" (default), "vehicle", "animal", "package".
  void set_default_object_type(const std::string& type);

  /// Override the detection type for all events from a specific camera IP.
  /// The per-camera type takes precedence over the ONVIF-reported type and
  /// over the default_object_type.
  /// Valid values: "person", "vehicle", "animal", "package".
  void set_camera_object_type(const std::string& ip, const std::string& type);

  /// When override is true the detector is always run, ignoring any ONVIF
  /// bounding box provided by the camera. Has no effect if no detector is set.
  void set_detect_override(bool override);

  // Defined in detection_recorder.cpp -- public so concrete backends in the
  // .cpp translation unit can inherit from it without friendship.
  struct IDbBackend {
    virtual ~IDbBackend() = default;

    virtual absl::Status create_schema() = 0;

    /// Register a camera's identifiers before the listener starts.
    /// Stores ip->id and ip->mac for later lookups.
    virtual void register_camera(const std::string& ip,
                                 const std::string& id,
                                 const std::string& mac) = 0;

    /// Compute the thumbnailId string for an event.
    virtual std::string make_thumbnail_id(const std::string& camera_ip,
                                          uint64_t           ts_ms) = 0;

    /// True when the backend needs a snapshot fetched on detection.
    virtual bool needs_snapshot() const = 0;

    virtual void insert_event(const std::string& id,
                              uint64_t           ts_ms,
                              const std::string& camera_ip,
                              const std::string& sdt_json,
                              const std::string& thumb_id,
                              const std::string& now_str) = 0;

    virtual void insert_sdo(const std::string& id,
                            const std::string& event_id,
                            const std::string& thumb_id,
                            const std::string& camera_ip,
                            const std::string& obj_type,
                            const std::string& attributes,
                            uint64_t           ts_ms,
                            const std::string& now_str) = 0;

    virtual void update_event_end(const std::string& event_id,
                                  uint64_t           end_ms,
                                  const std::string& now_str) = 0;

    /// Store a JPEG thumbnail: INSERT INTO thumbnails.
    virtual void write_thumbnail(const std::string&              thumb_id,
                                 const std::string&              event_id,
                                 const std::string&              camera_ip,
                                 uint64_t                        ts_ms,
                                 const std::string&              now_str,
                                 const std::vector<unsigned char>& jpeg) = 0;

    /// Insert one row into smartDetectRaws with a minimal payload JSON.
    virtual void insert_smart_detect_raw(const std::string& /*id*/,
                                         const std::string& /*camera_ip*/,
                                         uint64_t           /*ts_ms*/,
                                         const std::string& /*obj_type*/,
                                         const std::string& /*now_str*/) {}

    /// Upsert label names into the `labels` table and return their serial lid
    /// values. Existing names are returned from cache to avoid redundant queries.
    /// Returns an empty vector on failure or if not implemented.
    virtual std::vector<int> upsert_labels(
        const std::vector<std::string>& /*names*/,
        const std::string& /*now_str*/) { return {}; }

    /// Insert one row into `detectionLabels`.
    /// Pass object_id empty to store NULL (event-level row).
    /// The event-level row (objectId IS NULL) is required for the INNER JOIN
    /// in Protect's find-anything / detection-search endpoint.
    virtual void insert_detection_label(const std::string&      /*event_id*/,
                                        const std::string&      /*object_id*/,
                                        const std::vector<int>& /*lids*/,
                                        const std::string&      /*now_str*/) {}

    /// One row returned by query_recent_events().
    struct EventSummary {
      std::string id;
      std::string camera_id;
      std::string sdt_json;   // smartDetectTypes text, e.g. '["person"]'
      uint64_t    start_ms{0};
      uint64_t    end_ms{0};
    };

    /// Fetch ended (non-NULL end) smartDetectZone events from the last @p days,
    /// sorted by (camera_id, sdt_json, start_ms). Used by coalesce_history().
    virtual std::vector<EventSummary> query_recent_events(int /*days*/) {
      return {};
    }

    /// Extend the surviving event's end to new_end_ms, then delete the merged
    /// event (from_id) and its dependent rows (smartDetectObjects, thumbnails,
    /// detectionLabels, smartDetectRaws).
    virtual void coalesce_events(const std::string& /*into_id*/,
                                  uint64_t           /*new_end_ms*/,
                                  const std::string& /*from_id*/,
                                  const std::string& /*now_str*/) {}

    /// Delete smartDetectRaws rows for third-party cameras whose timestamp
    /// does not fall within any existing smartDetectZone event for that camera.
    /// Returns the number of rows deleted.
    virtual int purge_orphaned_smart_detect_raws() { return 0; }

    /// Delete thumbnails rows for third-party cameras whose eventId no longer
    /// references an existing event. Returns the number of rows deleted.
    virtual int purge_orphaned_thumbnails() { return 0; }

    /// Delete smartDetectObjects rows for third-party cameras whose eventId no
    /// longer references an existing event. Returns the number of rows deleted.
    virtual int purge_orphaned_smart_detect_objects() { return 0; }

    /// Delete detectionLabels rows whose eventId no longer references an
    /// existing event. Returns the number of rows deleted.
    virtual int purge_orphaned_detection_labels() { return 0; }

    /// Delete stuck-open events (end IS NULL) for third-party cameras whose
    /// start is older than older_than_ms, plus their dependent rows.
    /// Returns the number of event rows deleted.
    virtual int purge_stale_open_events(uint64_t /*older_than_ms*/) { return 0; }
  };

  /// Factory for testing: injects a custom backend (skips PostgreSQL connect).
  static absl::StatusOr<std::unique_ptr<DetectionRecorder>> CreateWithBackend(
      std::unique_ptr<IDbBackend> backend);

 private:
  DetectionRecorder() = default;

  struct SnapshotInfo {
    std::string url;
    std::string user;
    std::string password;
  };

  std::unique_ptr<IDbBackend> db_;
  std::mutex mu_;

  uint64_t pre_buffer_ms_{2000};   // subtracted from event start timestamp
  uint64_t post_buffer_ms_{2000};  // added to event end timestamp

  // Snapshot info per camera IP -- written before run(), read-only after.
  std::map<std::string, SnapshotInfo> snapshot_info_;

  // Camera IP -> UniFi Protect UUID (from register_camera).
  // Written before run(); read-only after that.
  std::map<std::string, std::string> camera_ids_;

  // Camera IP -> MAC address (uppercase, no colons).
  // Written before run(); read-only after that.
  std::map<std::string, std::string> camera_macs_;

  // Directory for per-camera UBV thumbnail files; empty = disabled.
  std::string ubv_dir_;

  // Tracks the UUID of each open (not-yet-ended) event row in `events`.
  // Key: (camera_ip, detection_type)
  std::map<std::pair<std::string, std::string>, std::string> open_;

  // Cameras that have emitted at least one AI-level detection event
  // (FieldDetector or HumanShapeDetect).  CellMotionDetector events from
  // these cameras are suppressed to avoid PTZ-patrol false positives.
  std::set<std::string> ai_capable_cameras_;

  // Cameras that have emitted at least one CellMotionDetector event.
  // VideoSource/MotionAlarm events from these cameras are suppressed to
  // avoid double-counting (both topics fire simultaneously on most cameras).
  std::set<std::string> cell_motion_cameras_;

  // Optional object detector for thumbnail subject cropping.
  // Set before run(); read-only (non-owning pointer) after that.
  const object_detect::ObjectDetector* detector_{nullptr};

  // When true the detector is preferred over ONVIF-provided bounding boxes.
  bool detect_override_{false};

  // Optional alarm notifier. Set before run(); non-owning raw pointer.
  AlarmNotifier* alarm_notifier_{nullptr};

  // Object type for generic motion events (CellMotionDetector, MotionAlarm).
  std::string default_object_type_{"person"};

  // Per-camera type overrides: all events from the keyed IP use this type.
  std::map<std::string, std::string> camera_object_types_;

  // Coalescing: last completed event per (camera_ip, detection_type).
  // real_end_ms is the wall-clock time (ms) when the detection ended, without
  // the post-buffer offset.  0 means the event has been re-opened for coalescing.
  struct LastEventInfo {
    std::string event_id;
    uint64_t    real_end_ms{0};
  };
  std::map<std::pair<std::string, std::string>, LastEventInfo> last_event_;

  // Rate limiting: wall-clock creation timestamps (ms) of recent events per
  // camera IP.  Entries older than one hour are purged before each check.
  std::map<std::string, std::deque<uint64_t>> recent_event_times_;

  uint64_t coalesce_window_ms_{30000};  // --coalesce_window_sec * 1000
  uint32_t max_events_per_hour_{10};    // 0 = unlimited
};

}  // namespace onvif
