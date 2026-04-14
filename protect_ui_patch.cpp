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

#include <cstring>
#include <fstream>
#include <string>
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

// 1. Camera picker filter: always pass third-party cameras. (43 bytes)
static const Patch kUiPatch1 = {
"!e.isThirdPartyCamera||e.isPairedWithAiPort",
"!0/*sThirdPartyCamera||isPairedWithAiPort*/", 43};

// 2. Automation camera list negated filter. (43 bytes)
static const Patch kUiPatch2 = {
"e.isThirdPartyCamera&&!e.isPairedWithAiPort",
"!1/*ThirdPartyCamera&&!isPairedWithAiPort*/", 43};

// 3. hasFullFeatureSet getter: always true. (55 bytes)
static const Patch kUiPatch3 = {
"return!this.isThirdPartyCamera||this.isPairedWithAiPort",
"return!0/*isThirdPartyCamera||this.isPairedWithAiPort*/", 55};

static const Patch kUiPatches[] = {kUiPatch1, kUiPatch2, kUiPatch3};
static constexpr size_t kUiPatchCount = 3;

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
// ---------------------------------------------------------------
static int apply_patches(const std::string& path,
                         const Patch* patches, size_t count) {
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

  // Back up before patching.  Always overwrite .bak so it tracks firmware.
  std::string bak_path = path + ".bak";
  if (!write_file(bak_path, content)) {
    LOG(ERROR) << "[ui_patch] failed to write backup: " << bak_path;
    return 0;
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
  int total = 0;
  int files_found = 0;

  // Patch all swai*.js and vantage*.js variants (versioned and unversioned).
  for (const char* prefix : {"swai", "vantage"}) {
    for (const auto& path : find_ui_files(kUiDir, prefix)) {
      int n = apply_patches(path, kUiPatches, kUiPatchCount);
      if (n >= 0) {
        ++files_found;
        total += n;
      }
    }
  }

  // Patch service.js (backend scope filter).
  {
    int n = apply_patches(kServicePath, kBackendPatches, kBackendPatchCount);
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
  int restored = 0;

  // Revert all swai*.js.bak and vantage*.js.bak in the UI dist directory.
  for (const char* prefix : {"swai", "vantage"}) {
    for (const auto& path : find_ui_files(kUiDir, prefix)) {
      std::string bak = path + ".bak";
      std::string content = read_file(bak);
      if (content.empty()) continue;
      if (!write_file(path, content)) {
        LOG(ERROR) << "[ui_patch] failed to restore " << path;
        continue;
      }
      LOG(INFO) << "[ui_patch] restored " << path << " from backup";
      ++restored;
    }
  }

  // Revert service.js.
  {
    std::string bak = std::string(kServicePath) + ".bak";
    std::string content = read_file(bak);
    if (!content.empty()) {
      if (write_file(kServicePath, content)) {
        LOG(INFO) << "[ui_patch] restored " << kServicePath << " from backup";
        ++restored;
      } else {
        LOG(ERROR) << "[ui_patch] failed to restore " << kServicePath;
      }
    }
  }

  if (restored > 0) {
    LOG(INFO) << "[ui_patch] reverted " << restored << " file(s)";
  } else {
    LOG(INFO) << "[ui_patch] no backup files found -- nothing to revert";
  }

  return absl::OkStatus();
}

}  // namespace protect_ui
