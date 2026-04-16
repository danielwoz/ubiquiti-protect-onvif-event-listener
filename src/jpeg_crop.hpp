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
#include <vector>

// ---------------------------------------------------------------------------
// JPEG crop utilities
//
// Decodes an input JPEG, crops a region centred on the detection subject, and
// re-encodes to JPEG.  The crop is a square whose side equals the shorter
// dimension of the source image, biased vertically toward the lower portion of
// the frame — matching the typical overhead mounting angle of security cameras.
//
// When an explicit bounding box is available (e.g. parsed from ONVIF analytics
// extensions) the caller can supply a normalised BoundingBox instead of using
// the default smart-crop heuristic.
// ---------------------------------------------------------------------------

namespace jpeg_crop {

// Normalised bounding box, all values in [0.0, 1.0].
// (x, y) is the top-left corner; (w, h) is width × height of the subject.
struct BoundingBox {
  float x = 0.0f;  ///< left edge as fraction of frame width
  float y = 0.0f;  ///< top edge as fraction of frame height
  float w = 1.0f;  ///< subject width as fraction of frame width
  float h = 1.0f;  ///< subject height as fraction of frame height
};

/// Crop padding added around an explicit bounding box (fraction of the shorter
/// dimension of the source image).  Avoids tightly clipping the subject.
static constexpr float kBboxPadding = 0.15f;

/// Re-encode quality for the cropped JPEG (0–100).
static constexpr int kOutputQuality = 85;

/// Compute a smart-crop square for a security-camera frame.
///
/// Returns a BoundingBox (in normalised image coordinates) whose square
/// region is centred horizontally and positioned so its vertical centre is
/// at 60 % of the frame height — capturing the lower field where subjects
/// typically appear when cameras are mounted overhead.
///
/// @param width   source image width  in pixels
/// @param height  source image height in pixels
BoundingBox smart_crop_box(int width, int height);

/// Crop and re-encode a JPEG.
///
/// Decodes @p input_jpeg, crops the region described by @p box, and
/// re-encodes the result at @p kOutputQuality.
///
/// Returns an empty vector on any decode/encode error.
std::vector<uint8_t> crop(const std::vector<uint8_t>& input_jpeg,
                           const BoundingBox& box);

/// Convenience wrapper: applies the smart-crop heuristic automatically.
///
/// Reads the source image dimensions, computes the smart-crop box, and
/// calls crop().  Returns an empty vector on error.
std::vector<uint8_t> smart_crop(const std::vector<uint8_t>& input_jpeg);

/// Select the best crop region given optional inputs.
///
/// Priority: onvif_bbox > det_bbox > smart_crop heuristic.
/// @param width       source image width  in pixels
/// @param height      source image height in pixels
/// @param onvif_bbox  ONVIF-provided bbox (may be nullptr)
/// @param det_bbox    Detector-provided bbox (may be nullptr)
BoundingBox select_crop_box(int width, int height,
                             const BoundingBox* onvif_bbox,
                             const BoundingBox* det_bbox);

}  // namespace jpeg_crop
