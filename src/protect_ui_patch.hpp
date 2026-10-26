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

#include <sys/types.h>

#include <cstddef>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace protect_ui {

/// The optional service.js patches patch_alarm_picker() applies.  A patch
/// that is off is removed again where an earlier run applied it.
struct PatchOptions {
  /// Protect's ONVIF client sends the WS-Addressing To and Action headers
  /// with PullMessages, Renew and Unsubscribe (Protect 7.1+).
  bool onvif_addressing = true;
  /// Protect offers its ONVIF event subscriptions to onvif-recorder's
  /// PullPoint service on 127.0.0.1:7892 first, and falls back to the
  /// camera when that refuses or does not answer within 4 s (Protect 7.1+).
  bool event_proxy = false;
  /// Path of the user's local patches file (--local_patches), read on
  /// every pass; empty for none.  See load_local_patches().
  std::string local_patches{};
};

/// One entry of the local patches file: a literal find/replace in one of
/// Protect's files, applied after onvif-recorder's own patches and by the
/// same rules.
struct LocalPatch {
  std::string name;         // "entry <n>" when the file gives none
  std::string file;         // only "service.js"
  std::string original;     // non-empty; must occur exactly once
  std::string replacement;  // non-empty; must not contain original
};

/// Largest local patches file read, in bytes.
inline constexpr size_t kMaxLocalPatchesBytes = 1024 * 1024;
/// Most entries a local patches file may hold.
inline constexpr size_t kMaxLocalPatches = 100;

/// Parse the text of a local patches file: a JSON array of objects with
/// the string members "name" (optional), "file" ("service.js"), "original"
/// and "replacement", and no others.  Text that is empty or only white
/// space holds no entries.  Returns InvalidArgumentError naming the
/// problem (with line and column for malformed JSON) otherwise.
absl::StatusOr<std::vector<LocalPatch>> parse_local_patches(
    const std::string& text);

/// Read and parse the local patches file at @p path.  A file that does not
/// exist holds no entries.  Returns an error for a file that cannot be
/// read, is larger than kMaxLocalPatchesBytes or does not parse.
absl::StatusOr<std::vector<LocalPatch>> load_local_patches(
    const std::string& path);

/// What patch_alarm_picker() did with one optional patch.
enum class PatchOutcome {
  kNotRun,         // patching skipped (dpkg unpacking, file missing), or
                   // the file could not be written
  kApplied,        // written by this call
  kAlready,        // already present
  kRemoved,        // was present, removed by this call
  kAbsent,         // disabled and not present
  kNotApplicable,  // anchor not found, or safety scan refused
  kAmbiguous,      // anchor found more than once
};

/// Lower-case name of @p outcome: "not_run", "applied", "already",
/// "removed", "absent", "not_applicable" or "ambiguous".
const char* patch_outcome_name(PatchOutcome outcome);

/// What patch_alarm_picker() did with one local patch.
struct LocalPatchOutcome {
  std::string name;
  /// kApplied, kAlready, kNotApplicable, kAmbiguous, or kNotRun when
  /// service.js was not patched.
  PatchOutcome outcome = PatchOutcome::kNotRun;
};

/// Outcome of each optional service.js patch in one patch_alarm_picker()
/// call.
struct PatchReport {
  PatchOutcome onvif_addressing = PatchOutcome::kNotRun;
  PatchOutcome event_proxy = PatchOutcome::kNotRun;
  /// PatchOptions::local_patches of the call.
  std::string local_patches_path;
  /// Why no local patch was applied: the file could not be read or is
  /// not valid.  Empty when it was used or does not exist.
  std::string local_patches_error;
  /// Each entry of the local patches file, in file order.
  std::vector<LocalPatchOutcome> local_patches;
};

/// JSON object for /api/status describing the local patches of @p report:
/// {"path":..., "error":..., "entries":[{"name":..., "outcome":...}]},
/// with outcomes named as by patch_outcome_name().
std::string local_patches_json(const PatchReport& report);

/// Live-patch the Protect UI to allow third-party cameras in the alarm picker.
///
/// The Protect frontend (swai.js) hard-codes a filter that excludes third-party
/// cameras from the alarm creation picker unless paired with an AI Port:
///
///   t.filter(e => !e.isThirdPartyCamera || e.isPairedWithAiPort)
///
/// This function replaces that predicate with a constant `true` (same byte
/// length) so all cameras pass through, and removes the matching exclusion
/// from the backend's camera scope in service.js.  With
/// @p opts.onvif_addressing it also makes Protect's own ONVIF client send the
/// WS-Addressing To and Action headers with its event requests (Protect
/// 7.1+), which some cameras (Reolink) require.  With @p opts.event_proxy it
/// routes Protect's ONVIF event subscriptions through onvif-recorder (see
/// PatchOptions).  An option that is off removes its change again.  The
/// entries of the local patches file @p opts.local_patches follow
/// onvif-recorder's own patches; when the file cannot be read or is not
/// valid, none of them is applied.  Changes to service.js load at
/// Protect's next start.
///
/// Each patch is applied only where its target code occurs exactly once.
/// The event-sharing patch is also refused when Protect's SOAP transport
/// function cannot be delimited, uses `this` or `arguments`, or service.js
/// already uses the names the patch injects.  A file that differs from the
/// package original is rebuilt from its .bak when dpkg confirms the .bak is
/// that original; otherwise an unpatched file is backed up to .bak first.
/// Files are replaced via rename(), so a failed write leaves the old one.
/// Nothing is patched while dpkg is unpacking unifi-protect.
///
/// When @p report is set it receives the outcome of each optional patch.
///
/// Returns OK if the files were patched or already patched, including when
/// no patch matches this Protect version.
/// Returns NotFoundError if none of the Protect files exist.
absl::Status patch_alarm_picker(const PatchOptions& opts,
                                PatchReport* report = nullptr);

/// unifi-protect's MainPID from systemd, or 0 when it is not running.
pid_t protect_main_pid();

/// Whether the running unifi-protect process started before service.js was
/// last written, so it is still running the code it loaded at start-up and
/// only a restart loads the patched file.  False when Protect is not running
/// or the times cannot be read.
bool protect_predates_service_js();

/// Revert all alarm-picker patches by restoring .bak files.
///
/// For each previously patched file (swai*.js, vantage*.js, service.js),
/// if a .bak exists it is copied back over the patched version.
/// Returns OK with the number of files restored.
absl::Status revert_alarm_picker();

/// Test seam for revert_alarm_picker(): operate on a custom UI dist
/// directory (containing swai*.js / vantage*.js + their .bak siblings)
/// and an explicit service.js path.  Used by unit tests to avoid
/// touching /usr/share/unifi-protect.
absl::Status revert_alarm_picker_in(
    const std::string& ui_dir,
    const std::string& service_path);

/// Inject an nginx `location /onvif/events/log` block into the UniFi OS
/// site-local-ip.conf (HTTPS server) that proxies authenticated requests
/// to the in-process log server on localhost:@p port.
///
/// The block is bracketed by marker comments so it can be cleanly removed.
/// Reloads nginx after a successful patch.
/// No-op if the block is already present.
absl::Status patch_nginx_log_proxy(uint16_t port);

/// Remove the injected nginx location block and reload nginx.
absl::Status revert_nginx_log_proxy();

/// Inject an nginx `location /onvif/admin/` block into site-local-ip.conf
/// proxying authenticated requests to the in-process admin server on
/// localhost:@p port.  Paired with revert_nginx_admin_proxy() invoked from the
/// Debian prerm script.
absl::Status patch_nginx_admin_proxy(uint16_t port);

/// Remove the injected nginx admin location block and reload nginx.
absl::Status revert_nginx_admin_proxy();

}  // namespace protect_ui
