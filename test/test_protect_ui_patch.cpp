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
 * test_protect_ui_patch.cpp
 *
 * Tests for the protect_ui_patch module's internal apply/revert logic.
 * Includes the .cpp directly to access file-local (static) functions.
 */

// Include the implementation directly to test file-local (static) functions.
#include "src/protect_ui_patch.cpp"  // NOLINT(build/include)

using namespace protect_ui;  // NOLINT(build/namespaces)

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* label) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << label << '\n';
  }
}

static std::string temp_dir() {
  const char* d = std::getenv("TEST_TMPDIR");
  return d ? d : "/tmp";
}

static std::string read_test_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return {std::istreambuf_iterator<char>(f),
          std::istreambuf_iterator<char>()};
}

static bool write_test_file(const std::string& path,
                            const std::string& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  return f.good();
}

// ---------------------------------------------------------------
// Test 1: Patch string lengths are consistent
// ---------------------------------------------------------------
static void test_patch_lengths() {
  for (size_t i = 0; i < kUiPatchCount; ++i) {
    const Patch& p = kUiPatches[i];
    check(std::strlen(p.original) == p.len,
          "UI patch original len matches");
    check(std::strlen(p.replacement) == p.len,
          "UI patch replacement len matches");
    check(std::strlen(p.original) == std::strlen(p.replacement),
          "UI patch same byte length");
  }
  for (size_t i = 0; i < kBackendPatchCount; ++i) {
    const Patch& p = kBackendPatches[i];
    check(std::strlen(p.original) == p.len,
          "Backend patch original len matches");
    check(std::strlen(p.replacement) == p.len,
          "Backend patch replacement len matches");
  }
}

// ---------------------------------------------------------------
// Test 2: apply_patches on a file containing the original strings
// ---------------------------------------------------------------
static void test_apply_patches() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_apply.js";
  std::string bak = path + ".bak";
  std::remove(path.c_str());
  std::remove(bak.c_str());

  // Build content containing all three UI patch originals.
  std::string content = "prefix_";
  content += kUiPatch1.original;
  content += "_middle_";
  content += kUiPatch2.original;
  content += "_gap_";
  content += kUiPatch3.original;
  content += "_suffix";

  write_test_file(path, content);

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(path, kUiPatches, kUiPatchCount, empty_md5);
  check(n == 3, "apply_patches returns 3");

  // Verify replacements.
  std::string patched = read_test_file(path);
  check(patched.find(kUiPatch1.replacement) != std::string::npos,
        "patch1 replacement present");
  check(patched.find(kUiPatch2.replacement) != std::string::npos,
        "patch2 replacement present");
  check(patched.find(kUiPatch3.replacement) != std::string::npos,
        "patch3 replacement present");

  // Original strings should be gone.
  check(patched.find(kUiPatch1.original) == std::string::npos,
        "patch1 original removed");

  // File size should be unchanged (same-length replacements).
  check(patched.size() == content.size(), "file size unchanged");

  // .bak should have been created with original content.
  std::string backup = read_test_file(bak);
  check(backup == content, ".bak contains original");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// ---------------------------------------------------------------
// Test 3: Already-patched file is a no-op
// ---------------------------------------------------------------
static void test_already_patched() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_noop.js";
  std::remove(path.c_str());

  // Content already has the replacement strings.
  std::string content = "prefix_";
  content += kUiPatch1.replacement;
  content += "_middle_";
  content += kUiPatch2.replacement;
  content += "_suffix";

  write_test_file(path, content);

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(path, kUiPatches, kUiPatchCount, empty_md5);
  check(n == 0, "already-patched returns 0");

  // No .bak should be created.
  std::string bak = read_test_file(path + ".bak");
  check(bak.empty(), "no .bak created for already-patched");

  std::remove(path.c_str());
}

// ---------------------------------------------------------------
// Test 4: Missing file returns -1
// ---------------------------------------------------------------
static void test_missing_file() {
  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches("/nonexistent/path.js", kUiPatches, kUiPatchCount,
                        empty_md5);
  check(n == -1, "missing file returns -1");
}

// ---------------------------------------------------------------
// Test 5: Backend patches work
// ---------------------------------------------------------------
static void test_backend_patch() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_service.js";
  std::remove(path.c_str());

  std::string content = "var x = cameras.filter(e => e.isAdopted";
  content += kBackendPatch1.original;
  content += ");";

  write_test_file(path, content);

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(path, kBackendPatches, kBackendPatchCount, empty_md5);
  check(n == 1, "backend patch returns 1");

  std::string patched = read_test_file(path);
  check(patched.find(kBackendPatch1.replacement) != std::string::npos,
        "backend replacement present");
  check(patched.size() == content.size(), "backend file size unchanged");

  std::remove(path.c_str());
  std::remove((path + ".bak").c_str());
}

// ---------------------------------------------------------------
// Test 6: dpkg md5sum backup logic
//
// When md5sums map is non-empty:
//   - If live file matches dpkg md5 -> overwrite .bak
//   - If live file does NOT match -> keep existing .bak
// ---------------------------------------------------------------
static void test_dpkg_backup_logic() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_dpkg.js";
  std::string bak = path + ".bak";
  std::remove(path.c_str());
  std::remove(bak.c_str());

  std::string content = "prefix_";
  content += kUiPatch1.original;
  content += "_suffix";

  write_test_file(path, content);

  // Simulate a dpkg md5sums map where the live file does NOT match.
  // This means the file has been modified — should NOT overwrite .bak.
  std::unordered_map<std::string, std::string> md5sums;
  std::string rel = path.substr(1);  // remove leading /
  md5sums[rel] = "0000000000000000aaaaaaaaaaaaaaaa";  // wrong md5

  // Pre-create a .bak with different content (simulating old backup).
  std::string old_bak = "old backup content";
  write_test_file(bak, old_bak);

  int n = apply_patches(path, kUiPatches, kUiPatchCount, md5sums);
  check(n == 1, "dpkg: patch applied");

  // .bak should still have the old content (not overwritten).
  std::string bak_content = read_test_file(bak);
  check(bak_content == old_bak, "dpkg: existing .bak preserved");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// ---------------------------------------------------------------
// Test 7: Partial patch (only some patterns present)
// ---------------------------------------------------------------
static void test_partial_patch() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_partial.js";
  std::remove(path.c_str());

  // Only include patch 1, not patches 2 or 3.
  std::string content = "header_";
  content += kUiPatch1.original;
  content += "_footer";

  write_test_file(path, content);

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(path, kUiPatches, kUiPatchCount, empty_md5);
  check(n == 1, "partial: only 1 patch applied");

  std::string patched = read_test_file(path);
  check(patched.find(kUiPatch1.replacement) != std::string::npos,
        "partial: patch1 applied");

  std::remove(path.c_str());
  std::remove((path + ".bak").c_str());
}

int main() {
  test_patch_lengths();
  test_apply_patches();
  test_already_patched();
  test_missing_file();
  test_backend_patch();
  test_dpkg_backup_logic();
  test_partial_patch();

  std::cout << "test_protect_ui_patch: "
            << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
