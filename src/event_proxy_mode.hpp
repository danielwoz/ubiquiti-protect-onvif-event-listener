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

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"

namespace onvif {
namespace event_proxy {

// State files under --state_dir.
// MACs shared while the route patch is wanted.
inline constexpr char kActiveFile[] = "event-proxy-active";
// "<Protect MainPID> <mac,mac,...>": cameras a Protect process that loaded
// the route patch may still send to the recorder after sharing ended.
inline constexpr char kDrainFile[] = "event-proxy-drain";
// PullPointHub history.
inline constexpr char kHubStateFile[] = "event-proxy-state.json";
// Camera subscriptions kept across recorder restarts.
inline constexpr char kResumeFile[] = "upstream-subscriptions.json";

// Lower-case, with ':', '-' and ' ' removed (the --excluded_cameras form).
std::string normalize_mac(const std::string& mac);
// Comma-separated MACs, each normalised; empty items are dropped.
std::set<std::string> parse_mac_list(const std::string& csv);
// @p macs joined with ','.
std::string join_macs(const std::set<std::string>& macs);

enum class Mode {
  kOff,    // port not bound, route patch removed
  kServe,  // route patch applied, configured cameras shared
  kDrain,  // route patch removed, cameras still shared until Protect restarts
};
// "off", "serve" or "drain".
const char* mode_name(Mode mode);

struct DrainState {
  pid_t protect_pid = 0;
  std::set<std::string> macs;
};
// "<pid> <mac,mac,...>\n".
std::string format_drain(const DrainState& drain);
// The drain file's contents, or nullopt unless @p text holds a pid > 0
// and at least one hex MAC.
std::optional<DrainState> parse_drain(const std::string& text);

// Contents of the state files; nullopt for a missing file.
struct StateFiles {
  std::optional<std::string> active;
  std::optional<std::string> drain;
};
StateFiles read_state_files(const std::string& state_dir);

// What to do with one state file.
struct FileChange {
  enum class Op { kKeep, kWrite, kRemove };
  Op op = Op::kKeep;
  std::string content;  // for kWrite
};

struct Decision {
  Mode mode = Mode::kOff;
  // Normalised MACs of the shared cameras (Serve, Drain).
  std::set<std::string> macs;
  // Drain: the Protect process that may still send requests here.
  pid_t protect_pid = 0;
  FileChange active;
  FileChange drain;
  // MACs are configured but Protect patching is off.
  bool patching_off = false;
  // Off: state files left by an earlier run are being removed.
  bool cleared = false;
};

// Picks the mode from the setting and the state files:
//   * patching on and MACs configured: Serve them; write the active file,
//     remove the drain file.
//   * patching off: Off; files are kept.
//   * an active file and Protect running: Drain its MACs (plus those of a
//     drain file for the same Protect process); the active file becomes a
//     drain file for that process.
//   * a drain file for the running Protect process: Drain its MACs.
//   * otherwise: Off; both files are removed.
// @p protect_pid returns unifi-protect's MainPID (0 when not running) and
// is called at most once, only when a state file needs it.
Decision decide_mode(bool patch_protect,
                     const std::set<std::string>& configured,
                     const StateFiles& files,
                     const std::function<pid_t()>& protect_pid);

// Writes and removes the state files under @p state_dir as @p decision
// says.  Writes are atomic (temp file, fsync, rename, mode 0600) and come
// first; when one fails, no file is removed.
absl::Status apply_decision(const std::string& state_dir,
                            const Decision& decision);

// Removes every event-sharing state file under @p state_dir.  Returns how
// many were removed.
int remove_state_files(const std::string& state_dir);

// Why each MAC of @p wanted is not among @p managed: "paired with an AI
// Port", "excluded" or "not a third-party camera in Protect".  All sets
// hold normalised MACs.
std::vector<std::pair<std::string, std::string>> unmanaged_reasons(
    const std::set<std::string>& wanted, const std::set<std::string>& managed,
    const std::set<std::string>& ai_port_paired,
    const std::set<std::string>& excluded);

// Body of GET /api/event_proxy.
struct StatusInfo {
  Mode mode = Mode::kOff;       // the mode in effect (off after bind_failed)
  uint16_t port = 0;
  bool bound = false;           // the PullPoint service is listening
  bool bind_failed = false;     // it could not listen, so sharing is off
  std::string patch;            // outcome name of the route patch
  bool protect_restart_pending = false;
  std::string hub_json;         // PullPointHub::snapshot_json(); "" = null
};
std::string status_json(const StatusInfo& status);

}  // namespace event_proxy
}  // namespace onvif
