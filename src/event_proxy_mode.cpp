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

#include "event_proxy_mode.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "util.hpp"

namespace onvif {
namespace event_proxy {

namespace {

bool all_hex(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

// MACs of a state file: the hex entries of the comma-separated list.
std::set<std::string> state_macs(const std::string& csv) {
  std::set<std::string> out;
  for (const auto& m : parse_mac_list(csv)) {
    if (all_hex(m)) out.insert(m);
  }
  return out;
}

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::optional<std::string> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

absl::Status write_file_atomic(const std::string& path,
                               const std::string& content) {
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                        0600);
  if (fd < 0) {
    return absl::InternalError(tmp + ": " + std::strerror(errno));
  }
  size_t off = 0;
  while (off < content.size()) {
    const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      const std::string err = std::strerror(errno);
      ::close(fd);
      ::unlink(tmp.c_str());
      return absl::InternalError(tmp + ": " + err);
    }
    off += static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0 || ::close(fd) != 0) {
    const std::string err = std::strerror(errno);
    ::unlink(tmp.c_str());
    return absl::InternalError(tmp + ": " + err);
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    const std::string err = std::strerror(errno);
    ::unlink(tmp.c_str());
    return absl::InternalError(path + ": " + err);
  }
  return absl::OkStatus();
}

absl::Status apply_change(const std::string& path, const FileChange& c) {
  switch (c.op) {
    case FileChange::Op::kKeep:
      return absl::OkStatus();
    case FileChange::Op::kWrite:
      return write_file_atomic(path, c.content);
    case FileChange::Op::kRemove:
      if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        return absl::InternalError(path + ": " + std::strerror(errno));
      }
      return absl::OkStatus();
  }
  return absl::OkStatus();
}

FileChange write_change(std::string content) {
  FileChange c;
  c.op = FileChange::Op::kWrite;
  c.content = std::move(content);
  return c;
}

FileChange remove_change() {
  FileChange c;
  c.op = FileChange::Op::kRemove;
  return c;
}

}  // namespace

std::string normalize_mac(const std::string& mac) {
  std::string out;
  out.reserve(mac.size());
  for (char c : mac) {
    if (c == ':' || c == '-' || c == ' ') continue;
    out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::set<std::string> parse_mac_list(const std::string& csv) {
  std::set<std::string> out;
  size_t pos = 0;
  while (pos <= csv.size()) {
    size_t comma = csv.find(',', pos);
    if (comma == std::string::npos) comma = csv.size();
    std::string m = normalize_mac(trim(csv.substr(pos, comma - pos)));
    if (!m.empty()) out.insert(std::move(m));
    pos = comma + 1;
  }
  return out;
}

std::string join_macs(const std::set<std::string>& macs) {
  std::string out;
  for (const auto& m : macs) {
    if (!out.empty()) out += ',';
    out += m;
  }
  return out;
}

const char* mode_name(Mode mode) {
  switch (mode) {
    case Mode::kOff:   return "off";
    case Mode::kServe: return "serve";
    case Mode::kDrain: return "drain";
  }
  return "off";
}

std::string format_drain(const DrainState& drain) {
  return std::to_string(drain.protect_pid) + " " + join_macs(drain.macs) +
         "\n";
}

std::optional<DrainState> parse_drain(const std::string& text) {
  const std::string t = trim(text);
  const auto sp = t.find_first_of(" \t");
  if (sp == std::string::npos) return std::nullopt;
  const std::string pid_str = t.substr(0, sp);
  if (pid_str.empty() || pid_str.size() > 10) return std::nullopt;
  int64_t pid = 0;
  for (char c : pid_str) {
    if (c < '0' || c > '9') return std::nullopt;
    pid = pid * 10 + (c - '0');
  }
  if (pid <= 0 || pid > std::numeric_limits<pid_t>::max()) return std::nullopt;
  DrainState d;
  d.protect_pid = static_cast<pid_t>(pid);
  d.macs = state_macs(t.substr(sp + 1));
  if (d.macs.empty()) return std::nullopt;
  return d;
}

StateFiles read_state_files(const std::string& state_dir) {
  StateFiles f;
  f.active = read_file(state_dir + "/" + kActiveFile);
  f.drain = read_file(state_dir + "/" + kDrainFile);
  return f;
}

Decision decide_mode(bool patch_protect,
                     const std::set<std::string>& configured,
                     const StateFiles& files,
                     const std::function<pid_t()>& protect_pid) {
  Decision d;
  if (patch_protect && !configured.empty()) {
    d.mode = Mode::kServe;
    d.macs = configured;
    d.active = write_change(join_macs(configured) + "\n");
    if (files.drain) d.drain = remove_change();
    return d;
  }
  if (!patch_protect) {
    d.patching_off = !configured.empty();
    return d;
  }

  std::optional<pid_t> pid_once;
  auto pid = [&]() -> pid_t {
    if (!pid_once) pid_once = protect_pid ? protect_pid() : 0;
    return *pid_once;
  };
  const std::set<std::string> active =
      files.active ? state_macs(*files.active) : std::set<std::string>{};
  const std::optional<DrainState> drain =
      files.drain ? parse_drain(*files.drain) : std::nullopt;

  if (!active.empty() && pid() > 0) {
    d.mode = Mode::kDrain;
    d.protect_pid = pid();
    d.macs = active;
    if (drain && drain->protect_pid == d.protect_pid)
      d.macs.insert(drain->macs.begin(), drain->macs.end());
    d.drain = write_change(format_drain({d.protect_pid, d.macs}));
    d.active = remove_change();
    return d;
  }
  if (drain && pid() > 0 && drain->protect_pid == pid()) {
    d.mode = Mode::kDrain;
    d.protect_pid = pid();
    d.macs = drain->macs;
    // An active file here holds no MAC, or Protect was not running.
    if (files.active) d.active = remove_change();
    return d;
  }
  d.cleared = files.active.has_value() || files.drain.has_value();
  if (files.active) d.active = remove_change();
  if (files.drain) d.drain = remove_change();
  return d;
}

absl::Status apply_decision(const std::string& state_dir,
                            const Decision& decision) {
  // Writes come before removals, and a failed write removes nothing, so
  // an interrupted or failed transition still finds the cameras on the
  // next start (the active file goes only once the drain file is written).
  const std::pair<std::string, const FileChange*> changes[] = {
      {state_dir + "/" + kDrainFile, &decision.drain},
      {state_dir + "/" + kActiveFile, &decision.active},
  };
  absl::Status result;
  for (const auto& [path, change] : changes) {
    if (change->op != FileChange::Op::kWrite) continue;
    absl::Status s = apply_change(path, *change);
    if (!s.ok() && result.ok()) result = s;
  }
  if (!result.ok()) return result;
  for (const auto& [path, change] : changes) {
    if (change->op != FileChange::Op::kRemove) continue;
    absl::Status s = apply_change(path, *change);
    if (!s.ok() && result.ok()) result = s;
  }
  return result;
}

int remove_state_files(const std::string& state_dir) {
  int removed = 0;
  for (const char* name :
       {kActiveFile, kDrainFile, kHubStateFile, kResumeFile}) {
    const std::string path = state_dir + "/" + name;
    if (::unlink(path.c_str()) == 0) ++removed;
  }
  return removed;
}

std::vector<std::pair<std::string, std::string>> unmanaged_reasons(
    const std::set<std::string>& wanted, const std::set<std::string>& managed,
    const std::set<std::string>& ai_port_paired,
    const std::set<std::string>& excluded) {
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& m : wanted) {
    if (managed.count(m)) continue;
    if (ai_port_paired.count(m)) {
      out.emplace_back(m, "paired with an AI Port");
    } else if (excluded.count(m)) {
      out.emplace_back(m, "excluded");
    } else {
      out.emplace_back(m, "not a third-party camera in Protect");
    }
  }
  return out;
}

std::string status_json(const StatusInfo& status) {
  std::string j = "{\"mode\":";
  j += util::json_str(mode_name(status.mode));
  j += ",\"port\":" + std::to_string(status.port);
  j += ",\"bound\":";
  j += status.bound ? "true" : "false";
  j += ",\"bind_failed\":";
  j += status.bind_failed ? "true" : "false";
  j += ",\"patch\":" + util::json_str(status.patch);
  j += ",\"protect_restart_pending\":";
  j += status.protect_restart_pending ? "true" : "false";
  j += ",\"hub\":";
  j += status.hub_json.empty() ? "null" : status.hub_json;
  j += "}";
  return j;
}

}  // namespace event_proxy
}  // namespace onvif
