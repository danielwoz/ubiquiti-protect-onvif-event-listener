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
/// Returns the number of cameras updated.
absl::StatusOr<int> rollback_camera_changes(
    const std::string& scope,
    const std::string& log_path,
    const std::vector<std::string>& first_party_ids,
    const DbConfig& db = {});

}  // namespace unifi
