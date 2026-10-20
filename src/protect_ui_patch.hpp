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

#include <string>

#include "absl/status/status.h"

namespace protect_ui {

/// Live-patch the Protect UI to allow third-party cameras in the alarm picker.
///
/// The Protect frontend (swai.js) hard-codes a filter that excludes third-party
/// cameras from the alarm creation picker unless paired with an AI Port:
///
///   t.filter(e => !e.isThirdPartyCamera || e.isPairedWithAiPort)
///
/// This function replaces that predicate with a constant `true` (same byte
/// length) so all cameras pass through, and removes the matching exclusion
/// from the backend's camera scope in service.js.  With @p onvif_addressing
/// it also makes Protect's own ONVIF client send the WS-Addressing To and
/// Action headers with its event requests (Protect 7.1+), which some cameras
/// (Reolink) require.  Without it that change is removed again.  Changes to
/// service.js load at Protect's next start.
///
/// Each patch is applied only where its target code occurs exactly once.  A
/// file that differs from the package original is rebuilt from its .bak when
/// dpkg confirms the .bak is that original; otherwise an unpatched file is
/// backed up to .bak first.  Files are replaced via rename(), so a failed
/// write leaves the old one.  Nothing is patched while dpkg is unpacking
/// unifi-protect.
///
/// Returns OK if the files were patched or already patched, including when
/// no patch matches this Protect version.
/// Returns NotFoundError if none of the Protect files exist.
absl::Status patch_alarm_picker(bool onvif_addressing);

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
