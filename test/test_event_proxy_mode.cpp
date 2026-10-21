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
 * test_event_proxy_mode.cpp
 *
 * Tests for the Protect event-sharing mode decision: MAC lists, the drain
 * file format, every branch of the mode table, the state-file changes it
 * makes on disk, and the /api/event_proxy JSON.
 */

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "event_proxy_mode.hpp"

namespace ep = onvif::event_proxy;
using Op = ep::FileChange::Op;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& label) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << label << '\n';
  }
}

// A Protect PID source that counts how often it is asked.
struct PidSource {
  pid_t pid = 0;
  int calls = 0;
  std::function<pid_t()> fn() {
    return [this] {
      ++calls;
      return pid;
    };
  }
};

static std::set<std::string> macs(std::initializer_list<const char*> l) {
  std::set<std::string> s;
  for (const char* m : l) s.insert(m);
  return s;
}

static const char kPtz[] = "f00000fdaed9";
static const char kCam2[] = "aabbccddeeff";

// ---------------------------------------------------------------
// MAC lists
// ---------------------------------------------------------------
static void test_normalize_mac() {
  check(ep::normalize_mac("F0:00:00:FD:AE:D9") == kPtz, "normalize: colons");
  check(ep::normalize_mac("f0-00-00-fd-ae-d9") == kPtz, "normalize: dashes");
  check(ep::normalize_mac(" F000 00FD AED9 ") == kPtz, "normalize: spaces");
  check(ep::normalize_mac("") == "", "normalize: empty");
}

static void test_parse_mac_list() {
  check(ep::parse_mac_list("") == std::set<std::string>{},
        "parse_mac_list: empty");
  check(ep::parse_mac_list(" , ,,") == std::set<std::string>{},
        "parse_mac_list: only separators");
  check(ep::parse_mac_list("F0:00:00:FD:AE:D9") == macs({kPtz}),
        "parse_mac_list: one");
  check(ep::parse_mac_list(" F000.., AA-BB-CC-DD-EE-FF ,\tf00000fdaed9 ,") ==
            macs({"f000..", kCam2, kPtz}),
        "parse_mac_list: mixed forms, tab, trailing comma");
  check(ep::parse_mac_list("F00000FDAED9,f0:00:00:fd:ae:d9") == macs({kPtz}),
        "parse_mac_list: duplicates collapse");
  check(ep::join_macs(macs({kPtz, kCam2})) ==
            std::string(kCam2) + "," + kPtz,
        "join_macs: sorted, comma-separated");
  check(ep::join_macs({}) == "", "join_macs: empty");
}

// ---------------------------------------------------------------
// Drain file
// ---------------------------------------------------------------
static void test_drain_format() {
  ep::DrainState d{4242, macs({kPtz, kCam2})};
  const std::string text = ep::format_drain(d);
  check(text == std::string("4242 ") + kCam2 + "," + kPtz + "\n",
        "format_drain: text");
  auto back = ep::parse_drain(text);
  check(back.has_value() && back->protect_pid == 4242 &&
            back->macs == d.macs,
        "parse_drain: round trip");

  auto spaced = ep::parse_drain("  77\t F0:00:00:FD:AE:D9 \r\n");
  check(spaced && spaced->protect_pid == 77 && spaced->macs == macs({kPtz}),
        "parse_drain: whitespace and colon MAC");

  auto partly = ep::parse_drain("77 junk!,f00000fdaed9");
  check(partly && partly->macs == macs({kPtz}),
        "parse_drain: non-hex entries are dropped");

  check(!ep::parse_drain(""), "parse_drain: empty");
  check(!ep::parse_drain("4242"), "parse_drain: pid only");
  check(!ep::parse_drain("4242 "), "parse_drain: pid and blank");
  check(!ep::parse_drain("abc f00000fdaed9"), "parse_drain: pid not a number");
  check(!ep::parse_drain("-5 f00000fdaed9"), "parse_drain: negative pid");
  check(!ep::parse_drain("0 f00000fdaed9"), "parse_drain: pid 0");
  check(!ep::parse_drain("99999999999 f00000fdaed9"),
        "parse_drain: pid too large");
  check(!ep::parse_drain("12x f00000fdaed9"), "parse_drain: pid with junk");
  check(!ep::parse_drain("4242 zz:zz,!!"), "parse_drain: no hex MAC");
  check(!ep::parse_drain(std::string("\x01\x02\xff garbage", 11)),
        "parse_drain: binary garbage");
}

// ---------------------------------------------------------------
// Mode table
// ---------------------------------------------------------------
static ep::StateFiles files(std::optional<std::string> active,
                            std::optional<std::string> drain) {
  ep::StateFiles f;
  f.active = std::move(active);
  f.drain = std::move(drain);
  return f;
}

static void test_serve() {
  PidSource pid{100};
  auto d = ep::decide_mode(true, macs({kPtz}), files({}, {}), pid.fn());
  check(d.mode == ep::Mode::kServe, "serve: mode");
  check(d.macs == macs({kPtz}), "serve: configured cameras");
  check(d.active.op == Op::kWrite && d.active.content == std::string(kPtz) + "\n",
        "serve: active file written");
  check(d.drain.op == Op::kKeep, "serve: no drain file to remove");
  check(!d.patching_off && !d.cleared, "serve: no warning flags");
  check(pid.calls == 0, "serve: Protect PID not queried");

  // Sharing turned on again while an earlier drain is pending.
  auto d2 = ep::decide_mode(true, macs({kCam2}),
                            files(std::string(kPtz) + "\n",
                                  std::string("100 ") + kPtz + "\n"),
                            pid.fn());
  check(d2.mode == ep::Mode::kServe && d2.macs == macs({kCam2}),
        "serve over drain: configured cameras only");
  check(d2.active.op == Op::kWrite &&
            d2.active.content == std::string(kCam2) + "\n",
        "serve over drain: active file rewritten");
  check(d2.drain.op == Op::kRemove, "serve over drain: drain file removed");
  check(pid.calls == 0, "serve over drain: Protect PID not queried");
}

static void test_patching_off() {
  PidSource pid{100};
  auto d = ep::decide_mode(false, macs({kPtz}), files({}, {}), pid.fn());
  check(d.mode == ep::Mode::kOff, "patch off: mode off");
  check(d.patching_off, "patch off: warns about the unused setting");
  check(d.active.op == Op::kKeep && d.drain.op == Op::kKeep,
        "patch off: files untouched");

  auto d2 = ep::decide_mode(false, {},
                            files(std::string(kPtz), std::string("100 ") + kPtz),
                            pid.fn());
  check(d2.mode == ep::Mode::kOff && !d2.patching_off,
        "patch off, empty setting: off, no warning");
  check(d2.active.op == Op::kKeep && d2.drain.op == Op::kKeep,
        "patch off: existing state files kept");
  check(!d2.cleared, "patch off: nothing cleared");
  check(pid.calls == 0, "patch off: Protect PID not queried");
}

static void test_off_without_state() {
  PidSource pid{100};
  auto d = ep::decide_mode(true, {}, files({}, {}), pid.fn());
  check(d.mode == ep::Mode::kOff, "off: mode");
  check(d.active.op == Op::kKeep && d.drain.op == Op::kKeep,
        "off: nothing to remove");
  check(!d.cleared && !d.patching_off, "off: nothing to report");
  check(pid.calls == 0, "off: Protect PID not queried without state files");

  auto d2 = ep::decide_mode(true, {}, files({}, {}), nullptr);
  check(d2.mode == ep::Mode::kOff, "off: no PID source needed");
}

static void test_active_to_drain() {
  PidSource pid{4242};
  auto d = ep::decide_mode(true, {}, files(std::string(kPtz) + "\n", {}),
                           pid.fn());
  check(d.mode == ep::Mode::kDrain, "active->drain: mode");
  check(d.macs == macs({kPtz}), "active->drain: cameras from active file");
  check(d.protect_pid == 4242, "active->drain: Protect PID kept");
  check(d.drain.op == Op::kWrite &&
            d.drain.content == std::string("4242 ") + kPtz + "\n",
        "active->drain: drain file written for this Protect");
  check(d.active.op == Op::kRemove, "active->drain: active file removed");
  check(pid.calls == 1, "active->drain: Protect PID queried once");

  // A drain file for the same Protect adds its cameras.
  PidSource same{4242};
  auto d2 = ep::decide_mode(true, {},
                            files(std::string(kPtz),
                                  std::string("4242 ") + kCam2),
                            same.fn());
  check(d2.mode == ep::Mode::kDrain && d2.macs == macs({kPtz, kCam2}),
        "active->drain: merges a drain file of the same Protect");
  check(d2.drain.op == Op::kWrite &&
            ep::parse_drain(d2.drain.content)->macs == macs({kPtz, kCam2}),
        "active->drain: merged drain file");
  check(same.calls == 1, "active->drain: PID queried once with both files");

  // A drain file of an earlier Protect is replaced.
  PidSource other{5000};
  auto d3 = ep::decide_mode(true, {},
                            files(std::string(kPtz),
                                  std::string("4242 ") + kCam2),
                            other.fn());
  check(d3.mode == ep::Mode::kDrain && d3.macs == macs({kPtz}),
        "active->drain: ignores a drain file of another Protect");
  check(d3.drain.op == Op::kWrite &&
            d3.drain.content == std::string("5000 ") + kPtz + "\n",
        "active->drain: drain file rewritten for the running Protect");
}

static void test_active_protect_not_running() {
  PidSource pid{0};
  auto d = ep::decide_mode(true, {}, files(std::string(kPtz), {}), pid.fn());
  check(d.mode == ep::Mode::kOff,
        "active, Protect not running: off (next Protect loads no patch)");
  check(d.active.op == Op::kRemove && d.drain.op == Op::kKeep,
        "active, Protect not running: active file removed");
  check(d.cleared, "active, Protect not running: reported as cleared");
}

static void test_drain_same_pid() {
  PidSource pid{4242};
  auto d = ep::decide_mode(true, {},
                           files({}, std::string("4242 ") + kPtz + "\n"),
                           pid.fn());
  check(d.mode == ep::Mode::kDrain, "drain, same Protect: mode");
  check(d.macs == macs({kPtz}) && d.protect_pid == 4242,
        "drain, same Protect: cameras and PID");
  check(d.drain.op == Op::kKeep && d.active.op == Op::kKeep,
        "drain, same Protect: files kept");
  check(pid.calls == 1, "drain, same Protect: PID queried once");
}

static void test_drain_pid_mismatch() {
  PidSource pid{5000};
  auto d = ep::decide_mode(true, {},
                           files({}, std::string("4242 ") + kPtz + "\n"),
                           pid.fn());
  check(d.mode == ep::Mode::kOff, "drain, Protect restarted: off");
  check(d.drain.op == Op::kRemove && d.active.op == Op::kKeep,
        "drain, Protect restarted: drain file removed");
  check(d.cleared, "drain, Protect restarted: reported as cleared");

  PidSource none{0};
  auto d2 = ep::decide_mode(true, {},
                            files({}, std::string("4242 ") + kPtz),
                            none.fn());
  check(d2.mode == ep::Mode::kOff && d2.drain.op == Op::kRemove,
        "drain, Protect not running: off, drain file removed");
}

static void test_garbled_files() {
  PidSource pid{4242};
  auto d = ep::decide_mode(true, {}, files({}, std::string("garbage")),
                           pid.fn());
  check(d.mode == ep::Mode::kOff, "garbled drain: off");
  check(d.drain.op == Op::kRemove, "garbled drain: removed");
  check(d.cleared, "garbled drain: reported as cleared");
  check(pid.calls == 0, "garbled drain: PID not queried");

  auto d2 = ep::decide_mode(true, {}, files({}, std::string("")), pid.fn());
  check(d2.mode == ep::Mode::kOff && d2.drain.op == Op::kRemove,
        "empty drain file: off, removed");

  auto d3 = ep::decide_mode(true, {}, files(std::string(" ,\n"), {}),
                            pid.fn());
  check(d3.mode == ep::Mode::kOff && d3.active.op == Op::kRemove,
        "active file without a MAC: off, removed");
  check(pid.calls == 0, "active file without a MAC: PID not queried");

  // A garbled active file next to a valid drain file of this Protect.
  PidSource same{4242};
  auto d4 = ep::decide_mode(true, {},
                            files(std::string("!!"),
                                  std::string("4242 ") + kPtz),
                            same.fn());
  check(d4.mode == ep::Mode::kDrain && d4.macs == macs({kPtz}),
        "garbled active, valid drain: drain");
  check(d4.active.op == Op::kRemove && d4.drain.op == Op::kKeep,
        "garbled active, valid drain: active removed, drain kept");
}

static void test_mode_names() {
  check(std::string(ep::mode_name(ep::Mode::kOff)) == "off", "mode_name off");
  check(std::string(ep::mode_name(ep::Mode::kServe)) == "serve",
        "mode_name serve");
  check(std::string(ep::mode_name(ep::Mode::kDrain)) == "drain",
        "mode_name drain");
}

// ---------------------------------------------------------------
// State files on disk
// ---------------------------------------------------------------
static std::string make_temp_dir() {
  const char* base = std::getenv("TEST_TMPDIR");
  std::string tmpl = std::string(base ? base : "/tmp") + "/epmodeXXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* dir = mkdtemp(buf.data());
  return dir ? std::string(dir) : std::string();
}

static void write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

static std::optional<std::string> read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static bool exists(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0;
}

static void test_files_on_disk() {
  const std::string dir = make_temp_dir();
  check(!dir.empty(), "temp dir created");
  if (dir.empty()) return;
  const std::string active = dir + "/" + ep::kActiveFile;
  const std::string drain = dir + "/" + ep::kDrainFile;

  auto f0 = ep::read_state_files(dir);
  check(!f0.active && !f0.drain, "read_state_files: none yet");

  // Serve writes the active file.
  PidSource pid{4242};
  auto serve = ep::decide_mode(true, macs({kPtz}), f0, pid.fn());
  check(ep::apply_decision(dir, serve).ok(), "apply serve: ok");
  check(read_text(active) == std::string(kPtz) + "\n",
        "apply serve: active file content");
  struct stat st{};
  check(::stat(active.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
        "apply serve: active file mode 0600");
  check(!exists(active + ".tmp"), "apply serve: no temp file left");

  // Sharing turned off: active becomes drain for the running Protect.
  auto f1 = ep::read_state_files(dir);
  check(f1.active.has_value() && !f1.drain, "read_state_files: active only");
  auto to_drain = ep::decide_mode(true, {}, f1, pid.fn());
  check(to_drain.mode == ep::Mode::kDrain, "disk: active->drain mode");
  check(ep::apply_decision(dir, to_drain).ok(), "apply drain: ok");
  check(!exists(active), "apply drain: active file removed");
  check(read_text(drain) == std::string("4242 ") + kPtz + "\n",
        "apply drain: drain file content");

  // Recorder restarted, same Protect: still drain, files unchanged.
  auto f2 = ep::read_state_files(dir);
  auto again = ep::decide_mode(true, {}, f2, pid.fn());
  check(again.mode == ep::Mode::kDrain && again.macs == macs({kPtz}),
        "disk: drain kept for the same Protect");
  check(ep::apply_decision(dir, again).ok(), "apply drain again: ok");
  check(exists(drain), "apply drain again: drain file kept");

  // Protect restarted: off, drain file removed.
  PidSource restarted{5000};
  auto off = ep::decide_mode(true, {}, ep::read_state_files(dir),
                             restarted.fn());
  check(off.mode == ep::Mode::kOff && off.cleared, "disk: off after restart");
  check(ep::apply_decision(dir, off).ok(), "apply off: ok");
  check(!exists(drain) && !exists(active), "apply off: no state files left");

  // Removing a missing file is not an error.
  ep::Decision rm;
  rm.active.op = Op::kRemove;
  rm.drain.op = Op::kRemove;
  check(ep::apply_decision(dir, rm).ok(), "apply: removing missing files ok");

  // A write into a missing directory fails.
  check(!ep::apply_decision(dir + "/missing", serve).ok(),
        "apply: write failure reported");

  // A failed write removes nothing.  A directory in place of the temp
  // file makes the write fail.
  write_text(active, std::string(kPtz) + "\n");
  check(::mkdir((drain + ".tmp").c_str(), 0700) == 0, "drain blocker made");
  auto blocked_drain =
      ep::decide_mode(true, {}, ep::read_state_files(dir), pid.fn());
  check(blocked_drain.mode == ep::Mode::kDrain, "blocked: drain mode");
  check(!ep::apply_decision(dir, blocked_drain).ok(),
        "blocked drain: write failure reported");
  check(read_text(active) == std::string(kPtz) + "\n",
        "blocked drain: active file kept");
  ::rmdir((drain + ".tmp").c_str());
  ::unlink(active.c_str());

  write_text(drain, std::string("4242 ") + kPtz + "\n");
  check(::mkdir((active + ".tmp").c_str(), 0700) == 0, "active blocker made");
  auto blocked_serve = ep::decide_mode(true, macs({kPtz}),
                                       ep::read_state_files(dir), pid.fn());
  check(blocked_serve.mode == ep::Mode::kServe, "blocked: serve mode");
  check(!ep::apply_decision(dir, blocked_serve).ok(),
        "blocked serve: write failure reported");
  check(read_text(drain) == std::string("4242 ") + kPtz + "\n",
        "blocked serve: drain file kept");
  ::rmdir((active + ".tmp").c_str());
  ::unlink(drain.c_str());

  // remove_state_files clears all four files.
  write_text(active, "x");
  write_text(drain, "x");
  write_text(dir + "/" + ep::kHubStateFile, "{}");
  write_text(dir + "/" + ep::kResumeFile, "{}");
  write_text(dir + "/healer-restarts", "keep");
  check(ep::remove_state_files(dir) == 4, "remove_state_files: four removed");
  check(!exists(active) && !exists(drain) &&
            !exists(dir + "/" + ep::kHubStateFile) &&
            !exists(dir + "/" + ep::kResumeFile),
        "remove_state_files: all gone");
  check(exists(dir + "/healer-restarts"),
        "remove_state_files: other state kept");
  check(ep::remove_state_files(dir) == 0, "remove_state_files: none left");
}

// ---------------------------------------------------------------
// Unmanaged cameras
// ---------------------------------------------------------------
static void test_unmanaged_reasons() {
  const auto r = ep::unmanaged_reasons(
      macs({"aa", "bb", "cc", "dd", "ee"}),  // wanted
      macs({"aa"}),                          // managed
      macs({"bb", "dd"}),                    // AI Port paired
      macs({"cc", "dd"}));                   // excluded
  check(r.size() == 4, "unmanaged: four cameras");
  if (r.size() == 4) {
    check(r[0] == std::make_pair(std::string("bb"),
                                 std::string("paired with an AI Port")),
          "unmanaged: AI Port");
    check(r[1] == std::make_pair(std::string("cc"), std::string("excluded")),
          "unmanaged: excluded");
    check(r[2].second == "paired with an AI Port",
          "unmanaged: AI Port wins over excluded");
    check(r[3] == std::make_pair(std::string("ee"),
                                 std::string(
                                     "not a third-party camera in Protect")),
          "unmanaged: not found");
  }
  check(ep::unmanaged_reasons(macs({"aa"}), macs({"aa"}), {}, {}).empty(),
        "unmanaged: none");
}

// ---------------------------------------------------------------
// Status JSON
// ---------------------------------------------------------------
static void test_status_json() {
  ep::StatusInfo off;
  check(ep::status_json(off) ==
            "{\"mode\":\"off\",\"port\":0,\"bound\":false,"
            "\"bind_failed\":false,\"patch\":\"\","
            "\"protect_restart_pending\":false,\"hub\":null}",
        "status_json: off");

  ep::StatusInfo serve;
  serve.mode = ep::Mode::kServe;
  serve.port = 7892;
  serve.bound = true;
  serve.patch = "applied";
  serve.protect_restart_pending = true;
  serve.hub_json = "{\"ready\":true,\"cameras\":[]}";
  check(ep::status_json(serve) ==
            "{\"mode\":\"serve\",\"port\":7892,\"bound\":true,"
            "\"bind_failed\":false,\"patch\":\"applied\","
            "\"protect_restart_pending\":true,"
            "\"hub\":{\"ready\":true,\"cameras\":[]}}",
        "status_json: serve");

  ep::StatusInfo failed;
  failed.mode = ep::Mode::kOff;
  failed.port = 7892;
  failed.bind_failed = true;
  failed.patch = "removed";
  check(ep::status_json(failed) ==
            "{\"mode\":\"off\",\"port\":7892,\"bound\":false,"
            "\"bind_failed\":true,\"patch\":\"removed\","
            "\"protect_restart_pending\":false,\"hub\":null}",
        "status_json: bind failed");

  ep::StatusInfo quoted;
  quoted.patch = "a\"b";
  check(ep::status_json(quoted).find("\"patch\":\"a\\\"b\"") !=
            std::string::npos,
        "status_json: patch name escaped");
}

int main() {
  test_normalize_mac();
  test_parse_mac_list();
  test_drain_format();
  test_serve();
  test_patching_off();
  test_off_without_state();
  test_active_to_drain();
  test_active_protect_not_running();
  test_drain_same_pid();
  test_drain_pid_mismatch();
  test_garbled_files();
  test_mode_names();
  test_files_on_disk();
  test_unmanaged_reasons();
  test_status_json();

  std::cout << "test_event_proxy_mode: " << g_pass << " passed, " << g_fail
            << " failed\n";
  return g_fail == 0 ? 0 : 1;
}
