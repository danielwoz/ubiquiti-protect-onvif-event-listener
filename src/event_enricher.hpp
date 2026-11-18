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

// Translate sparse onvif-recorder-style event rows into the rich shape
// Protect 7.1+ writes natively (events.metadata, smartDetectObjects.attributes,
// smartDetectObjectAreas).  Every synthesised field is annotated with a TODO
// in event_enricher.cpp -- when we have a real source for the data we should
// come back and replace each one.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace onvif {
namespace enricher {

// Protect's UI grid: 12 columns x 10 rows = 120 cells.  Observed in real
// 7.1.47 G4 Doorbell events (highest areaIndex seen: 109).
constexpr int kGridCols  = 12;
constexpr int kGridRows  = 10;
constexpr int kGridCells = kGridCols * kGridRows;

struct Bbox {
  int x1;
  int y1;
  int x2;
  int y2;
};

struct EventInput {
  std::string event_id;             // events.id
  std::string camera_id;            // events.cameraId
  std::string event_type;           // "smartDetectZone", "smartDetectLine", ...
  std::vector<std::string> smart_detect_types;  // ["vehicle"], ["person"], ...
  int      score        = 0;        // events.score (0..100)
  std::string thumbnail_id;         // MSR-format "{MAC}-{ms}"
  uint64_t start_ms     = 0;
  uint64_t end_ms       = 0;
  int      image_width  = 2560;
  int      image_height = 1440;
  // UUIDs already present in detectionLabels.objectId for this event;
  // these become smartDetectObjects.id values so the FK chain stays intact.
  std::vector<std::string> object_ids;
  // smartDetectType of each object, parallel to object_ids.  One event can
  // carry more than one class (a vehicle and its licence plate).  A
  // missing entry falls back to smart_detect_types.front().
  std::vector<std::string> object_types;
  // Licence-plate text.  Non-empty adds a name and matchedName to the
  // licensePlate object's metadata entries.
  std::string plate;
};

// Synth a placeholder bbox sized for the given detection type.
// TODO: replace with the real ONVIF tt:BoundingBox / NanoDet-M output.
Bbox PlaceholderBbox(int width, int height, std::string_view det_type);

// Build events.metadata JSON for an enriched event (matches the shape of a
// Protect 7.1+ native event).
std::string BuildEnrichedMetadata(const EventInput& ev);

// Build smartDetectObjects.attributes JSON for one detected object.
// `det_type` is the smartDetectType ("person", "vehicle", ...).  A
// non-empty `plate` writes the text to matchedName (and marks the object
// as read by the camera's own reader), the shape Protect uses for a
// licensePlate object.
std::string BuildSmartDetectObjectAttributes(int confidence,
                                              std::string_view det_type,
                                              std::string_view plate = {});

// Render the full areaIndexes int[] literal for SQL: e.g. "ARRAY[0,1,...,119]".
// TODO: derive from bbox-vs-grid intersection instead of full coverage.
std::string FullGridAreaIndexesSqlArray();

}  // namespace enricher
}  // namespace onvif
