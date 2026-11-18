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

#include "event_enricher.hpp"

#include <cstdio>
#include <sstream>
#include <string>

#include "util.hpp"

namespace onvif {
namespace enricher {

namespace {

// Tiny JSON builder used only inside this translation unit.  Hand-rolled
// because the rest of the project doesn't depend on a JSON library and we
// want to keep this code aligned with the existing style in detection_recorder.
// String escaping is delegated to util::json_str.
class Json {
 public:
  std::string str;

  Json& object_open()   { str += '{'; need_comma_ = false; return *this; }
  Json& object_close()  { str += '}'; need_comma_ = true;  return *this; }
  Json& array_open()    { str += '['; need_comma_ = false; return *this; }
  Json& array_close()   { str += ']'; need_comma_ = true;  return *this; }

  // Write a key for the next value.  Inserts a leading ',' if needed.
  Json& key(std::string_view k) {
    if (need_comma_) str += ',';
    str += util::json_str(std::string(k));
    str += ':';
    need_comma_ = false;
    return *this;
  }

  // Write a value inside an array.  Inserts a leading ',' if needed.
  Json& sep() {
    if (need_comma_) str += ',';
    need_comma_ = false;
    return *this;
  }

  Json& val_null() {
    sep(); str += "null"; need_comma_ = true; return *this;
  }
  Json& val_int(int64_t v) {
    sep(); str += std::to_string(v); need_comma_ = true; return *this;
  }
  Json& val_double(double v) {
    sep();
    // Match Python json: "21.0" not "21" (preserve the .0 for floats).
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    str += buf;
    need_comma_ = true;
    return *this;
  }
  Json& val_str(std::string_view v) {
    sep();
    str += util::json_str(std::string(v));
    need_comma_ = true;
    return *this;
  }
  Json& val_bool(bool v) {
    sep();
    str += (v ? "true" : "false");
    need_comma_ = true;
    return *this;
  }

 private:
  bool need_comma_ = false;
};

// Append [a, b, c, ..., n-1] as a JSON integer array.
void appendIntRange(Json& j, int n) {
  j.array_open();
  for (int i = 0; i < n; ++i) j.val_int(i);
  j.array_close();
}

// Determine the detected object's type string from an EventInput.  Uses the
// first entry in smart_detect_types; defaults to "unknown".
std::string DetTypeForEvent(const EventInput& ev) {
  if (!ev.smart_detect_types.empty()) return ev.smart_detect_types.front();
  return "unknown";
}

}  // namespace

Bbox PlaceholderBbox(int width, int height, std::string_view det_type) {
  // TODO: replace with the actual bbox from ONVIF tt:BoundingBox / NanoDet-M
  // when we have it.  These are eyeballed defaults that put a plausible shape
  // on screen so the UI overlay renders.
  int x1, y1, w, h;
  if (det_type == "person") {
    // Tall thin rectangle, centred horizontally, lower half.
    x1 = width  / 3;  w = width  / 4;
    y1 = height / 4;  h = height / 2;
  } else if (det_type == "vehicle") {
    // Wide rectangle, centred, middle third vertically.
    x1 = width  / 4;  w = width  / 2;
    y1 = height / 3;  h = height / 3;
  } else {
    // Generic centred quad.
    x1 = width  / 3;  w = width  / 3;
    y1 = height / 3;  h = height / 3;
  }
  return {x1, y1, x1 + w, y1 + h};
}

std::string FullGridAreaIndexesSqlArray() {
  std::string s = "ARRAY[";
  for (int i = 0; i < kGridCells; ++i) {
    if (i > 0) s += ',';
    s += std::to_string(i);
  }
  s += "]::int[]";
  return s;
}

std::string BuildSmartDetectObjectAttributes(int confidence,
                                              std::string_view det_type,
                                              std::string_view plate) {
  // Matches the 22-field shape Protect 7.1.47 writes for a native person event
  // on a G4 Doorbell.  Most fields are null placeholders; we populate the ones
  // we can synthesise.
  Json j;
  j.object_open();
  // TODO: associatedFaceTrackerID = face tracker id when face detection runs.
  j.key("associatedFaceTrackerID").val_null();
  j.key("blurness").val_null();
  // Only an object read by the camera's own plate reader carries this.
  if (!plate.empty()) j.key("capturedByAiKey").val_bool(true);
  j.key("color").val_null();
  // TODO: per-object confidence.  Using event.score as approximation.
  j.key("confidence").val_int(confidence);
  j.key("faceEmbed").val_null();
  j.key("faceLandmarks").val_null();
  j.key("faceMask").val_null();
  j.key("facePose").val_null();
  j.key("faceVerifyStatus").val_null();
  j.key("line").val_null();
  j.key("matchedId").val_null();
  if (plate.empty()) {
    j.key("matchedName").val_null();
  } else {
    j.key("matchedName").val_str(plate);
  }
  j.key("namesTopK").val_null();
  j.key("objectType").val_str(det_type);
  j.key("personEmbedFromCamera").val_null();
  j.key("qualityScore").val_null();
  j.key("topKCandidate").val_null();
  // TODO: real tracker id (sequential per camera, monotonic across an event).
  j.key("trackerId").val_int(1);
  j.key("vehicleType").val_null();
  // TODO: real zone array from smartDetectZones intersection.
  j.key("zone").array_open().val_int(1).array_close();
  j.object_close();
  return j.str;
}

std::string BuildEnrichedMetadata(const EventInput& ev) {
  const std::string det_type = DetTypeForEvent(ev);
  // One event can carry several classes (a vehicle and its plate); the
  // object_types list names each object.  Callers that only set
  // smart_detect_types fall back to its first entry for every object.
  auto object_type_at = [&](std::size_t i) {
    return i < ev.object_types.size() ? ev.object_types[i] : det_type;
  };
  Json j;
  j.object_open();

  // Marks the event as ours.  Six queries across motion_poller and
  // detection_recorder filter on metadata->>'source' = 'onvif-recorder'
  // to tell our rows from Protect's own -- the high-water-mark init and
  // the orphan purges among them.  The legacy sparse-metadata path has
  // always written it, but this rich path (Protect 7.1+) did not, so on
  // every modern install those queries silently matched nothing: the
  // per-camera hwm fell back to (now - 1h) on every start, and the
  // purges only ever covered third-party cameras via their other branch.
  j.key("source").val_str("onvif-recorder");

  j.key("count").val_null();

  // ---- detectedAreas ----
  j.key("detectedAreas").array_open();
  for (std::size_t i = 0; i < ev.object_ids.size(); ++i) {
    Bbox bb = PlaceholderBbox(ev.image_width, ev.image_height,
                              object_type_at(i));
    int cx = (bb.x1 + bb.x2) / 2;
    int cy = (bb.y1 + bb.y2) / 2;
    j.sep().object_open();
      // TODO: derive real areaIndexes from the bbox + grid intersection.
      j.key("areaIndexes");
      appendIntRange(j, kGridCells);
      j.key("routePath").object_open()
        // TODO: real waypoints sampled across the track.
        .key("lastDirection").array_open().val_int(0).val_int(0).array_close()
        .key("waypoints").array_open()
          .sep().array_open().val_int(cx).val_int(cy).array_close()
        .array_close()
      .object_close();
      j.key("smartDetectObject").val_str(ev.object_ids[i]);
    j.object_close();
  }
  j.array_close();

  // ---- detectedThumbnails ----
  j.key("detectedThumbnails").array_open();
  for (std::size_t i = 0; i < ev.object_ids.size(); ++i) {
    const std::string otype = object_type_at(i);
    Bbox bb = PlaceholderBbox(ev.image_width, ev.image_height, otype);
    int w = bb.x2 - bb.x1;
    int h = bb.y2 - bb.y1;
    j.sep().object_open();
      j.key("attributes").object_open();
        if (otype == "licensePlate" && !ev.plate.empty())
          j.key("capturedByAiKey").val_bool(true);
        j.key("objectType").val_str(otype);
        if (otype == "licensePlate" && !ev.plate.empty())
          j.key("matchedName").val_str(ev.plate);
        // TODO: real tracker id from the camera's analytics stream.
        j.key("trackerId").val_int(1);
        // TODO: real zones array from smartDetectZones intersection.
        j.key("zone").array_open().val_int(1).array_close();
      j.object_close();
      // TODO: clockBestWall should be the wall-clock at the frame the best
      // thumbnail was captured; for now use event.start.
      j.key("clockBestWall").val_int(static_cast<int64_t>(ev.start_ms));
      j.key("confidence").val_int(ev.score);
      // TODO: real per-frame bounding box.  Format is [x, y, w, h].
      j.key("coord").array_open()
        .val_double(static_cast<double>(bb.x1))
        .val_double(static_cast<double>(bb.y1))
        .val_double(static_cast<double>(w))
        .val_double(static_cast<double>(h))
      .array_close();
      j.key("croppedId").val_str(ev.thumbnail_id);
      // TODO: real label strings -- for now we synthesise from event type.
      j.key("labels").array_open()
        .val_str(std::string("smartDetectType:") + otype)
        .val_str(std::string("zone:") + ev.camera_id + ":1")
      .array_close();
      // Protect's own plate entries carry the plate text in name.
      if (otype == "licensePlate" && !ev.plate.empty())
        j.key("name").val_str(ev.plate);
      j.key("objectId").val_str(ev.object_ids[i]);
      j.key("type").val_str(otype);
    j.object_close();
  }
  j.array_close();

  // TODO: hallwayMode mirrors cameras.recordingSettings.hallwayMode at the
  // time of the event.  Stub null for now.
  j.key("hallwayMode").val_null();
  // TODO: lines* fields populated only when smartDetectLines was active.
  j.key("linesSettings").val_null();
  j.key("linesStatus").val_null();
  j.key("loiterStatus").val_null();
  j.key("tamperStatus").val_null();

  // TODO: pull from a real weather source (openweathermap?) at event time.
  // Until then, emit a benign default ("fine, 21°C") so the UI's weather
  // pill renders.  iconCode "32" is the common "sunny/clear day" code in
  // TWC-style icon sets.
  j.key("weather").object_open()
    .key("iconCode").val_str("32")
    .key("temperature").val_double(21.0)
    .key("temperatureUnit").val_str("C")
  .object_close();

  // TODO: real zonesStatus -- level (0-100) is the score the zone reached,
  // status is "enter"/"leave"/"motion".  Synthesised "leave" so the UI
  // treats the event as completed.
  j.key("zonesStatus").object_open()
    .key("1").object_open()
      .key("level").val_int(ev.score)
      .key("status").val_str("leave")
    .object_close()
  .object_close();

  j.object_close();
  return j.str;
}

}  // namespace enricher
}  // namespace onvif
