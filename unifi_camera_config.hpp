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

/// Connection parameters for the UniFi Protect PostgreSQL instance.
struct DbConfig {
  std::string host     = "127.0.0.1";
  int         port     = 5433;
  std::string dbname   = "unifi-protect";
  std::string user     = "postgres";
  std::string password = "";
};

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

/// For each camera in `ids`, ensure that smart detection is enabled in the
/// Protect database.  Specifically, for any camera whose
/// `featureFlags.smartDetectTypes` or `smartDetectSettings.objectTypes` is
/// empty, sets both to ["person","vehicle"] and updates `updatedAt`.
///
/// This is idempotent — cameras already configured are not touched.
/// Returns error Status on connection or query failure.
absl::Status enable_smart_detect(
    const std::vector<onvif::CameraConfig>& cameras,
    const DbConfig& db = {});

/// Ensure every third-party (ONVIF) camera has at least one smart-detect zone
/// in the Protect database.  Cameras with an empty `smartDetectZones` array are
/// updated with a single full-frame Default zone covering person and vehicle
/// object types.
///
/// This makes ONVIF cameras appear in the `/protect/alarms/new` trigger
/// dropdowns, which filter on `smartDetectZones.length > 0`.
///
/// Idempotent — cameras that already have zones are not modified.
/// Returns error Status on connection or query failure.
absl::Status ensure_smart_detect_zones(
    const std::vector<onvif::CameraConfig>& cameras,
    const DbConfig& db = {});

}  // namespace unifi
