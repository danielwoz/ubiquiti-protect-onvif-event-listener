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
#include <cstring>
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
  content += kUiPatch1a.original;
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
  check(patched.find(kUiPatch1a.replacement) != std::string::npos,
        "patch1 replacement present");
  check(patched.find(kUiPatch2.replacement) != std::string::npos,
        "patch2 replacement present");
  check(patched.find(kUiPatch3.replacement) != std::string::npos,
        "patch3 replacement present");

  // Original strings should be gone.
  check(patched.find(kUiPatch1a.original) == std::string::npos,
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
  content += kUiPatch1a.replacement;
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

  // Protect 7.3.70 uses the same fragment in the ONVIF stream-settings
  // validator *before* the scope predicate.  Only the predicate may change.
  const std::string validator =
      "if((g||f||p||m||h||u)&&!e.isThirdPartyCamera)throw new "
      "i.BadRequestError(\"Camera is not a third-party (ONVIF) camera\");";
  const std::string scope =
      "scope_all_ui_cameras:e=>(0,t.deviceNotDeleted)(e)&&e.isAdopted&&"
      "(0,s.isCamera)(e)&&!e.isThirdPartyCamera,"
      "scope_all_cameras_with_nfcs:e=>e.isAdopted";
  const std::string content =
      validator + "x={scope_all_ui_cameras:_};" + scope;
  write_test_file(path, content);

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(path, kBackendPatches, kBackendPatchCount, empty_md5);
  check(n == 1, "backend patch returns 1");

  std::string patched = read_test_file(path);
  check(patched.find(validator) == 0,
        "backend: ONVIF stream-settings validator left untouched");
  check(patched.find("(0,s.isCamera)(e)/*isThirdPartyCamera */,"
                     "scope_all_cameras_with_nfcs") != std::string::npos,
        "backend: scope predicate patched");
  check(patched.size() == content.size(), "backend file size unchanged");

  // Re-running is a no-op.
  check(apply_patches(path, kBackendPatches, kBackendPatchCount,
                      empty_md5) == 0,
        "backend: second run is a no-op");

  std::remove(path.c_str());
  std::remove((path + ".bak").c_str());
}

// An anchor that occurs more than once no longer identifies one piece of
// code, so the patch must be skipped and the file left alone.
static void test_ambiguous_anchor_skipped() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_ambiguous.js";
  std::remove(path.c_str());

  std::string content = "a=";
  content += kUiPatch3.original;
  content += ";b=";
  content += kUiPatch3.original;
  write_test_file(path, content);

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(path, kUiPatches, kUiPatchCount, empty_md5);
  check(n == 0, "ambiguous: nothing applied");
  check(read_test_file(path) == content, "ambiguous: file unchanged");

  std::remove(path.c_str());
  std::remove((path + ".bak").c_str());
}

// Patches written for pre-7.0.57 bundles must not run on newer Protect,
// where the same expression is the microphone capability check.
static void test_version_gate() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_gate.js";
  std::remove(path.c_str());

  std::string content = "eqy=e=>";
  content += kUiPatch2.original;
  content += "?!!e.thirdPartyCameraInfo?.enableRtspAudio:e.featureFlags.hasMic";
  write_test_file(path, content);

  std::unordered_map<std::string, std::string> empty_md5;
  const onvif::protect_version::Version v7370 = {7, 3, 70};
  int n = apply_patches(path, kUiPatches, kUiPatchCount, empty_md5, &v7370);
  check(n == 0, "gate: pre-7.0.57 patch not applied on 7.3.70");
  check(read_test_file(path) == content, "gate: file unchanged on 7.3.70");

  const onvif::protect_version::Version v7050 = {7, 0, 50};
  n = apply_patches(path, kUiPatches, kUiPatchCount, empty_md5, &v7050);
  check(n == 1, "gate: pre-7.0.57 patch applied on 7.0.50");

  std::remove(path.c_str());
  std::remove((path + ".bak").c_str());
}

// A file patched by an older rule set (here: the stream-settings validator
// instead of the scope predicate) must be rebuilt from the pristine .bak
// when dpkg confirms the .bak is the package original.
static void test_rebuild_from_pristine_bak() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_rebuild_service.js";
  std::string bak = path + ".bak";
  std::remove(path.c_str());
  std::remove(bak.c_str());

  const std::string original =
      "if((g)&&!e.isThirdPartyCamera)throw 1;"
      "scope_all_ui_cameras:e=>e.isAdopted&&!e.isThirdPartyCamera,scope_x:1";
  std::string mispatched = original;
  mispatched.replace(mispatched.find(kBackendPatch1.original),
                     kBackendPatch1.len, kBackendPatch1.replacement);
  write_test_file(bak, original);
  write_test_file(path, mispatched);

  std::unordered_map<std::string, std::string> md5sums;
  md5sums[path.substr(1)] = md5_of_file(bak);

  int n = apply_patches(path, kBackendPatches, kBackendPatchCount, md5sums);
  check(n == 1, "rebuild: one patch applied to the pristine source");

  std::string expected = original;
  size_t at = expected.find("e.isAdopted&&!e.isThirdPartyCamera");
  expected.replace(at + std::strlen("e.isAdopted"), kBackendPatch1.len,
                   kBackendPatch1.replacement);
  check(read_test_file(path) == expected,
        "rebuild: validator restored and scope predicate patched");
  check(read_test_file(bak) == original, "rebuild: pristine .bak kept");

  std::remove(path.c_str());
  std::remove(bak.c_str());
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
  content += kUiPatch1a.original;
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
  content += kUiPatch1a.original;
  content += "_footer";

  write_test_file(path, content);

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(path, kUiPatches, kUiPatchCount, empty_md5);
  check(n == 1, "partial: only 1 patch applied");

  std::string patched = read_test_file(path);
  check(patched.find(kUiPatch1a.replacement) != std::string::npos,
        "partial: patch1 applied");

  std::remove(path.c_str());
  std::remove((path + ".bak").c_str());
}

// ---------------------------------------------------------------
// Test: revert_alarm_picker_in restores files byte-for-byte from .bak.
// Simulates the production sequence: write original -> apply (creates
// .bak + modifies live) -> revert -> live must equal original.
// ---------------------------------------------------------------
static void test_revert_byte_for_byte() {
  const std::string ui_dir = temp_dir() + "/ui_revert_test";
  std::system(("rm -rf " + ui_dir + " && mkdir -p " + ui_dir).c_str());

  // Build a minimal swai.js containing the v1.4.3+ patch1a pattern.
  std::string original;
  original += "/* head */\n";
  original += kUiPatch1a.original;
  original += "\n/* tail */\n";
  // Pad to 1 KiB to make any drift byte-detectable.
  original.resize(1024, ' ');

  const std::string live_path = ui_dir + "/swai.js";
  check(write_test_file(live_path, original), "write swai.js original");

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(live_path, kUiPatches, kUiPatchCount, empty_md5);
  check(n >= 1, "apply produced at least 1 replacement");

  // Sanity: live file is now patched, .bak holds the original.
  std::string patched = read_test_file(live_path);
  check(patched != original, "live file modified by apply");
  check(read_test_file(live_path + ".bak") == original,
        ".bak holds the original byte-for-byte");

  // Revert from the test-only directory, no service.js path.
  auto s = revert_alarm_picker_in(ui_dir, /*service_path=*/"");
  const std::string revert_msg =
      std::string("revert ok: ") + std::string(s.message());
  check(s.ok(), revert_msg.c_str());

  std::string restored = read_test_file(live_path);
  check(restored == original, "live file restored byte-for-byte");

  std::system(("rm -rf " + ui_dir).c_str());
}

// ---------------------------------------------------------------
// Test: nginx block injection places the block inside the server
// block, not inside a previously-injected location block.
// Regression for the v1.4.3/v1.4.4 bug where the admin-proxy block
// got nested inside the log-viewer location, causing
//   "location /onvif/admin/ is outside location /onvif/events/log"
// on nginx -t.
// ---------------------------------------------------------------
static void test_nginx_inject_not_nested() {
  static const char kBaseConf[] =
    "server {\n"
    "    listen 443 ssl http2 default_server;\n"
    "    server_name _;\n"
    "    include /usr/share/unifi-core/http/shared-post-setup-server.conf;\n"
    "}\n";
  static const char kLogBegin[] = "    # --- log begin ---\n";
  static const char kLogEnd[]   = "    # --- log end ---\n";
  static const char kAdminBegin[] = "    # --- admin begin ---\n";
  static const char kAdminEnd[]   = "    # --- admin end ---\n";

  const std::string log_block =
    std::string(kLogBegin) +
    "    location /onvif/events/log {\n"
    "        proxy_pass http://127.0.0.1:7890/;\n"
    "    }\n" +
    kLogEnd;
  const std::string admin_block =
    std::string(kAdminBegin) +
    "    location /onvif/admin/ {\n"
    "        proxy_pass http://127.0.0.1:7891/;\n"
    "    }\n" +
    kAdminEnd;

  // 1. Inject log block into a clean config.
  auto c1 = protect_ui::inject_nginx_block_into(kBaseConf, log_block,
                                                kLogBegin, kLogEnd);
  check(c1.ok(), "1st inject ok");
  check(c1->find("/onvif/events/log") != std::string::npos,
        "1st inject placed log location");

  // 2. Inject admin block into the already-patched config.  Previously,
  //    find('}', s443) found the log location's '}' first, nesting admin
  //    inside log.  The fixed finder should locate the server's '}'.
  auto c2 = protect_ui::inject_nginx_block_into(*c1, admin_block,
                                                kAdminBegin, kAdminEnd);
  check(c2.ok(), "2nd inject ok");

  const std::string& out = *c2;
  size_t log_loc   = out.find("location /onvif/events/log {");
  size_t log_close = out.find("    }\n", log_loc);
  size_t admin_loc = out.find("location /onvif/admin/ {");
  check(log_loc   != std::string::npos, "log location present");
  check(log_close != std::string::npos, "log location closed");
  check(admin_loc != std::string::npos, "admin location present");
  check(admin_loc > log_close,
        "admin location must be OUTSIDE (after) the log location's close");

  // Overall brace balance must be consistent with the pre-inject
  // brace balance (kBaseConf has depth 0 at the end).
  int depth = 0;
  for (char c : out) {
    if (c == '{') ++depth;
    else if (c == '}') --depth;
  }
  check(depth == 0, "config brace-balanced after inject");
}

// ---------------------------------------------------------------
// Test: injection into a config without `listen 443` returns an error.
// ---------------------------------------------------------------
static void test_nginx_inject_no_server() {
  static const char kNoServer[] = "# no server here\n";
  auto r = protect_ui::inject_nginx_block_into(kNoServer, "block",
                                               "# begin\n", "# end\n");
  check(!r.ok(), "no listen 443 => error");
}

int main() {
  test_patch_lengths();
  test_apply_patches();
  test_already_patched();
  test_missing_file();
  test_backend_patch();
  test_ambiguous_anchor_skipped();
  test_version_gate();
  test_rebuild_from_pristine_bak();
  test_dpkg_backup_logic();
  test_partial_patch();
  test_revert_byte_for_byte();
  test_nginx_inject_not_nested();
  test_nginx_inject_no_server();

  std::cout << "test_protect_ui_patch: "
            << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
