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
#include "absl/status/statusor.h"
#include "protect_version.hpp"

namespace protect_ui {

// ---------------------------------------------------------------
// Patch table.  Each entry is {original, replacement} where both
// strings MUST be the same byte length so file offsets are preserved.
//
// A patch is applied only when `original` occurs exactly once in its
// search range.  Minified bundles reuse short expressions in unrelated
// places, so a second occurrence means the anchor no longer identifies
// the intended code and the patch is skipped rather than guessed.
//
// `region_begin` / `region_end` optionally narrow the search range to the
// text between the single occurrence of `region_begin` and the next
// `region_end` after it (or end of file).
//
// Patches are selected by matching code shape, not by Protect version:
// the UI bundle version (swai-7.0.57.js) differs from the package version
// (7.0.107), and a pattern can survive across many releases.  Checked
// against Protect 6.2.88, 7.0.88, 7.0.107, 7.1.87, 7.2.105 and 7.3.70.
// ---------------------------------------------------------------
struct Patch {
  const char* original;
  const char* replacement;
  size_t len;
  const char* region_begin = nullptr;
  const char* region_end = nullptr;
};

// --- Frontend patches (swai*.js, vantage*.js) ---

// 1a. Camera picker filter (Protect 6.x - 7.0.x): always pass third-party
// cameras.
static const Patch kUiPatch1a = {
"!e.isThirdPartyCamera||e.isPairedWithAiPort",
"!0/*sThirdPartyCamera||isPairedWithAiPort*/", 43};

// 1b. Camera picker filter (UI bundle 7.0.57, shipped in Protect 7.0.107):
// new exclusion filter. (40 bytes)
static const Patch kUiPatch1b = {
"!e.isThirdPartyCamera&&!v.includes(e.id)",
"!0/*hirdPartyCamera&&!v.includes(e.id)*/", 40};

// 2. Automation camera list filter
//    !(… e.nvrMac!==… || e.isThirdPartyCamera&&!e.isPairedWithAiPort)
// (Protect 6.x - 7.2.x; gone in 7.3).  Anchored on the surrounding "||" and
// ")" because later bundles use the bare expression for the microphone
// capability check and the ONVIF profile settings form. (46 bytes)
static const Patch kUiPatch2 = {
"||e.isThirdPartyCamera&&!e.isPairedWithAiPort)",
"||!1/*ThirdPartyCamera&&!isPairedWithAiPort*/)", 46};

// 3. hasFullFeatureSet getter: always true. (55 bytes)
static const Patch kUiPatch3 = {
"return!this.isThirdPartyCamera||this.isPairedWithAiPort",
"return!0/*isThirdPartyCamera||this.isPairedWithAiPort*/", 55};

static const Patch kUiPatches[] = {
kUiPatch1a, kUiPatch1b, kUiPatch2, kUiPatch3};
static constexpr size_t kUiPatchCount = 4;

// --- Backend patches (service.js) ---

// 4. scope_all_ui_cameras: remove third-party exclusion. (23 bytes)
// The same fragment also guards the ONVIF stream-settings validator, so
// the search is confined to the scope predicate's own definition.
static const Patch kBackendPatch1 = {
"&&!e.isThirdPartyCamera",
"/*isThirdPartyCamera */", 23,
"scope_all_ui_cameras:e=>", ",scope_"};

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
// Matching helpers.
// ---------------------------------------------------------------
static size_t count_in(const std::string& s, const char* needle,
                       size_t from, size_t to) {
  const size_t n = std::strlen(needle);
  size_t count = 0;
  for (size_t pos = s.find(needle, from);
       pos != std::string::npos && pos + n <= to;
       pos = s.find(needle, pos + n)) {
    ++count;
  }
  return count;
}

// Resolve the [begin, end) search range for a patch.  Returns false if the
// patch defines a region whose start marker is absent or not unique.
static bool resolve_region(const std::string& content, const Patch& p,
                           size_t* begin, size_t* end) {
  *begin = 0;
  *end = content.size();
  if (p.region_begin == nullptr) return true;
  if (count_in(content, p.region_begin, 0, content.size()) != 1) return false;
  *begin = content.find(p.region_begin);
  if (p.region_end != nullptr) {
    size_t stop = content.find(p.region_end,
                               *begin + std::strlen(p.region_begin));
    if (stop != std::string::npos) *end = stop;
  }
  return true;
}

enum class PatchState { kApply, kAlreadyApplied, kNotApplicable, kAmbiguous };

static PatchState classify_patch(const std::string& content, const Patch& p,
                                 size_t* pos) {
  size_t begin = 0;
  size_t end = 0;
  if (!resolve_region(content, p, &begin, &end)) {
    return PatchState::kNotApplicable;
  }
  const size_t originals = count_in(content, p.original, begin, end);
  if (originals == 1) {
    *pos = content.find(p.original, begin);
    return PatchState::kApply;
  }
  if (originals > 1) return PatchState::kAmbiguous;
  return count_in(content, p.replacement, begin, end) == 1
             ? PatchState::kAlreadyApplied
             : PatchState::kNotApplicable;
}

static bool file_md5_is(const std::string& path, const std::string& md5) {
  return !md5.empty() && md5_of_file(path) == md5;
}

// ---------------------------------------------------------------
// Apply patches to a single file.
// Returns number of patches applied, or -1 if file not readable.
//
// The patched file is always derived from the unmodified package original
// when one is available, so a file patched by an older, less precise rule
// set is rebuilt correctly rather than patched on top of:
//   - live file matches its dpkg md5 -> it is the original; refresh .bak.
//   - live file differs and .bak matches dpkg md5 -> start from .bak.
//   - neither matches -> start from the live file and keep the existing
//     .bak (we would otherwise back up a modified file).
//   - no dpkg md5sums (dev machine) -> start from the live file and back
//     it up before the first modification.
// ---------------------------------------------------------------
static int apply_patches(
    const std::string& path, const Patch* patches, size_t count,
    const std::unordered_map<std::string, std::string>& md5sums) {
  const std::string live = read_file(path);
  if (live.empty()) return -1;

  const std::string bak_path = path + ".bak";
  const std::string expected = dpkg_expected_md5(path, md5sums);
  const bool live_is_original = file_md5_is(path, expected);

  std::string source = live;
  bool source_is_bak = false;
  if (!expected.empty() && !live_is_original &&
      file_md5_is(bak_path, expected)) {
    source = read_file(bak_path);
    source_is_bak = !source.empty();
    if (!source_is_bak) source = live;
  }

  std::string result = source;
  int applied = 0;
  for (size_t i = 0; i < count; ++i) {
    const Patch& p = patches[i];
    size_t pos = 0;
    switch (classify_patch(result, p, &pos)) {
      case PatchState::kApply:
        result.replace(pos, p.len, p.replacement);
        ++applied;
        break;
      case PatchState::kAmbiguous:
        LOG(WARNING) << "[ui_patch] " << path << ": pattern \""
                     << p.original << "\" is not unique -- skipped";
        break;
      case PatchState::kAlreadyApplied:
      case PatchState::kNotApplicable:
        break;
    }
  }

  if (result == live) {
    LOG(INFO) << "[ui_patch] " << path << " already patched";
    return 0;
  }

  if (!source_is_bak) {
    if (md5sums.empty() || live_is_original) {
      if (!write_file(bak_path, live)) {
        LOG(ERROR) << "[ui_patch] failed to write backup: " << bak_path;
        return 0;
      }
    } else {
      LOG(INFO) << "[ui_patch] " << path
                << " already modified -- keeping existing .bak";
    }
  } else {
    LOG(WARNING) << "[ui_patch] " << path
                 << " differs from the patch set for this version -- "
                    "rebuilding from " << bak_path;
  }

  if (!write_file(path, result)) {
    LOG(ERROR) << "[ui_patch] failed to write: " << path;
    return 0;
  }

  LOG(INFO) << "[ui_patch] patched " << path
            << " (" << applied << " replacement(s))";
  return applied;
}

// ---------------------------------------------------------------
// Public API
// ---------------------------------------------------------------
absl::Status patch_alarm_picker() {
  // Log the detected Protect firmware so a journal capture from a user
  // running a brand-new firmware makes the version visible up-front.
  // Fast popen path; failure is silent (we still proceed with patching).
  {
    std::string ver;
    FILE* p = popen("dpkg-query -W -f='${Version}' "
                    "unifi-protect 2>/dev/null", "r");  // NOLINT
    if (p) {
      char buf[64] = {0};
      if (std::fgets(buf, sizeof(buf), p)) {
        ver = buf;
        while (!ver.empty() &&
               (ver.back() == '\n' || ver.back() == ' '))
          ver.pop_back();
      }
      pclose(p);  // NOLINT
    }
    if (!ver.empty()) {
      LOG(INFO) << "[ui_patch] detected unifi-protect " << ver;
      // Publish the version so live writers (detection_recorder,
      // motion_poller) can gate on it via IsAtLeast(7, 1, 0).
      if (auto v = onvif::protect_version::Parse(ver)) {
        onvif::protect_version::SetCurrent(*v);
        LOG(INFO) << "[ui_patch] protect_version published: "
                  << v->major << "." << v->minor << "." << v->patch;
      }
    } else {
      LOG(INFO) << "[ui_patch] unifi-protect version unknown";
    }
  }

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

absl::Status revert_alarm_picker_in(const std::string& ui_dir,
                                     const std::string& service_path) {
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
    for (const auto& path : find_ui_files(ui_dir.c_str(), prefix))
      try_restore(path);
  }

  // Revert service.js.
  if (!service_path.empty())
    try_restore(service_path);

  if (restored > 0) {
    LOG(INFO) << "[ui_patch] reverted " << restored << " file(s)";
  } else {
    LOG(INFO) << "[ui_patch] no backup files found -- nothing to revert";
  }

  return absl::OkStatus();
}

absl::Status revert_alarm_picker() {
  return revert_alarm_picker_in(kUiDir, kServicePath);
}

// ---------------------------------------------------------------
// Nginx proxy patches (log viewer + admin page)
// ---------------------------------------------------------------
// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kNginxConf[] =
    "/data/unifi-core/config/http/site-local-ip.conf";  // NOLINT

// Log viewer markers (pre-existing; kept verbatim for upgrade compat).
// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kLogMarkerBegin[] =
    "    # --- onvif-recorder log viewer "  // NOLINT
    "(managed by onvif_recorder) ---\n";  // NOLINT
// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kLogMarkerEnd[] =
    "    # --- end onvif-recorder log viewer ---\n";  // NOLINT

// Admin page markers.
// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kAdminMarkerBegin[] =
    "    # --- onvif-recorder admin "  // NOLINT
    "(managed by onvif_recorder) ---\n";  // NOLINT
// NOLINTNEXTLINE(whitespace/indent_namespace)
static const char kAdminMarkerEnd[] =
    "    # --- end onvif-recorder admin ---\n";  // NOLINT

// Remove any block bounded by @p begin/@p end from @p content.
static bool strip_block(std::string* content,
                        const char* begin, const char* end) {
  size_t b = content->find(begin);
  if (b == std::string::npos) return false;
  size_t e = content->find(end, b);
  if (e == std::string::npos) return false;
  content->erase(b, e + std::strlen(end) - b);
  return true;
}

// Walk forward from the position of `listen 443` counting braces to find
// the closing `}` of the enclosing `server { ... }` block.  We start at
// depth=1 because the `listen` directive is already inside the opened
// server block.  Returns std::string::npos if `listen 443` isn't in @p
// content or the block is unbalanced.
//
// Naive content.find('}', s443) lands inside nested location blocks from
// earlier injections, which breaks nginx config validity when a second
// block is inserted later.
static size_t find_server_close_brace(const std::string& content) {
  size_t s443 = content.find("listen 443");
  if (s443 == std::string::npos) return std::string::npos;
  int depth = 1;
  for (size_t i = s443; i < content.size(); ++i) {
    char c = content[i];
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (--depth == 0) return i;
    }
  }
  return std::string::npos;
}

// Returns @p content with any existing block bounded by begin/end removed,
// and @p block inserted just before the closing `}` of the `listen 443`
// server block.  Returns empty absl::StatusOr on structural failure.
static absl::StatusOr<std::string> inject_nginx_block_into(
    const std::string& content_in,
    const std::string& block,
    const char* begin,
    const char* end) {
  std::string content = content_in;
  strip_block(&content, begin, end);
  size_t brace = find_server_close_brace(content);
  if (brace == std::string::npos) {
    return absl::InternalError(
        "cannot find closing brace for 443 server");
  }
  content.insert(brace, block);
  return content;
}

// Shared: inject @p block just before the closing '}' of the `listen 443`
// server block in site-local-ip.conf.  @p begin/@p end mark a pre-existing
// copy to replace.  @p label is used for log messages.
static absl::Status inject_nginx_block(const std::string& block,
                                       const char* begin,
                                       const char* end,
                                       const char* label) {
  std::string content = read_file(kNginxConf);
  if (content.empty()) {
    return absl::NotFoundError(
        "nginx config not found "
        "-- not running on a Dream Router/NVR?");
  }

  auto new_content_or = inject_nginx_block_into(content, block, begin, end);
  if (!new_content_or.ok()) return new_content_or.status();
  const std::string& new_content = *new_content_or;

  if (new_content == content) {
    LOG(INFO) << "[nginx] " << label << " block already present";
    return absl::OkStatus();
  }

  if (!write_file(kNginxConf, new_content)) {
    return absl::InternalError("failed to write nginx config");
  }
  LOG(INFO) << "[nginx] injected " << label << " block";

  int rc = std::system("nginx -s reload 2>/dev/null");  // NOLINT
  if (rc != 0)
    LOG(WARNING) << "[nginx] reload returned " << rc;
  return absl::OkStatus();
}

// Shared: strip a block and reload nginx.
static absl::Status strip_nginx_block(const char* begin,
                                      const char* end,
                                      const char* label) {
  std::string content = read_file(kNginxConf);
  if (content.empty()) {
    LOG(INFO) << "[nginx] config not found -- nothing to revert";
    return absl::OkStatus();
  }
  if (!strip_block(&content, begin, end)) {
    LOG(INFO) << "[nginx] no " << label
              << " block found -- nothing to revert";
    return absl::OkStatus();
  }
  if (!write_file(kNginxConf, content)) {
    return absl::InternalError("failed to write nginx config");
  }
  LOG(INFO) << "[nginx] removed " << label << " block";

  int rc = std::system("nginx -s reload 2>/dev/null");  // NOLINT
  if (rc != 0)
    LOG(WARNING) << "[nginx] reload returned " << rc;
  return absl::OkStatus();
}

absl::Status patch_nginx_log_proxy(uint16_t port) {
  // Same 401 -> 302-to-root redirect as patch_nginx_admin_proxy: an
  // unauthenticated visitor pasting the log URL gets the UniFi OS login
  // flow instead of a bare "401 Unauthorized".
  std::string block;
  block += kLogMarkerBegin;
  block += "    location /onvif/events/log {\n";
  block += "        include /usr/share/unifi-core/http/auth.conf;\n";
  block += "        include /usr/share/unifi-core/http/proxy.conf;\n";
  block += "        error_page 401 = @onvif_log_unauth;\n";
  block += "        proxy_pass http://127.0.0.1:";
  block += std::to_string(port);
  block += "/;\n";
  block += "    }\n";
  block += "    location @onvif_log_unauth {\n";
  block += "        return 302 /;\n";
  block += "    }\n";
  block += kLogMarkerEnd;
  return inject_nginx_block(block, kLogMarkerBegin, kLogMarkerEnd,
                            "log proxy");
}

absl::Status revert_nginx_log_proxy() {
  return strip_nginx_block(kLogMarkerBegin, kLogMarkerEnd, "log proxy");
}

absl::Status patch_nginx_admin_proxy(uint16_t port) {
  // When auth.conf's auth_request returns 401 (unauthenticated visitor),
  // nginx would otherwise propagate a bare "401 Unauthorized" -- friendly
  // for an XHR but jarring for a user who just pasted the admin URL into
  // a browser.  Override 401 to a 302 redirect at the UniFi OS root,
  // which presents the standard login flow.  On successful login the
  // user can re-navigate to /onvif/admin/.
  std::string block;
  block += kAdminMarkerBegin;
  block += "    location /onvif/admin/ {\n";
  block += "        include /usr/share/unifi-core/http/auth.conf;\n";
  block += "        include /usr/share/unifi-core/http/proxy.conf;\n";
  block += "        error_page 401 = @onvif_admin_unauth;\n";
  block += "        proxy_pass http://127.0.0.1:";
  block += std::to_string(port);
  block += "/;\n";
  block += "    }\n";
  block += "    location @onvif_admin_unauth {\n";
  block += "        return 302 /;\n";
  block += "    }\n";
  block += kAdminMarkerEnd;
  return inject_nginx_block(block, kAdminMarkerBegin, kAdminMarkerEnd,
                            "admin proxy");
}

absl::Status revert_nginx_admin_proxy() {
  return strip_nginx_block(kAdminMarkerBegin, kAdminMarkerEnd,
                           "admin proxy");
}

}  // namespace protect_ui
