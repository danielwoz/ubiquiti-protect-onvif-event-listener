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

// Unit tests for the event_enricher library.  We don't pull in a JSON parser
// (the project doesn't depend on one) so the assertions look for required
// substrings + structural anchors rather than parse the JSON.  Substring
// checks are sufficient because the producer is hand-rolled and deterministic.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "event_enricher.hpp"

namespace {

// Cheap assert wrapper that prints a context-rich message and exits non-zero.
#define CHECK(expr) do { \
  if (!(expr)) { \
    std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    std::exit(1); \
  } \
} while (0)

#define CHECK_CONTAINS(haystack, needle) do { \
  if ((haystack).find(needle) == std::string::npos) { \
    std::fprintf(stderr, "CHECK_CONTAINS failed at %s:%d: needle=%s\n  haystack head=%.200s...\n", \
                 __FILE__, __LINE__, needle, (haystack).c_str()); \
    std::exit(1); \
  } \
} while (0)

void test_placeholder_bbox_person() {
  auto bb = onvif::enricher::PlaceholderBbox(2560, 1440, "person");
  // Tall thin: width / 4 (640), height / 2 (720).
  CHECK(bb.x2 - bb.x1 == 640);
  CHECK(bb.y2 - bb.y1 == 720);
  // Centred horizontally: x1 ~ width/3.
  CHECK(bb.x1 == 2560 / 3);
}

void test_placeholder_bbox_vehicle() {
  auto bb = onvif::enricher::PlaceholderBbox(2560, 1440, "vehicle");
  // Wide: width / 2 (1280), height / 3 (480).
  CHECK(bb.x2 - bb.x1 == 1280);
  CHECK(bb.y2 - bb.y1 == 480);
}

void test_placeholder_bbox_unknown_falls_back() {
  auto bb = onvif::enricher::PlaceholderBbox(2560, 1440, "totally_made_up_class");
  // Default centred quad: width / 3, height / 3.
  CHECK(bb.x2 - bb.x1 == 2560 / 3);
  CHECK(bb.y2 - bb.y1 == 1440 / 3);
}

void test_full_grid_area_indexes() {
  std::string s = onvif::enricher::FullGridAreaIndexesSqlArray();
  CHECK_CONTAINS(s, "ARRAY[0,1,2");
  CHECK_CONTAINS(s, "118,119]::int[]");
}

void test_smart_detect_object_attributes_person() {
  std::string s = onvif::enricher::BuildSmartDetectObjectAttributes(85, "person");
  // All 22 keys must be present.
  for (const char* key : {"associatedFaceTrackerID", "blurness", "color",
                          "confidence", "faceEmbed", "faceLandmarks",
                          "faceMask", "facePose", "faceVerifyStatus", "line",
                          "matchedId", "matchedName", "namesTopK",
                          "objectType", "personEmbedFromCamera", "qualityScore",
                          "topKCandidate", "trackerId", "vehicleType", "zone"}) {
    std::string quoted = std::string("\"") + key + "\":";
    CHECK_CONTAINS(s, quoted.c_str());
  }
  CHECK_CONTAINS(s, "\"objectType\":\"person\"");
  CHECK_CONTAINS(s, "\"confidence\":85");
  CHECK_CONTAINS(s, "\"trackerId\":1");
  CHECK_CONTAINS(s, "\"zone\":[1]");
}

void test_smart_detect_object_attributes_license_plate() {
  std::string s = onvif::enricher::BuildSmartDetectObjectAttributes(
      100, "licensePlate", "XX-111-X");
  CHECK_CONTAINS(s, "\"matchedName\":\"XX-111-X\"");
  CHECK_CONTAINS(s, "\"capturedByAiKey\":true");
  CHECK_CONTAINS(s, "\"objectType\":\"licensePlate\"");

  // Without a plate the attribute stays null and the flag absent.
  std::string p = onvif::enricher::BuildSmartDetectObjectAttributes(
      100, "person");
  CHECK_CONTAINS(p, "\"matchedName\":null");
  CHECK(p.find("capturedByAiKey") == std::string::npos);
}

// One event can carry two objects, a vehicle and its plate.  The plate
// entry is typed licensePlate and carries the text in matchedName and
// name, the way Protect's own LPR events do.
void test_metadata_license_plate_object() {
  onvif::enricher::EventInput ev;
  ev.event_id = "plate-event";
  ev.camera_id = "cam";
  ev.event_type = "smartDetectZone";
  ev.smart_detect_types = {"vehicle", "licensePlate"};
  ev.thumbnail_id = "AA-3";
  ev.start_ms = 1779465293952ULL;
  ev.end_ms = 1779465312040ULL;
  ev.object_ids = {"veh-uuid", "plate-uuid"};
  ev.object_types = {"vehicle", "licensePlate"};
  ev.plate = "XX-111-X";

  std::string md = onvif::enricher::BuildEnrichedMetadata(ev);
  CHECK_CONTAINS(md, "\"smartDetectObject\":\"veh-uuid\"");
  CHECK_CONTAINS(md, "\"smartDetectObject\":\"plate-uuid\"");
  CHECK_CONTAINS(md, "\"objectType\":\"vehicle\"");
  CHECK_CONTAINS(md, "\"objectType\":\"licensePlate\"");
  CHECK_CONTAINS(md, "\"matchedName\":\"XX-111-X\"");
  CHECK_CONTAINS(md, "\"name\":\"XX-111-X\"");
  CHECK_CONTAINS(md, "\"type\":\"licensePlate\"");
  CHECK_CONTAINS(md, "\"smartDetectType:licensePlate\"");
  CHECK_CONTAINS(md, "\"capturedByAiKey\":true");
}

void test_metadata_has_seven_top_level_keys() {
  onvif::enricher::EventInput ev;
  ev.event_id = "test-event-id";
  ev.camera_id = "test-cam";
  ev.event_type = "smartDetectZone";
  ev.smart_detect_types = {"vehicle"};
  ev.score = 100;
  ev.thumbnail_id = "AABBCCDDEEFF-1779465315956";
  ev.start_ms = 1779465293952ULL;
  ev.end_ms = 1779465312040ULL;
  ev.image_width = 2560;
  ev.image_height = 1440;
  ev.object_ids = {"obj-uuid-1"};

  std::string md = onvif::enricher::BuildEnrichedMetadata(ev);

  // Top-level keys that Protect 7.1.47 writes natively.
  for (const char* key : {"count", "detectedAreas", "detectedThumbnails",
                          "hallwayMode", "linesSettings", "linesStatus",
                          "loiterStatus", "tamperStatus", "weather",
                          "zonesStatus"}) {
    std::string quoted = std::string("\"") + key + "\":";
    CHECK_CONTAINS(md, quoted.c_str());
  }

  // Weather is the hardcoded default.
  CHECK_CONTAINS(md, "\"iconCode\":\"32\"");
  CHECK_CONTAINS(md, "\"temperature\":21.0");
  CHECK_CONTAINS(md, "\"temperatureUnit\":\"C\"");

  // zonesStatus uses event.score for level.
  CHECK_CONTAINS(md, "\"level\":100");
  CHECK_CONTAINS(md, "\"status\":\"leave\"");

  // detectedAreas references our object uuid via smartDetectObject.
  CHECK_CONTAINS(md, "\"smartDetectObject\":\"obj-uuid-1\"");

  // detectedThumbnails carries the right object type + label strings.
  CHECK_CONTAINS(md, "\"objectType\":\"vehicle\"");
  CHECK_CONTAINS(md, "\"smartDetectType:vehicle\"");
  CHECK_CONTAINS(md, "\"zone:test-cam:1\"");

  // croppedId mirrors thumbnailId so the UI resolves via MSR.
  CHECK_CONTAINS(md, "\"croppedId\":\"AABBCCDDEEFF-1779465315956\"");

  // clockBestWall is the event.start placeholder.
  CHECK_CONTAINS(md, "\"clockBestWall\":1779465293952");
}

void test_metadata_with_no_objects_has_empty_arrays() {
  onvif::enricher::EventInput ev;
  ev.event_id = "no-obj";
  ev.camera_id = "cam";
  ev.event_type = "smartDetectZone";
  ev.smart_detect_types = {"person"};
  ev.thumbnail_id = "AA-1";
  // object_ids deliberately empty.
  std::string md = onvif::enricher::BuildEnrichedMetadata(ev);
  CHECK_CONTAINS(md, "\"detectedAreas\":[]");
  CHECK_CONTAINS(md, "\"detectedThumbnails\":[]");
}

void test_metadata_multi_object_emits_one_entry_per_object() {
  onvif::enricher::EventInput ev;
  ev.event_id = "multi";
  ev.camera_id = "cam";
  ev.event_type = "smartDetectZone";
  ev.smart_detect_types = {"person"};
  ev.thumbnail_id = "AA-2";
  ev.object_ids = {"u1", "u2", "u3"};
  std::string md = onvif::enricher::BuildEnrichedMetadata(ev);
  // Count occurrences of "smartDetectObject":"u" (one per object).
  size_t pos = 0, count = 0;
  while ((pos = md.find("\"smartDetectObject\":\"u", pos)) != std::string::npos) {
    ++count; ++pos;
  }
  CHECK(count == 3);
}

}  // namespace


// Six queries across motion_poller and detection_recorder identify our
// rows by metadata->>'source'.  The rich path omitted it for the whole of
// Protect 7.1+, so the per-camera high-water mark silently fell back to
// (now - 1h) on every start and the orphan purges only ever matched
// third-party cameras.  Nothing failed loudly -- pin it.
static void test_metadata_carries_source_tag() {
  onvif::enricher::EventInput ev;
  ev.event_id = "evt-src";
  ev.camera_id = "cam-src";
  ev.event_type = "smartDetectZone";
  ev.smart_detect_types = {"person"};
  ev.start_ms = 1779465293952ULL;
  ev.end_ms = 1779465312040ULL;
  ev.image_width = 2560;
  ev.image_height = 1440;
  ev.object_ids = {"obj-uuid-1"};

  std::string md = onvif::enricher::BuildEnrichedMetadata(ev);
  CHECK_CONTAINS(md, "\"source\":\"onvif-recorder\"");

  // Also true with no objects -- the tag is about provenance, not content.
  ev.object_ids.clear();
  std::string empty_md = onvif::enricher::BuildEnrichedMetadata(ev);
  CHECK_CONTAINS(empty_md, "\"source\":\"onvif-recorder\"");
}

int main() {
  test_placeholder_bbox_person();
  test_placeholder_bbox_vehicle();
  test_placeholder_bbox_unknown_falls_back();
  test_full_grid_area_indexes();
  test_smart_detect_object_attributes_person();
  test_smart_detect_object_attributes_license_plate();
  test_metadata_license_plate_object();
  test_metadata_has_seven_top_level_keys();
  test_metadata_carries_source_tag();
  test_metadata_with_no_objects_has_empty_arrays();
  test_metadata_multi_object_emits_one_entry_per_object();
  std::printf("event_enricher tests passed\n");
  return 0;
}
