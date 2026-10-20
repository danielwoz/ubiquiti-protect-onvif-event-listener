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

#include <fcntl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

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
// Test 1: Patch strings are consistent
// ---------------------------------------------------------------
static void test_patch_lengths() {
  for (size_t i = 0; i < kUiPatchCount; ++i) {
    const Patch& p = kUiPatches[i];
    check(std::strlen(p.original) == std::strlen(p.replacement),
          "UI patch same byte length");
  }
  for (size_t i = 0; i < kBackendPatchCount; ++i) {
    const Patch& p = kBackendPatches[i];
    check(std::strlen(p.original) == std::strlen(p.replacement),
          "Backend patch same byte length");
  }
  // A replacement that contains its original would be applied again on
  // every pass.
  std::vector<Patch> all(kUiPatches, kUiPatches + kUiPatchCount);
  all.insert(all.end(), kBackendPatches, kBackendPatches + kBackendPatchCount);
  all.push_back(kBackendPatchWsa);
  for (const Patch& p : all) {
    check(std::strstr(p.replacement, p.original) == nullptr,
          "replacement does not contain its original");
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

// Patch 2 targets the automation camera-list filter.  Protect 7.2+ reuses
// the bare expression for the microphone capability check, which must be
// left alone; the filter itself is patched wherever it still exists.
static void test_patch2_targets_automation_filter() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_patch2.js";
  std::remove(path.c_str());

  const std::string mic =
      "oxe=e=>e.isThirdPartyCamera&&!e.isPairedWithAiPort?"
      "!!e.thirdPartyCameraInfo?.enableRtspAudio:e.featureFlags.hasMic,";
  const std::string filter =
      "LH=(e,t,n)=>!(t&&e.nvrMac!==n||e.isThirdPartyCamera&&"
      "!e.isPairedWithAiPort),";
  write_test_file(path, mic + filter);

  std::unordered_map<std::string, std::string> empty_md5;
  int n = apply_patches(path, kUiPatches, kUiPatchCount, empty_md5);
  check(n == 1, "patch2: one replacement");
  const std::string out = read_test_file(path);
  check(out.find(mic) == 0, "patch2: microphone check untouched");
  check(out.find("e.nvrMac!==n||!1/*ThirdPartyCamera&&!isPairedWithAiPort*/)")
            != std::string::npos,
        "patch2: automation filter patched");

  // On a bundle with only the microphone check (Protect 7.3), nothing
  // changes.
  write_test_file(path, mic);
  check(apply_patches(path, kUiPatches, kUiPatchCount, empty_md5) == 0,
        "patch2: mic-only bundle left alone");
  check(read_test_file(path) == mic, "patch2: mic-only file unchanged");

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
                     std::strlen(kBackendPatch1.original),
                     kBackendPatch1.replacement);
  write_test_file(bak, original);
  write_test_file(path, mispatched);

  std::unordered_map<std::string, std::string> md5sums;
  md5sums[path.substr(1)] = md5_of_file(bak);

  int n = apply_patches(path, kBackendPatches, kBackendPatchCount, md5sums);
  check(n == 1, "rebuild: one patch applied to the pristine source");

  std::string expected = original;
  size_t at = expected.find("e.isAdopted&&!e.isThirdPartyCamera");
  expected.replace(at + std::strlen("e.isAdopted"),
                   std::strlen(kBackendPatch1.original),
                   kBackendPatch1.replacement);
  check(read_test_file(path) == expected,
        "rebuild: validator restored and scope predicate patched");
  check(read_test_file(bak) == original, "rebuild: pristine .bak kept");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// Protect 7.1 - 7.3's ONVIF client definition with its neighbours.
static const char kSoapClient[] =
    "return{body:M,rawText:C.text}};"
    "t.connSoapRequest=(e,r,o,i,a,n)=>(0,t.soapRequest)({hostname:e.hostname,"
    "port:e.port,path:r,body:{[o]:{\"@_xmlns\":i,...a}},username:e.username,"
    "password:e.password,timeShift:e.timeShift,extraHeaders:n}),"
    "t.soapRequest=async e=>(await(0,t.soapRequestRaw)(e)).body,";

// The WS-Addressing patch rewrites only the ONVIF client definition, is a
// no-op on the next pass, and is removed again when left out of the set.
static void test_wsa_patch() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_wsa_service.js";
  std::string bak = path + ".bak";
  std::remove(path.c_str());
  std::remove(bak.c_str());

  const std::string original = kSoapClient;
  write_test_file(path, original);

  const Patch with_wsa[] = {kBackendPatch1, kBackendPatchWsa};
  std::unordered_map<std::string, std::string> empty_md5;
  check(apply_patches(path, with_wsa, 2, empty_md5) == 1,
        "wsa: one replacement");
  const std::string patched = read_test_file(path);
  check(patched.find("extraHeaders:n})") == std::string::npos,
        "wsa: original definition replaced");
  check(patched.find("extraHeaders:/*onvif-recorder:wsa*/") !=
            std::string::npos,
        "wsa: headers added to extraHeaders");
  check(patched.rfind("return{body:M,rawText:C.text}};"
                      "t.connSoapRequest=(e,r,o,i,a,n)=>", 0) == 0,
        "wsa: preceding code untouched");
  const std::string tail =
      "(n,`http://${e.hostname}${e.port==80?\"\":\":\"+e.port}${r}`,o)}),"
      "t.soapRequest=async e=>(await(0,t.soapRequestRaw)(e)).body,";
  check(patched.size() > tail.size() &&
            patched.compare(patched.size() - tail.size(), tail.size(),
                            tail) == 0,
        "wsa: following code untouched");
  check(read_test_file(bak) == original, "wsa: original backed up");
  check(apply_patches(path, with_wsa, 2, empty_md5) == 0,
        "wsa: second run is a no-op");
  check(read_test_file(path) == patched, "wsa: second run leaves file");

  // Leaving the patch out rebuilds the file from the package original.
  std::unordered_map<std::string, std::string> md5sums;
  md5sums[path.substr(1)] = md5_of_file(bak);
  apply_patches(path, kBackendPatches, kBackendPatchCount, md5sums,
                &kBackendPatchWsa, 1);
  check(read_test_file(path) == original, "wsa: disabling restores original");

  // Without a backup dpkg can vouch for, a disabled patch is taken out of
  // the live file and the existing backup is kept.
  write_test_file(path, patched);
  write_test_file(bak, "older backup");
  apply_patches(path, kBackendPatches, kBackendPatchCount, empty_md5,
                &kBackendPatchWsa, 1);
  check(read_test_file(path) == original,
        "wsa: disabling without a verified backup removes it");
  check(read_test_file(bak) == "older backup",
        "wsa: disabling keeps the existing backup");

  // Two definitions no longer identify one client: leave the file alone.
  const std::string twice = original + original;
  write_test_file(path, twice);
  std::remove(bak.c_str());
  check(apply_patches(path, with_wsa, 2, empty_md5) == 0,
        "wsa: ambiguous anchor skipped");
  check(read_test_file(path) == twice, "wsa: ambiguous file unchanged");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// Protect needs a restart only when service.js was written after the
// running process started.
static void test_file_newer_than_process() {
  const std::string path = temp_dir() + "/test_newer_than_process.js";
  // Let this process age past the clock-tick resolution of its start time.
  usleep(100 * 1000);
  write_test_file(path, "x");
  check(file_newer_than_process(path, getpid()),
        "newer: file written after the process started");

  const struct timespec y2k[2] = {{946684800, 0}, {946684800, 0}};
  check(utimensat(AT_FDCWD, path.c_str(), y2k, 0) == 0, "newer: set mtime");
  check(!file_newer_than_process(path, getpid()),
        "newer: file older than the process");
  check(!file_newer_than_process(path + ".missing", getpid()),
        "newer: missing file");
  check(!file_newer_than_process(path, 0), "newer: no such process");
  std::remove(path.c_str());
}

// ---------------------------------------------------------------
// Test 6: backup logic
//
// An unpatched live file is the package's file and becomes the backup,
// whatever dpkg's md5sums say (they lag behind during an upgrade).  A
// patched live file never replaces the backup.
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
  write_test_file(bak, "old backup content");

  std::unordered_map<std::string, std::string> md5sums;
  md5sums[path.substr(1)] = "0000000000000000aaaaaaaaaaaaaaaa";  // stale
  int n = apply_patches(path, kUiPatches, kUiPatchCount, md5sums);
  check(n == 1, "dpkg: patch applied");
  check(read_test_file(bak) == content, "dpkg: unpatched file backed up");

  // A later pass over the patched file (no md5sums, as on a dev machine)
  // with an extra patch keeps the backup of the unpatched file.
  const std::string patched = read_test_file(path);
  write_test_file(path, patched + ";" + kUiPatch3.original);
  std::unordered_map<std::string, std::string> empty_md5;
  check(apply_patches(path, kUiPatches, kUiPatchCount, empty_md5) == 1,
        "dpkg: extra patch applied");
  check(read_test_file(bak) == content, "dpkg: patched file not backed up");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// A live file that differs from the package original without carrying our
// patches (damaged, or edited by hand) is rebuilt from the verified .bak,
// and that .bak is never replaced by it.
static void test_unpatched_damage_rebuilt_from_bak() {
  std::string dir = temp_dir();
  std::string path = dir + "/test_damaged_service.js";
  std::string bak = path + ".bak";
  const std::string pristine =
      "v1;scope_all_ui_cameras:e=>e.isAdopted&&!e.isThirdPartyCamera,scope_x";
  const std::string damaged = pristine.substr(0, 40);
  write_test_file(bak, pristine);
  write_test_file(path, damaged);
  std::unordered_map<std::string, std::string> md5sums;
  md5sums[path.substr(1)] = md5_of_file(bak);

  check(apply_patches(path, kBackendPatches, kBackendPatchCount, md5sums) ==
            1,
        "damaged: rebuilt and patched");
  const std::string out = read_test_file(path);
  check(out.rfind("v1;", 0) == 0 && out.size() == pristine.size() &&
            out.find(kBackendPatch1.replacement) != std::string::npos,
        "damaged: original restored and patched");
  check(read_test_file(bak) == pristine, "damaged: verified .bak kept");

  std::remove(path.c_str());
  std::remove(bak.c_str());
}

// dpkg marks a package half-installed while it unpacks it.
static void test_half_installed_status() {
  check(status_is_half_installed("install ok half-installed"),
        "status: unpacking");
  check(status_is_half_installed("install reinstreq half-installed"),
        "status: failed unpack");
  check(!status_is_half_installed("install ok installed"), "status: done");
  check(!status_is_half_installed("install ok unpacked"), "status: unpacked");
  check(!status_is_half_installed(""), "status: unknown package");
}

// Files are replaced whole, keeping their mode.
static void test_replace_file() {
  const std::string path = temp_dir() + "/test_replace.js";
  write_test_file(path, "old");
  chmod(path.c_str(), 0755);
  check(replace_file(path, "new content"), "replace: succeeds");
  check(read_test_file(path) == "new content", "replace: content written");
  struct stat st;
  check(stat(path.c_str(), &st) == 0 && (st.st_mode & 07777) == 0755,
        "replace: mode kept");
  check(read_test_file(path + ".onvif-recorder.tmp").empty(),
        "replace: no temporary file left");
  std::remove(path.c_str());

  check(replace_file(path, "fresh"), "replace: creates a missing file");
  check(read_test_file(path) == "fresh", "replace: new file written");
  std::remove(path.c_str());
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
  test_patch2_targets_automation_filter();
  test_rebuild_from_pristine_bak();
  test_wsa_patch();
  test_file_newer_than_process();
  test_dpkg_backup_logic();
  test_unpatched_damage_rebuilt_from_bak();
  test_half_installed_status();
  test_replace_file();
  test_partial_patch();
  test_revert_byte_for_byte();
  test_nginx_inject_not_nested();
  test_nginx_inject_no_server();

  std::cout << "test_protect_ui_patch: "
            << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
