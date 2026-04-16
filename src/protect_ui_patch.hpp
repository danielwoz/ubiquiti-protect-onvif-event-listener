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
/// length) so all cameras pass through.  A .bak copy of the unpatched file is
/// always written before modifying, so it tracks the current firmware version.
///
/// Returns OK if the patch was applied or was already present.
/// Returns NotFoundError if swai.js does not exist.
/// Returns InternalError on I/O failures.
/// Returns FailedPreconditionError if the expected pattern is not found
/// (firmware changed the code layout).
absl::Status patch_alarm_picker();

/// Revert all alarm-picker patches by restoring .bak files.
///
/// For each previously patched file (swai*.js, vantage*.js, service.js),
/// if a .bak exists it is copied back over the patched version.
/// Returns OK with the number of files restored.
absl::Status revert_alarm_picker();

}  // namespace protect_ui
