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

#include "ubv_thumbnail.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <array>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace ubv {

// ---------------------------------------------------------------------------
// Record type and codec constants (logical, host-byte-order values)
// ---------------------------------------------------------------------------
static constexpr uint32_t TYPE_FILE_HEADER = 0xa00009a9u;
static constexpr uint32_t TYPE_META        = 0xa0da7e04u;
static constexpr uint32_t TYPE_JPEG        = 0xa04a709au;

static constexpr uint32_t CODEC_META       = 0xfd020000u;
static constexpr uint32_t CODEC_JPEG       = 0xfd020001u;

// Byte size of the fixed record header fields (type+codec+timestamp+len).
static constexpr uint32_t RECORD_HEADER_SIZE = 20u;

// ---------------------------------------------------------------------------
// Big-endian I/O helpers
// ---------------------------------------------------------------------------
static uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
       | (static_cast<uint32_t>(p[2]) <<  8) |  static_cast<uint32_t>(p[3]);
}

static uint64_t be64(const uint8_t* p) {
  return (static_cast<uint64_t>(be32(p)) << 32) | static_cast<uint64_t>(be32(p + 4));
}

static void put_be32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint32_t>(v >> 24); p[1] = static_cast<uint32_t>(v >> 16);
  p[2] = static_cast<uint32_t>(v >>  8); p[3] = static_cast<uint32_t>(v);
}

static void put_be64(uint8_t* p, uint64_t v) {
  put_be32(p,     static_cast<uint32_t>(v >> 32));
  put_be32(p + 4, static_cast<uint32_t>(v));
}

// ---------------------------------------------------------------------------
// Read one complete record from @p in.
// Sets eof=true and returns OkStatus on clean EOF.
// Returns error Status on partial/corrupt data.
// Sets eof=false on successful read.
// ---------------------------------------------------------------------------
struct Record {
  uint32_t             type;
  uint32_t             codec;
  uint64_t             timestamp_ms;
  std::vector<uint8_t> payload;
};

static absl::Status read_record(std::ifstream& in, Record& out, bool& eof) {
  eof = false;
  uint8_t hdr[RECORD_HEADER_SIZE];
  if (!in.read(reinterpret_cast<char*>(hdr), RECORD_HEADER_SIZE)) {
    if (in.eof() && in.gcount() == 0) {
      eof = true;
      return absl::OkStatus();
    }
    return absl::InternalError("ubv: truncated record header");
  }

  out.type         = be32(hdr + 0);
  out.codec        = be32(hdr + 4);
  out.timestamp_ms = be64(hdr + 8);
  const uint32_t n = be32(hdr + 16);

  out.payload.resize(n);
  if (n > 0) {
    in.read(reinterpret_cast<char*>(out.payload.data()), n);
    if (in.gcount() < static_cast<std::streamsize>(n)) {
      // truncated at end of file -- stop cleanly
      eof = true;
      return absl::OkStatus();
    }
  }

  uint8_t trailer[4];
  if (!in.read(reinterpret_cast<char*>(trailer), 4))
    return absl::InternalError("ubv: truncated record trailer");

  const uint32_t back_ref = be32(trailer);
  if (back_ref == RECORD_HEADER_SIZE + n) {
    // Normal case: back_ref correctly encodes 20 + payload_len.
  } else if (back_ref == 0) {
    // Some JPEG records use back_ref=0 and are followed by a small number
    // of alignment/padding bytes before the next record's 0xa0 type marker.
    // Scan forward byte-by-byte until we find the next record start.
    uint8_t b;
    for (int limit = 16; limit > 0; --limit) {
      if (!in.read(reinterpret_cast<char*>(&b), 1)) break;
      if (b == 0xa0u) {
        in.seekg(-1, std::ios::cur);
        break;
      }
    }
  } else {
    return absl::InternalError(
      "ubv: unexpected back_ref " + std::to_string(back_ref)
      + " (expected " + std::to_string(RECORD_HEADER_SIZE + n) + ')');
  }

  return absl::OkStatus();
}

// ---------------------------------------------------------------------------
// Write one record to @p out.
// ---------------------------------------------------------------------------
static void write_record(std::ofstream& out,
                         uint32_t type, uint32_t codec, uint64_t ts,
                         const uint8_t* payload, uint32_t n) {
  uint8_t hdr[RECORD_HEADER_SIZE];
  put_be32(hdr + 0,  type);
  put_be32(hdr + 4,  codec);
  put_be64(hdr + 8,  ts);
  put_be32(hdr + 16, n);

  uint8_t trailer[4];
  put_be32(trailer, RECORD_HEADER_SIZE + n);

  out.write(reinterpret_cast<const char*>(hdr), RECORD_HEADER_SIZE);
  if (n > 0)
    out.write(reinterpret_cast<const char*>(payload), n);
  out.write(reinterpret_cast<const char*>(trailer), 4);
}

// ---------------------------------------------------------------------------
// decode -- extract all JPEG frames
// ---------------------------------------------------------------------------
absl::StatusOr<std::vector<Frame>> decode(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open())
    return absl::InternalError("ubv::decode: cannot open " + path);

  std::vector<Frame> frames;
  Record rec;
  bool eof = false;
  while (true) {
    absl::Status st = read_record(in, rec, eof);
    if (!st.ok()) return st;
    if (eof) break;
    if (rec.type != TYPE_JPEG) continue;
    if (rec.payload.size() < 2
        || rec.payload[0] != 0xff || rec.payload[1] != 0xd8)
      continue;  // not a real JPEG, skip
    frames.push_back({rec.timestamp_ms, std::move(rec.payload)});
  }
  return frames;
}

// ---------------------------------------------------------------------------
// decode_one -- find a single frame by timestamp
// ---------------------------------------------------------------------------
absl::StatusOr<Frame> decode_one(const std::string& path, uint64_t timestamp_ms) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open())
    return absl::InternalError("ubv::decode_one: cannot open " + path);

  Record rec;
  bool eof = false;
  while (true) {
    absl::Status st = read_record(in, rec, eof);
    if (!st.ok()) return st;
    if (eof) break;
    if (rec.type != TYPE_JPEG)             continue;
    if (rec.timestamp_ms != timestamp_ms)  continue;
    if (rec.payload.size() < 2
        || rec.payload[0] != 0xff || rec.payload[1] != 0xd8)
      continue;
    return Frame{rec.timestamp_ms, std::move(rec.payload)};
  }
  return absl::NotFoundError(
    "ubv::decode_one: no frame with timestamp "
    + std::to_string(timestamp_ms) + " in " + path);
}

// ---------------------------------------------------------------------------
// encode -- write frames into a new UBV file
// ---------------------------------------------------------------------------
absl::Status encode(const std::string& path, const std::vector<Frame>& frames) {
  if (frames.empty())
    return absl::InvalidArgumentError("ubv::encode: no frames provided");

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open())
    return absl::InternalError("ubv::encode: cannot open " + path);

  const uint64_t first_ts = frames.front().timestamp_ms;

  // File-header record: fixed 32-byte zeroed payload.
  static const uint8_t file_hdr_payload[32]{};
  write_record(out, TYPE_FILE_HEADER, CODEC_META, first_ts,
               file_hdr_payload, sizeof(file_hdr_payload));

  // One [meta + JPEG] pair per frame.
  static const uint8_t meta_payload[8]{};
  for (const auto& f : frames) {
    write_record(out, TYPE_META, CODEC_META, f.timestamp_ms,
                 meta_payload, sizeof(meta_payload));
    write_record(out, TYPE_JPEG, CODEC_JPEG, f.timestamp_ms,
                 f.jpeg.data(), static_cast<uint32_t>(f.jpeg.size()));
  }

  if (!out.flush())
    return absl::InternalError("ubv::encode: write error on " + path);
  return absl::OkStatus();
}

// ---------------------------------------------------------------------------
// append -- add one frame to an existing UBV file (create if new)
// ---------------------------------------------------------------------------
absl::Status append(const std::string& path, const Frame& frame) {
  // Determine whether the file already exists and has content.
  bool is_new = true;
  {
    std::ifstream probe(path, std::ios::binary | std::ios::ate);
    if (probe.is_open() && probe.tellg() > 0)
      is_new = false;
  }

  static const uint8_t file_hdr_payload[32]{};
  static const uint8_t meta_payload[8]{};

  if (is_new) {
    // Create file and write the file-header record first.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
      return absl::InternalError("ubv::append: cannot create " + path);

    write_record(out, TYPE_FILE_HEADER, CODEC_META, frame.timestamp_ms,
                 file_hdr_payload, sizeof(file_hdr_payload));
    write_record(out, TYPE_META, CODEC_META, frame.timestamp_ms,
                 meta_payload, sizeof(meta_payload));
    write_record(out, TYPE_JPEG, CODEC_JPEG, frame.timestamp_ms,
                 frame.jpeg.data(), static_cast<uint32_t>(frame.jpeg.size()));

    if (!out.flush())
      return absl::InternalError("ubv::append: write error on " + path);
  } else {
    // Append a meta + JPEG pair to the existing file.
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out.is_open())
      return absl::InternalError("ubv::append: cannot open for append " + path);

    write_record(out, TYPE_META, CODEC_META, frame.timestamp_ms,
                 meta_payload, sizeof(meta_payload));
    write_record(out, TYPE_JPEG, CODEC_JPEG, frame.timestamp_ms,
                 frame.jpeg.data(), static_cast<uint32_t>(frame.jpeg.size()));

    if (!out.flush())
      return absl::InternalError("ubv::append: write error on " + path);
  }
  return absl::OkStatus();
}

// ---------------------------------------------------------------------------
// protect_path -- build native Protect UBV thumbnail path
// ---------------------------------------------------------------------------

// Recursive mkdir (like mkdir -p).
static void mkdirs(const std::string& path) {
  struct stat st{};
  if (stat(path.c_str(), &st) == 0) return;
  // Ensure parent exists.
  size_t slash = path.rfind('/');
  if (slash != std::string::npos && slash > 0)
    mkdirs(path.substr(0, slash));
  mkdir(path.c_str(), 0755);
}

std::string protect_path(const std::string& base_dir,
                         const std::string& mac,
                         uint64_t timestamp_ms) {
  // Compute YYYY/MM/DD from timestamp.
  std::time_t sec = static_cast<std::time_t>(timestamp_ms / 1000);
  std::tm tm{};
  gmtime_r(&sec, &tm);
  char date_buf[16];
  std::strftime(date_buf, sizeof(date_buf), "%Y/%m/%d", &tm);

  std::string dir = base_dir + "/" + date_buf;
  mkdirs(dir);

  // Look for an existing file for this MAC in today's directory.
  std::string prefix = mac + "_0_thumbnails_";
  DIR* d = opendir(dir.c_str());
  if (d) {
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
      std::string name = entry->d_name;
      if (name.compare(0, prefix.size(), prefix) == 0 &&
          name.size() > 4 &&
          name.compare(name.size() - 4, 4, ".ubv") == 0) {
        closedir(d);
        return dir + "/" + name;
      }
    }
    closedir(d);
  }

  // No existing file -- create a new path.
  return dir + "/" + prefix + std::to_string(timestamp_ms) + ".ubv";
}

}  // namespace ubv
