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

#include "protect_ui_patch.hpp"

#include <dirent.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"

namespace protect_ui {

// ---------------------------------------------------------------
// Patch table.  Each entry is {original, replacement} where both
// strings MUST be the same byte length so file offsets are preserved.
// ---------------------------------------------------------------
struct Patch {
  const char* original;
  const char* replacement;
  size_t len;
};

// --- Frontend patches (swai*.js, vantage*.js) ---

// 1a. Camera picker filter (pre-7.0.57): always pass third-party cameras.
static const Patch kUiPatch1a = {
"!e.isThirdPartyCamera||e.isPairedWithAiPort",
"!0/*sThirdPartyCamera||isPairedWithAiPort*/", 43};

// 1b. Camera picker filter (7.0.57+): new exclusion filter. (40 bytes)
static const Patch kUiPatch1b = {
"!e.isThirdPartyCamera&&!v.includes(e.id)",
"!0/*hirdPartyCamera&&!v.includes(e.id)*/", 40};

// 2. Automation camera list negated filter (pre-7.0.57). (43 bytes)
static const Patch kUiPatch2 = {
"e.isThirdPartyCamera&&!e.isPairedWithAiPort",
"!1/*ThirdPartyCamera&&!isPairedWithAiPort*/", 43};

// 3. hasFullFeatureSet getter: always true. (55 bytes)
static const Patch kUiPatch3 = {
"return!this.isThirdPartyCamera||this.isPairedWithAiPort",
"return!0/*isThirdPartyCamera||this.isPairedWithAiPort*/", 55};

static const Patch kUiPatches[] = {
    kUiPatch1a, kUiPatch1b, kUiPatch2, kUiPatch3};
static constexpr size_t kUiPatchCount = 4;

// --- Backend patches (service.js) ---

// 4. scope_all_ui_cameras: remove third-party exclusion. (23 bytes)
static const Patch kBackendPatch1 = {
"&&!e.isThirdPartyCamera",
"/*isThirdPartyCamera */", 23};

static const Patch kBackendPatches[] = {kBackendPatch1};
static constexpr size_t kBackendPatchCount = 1;

// ---------------------------------------------------------------
// Paths
// ---------------------------------------------------------------
// NOLINTNEXTLINE
static const char kUiDir[] = "/usr/share/unifi-protect/app/node_modules/@ubnt/unifi-protect-ui-internal/dist";
static const char kServicePath[] = "/usr/share/unifi-protect/app/service.js";

// ---------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------
static std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return {std::istreambuf_iterator<char>(f),
          std::istreambuf_iterator<char>()};
}

static bool write_file(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  return f.good();
}

// ---------------------------------------------------------------
// dpkg md5sum verification
// ---------------------------------------------------------------
// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kDpkgMd5sums[] = "/var/lib/dpkg/info/unifi-protect.md5sums";

// Compute the MD5 hex digest of a file on disk via md5sum(1).
// Returns empty string on failure.
static std::string md5_of_file(const std::string& path) {
  std::string cmd = "md5sum " + path + " 2>/dev/null";
  std::FILE* p = popen(cmd.c_str(), "r");  // NOLINT(runtime/int)
  if (!p) return {};
  char buf[128] = {};
  char* ok = std::fgets(buf, sizeof(buf), p);
  pclose(p);
  if (!ok || std::strlen(buf) < 32) return {};
  return std::string(buf, 32);
}

// Load the dpkg md5sums file into a map: relative_path -> md5hex.
static std::unordered_map<std::string, std::string> load_dpkg_md5sums() {
  std::unordered_map<std::string, std::string> result;
  std::ifstream f(kDpkgMd5sums);
  if (!f) return result;
  std::string line;
  while (std::getline(f, line)) {
    // Format: "<md5hex>  <relative_path>"
    if (line.size() < 35) continue;
    std::string md5 = line.substr(0, 32);
    std::string path = line.substr(34);  // skip "  "
    result[path] = md5;
  }
  return result;
}

// Look up the expected dpkg md5 for an absolute path.
// Returns empty string if the file is not in the md5sums database.
static std::string dpkg_expected_md5(
    const std::string& abs_path,
    const std::unordered_map<std::string, std::string>& md5sums) {
  // dpkg md5sums uses paths relative to /, without leading slash.
  std::string rel = abs_path;
  if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
  auto it = md5sums.find(rel);
  if (it == md5sums.end()) return {};
  return it->second;
}

// Check if a file on disk matches its dpkg md5sum.
static bool dpkg_md5_matches(
    const std::string& abs_path,
    const std::unordered_map<std::string, std::string>& md5sums) {
  std::string expected = dpkg_expected_md5(abs_path, md5sums);
  if (expected.empty()) return false;
  return md5_of_file(abs_path) == expected;
}

// ---------------------------------------------------------------
// Scan the UI dist directory for files matching a prefix
// (e.g. "swai" matches swai.js, swai-7.0.57.js).
// ---------------------------------------------------------------
static std::vector<std::string> find_ui_files(const char* dir,
                                              const char* prefix) {
  std::vector<std::string> result;
  size_t prefix_len = std::strlen(prefix);
  DIR* d = opendir(dir);
  if (!d) return result;
  while (struct dirent* entry = readdir(d)) {
    const char* name = entry->d_name;
    if (std::strncmp(name, prefix, prefix_len) != 0) continue;
    // Must be .js (not .js.bak, .js.map, etc.)
    const char* ext = std::strrchr(name, '.');
    if (!ext || std::strcmp(ext, ".js") != 0) continue;
    result.push_back(std::string(dir) + "/" + name);
  }
  closedir(d);
  return result;
}

// ---------------------------------------------------------------
// Apply patches to a single file.
// Returns number of patches applied, or -1 if file not readable.
//
// Backup strategy uses dpkg md5sums to validate file integrity:
//   - If the live file matches its dpkg md5 -> always overwrite .bak
//     (the live file is a known-good original; any existing .bak may
//     be stale from a prior firmware version).
//   - If the live file does NOT match -> do not overwrite .bak
//     (we would be backing up a modified file).
//   - If no dpkg md5sums exist (dev machine) -> always back up.
// ---------------------------------------------------------------
static int apply_patches(
    const std::string& path, const Patch* patches, size_t count,
    const std::unordered_map<std::string, std::string>& md5sums) {
  std::string content = read_file(path);
  if (content.empty()) return -1;

  std::vector<std::pair<size_t, const Patch*>> todo;
  for (size_t i = 0; i < count; ++i) {
    const Patch& p = patches[i];
    if (content.find(p.replacement) != std::string::npos) continue;
    size_t pos = content.find(p.original);
    if (pos == std::string::npos) continue;
    todo.emplace_back(pos, &p);
  }

  if (todo.empty()) {
    LOG(INFO) << "[ui_patch] " << path << " already patched";
    return 0;
  }

  // Back up the file if it is an unmodified package original.
  std::string bak_path = path + ".bak";
  if (md5sums.empty() || dpkg_md5_matches(path, md5sums)) {
    if (!write_file(bak_path, content)) {
      LOG(ERROR) << "[ui_patch] failed to write backup: " << bak_path;
      return 0;
    }
  } else {
    LOG(INFO) << "[ui_patch] " << path
              << " already modified -- keeping existing .bak";
  }

  for (auto& [pos, p] : todo) {
    content.replace(pos, p->len, p->replacement);
  }

  if (!write_file(path, content)) {
    LOG(ERROR) << "[ui_patch] failed to write: " << path;
    return 0;
  }

  LOG(INFO) << "[ui_patch] patched " << path
            << " (" << todo.size() << " replacement(s))";
  return static_cast<int>(todo.size());
}

// ---------------------------------------------------------------
// Public API
// ---------------------------------------------------------------
absl::Status patch_alarm_picker() {
  auto md5sums = load_dpkg_md5sums();
  int total = 0;
  int files_found = 0;

  // Patch all swai*.js and vantage*.js variants (versioned and unversioned).
  for (const char* prefix : {"swai", "vantage"}) {
    for (const auto& path : find_ui_files(kUiDir, prefix)) {
      int n = apply_patches(path, kUiPatches, kUiPatchCount, md5sums);
      if (n >= 0) {
        ++files_found;
        total += n;
      }
    }
  }

  // Patch service.js (backend scope filter).
  {
    int n = apply_patches(kServicePath, kBackendPatches, kBackendPatchCount,
                          md5sums);
    if (n >= 0) {
      ++files_found;
      total += n;
    }
  }

  if (files_found == 0) {
    return absl::NotFoundError(
        "Protect UI files not found -- not running on a Dream Router/NVR?");
  }

  if (total > 0) {
    LOG(INFO) << "[ui_patch] applied " << total
              << " patch(es) across " << files_found << " file(s)";
  } else {
    LOG(INFO) << "[ui_patch] all files already patched";
  }

  return absl::OkStatus();
}

absl::Status revert_alarm_picker() {
  auto md5sums = load_dpkg_md5sums();
  int restored = 0;

  auto try_restore = [&](const std::string& path) {
    std::string bak = path + ".bak";
    std::string content = read_file(bak);
    if (content.empty()) return;

    if (!md5sums.empty()) {
      bool bak_matches = dpkg_md5_matches(bak, md5sums);
      bool live_matches = dpkg_md5_matches(path, md5sums);

      if (live_matches) {
        // Live file is already the correct original -- nothing to do.
        LOG(INFO) << "[ui_patch] " << path << " already matches dpkg md5";
        return;
      }

      if (!bak_matches) {
        // Neither file matches -- .bak is stale but still better than
        // a patched file.  Warn but proceed with restore.
        LOG(WARNING) << "[ui_patch] " << bak << " does not match dpkg md5 "
                     << "-- may be from a prior firmware version";
      }
    }

    if (!write_file(path, content)) {
      LOG(ERROR) << "[ui_patch] failed to restore " << path;
      return;
    }
    LOG(INFO) << "[ui_patch] restored " << path << " from backup";
    ++restored;
  };

  // Revert all swai*.js.bak and vantage*.js.bak in the UI dist directory.
  for (const char* prefix : {"swai", "vantage"}) {
    for (const auto& path : find_ui_files(kUiDir, prefix))
      try_restore(path);
  }

  // Revert service.js.
  try_restore(kServicePath);

  if (restored > 0) {
    LOG(INFO) << "[ui_patch] reverted " << restored << " file(s)";
  } else {
    LOG(INFO) << "[ui_patch] no backup files found -- nothing to revert";
  }

  return absl::OkStatus();
}

// ---------------------------------------------------------------
// Nginx log proxy patch
// ---------------------------------------------------------------
// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kNginxConf[] =
    "/data/unifi-core/config/http/site-local-ip.conf";  // NOLINT

// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kMarkerBegin[] =
    "    # --- onvif-recorder log viewer "  // NOLINT
    "(managed by onvif_recorder) ---\n";  // NOLINT
// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kMarkerEnd[] =
    "    # --- end onvif-recorder log viewer ---\n";  // NOLINT

// Build the nginx location block with the given port.
static std::string build_nginx_block(uint16_t port) {
  std::string block;
  block += kMarkerBegin;
  block += "    location /onvif/events/log {\n";
  block += "        include "
           "/usr/share/unifi-core/http/auth.conf;\n";
  block += "        include "
           "/usr/share/unifi-core/http/proxy.conf;\n";
  block += "        proxy_pass http://127.0.0.1:";
  block += std::to_string(port);
  block += "/;\n";
  block += "    }\n";
  block += kMarkerEnd;
  return block;
}

// Remove any existing marker block from the content string.
static bool strip_marker_block(std::string* content) {
  size_t begin = content->find(kMarkerBegin);
  if (begin == std::string::npos) return false;
  size_t end = content->find(kMarkerEnd, begin);
  if (end == std::string::npos) return false;
  content->erase(begin, end + std::strlen(kMarkerEnd) - begin);
  return true;
}

absl::Status patch_nginx_log_proxy(uint16_t port) {
  std::string content = read_file(kNginxConf);
  if (content.empty()) {
    return absl::NotFoundError(
        "nginx config not found "
        "-- not running on a Dream Router/NVR?");
  }

  // Always strip any existing block first (may be in the wrong
  // server block from a prior version), then re-insert.
  strip_marker_block(&content);

  // Insert into the first server block (port 443, HTTPS).
  // The file has multiple server{} blocks; we need the one
  // with "listen 443".  Find it, then find its closing '}'.
  size_t s443 = content.find("listen 443");
  if (s443 == std::string::npos) {
    return absl::InternalError(
        "cannot find 'listen 443' in nginx config");
  }
  // Find the closing '}' for this server block.  Since the
  // 443 block uses include directives (no nested braces in
  // the config file itself), the first '}' after "listen 443"
  // closes it.
  size_t brace = content.find('}', s443);
  if (brace == std::string::npos) {
    return absl::InternalError(
        "cannot find closing brace for 443 server");
  }

  std::string block = build_nginx_block(port);
  content.insert(brace, block);

  // If the file already has exactly this content, skip the write.
  {
    std::string on_disk = read_file(kNginxConf);
    if (on_disk == content) {
      LOG(INFO) << "[nginx] log proxy block already present";
      return absl::OkStatus();
    }
  }

  if (!write_file(kNginxConf, content)) {
    return absl::InternalError("failed to write nginx config");
  }

  LOG(INFO) << "[nginx] injected log proxy block (port "
            << port << ")";

  // Reload nginx to pick up the change.
  int rc = std::system("nginx -s reload 2>/dev/null");  // NOLINT
  if (rc != 0)
    LOG(WARNING) << "[nginx] reload returned " << rc;

  return absl::OkStatus();
}

absl::Status revert_nginx_log_proxy() {
  std::string content = read_file(kNginxConf);
  if (content.empty()) {
    LOG(INFO) << "[nginx] config not found -- nothing to revert";
    return absl::OkStatus();
  }

  if (!strip_marker_block(&content)) {
    LOG(INFO) << "[nginx] no log proxy block found "
              << "-- nothing to revert";
    return absl::OkStatus();
  }

  if (!write_file(kNginxConf, content)) {
    return absl::InternalError("failed to write nginx config");
  }

  LOG(INFO) << "[nginx] removed log proxy block";

  int rc = std::system("nginx -s reload 2>/dev/null");  // NOLINT
  if (rc != 0)
    LOG(WARNING) << "[nginx] reload returned " << rc;

  return absl::OkStatus();
}

}  // namespace protect_ui
