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
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "jpeg_crop.hpp"

// On-device object detector using NCNN + NanoDet-M.
//
// Detects COCO classes relevant to security cameras:
//   person (0), bicycle (1), car (2), motorcycle (3), bus (5), truck (7),
//   bird (14), cat (15), dog (16), horse (17), sheep (18), cow (19)
//
// Compiled only when WITH_NCNN is defined (i.e. the NCNN library is present).
// Without NCNN, detect() always returns std::nullopt (falls back to smart_crop).
//
// Model files: nanodet_m.param + nanodet_m.bin from:
//   https://github.com/nihui/ncnn-assets/tree/master/models

namespace object_detect {

// COCO class IDs relevant to security cameras.
// Caller can filter by these if desired.
inline bool is_security_relevant(int class_id) {
  switch (class_id) {
    case 0:   // person
    case 1:   // bicycle
    case 2:   // car
    case 3:   // motorcycle
    case 5:   // bus
    case 7:   // truck
    case 14:  // bird
    case 15:  // cat
    case 16:  // dog
    case 17:  // horse
    case 18:  // sheep
    case 19:  // cow
      return true;
    default:
      return false;
  }
}

/// Result of a successful detection: bounding box + COCO class ID.
struct Detection {
  jpeg_crop::BoundingBox bbox;
  int class_id;  // COCO class ID (see is_security_relevant())
};

/// Map a COCO class ID to a UniFi Protect detection type string.
/// Covers the security-relevant subset detected by NanoDet-M.
/// Returns "person" for class 0 and any unrecognised ID.
inline std::string detection_type(int class_id) {
  switch (class_id) {
    case 1:   // bicycle
    case 2:   // car
    case 3:   // motorcycle
    case 5:   // bus
    case 7:   // truck
      return "vehicle";
    case 14:  // bird
    case 15:  // cat
    case 16:  // dog
    case 17:  // horse
    case 18:  // sheep
    case 19:  // cow
      return "animal";
    default:
      return "person";  // 0 = person; also fallback for anything else
  }
}

class ObjectDetector {
 public:
  /// Load model from .param and .bin files.
  /// Returns error if NCNN is not compiled in (WITH_NCNN undefined) or if
  /// the model files cannot be read.
  static absl::StatusOr<std::unique_ptr<ObjectDetector>> Load(
      const std::string& param_path, const std::string& bin_path);

  ~ObjectDetector();

  ObjectDetector(const ObjectDetector&)            = delete;
  ObjectDetector& operator=(const ObjectDetector&) = delete;

  /// Detect objects in a JPEG image.
  /// Returns the highest-confidence security-relevant detection (bounding box
  /// in normalised [0,1] coordinates + COCO class ID), or nullopt if none
  /// found above the confidence threshold.
  std::optional<Detection> detect(
      const std::vector<uint8_t>& jpeg_bytes) const;

 private:
  ObjectDetector() = default;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace object_detect
