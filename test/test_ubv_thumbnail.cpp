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

/**
 * test_ubv_thumbnail.cpp
 *
 * Round-trip test for the ubv::decode / ubv::encode library.
 *
 * Self-contained mode (default, used by bazel test):
 *   Reads snapshot_108.jpg and snapshot_109.jpg, encodes them into a
 *   temporary UBV file, decodes it back, and verifies round-trip fidelity.
 *
 * File mode (manual inspection):
 *   bazel run //test:test_ubv_thumbnail -- <path-to.ubv>
 *   Decodes an existing UBV file and runs the same verification steps.
 */

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "ubv_thumbnail.hpp"

static std::vector<uint8_t> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    std::cerr << "cannot open: " << path << '\n';
    return {};
  }
  auto sz = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> buf(static_cast<size_t>(sz));
  f.read(reinterpret_cast<char*>(buf.data()), sz);
  if (!f) {
    std::cerr << "read error: " << path << '\n';
    return {};
  }
  return buf;
}

static bool check_markers(const std::vector<ubv::Frame>& frames) {
  bool ok = true;
  for (size_t i = 0; i < frames.size(); ++i) {
    const auto& j = frames[i].jpeg;
    const bool soi = j.size() >= 2 && j[0] == 0xff && j[1] == 0xd8;
    const bool eoi = j.size() >= 2 && j[j.size()-2] == 0xff && j[j.size()-1] == 0xd9;
    if (!soi || !eoi) {
      std::cerr << "  [" << i << "] BAD JPEG markers"
                << " SOI=" << soi << " EOI=" << eoi << '\n';
      ok = false;
    }
  }
  return ok;
}

// -- Self-contained round-trip test ──────────────────────────────────────────
// Finds the snapshot JPEGs relative to the binary (Bazel runfiles).
// New date directories and thumbnail files must match Protect's video tree:
// the parent's group and (capped) permission bits, so Protect's recorder (msr,
// user ms) can still create recording files in a day directory the recorder
// (root) created first.  Before the fix they were 0755 / 0644 regardless of the
// parent, and root:root when run as root, so msr failed with EACCES.  The
// chown itself only happens as root and isn't exercised here.
static bool check_date_tree_modes(const ubv::Frame& frame, mode_t base_mode,
                                  mode_t want_dir, mode_t want_file) {
  const char* tmpdir = std::getenv("TEST_TMPDIR");  // set by bazel test
  std::string tmpl = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") +
                     "/ubv_perm_test_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (!mkdtemp(buf.data())) {
    std::cerr << "  mkdtemp failed\n";
    return false;
  }
  const std::string base = buf.data();
  chmod(base.c_str(), base_mode);
  struct stat base_st{};
  stat(base.c_str(), &base_st);

  const uint64_t ts = 1795423223001ULL;  // 2026-11-23 UTC
  const std::string path = ubv::protect_path(base, "AABBCCDDEEFF", ts);
  bool ok = ubv::append(path, {ts, frame.jpeg}).ok();
  if (!ok) std::cerr << "  append failed: " << path << '\n';

  const std::string day = path.substr(0, path.rfind('/'));
  const std::vector<std::pair<std::string, mode_t>> want_modes = {
      {base + "/2026", want_dir}, {base + "/2026/11", want_dir},
      {day, want_dir}, {path, want_file}};
  for (const auto& [p, want] : want_modes) {
    struct stat st{};
    if (stat(p.c_str(), &st) != 0) {
      std::cerr << "  missing: " << p << '\n';
      ok = false;
      continue;
    }
    const mode_t got = st.st_mode & 0777;
    if (got != want || st.st_gid != base_st.st_gid) {
      std::cerr << "  " << p << ": mode " << std::oct << got << " want " << want
                << std::dec << ", gid " << st.st_gid << " want " << base_st.st_gid
                << '\n';
      ok = false;
    }
  }
  unlink(path.c_str());
  rmdir(day.c_str());
  rmdir((base + "/2026/11").c_str());
  rmdir((base + "/2026").c_str());
  chmod(base.c_str(), 0700);
  rmdir(base.c_str());
  return ok;
}

static bool check_protect_path_permissions(const ubv::Frame& frame) {
  std::cout << "\n--- protect_path()/append() permissions ---\n";
  // Protect's tree (ms:unifi-streaming 0770): group-writable, like msr's own.
  bool ok = check_date_tree_modes(frame, 0770, 0770, 0660);
  if (ok) std::cout << "  0770 parent -> folders 0770, files 0660: OK\n";
  // A world-writable parent must not make thumbnails world-writable.
  const bool capped = check_date_tree_modes(frame, 0777, 0775, 0664);
  if (capped) std::cout << "  0777 parent -> folders 0775, files 0664 (capped): OK\n";
  return ok && capped;
}

static int run_self_contained() {
  // Locate snapshot files in the Bazel runfiles tree.
  const char* srcdir = std::getenv("TEST_SRCDIR");
  const char* workspace = std::getenv("TEST_WORKSPACE");
  std::string base;
  if (srcdir && workspace)
    base = std::string(srcdir) + "/" + workspace + "/test/";
  else
    base = "test/";  // fallback for manual runs from project root

  const std::string path108 = base + "testdata/snapshot_108.jpg";
  const std::string path109 = base + "testdata/snapshot_109.jpg";
  const std::string ubv_out = "/tmp/ubv_roundtrip_test.ubv";

  std::cout << "=== Self-contained round-trip test ===\n";

  // Build input frames from the two snapshot JPEGs.
  std::vector<ubv::Frame> input_frames;
  auto bytes108 = read_file(path108);
  auto bytes109 = read_file(path109);
  if (bytes108.empty() || bytes109.empty()) {
    std::cerr << "FAIL: could not read snapshot files\n";
    return 1;
  }
  input_frames.push_back({1000000000000ULL, std::move(bytes108)});
  input_frames.push_back({1000000001000ULL, std::move(bytes109)});

  std::cout << "  Loaded " << input_frames.size() << " frames from snapshot JPEGs\n";
  for (size_t i = 0; i < input_frames.size(); ++i)
    std::cout << "  [" << i << "] ts=" << input_frames[i].timestamp_ms
              << "  jpeg=" << input_frames[i].jpeg.size() << " bytes\n";

  // Verify input JPEG markers.
  if (!check_markers(input_frames)) {
    std::cerr << "FAIL: input snapshot files have bad JPEG markers\n";
    return 1;
  }

  // Encode.
  {
    auto s = ubv::encode(ubv_out, input_frames);
    if (!s.ok()) {
      std::cerr << "FAIL encode: " << s.message() << '\n';
      return 1;
    }
    std::cout << "  Encoded -> " << ubv_out << '\n';
  }

  // Decode.
  std::vector<ubv::Frame> decoded;
  {
    auto frames_or = ubv::decode(ubv_out);
    if (!frames_or.ok()) {
      std::cerr << "FAIL decode: " << frames_or.status().message() << '\n';
      return 1;
    }
    decoded = std::move(*frames_or);
    std::cout << "  Decoded " << decoded.size() << " frame(s)\n";
  }

  // Compare.
  bool all_ok = true;
  if (input_frames.size() != decoded.size()) {
    std::cerr << "  MISMATCH frame count: " << input_frames.size()
              << " vs " << decoded.size() << '\n';
    all_ok = false;
  } else {
    for (size_t i = 0; i < input_frames.size(); ++i) {
      const bool ts_ok   = input_frames[i].timestamp_ms == decoded[i].timestamp_ms;
      const bool jpeg_ok = input_frames[i].jpeg         == decoded[i].jpeg;
      if (!ts_ok || !jpeg_ok) {
        std::cerr << "  [" << i << "] MISMATCH"
                  << (ts_ok   ? "" : " timestamp")
                  << (jpeg_ok ? "" : " jpeg_bytes") << '\n';
        all_ok = false;
      }
    }
  }

  // Also test append() builds the same result one frame at a time.
  const std::string ubv_append = "/tmp/ubv_append_test.ubv";
  std::remove(ubv_append.c_str());
  {
    bool append_ok = true;
    for (const auto& f : input_frames) {
      auto s = ubv::append(ubv_append, f);
      if (!s.ok()) {
        std::cerr << "FAIL append: " << s.message() << '\n';
        all_ok = false;
        append_ok = false;
        break;
      }
    }
    if (append_ok) {
      auto appended_or = ubv::decode(ubv_append);
      if (!appended_or.ok()) {
        std::cerr << "FAIL decode after append: "
                  << appended_or.status().message() << '\n';
        all_ok = false;
      } else {
        const auto& appended = *appended_or;
        if (appended.size() != input_frames.size()) {
          std::cerr << "  MISMATCH append frame count\n";
          all_ok = false;
        } else {
          for (size_t i = 0; i < input_frames.size(); ++i) {
            if (input_frames[i].timestamp_ms != appended[i].timestamp_ms ||
                input_frames[i].jpeg         != appended[i].jpeg) {
              std::cerr << "  [" << i << "] MISMATCH after append\n";
              all_ok = false;
            }
          }
        }
        if (all_ok) std::cout << "  append() round-trip OK\n";
      }
    }
  }

  if (!input_frames.empty() && !check_protect_path_permissions(input_frames[0]))
    all_ok = false;

  std::cout << "\n=== Result ===\n";
  if (all_ok) {
    std::cout << "PASS: " << input_frames.size()
              << " frame(s), encode/decode/append round-trip identical\n";
    return 0;
  }
  std::cout << "FAIL\n";
  return 1;
}

// -- File-based mode (manual inspection of an existing UBV) ──────────────────
static int run_file_mode(const std::string& input_path) {
  std::cout << "=== Step 1: Decode " << input_path << " ===\n";
  std::vector<ubv::Frame> frames;
  {
    auto frames_or = ubv::decode(input_path);
    if (!frames_or.ok()) {
      std::cerr << "FAIL decode: " << frames_or.status().message() << '\n';
      return 1;
    }
    frames = std::move(*frames_or);
  }
  if (frames.empty()) {
    std::cerr << "FAIL: no JPEG frames found\n";
    return 1;
  }
  std::cout << "  Decoded " << frames.size() << " frame(s)\n";
  for (size_t i = 0; i < frames.size(); ++i)
    std::cout << "  [" << i << "] ts=" << frames[i].timestamp_ms
              << "  jpeg=" << frames[i].jpeg.size() << " bytes\n";

  std::cout << "\n=== Step 2: Verify JPEG markers ===\n";
  const bool markers_ok = check_markers(frames);
  if (markers_ok)
    std::cout << "  All " << frames.size() << " frame(s) have valid SOI+EOI\n";

  std::cout << "\n=== Step 3: Re-encode ===\n";
  const std::string reenc = "/tmp/ubv_test_reencoded.ubv";
  {
    auto s = ubv::encode(reenc, frames);
    if (!s.ok()) {
      std::cerr << "FAIL encode: " << s.message() << '\n';
      return 1;
    }
    std::cout << "  Encoded -> " << reenc << '\n';
  }

  std::cout << "\n=== Step 4: Decode re-encoded ===\n";
  std::vector<ubv::Frame> frames2;
  {
    auto frames2_or = ubv::decode(reenc);
    if (!frames2_or.ok()) {
      std::cerr << "FAIL decode2: " << frames2_or.status().message() << '\n';
      return 1;
    }
    frames2 = std::move(*frames2_or);
    std::cout << "  Decoded " << frames2.size() << " frame(s)\n";
  }

  std::cout << "\n=== Step 5: Compare ===\n";
  bool all_ok = markers_ok;
  if (frames.size() != frames2.size()) {
    std::cerr << "  MISMATCH frame count: " << frames.size()
              << " vs " << frames2.size() << '\n';
    all_ok = false;
  } else {
    for (size_t i = 0; i < frames.size(); ++i) {
      const bool ts_ok   = frames[i].timestamp_ms == frames2[i].timestamp_ms;
      const bool jpeg_ok = frames[i].jpeg         == frames2[i].jpeg;
      if (!ts_ok || !jpeg_ok) {
        std::cerr << "  [" << i << "] MISMATCH"
                  << (ts_ok   ? "" : " timestamp")
                  << (jpeg_ok ? "" : " jpeg_bytes") << '\n';
        all_ok = false;
      }
    }
  }

  std::cout << "\n=== Result ===\n";
  if (all_ok) {
    std::cout << "PASS: " << frames.size() << " frame(s), round-trip identical\n";
    return 0;
  }
  std::cout << "FAIL\n";
  return 1;
}

int main(int argc, char* argv[]) {
  if (argc >= 2)
    return run_file_mode(argv[1]);
  return run_self_contained();
}
