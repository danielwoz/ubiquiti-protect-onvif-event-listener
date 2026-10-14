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
#include <map>
#include <set>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "onvif_listener.hpp"

namespace unifi {

class CameraChangeLog;  // forward declaration

/// Connection parameters for the UniFi Protect PostgreSQL instance.
struct DbConfig {
  std::string host     = "127.0.0.1";
  int         port     = 5433;
  std::string dbname   = "unifi-protect";
  std::string user     = "postgres";
  std::string password = "";
};

/// Minimal info about a first-party camera discovered from the cameras table.
struct FirstPartyCamera {
  std::string id;
  std::string name;
  std::string mac;
};

namespace internal {

/// Build a libpq conninfo string from a DbConfig.  Exposed for testing.
std::string build_connstr(const DbConfig& db);

/// Extract a string value for @p key from a flat JSONB object @p json.
/// Handles string, numeric, boolean and null values; returns empty string on
/// null or missing key.  Supports basic backslash escapes.  Exposed for
/// testing.
std::string json_get(const std::string& json, const std::string& key);

/// Build a PostgreSQL array literal from @p ids, e.g. {"id1","id2"}.
/// Exposed for testing.
std::string pg_array(const std::vector<std::string>& ids);

/// Some camera models advertise a snapshotUrl via ONVIF that isn't actually
/// the working snapshot endpoint -- the classic case is Dahua (and Dahua
/// OEM rebrands like Amcrest), which advertises /onvif/snapshot?... but
/// only serves JPEGs from /cgi-bin/snapshot.cgi.  When @p camera_type
/// matches a known-broken vendor AND @p original_url's path is the
/// /onvif/snapshot one, this returns a rewritten URL with the path
/// replaced.  Otherwise @p original_url is returned unchanged.
///
/// Operators can override the per-camera snapshot URL via the
/// --camera_snapshot_urls flag; that override applies AFTER load_cameras
/// so it still wins over this auto-fix.
///
/// Exposed for testing.
std::string maybe_rewrite_dahua_snapshot_url(const std::string& camera_type,
                                              const std::string& original_url);

}  // namespace internal

/// Connect to the UniFi Protect database and return a CameraConfig for every
/// adopted third-party (ONVIF) camera.
///
/// Reads the `cameras` table where `isThirdPartyCamera = true` and
/// `isAdopted = true`, extracting the IP from `host` and credentials from
/// the `thirdPartyCameraInfo` JSONB column.
///
/// Returns error Status on connection or query failure.
absl::StatusOr<std::vector<onvif::CameraConfig>> load_cameras(
    const DbConfig& db = {});

/// Load specific first-party cameras by ID.  Returns only cameras where
/// `isThirdPartyCamera = false` and `isAdopted = true` that match the
/// provided IDs.
absl::StatusOr<std::vector<FirstPartyCamera>> load_first_party_cameras(
    const std::vector<std::string>& camera_ids,
    const DbConfig& db = {});

/// Auto-discover all adopted first-party cameras that lack native smart
/// detection (featureFlags.hasSmartDetect is null or false).  These are
/// eligible for NanoDet-M motion-to-smart-detect polling.
absl::StatusOr<std::vector<FirstPartyCamera>>  // NOLINT(whitespace/indent_namespace)
load_all_nonsmartdetect_first_party(const DbConfig& db = {});

/// Load every adopted first-party camera regardless of smart-detect state.
/// Used by the admin UI to populate the first-party tickbox list.
absl::StatusOr<std::vector<FirstPartyCamera>>  // NOLINT(whitespace/indent_namespace)
load_all_first_party(const DbConfig& db = {});

/// Aggregated health row used by the admin UI's Camera Health card.
struct CameraHealth {
  std::string id;
  std::string name;
  std::string host;        ///< IP / DNS name
  std::string mac;
  bool        is_third_party{false};
  uint64_t    last_event_ms{0};   ///< 0 if never
  uint64_t    events_1h{0};
};

/// Single-query camera health snapshot for every adopted camera (third-
/// and first-party).  Returns rows ordered with third-party first then by
/// name.  last_event_ms is the most recent events.start across all event
/// types; events_1h is the count of events in the last hour.
absl::StatusOr<std::vector<CameraHealth>>  // NOLINT(whitespace/indent_namespace)
load_camera_health(const DbConfig& db = {});

/// Recent-event row used by the admin UI's Recent Events panel.
struct RecentEvent {
  std::string id;
  std::string type;
  std::string camera_id;
  std::string camera_name;
  std::string thumbnail_id;     ///< empty when no thumbnail
  std::string smart_detect_types_json;  ///< raw JSON, e.g. ["person"]
  uint64_t    start_ms{0};
  uint64_t    end_ms{0};
  bool        thumbnail_in_db{false};   ///< true if thumbnails.id == thumbnail_id
};

/// Last @p limit smart-detect events (smartDetectZone / smartDetectLine /
/// smartAudioDetect / motion) ordered by start DESC.  Skips events with no
/// thumbnail_id.  Used by /api/recent_events.
absl::StatusOr<std::vector<RecentEvent>>  // NOLINT(whitespace/indent_namespace)
load_recent_events(int limit = 30, const DbConfig& db = {});

/// Read a thumbnail blob from the thumbnails table.  Returns the raw JPEG
/// bytes, or an empty vector on miss / error.  MSR-stored thumbnails (IDs
/// of length != 24) are not in this table; callers should treat misses as
/// "MSR-stored, fetch via MSR" and may show a placeholder.
absl::StatusOr<std::vector<uint8_t>>  // NOLINT(whitespace/indent_namespace)
load_thumbnail_bytes(const std::string& thumbnail_id,
                     const DbConfig& db = {});

/// Search for adopted first-party cameras whose `type` column contains any
/// of the given @p model_substrings (case-insensitive ILIKE match).
/// For example, passing {"G3 Instant"} matches cameras with type
/// "UVC G3 Instant".  Returns a vector of matching cameras.
absl::StatusOr<std::vector<FirstPartyCamera>>
load_first_party_cameras_by_model(
    const std::vector<std::string>& model_substrings,
    const DbConfig& db = {});

/// For each camera in `ids`, ensure that smart detection is enabled in the
/// Protect database.  Specifically, for any camera whose
/// `featureFlags.smartDetectTypes` or `smartDetectSettings.objectTypes` is
/// empty, sets both to ["person","vehicle"] and updates `updatedAt`.
///
/// This is idempotent — cameras already configured are not touched.
/// Returns error Status on connection or query failure.
///
/// When @p log is non-null, captures old values before each UPDATE and
/// records the change.
absl::Status enable_smart_detect(
    const std::vector<onvif::CameraConfig>& cameras,
    const DbConfig& db = {},
    CameraChangeLog* log = nullptr);

/// Enable smart detection for first-party cameras identified by ID.
/// Same behaviour as the CameraConfig overload but uses FirstPartyCamera.
absl::Status enable_smart_detect(
    const std::vector<FirstPartyCamera>& cameras,
    const DbConfig& db = {},
    CameraChangeLog* log = nullptr);

/// Read back `featureFlags.smartDetectTypes` from Postgres for
/// @p camera_ids, as raw JSON array text (e.g. `["person","vehicle"]`
/// or `[]`).  Cameras absent from the table are omitted from the map.
///
/// Used by the featureFlags drift check: what we wrote here is compared
/// against what Protect reports through its own API, to detect a
/// long-running Protect process holding stale in-memory camera state
/// (issue #34 -- Protect never re-reads the row, so our smartDetectZone
/// events get filtered out of the UI even though the DB is correct).
absl::StatusOr<std::map<std::string, std::string>>
read_smart_detect_flags(const std::vector<std::string>& camera_ids,
                        const DbConfig& db = {});

/// IDs of cameras paired with an AI Port.  The AI Port supplies their
/// smart detections and Protect owns their settings.  Empty on Protect
/// versions without AI Port support.
absl::StatusOr<std::set<std::string>> load_ai_port_paired_ids(
    const DbConfig& db = {});

/// Remove the "licensePlate" entry that enable_smart_detect() adds to
/// `featureFlags.smartDetectTypes` from the listed third-party cameras that
/// are not paired with an AI Port.  Protect's AI Port pairing dialog hides
/// cameras that report licence-plate or face detection (issue #67).
/// Returns the IDs that changed.
absl::StatusOr<std::vector<std::string>> clear_license_plate_marker(
    const std::vector<std::string>& camera_ids,
    const DbConfig& db = {},
    CameraChangeLog* log = nullptr);

/// Ensure the listed cameras have at least one smart-detect zone in the
/// Protect database.  Cameras with an empty `smartDetectZones` array are
/// updated with a single full-frame Default zone covering person and vehicle
/// object types.
///
/// This makes cameras appear in the `/protect/alarms/new` trigger
/// dropdowns, which filter on `smartDetectZones.length > 0`.
///
/// Idempotent — cameras that already have zones are not modified.
/// Returns error Status on connection or query failure.
absl::Status ensure_smart_detect_zones(
    const std::vector<onvif::CameraConfig>& cameras,
    const DbConfig& db = {},
    CameraChangeLog* log = nullptr);

/// Overload for first-party cameras.
absl::Status ensure_smart_detect_zones(
    const std::vector<FirstPartyCamera>& cameras,
    const DbConfig& db = {},
    CameraChangeLog* log = nullptr);

/// Set `thirdPartyCameraInfo.enableRtspAudio` for every adopted third-party
/// camera that has `hasAudio = true`.
///
/// @p enable  true  → set enableRtspAudio to true
///            false → set enableRtspAudio to false
///
/// Idempotent — cameras already at the requested value are still updated
/// (no read-modify-check; the write is cheap).
/// Returns error Status on connection or query failure.
absl::Status set_rtsp_audio(bool enable, const DbConfig& db = {},
                             CameraChangeLog* log = nullptr);

/// Detect the thumbnail ID format used by native first-party cameras.
///
/// Queries the most recent thumbnailId from events belonging to adopted
/// first-party cameras.  If the ID has length != 24 (e.g. "MAC-timestamp"
/// MSR format) returns true — callers should generate IDs in the same format.
/// Returns false when no native events are found or all use the 24-char DB
/// format.
absl::StatusOr<bool> detect_native_msr_thumbnail_format(
    const DbConfig& db = {});

/// Undo cameras-table changes and return.
///
/// @p scope   "third_party", "first_party", or "all".
/// @p log_path  Path to the change log.  If the file exists, changes are
///              reversed using recorded old values.  If it does not exist and
///              scope includes third-party cameras, a best-effort guesstimate
///              resets smartDetectTypes, objectTypes, and smartDetectZones to
///              empty arrays.
/// @p first_party_ids  Camera IDs from --first_party_cameras (used when scope
///                     includes first-party cameras and the log exists).
///
/// Cameras paired with an AI Port are skipped.
///
/// Returns the number of cameras updated.
absl::StatusOr<int> rollback_camera_changes(
    const std::string& scope,
    const std::string& log_path,
    const std::vector<std::string>& first_party_ids,
    const DbConfig& db = {});

}  // namespace unifi
