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
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace ubv {

// ---------------------------------------------------------------------------
// UBV thumbnail container format
//
// Ubiquiti's proprietary container used by UniFi Protect to store per-event
// JPEG thumbnail images.  A single .ubv file holds an ordered sequence of
// frames; each frame's timestamp_ms matches the value embedded in the
// UniFi Protect database's events.thumbnailId  ("<MAC>-<timestamp_ms>").
//
// Binary record layout (all multi-byte integers are big-endian):
//
//   [4]  type        — 0xa00009a9 file-header | 0xa0da7e04 frame-meta
//                    |  0xa04a709a JPEG frame
//   [4]  codec       — 0xfd020000 meta/header | 0xfd020001 JPEG
//   [8]  timestamp   — milliseconds since Unix epoch
//   [4]  payload_len — byte length N of the following payload
//   [N]  payload     — raw JPEG bytes (for JPEG records)
//   [4]  back_ref    — 20 + N; enables backward scanning
//
// File structure:
//   [file-header record]
//   ( [frame-meta record] [JPEG record] ) × frame count
// ---------------------------------------------------------------------------

/// One decoded JPEG thumbnail.
struct Frame {
    uint64_t             timestamp_ms;  ///< milliseconds since Unix epoch
    std::vector<uint8_t> jpeg;          ///< raw JPEG bytes
};

/// Decode all JPEG thumbnail frames from a UBV file.
/// Returns error Status on I/O or format errors.
absl::StatusOr<std::vector<Frame>> decode(const std::string& path);

/// Decode the single JPEG frame whose timestamp matches @p timestamp_ms.
/// Scans sequentially and returns as soon as the frame is found.
/// Returns error Status if not found or on I/O error.
absl::StatusOr<Frame> decode_one(const std::string& path, uint64_t timestamp_ms);

/// Encode @p frames into a UBV file at @p path (overwritten if it exists).
/// Returns error Status if @p frames is empty or on I/O error.
absl::Status encode(const std::string& path, const std::vector<Frame>& frames);

/// Append a single frame to a UBV file at @p path.
/// If the file does not exist (or is empty) it is created with a fresh
/// file-header record first.  Subsequent calls append only the meta+JPEG
/// record pair, making this suitable for a continuously-growing thumbnail log.
/// Returns error Status on I/O error.
absl::Status append(const std::string& path, const Frame& frame);

/// Build the native Protect UBV thumbnail path for a camera and create
/// the date directory if it does not exist.
///
/// The returned path follows the Protect convention:
///   {base_dir}/YYYY/MM/DD/{MAC}_0_thumbnails_{epoch_ms}.ubv
///
/// If a file for this MAC already exists in that day's directory it is reused
/// (the epoch_ms in the existing filename is kept).  Otherwise a new path is
/// returned using @p timestamp_ms for the filename timestamp.
///
/// @p base_dir  The Protect video root (e.g. /srv/unifi-protect/video).
/// @p mac       Camera MAC (uppercase, no colons, e.g. "FC5F49CA68D4").
/// @p timestamp_ms  Milliseconds since epoch of the frame being written.
std::string protect_path(const std::string& base_dir,
                         const std::string& mac,
                         uint64_t timestamp_ms);

}  // namespace ubv
