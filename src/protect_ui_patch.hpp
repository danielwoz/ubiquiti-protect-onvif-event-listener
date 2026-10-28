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
  std::string name;         // "entry <n>" (from 1) when the file gives none,
                            // or it could not be read
  std::string file;         // only "service.js"
  std::string original;     // non-empty; must occur exactly once
  std::string replacement;  // non-empty; must not contain original
  /// Why the entry cannot be used, with its line and column; empty for a
  /// valid entry.  An invalid entry has only its name, and is never
  /// applied.
  std::string error;
};

/// Largest part of a local patches file read, in bytes.
inline constexpr size_t kMaxLocalPatchesBytes = 1024 * 1024;
/// Most entries of a local patches file used; later ones are ignored.
inline constexpr size_t kMaxLocalPatches = 100;

/// What a local patches file holds.
struct LocalPatches {
  /// Its first kMaxLocalPatches entries, valid or not, in file order.
  std::vector<LocalPatch> entries;
  /// Why the file, or the part of it after some of its entries, is not
  /// used; empty when all of it was read.  Entries read before the
  /// problem are still in `entries`.
  std::string error;
};

/// Parse the text of a local patches file: a JSON array of objects with
/// the string members "name" (optional), "file" ("service.js"), "original"
/// and "replacement", and no others.  Text that is empty or only white
/// space holds no entries.  Each entry stands alone: one that is not an
/// object, has a missing, unknown, repeated or non-string member, a value
/// LocalPatch does not allow, or malformed JSON is kept with its error,
/// and parsing goes on at the next entry.  After malformed JSON that
/// happens only where the end of the broken entry can be found by its
/// quotes and brackets, a ',' or ']' follows it, and the text up to it
/// holds no '{' that starts like an entry (a '{' followed by one of the
/// four keys and ':', as the next entry would after a missing or extra
/// '"'); otherwise the rest of the text is not read and is reported in
/// LocalPatches::error, as are the entries after the first
/// kMaxLocalPatches.  Text that is not an array has no entries and an
/// error.
LocalPatches parse_local_patches(const std::string& text);

/// Read and parse the local patches file at @p path.  A file that does not
/// exist holds no entries and no error.  A file that cannot be read or is
/// not a regular file has no entries and an error.  Of a file larger than
/// kMaxLocalPatchesBytes only the entries that end within that size are
/// read, and the rest is reported as an error.
LocalPatches load_local_patches(const std::string& path);

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
  kInvalid,        // local patch entry not valid, so skipped
};

/// Lower-case name of @p outcome: "not_run", "applied", "already",
/// "removed", "absent", "not_applicable", "ambiguous" or "invalid".
const char* patch_outcome_name(PatchOutcome outcome);

/// What patch_alarm_picker() did with one local patch.
struct LocalPatchOutcome {
  std::string name;
  /// kApplied, kAlready, kNotApplicable, kAmbiguous, kInvalid, or kNotRun
  /// when service.js was not patched.
  PatchOutcome outcome = PatchOutcome::kNotRun;
  /// LocalPatch::error of a kInvalid entry; empty otherwise.
  std::string error{};
};

/// Outcome of each optional service.js patch in one patch_alarm_picker()
/// call.
struct PatchReport {
  PatchOutcome onvif_addressing = PatchOutcome::kNotRun;
  PatchOutcome event_proxy = PatchOutcome::kNotRun;
  /// PatchOptions::local_patches of the call.
  std::string local_patches_path;
  /// LocalPatches::error of the local patches file: it could not be read,
  /// or part of it was not used.  Entries read before the problem are
  /// still in `local_patches` and applied.  Empty when all of it was used
  /// or it does not exist.
  std::string local_patches_error;
  /// Each entry read from the local patches file, in file order.
  std::vector<LocalPatchOutcome> local_patches;
};

/// JSON object for /api/status describing the local patches of @p report:
/// {"path":..., "error":..., "entries":[{"name":..., "outcome":...,
/// "error":...}]}, with outcomes named as by patch_outcome_name().
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
/// valid entries of the local patches file @p opts.local_patches follow
/// onvif-recorder's own patches; invalid ones are skipped (see
/// parse_local_patches()).  Each local patch not applied and each problem
/// with the file is logged as an error, again only once it has changed or
/// gone away in between, since passes repeat.  Changes to service.js load
/// at Protect's next start.
///
/// Each patch is applied only where its target code occurs exactly once,
/// and only when its replacement does not form that code again.
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
