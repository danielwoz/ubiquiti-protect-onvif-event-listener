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
 * pullpoint_demo.cpp
 *
 * A long-running recorder for manual checks of Protect event sharing and
 * for scripts/protect-e2e: camera emulators, the OnvifListener sharing
 * them, the PullPointHub and the PullPointServer, wired as src/main.cpp
 * wires them, without the database, detection or Protect patching.
 *
 * Cameras: --capture starts a SlotLimitedCameraEmulator (--slots, default
 * 1) replaying that JSONL capture; --second_capture adds a second camera
 * (--second_slots, default 64).  With --cameras=host:port,... no emulator
 * is started and the recorder subscribes to those cameras instead, so a
 * camera process (--no_recorder) can outlive recorder restarts.
 *
 * The emulated cameras also answer Protect's own requests, which carry no
 * SOAPAction header: they dispatch on the body, answer
 * GetEventProperties, give each subscription its own queue of motion
 * messages and log every request with its sender (the recorder sends a
 * SOAPAction, Protect does not).
 *
 * On start it prints one JSON line:
 *   {"event":"ready","port":<server port or 0>,"state_dir":"...",
 *    "cameras":[{"index":0,"address":"127.0.0.1:P","key":"127.0.0.1:P",
 *                "mac":"...","shared":true,"slots":1}, ...]}
 * then reads commands from stdin, one per line, each answered with one
 * JSON line on stdout:
 *   motion <i> true|false   queue a MotionAlarm change on every live
 *                           subscription of camera <i>
 *   stats                   per-camera subscription counters
 *   requests <i> [<from>]   camera <i>'s request log from entry <from>
 *   hub                     the hub's snapshot_json()
 *   quit                    same as SIGTERM
 * SIGTERM or SIGINT shuts down in main's order (listener, hub, server),
 * keeping shared subscriptions for the next run in --state_dir, and
 * prints {"event":"stopped","ms":<shutdown time>}.  Logs go to stderr.
 */

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "camera_emulators.hpp"
#include "onvif_listener.hpp"
#include "pullpoint_hub.hpp"
#include "pullpoint_server.hpp"
#include "upstream_event.hpp"

ABSL_FLAG(std::string, capture, "",
          "JSONL capture replayed by the first emulated camera (slot limited)");
ABSL_FLAG(int, slots, 1, "Event subscription slots of the first camera");
ABSL_FLAG(std::string, second_capture, "",
          "JSONL capture for an optional second emulated camera");
ABSL_FLAG(int, second_slots, 64, "Event subscription slots of the second camera");
ABSL_FLAG(std::string, cameras, "",
          "Comma-separated host:port of cameras already running; no emulator is "
          "started");
ABSL_FLAG(bool, no_recorder, false,
          "Run the emulated cameras only (no listener, hub or server)");
ABSL_FLAG(std::string, share, "all",
          "Cameras whose events are shared with Protect: all, none, or "
          "comma-separated camera indexes");
ABSL_FLAG(int, port, 7892, "PullPoint server port on 127.0.0.1 (0 picks one)");
ABSL_FLAG(std::string, state_dir, "",
          "Directory for the hub state and the kept camera subscriptions "
          "(default: a new temporary directory)");
ABSL_FLAG(bool, replay_pulls, true,
          "PullMessages replay the capture; false answers only with messages "
          "queued by the motion command");
ABSL_FLAG(int, pace_ms, 200, "Pause before the emulators answer a PullMessages");
ABSL_FLAG(int, renew_interval_sec, 90, "Seconds between the recorder's Renews");
ABSL_FLAG(bool, verbose, true, "Log INFO to stderr (else WARNING and above)");

namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;
using std::chrono::system_clock;

int64_t now_ms() {
  return std::chrono::duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

std::string json_str(const std::string& s) {
  std::string out = "\"";
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  return out + "\"";
}

std::mutex g_out_mu;

void emit(const std::string& line) {
  std::lock_guard<std::mutex> lk(g_out_mu);
  std::cout << line << std::endl;
}

std::string iso_utc(system_clock::time_point t) {
  const std::time_t tt = system_clock::to_time_t(t);
  std::tm tm{};
  gmtime_r(&tt, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  std::istringstream in(s);
  while (std::getline(in, cur, sep)) {
    if (!cur.empty()) out.push_back(cur);
  }
  return out;
}

// Local name of the first element inside the SOAP Body.
std::string body_operation(const std::string& body) {
  size_t p = 0;
  while ((p = body.find("Body", p)) != std::string::npos) {
    if (p > 0 && (body[p - 1] == '<' || body[p - 1] == ':')) break;
    ++p;
  }
  if (p == std::string::npos) return "";
  p = body.find('>', p);
  if (p == std::string::npos) return "";
  p = body.find('<', p);
  if (p == std::string::npos || p + 1 >= body.size() || body[p + 1] == '/') return "";
  size_t e = p + 1;
  while (e < body.size() && (std::isalnum(static_cast<unsigned char>(body[e])) ||
                             body[e] == ':' || body[e] == '_' || body[e] == '-' ||
                             body[e] == '.')) {
    ++e;
  }
  std::string name = body.substr(p + 1, e - p - 1);
  const size_t colon = name.rfind(':');
  return colon == std::string::npos ? name : name.substr(colon + 1);
}

std::string action_operation(const std::string& soap_action) {
  std::string tail = soap_action.substr(soap_action.rfind('/') + 1);
  const std::string suffix = "Request";
  if (tail.size() > suffix.size() &&
      tail.compare(tail.size() - suffix.size(), suffix.size(), suffix) == 0) {
    tail.resize(tail.size() - suffix.size());
  }
  return tail;
}

int element_int(const std::string& body, const std::string& name, int fallback) {
  const size_t p = body.find(name + ">");
  if (p == std::string::npos) return fallback;
  return std::atoi(body.c_str() + p + name.size() + 1);
}

std::string envelope(const std::string& body) {
  return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?><SOAP-ENV:Envelope"
         " xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\""
         " xmlns:wsa5=\"http://www.w3.org/2005/08/addressing\""
         " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
         " xmlns:wstop=\"http://docs.oasis-open.org/wsn/t-1\""
         " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
         " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
         " xmlns:tns1=\"http://www.onvif.org/ver10/topics\"><SOAP-ENV:Body>") +
         body + "</SOAP-ENV:Body></SOAP-ENV:Envelope>";
}

std::string event_properties_response() {
  return envelope(
      "<tev:GetEventPropertiesResponse>"
      "<tev:TopicNamespaceLocation>http://www.onvif.org/onvif/ver10/topics/topicns.xml"
      "</tev:TopicNamespaceLocation><wsnt:FixedTopicSet>true</wsnt:FixedTopicSet>"
      "<wstop:TopicSet><tns1:VideoSource><MotionAlarm wstop:topic=\"true\"/>"
      "</tns1:VideoSource><tns1:RuleEngine><CellMotionDetector><Motion wstop:topic=\"true\"/>"
      "</CellMotionDetector></tns1:RuleEngine></wstop:TopicSet>"
      "<wsnt:TopicExpressionDialect>http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet"
      "</wsnt:TopicExpressionDialect><tev:MessageContentFilterDialect>"
      "http://www.onvif.org/ver10/tev/messageContentFilter/ItemFilter"
      "</tev:MessageContentFilterDialect></tev:GetEventPropertiesResponse>");
}

std::string motion_message(bool on, const char* op) {
  return std::string(
             "<wsnt:NotificationMessage><wsnt:Topic Dialect=\"http://www.onvif.org/ver10/tev/"
             "topicExpression/ConcreteSet\">tns1:VideoSource/MotionAlarm</wsnt:Topic>"
             "<wsnt:Message><tt:Message UtcTime=\"") +
         iso_utc(system_clock::now()) + "\" PropertyOperation=\"" + op +
         "\"><tt:Source><tt:SimpleItem Name=\"Source\" Value=\"VideoSource_1\"/></tt:Source>"
         "<tt:Data><tt:SimpleItem Name=\"State\" Value=\"" + (on ? "true" : "false") +
         "\"/></tt:Data></tt:Message></wsnt:Message></wsnt:NotificationMessage>";
}

std::string pull_response(const std::vector<std::string>& messages) {
  const auto now = system_clock::now();
  std::string body = "<tev:PullMessagesResponse><tev:CurrentTime>" + iso_utc(now) +
                     "</tev:CurrentTime><tev:TerminationTime>" +
                     iso_utc(now + std::chrono::seconds(60)) + "</tev:TerminationTime>";
  for (const auto& m : messages) body += m;
  return envelope(body + "</tev:PullMessagesResponse>");
}

// ---------------------------------------------------------------------
// Emulated camera
// ---------------------------------------------------------------------

class DemoCamera : public SlotLimitedCameraEmulator {
 public:
  DemoCamera(const std::string& jsonl, int slots, bool replay_pulls, int pace_ms)
      : SlotLimitedCameraEmulator(jsonl, slots), replay_pulls_(replay_pulls),
        pace_ms_(pace_ms) {}

  // Queue a MotionAlarm change on every live subscription.  Returns how
  // many subscriptions it was queued for.
  int inject_motion(bool on) {
    std::lock_guard<std::mutex> lk(demo_mu_);
    motion_ = on;
    const std::string msg = motion_message(on, "Changed");
    for (auto& kv : subs_) kv.second.push_back(msg);
    return static_cast<int>(subs_.size());
  }

  size_t request_count() const {
    std::lock_guard<std::mutex> lk(demo_mu_);
    return log_.size();
  }

  std::string requests_json(size_t from) const {
    std::lock_guard<std::mutex> lk(demo_mu_);
    std::string out = "[";
    for (size_t i = from; i < log_.size(); ++i) {
      const Request& r = log_[i];
      if (i > from) out += ',';
      out += "{\"i\":" + std::to_string(i) + ",\"t\":" + std::to_string(r.t_ms) +
             ",\"op\":" + json_str(r.op) + ",\"path\":" + json_str(r.path) +
             ",\"from\":\"" + (r.from_recorder ? "recorder" : "protect") +
             "\",\"status\":" + std::to_string(r.status) + '}';
    }
    return out + "]";
  }

  std::string counts_json() const {
    std::map<std::string, int> by;  // "<from>.<op>" -> count
    {
      std::lock_guard<std::mutex> lk(demo_mu_);
      for (const auto& r : log_) ++by[(r.from_recorder ? "recorder." : "protect.") + r.op];
    }
    std::string out = "{";
    bool first = true;
    for (const auto& kv : by) {
      if (!first) out += ',';
      first = false;
      out += json_str(kv.first) + ":" + std::to_string(kv.second);
    }
    return out + "}";
  }

 protected:
  std::pair<int, std::string> handle(const std::string& path, const std::string& soap_action,
                                     const std::string& body) override {
    const bool from_recorder = !soap_action.empty();
    const std::string op =
        from_recorder ? action_operation(soap_action) : body_operation(body);
    if (op == "PullMessages" && pace_ms_ > 0) {
      std::this_thread::sleep_for(milliseconds(pace_ms_));
    }
    std::pair<int, std::string> r;
    if (op == "GetEventProperties") {
      r = {200, event_properties_response()};
    } else {
      const std::string action =
          from_recorder ? soap_action
                        : "http://www.onvif.org/ver10/events/wsdl/EventPortType/" + op +
                              "Request";
      r = SlotLimitedCameraEmulator::handle(path, action, body);
      if (r.first == 400 && r.second.empty()) {
        r = {200, envelope("<tev:" + op + "Response/>")};
      }
    }
    std::lock_guard<std::mutex> lk(demo_mu_);
    if (op == "CreatePullPointSubscription" && r.first == 200) {
      const size_t a = r.second.find("Address>");
      const size_t e = a == std::string::npos ? a : r.second.find('<', a);
      if (e != std::string::npos) {
        std::string url = r.second.substr(a + 8, e - a - 8);
        const size_t scheme = url.find("://");
        const size_t slash =
            url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
        subs_[slash == std::string::npos ? "/" : url.substr(slash)];
      }
    } else if (op == "Unsubscribe" && r.first == 200) {
      subs_.erase(path);
    } else if (op == "SetSynchronizationPoint" && r.first == 200) {
      // The camera sends its current state again, as Initialized.
      subs_[path].push_back(motion_message(motion_, "Initialized"));
    } else if (op == "PullMessages") {
      auto it = subs_.find(path);
      if (r.first != 200) {
        if (it != subs_.end()) subs_.erase(it);
      } else if (it != subs_.end() && !it->second.empty()) {
        const size_t limit =
            static_cast<size_t>(std::max(1, element_int(body, "MessageLimit", 10)));
        std::vector<std::string> msgs;
        while (!it->second.empty() && msgs.size() < limit) {
          msgs.push_back(it->second.front());
          it->second.pop_front();
        }
        r.second = pull_response(msgs);
      } else if (!replay_pulls_) {
        r.second = pull_response({});
      }
    }
    log_.push_back(Request{now_ms(), op, path, from_recorder, r.first});
    return r;
  }

 private:
  struct Request {
    int64_t t_ms;
    std::string op;
    std::string path;
    bool from_recorder;
    int status;
  };

  const bool replay_pulls_;
  const int pace_ms_;
  mutable std::mutex demo_mu_;
  std::map<std::string, std::deque<std::string>> subs_;  // path -> queued messages
  bool motion_ = false;
  std::vector<Request> log_;
};

// ---------------------------------------------------------------------
// Signals
// ---------------------------------------------------------------------

int g_signal_pipe[2] = {-1, -1};

void on_signal(int /*sig*/) {
  const char c = 'x';
  const ssize_t n = write(g_signal_pipe[1], &c, 1);
  (void)n;
}

struct CameraInfo {
  std::string address;
  std::string mac;
  bool shared = false;
  int slots = 0;               // 0: not emulated here
  DemoCamera* emu = nullptr;   // null for --cameras
};

std::string camera_mac(size_t index) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "F00000FDAE%02X", static_cast<unsigned>(index & 0xff));
  return buf;
}

}  // namespace

int main(int argc, char* argv[]) {
  absl::SetProgramUsageMessage(
      "Recorder with emulated cameras for trying Protect event sharing; see "
      "scripts/protect-e2e/README.md.\n  pullpoint_demo --capture=<capture.jsonl> [options]");
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::GetFlag(FLAGS_verbose) ? absl::LogSeverityAtLeast::kInfo
                                                        : absl::LogSeverityAtLeast::kWarning);
  absl::SetMinLogLevel(absl::GetFlag(FLAGS_verbose) ? absl::LogSeverityAtLeast::kInfo
                                                    : absl::LogSeverityAtLeast::kWarning);

  const bool no_recorder = absl::GetFlag(FLAGS_no_recorder);
  const std::string external = absl::GetFlag(FLAGS_cameras);
  if (external.empty() && absl::GetFlag(FLAGS_capture).empty()) {
    std::cerr << "pullpoint_demo: give --capture=<capture.jsonl> or --cameras=host:port,...\n";
    return 2;
  }
  if (!external.empty() && no_recorder) {
    std::cerr << "pullpoint_demo: --cameras and --no_recorder leave nothing to run\n";
    return 2;
  }

  std::string state_dir = absl::GetFlag(FLAGS_state_dir);
  if (state_dir.empty()) {
    char tmpl[] = "/tmp/pullpoint_demo.XXXXXX";
    const char* made = mkdtemp(tmpl);
    if (made == nullptr) {
      std::cerr << "pullpoint_demo: mkdtemp: " << std::strerror(errno) << '\n';
      return 1;
    }
    state_dir = made;
  } else {
    mkdir(state_dir.c_str(), 0700);
  }

  if (pipe(g_signal_pipe) != 0) return 1;
  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  onvif::global_init();

  // ---- Cameras ----
  std::vector<std::unique_ptr<DemoCamera>> emulators;
  std::vector<CameraInfo> cams;
  if (external.empty()) {
    const bool replay = absl::GetFlag(FLAGS_replay_pulls);
    const int pace = absl::GetFlag(FLAGS_pace_ms);
    auto add = [&](const std::string& capture, int slots) {
      emulators.push_back(std::make_unique<DemoCamera>(capture, slots, replay, pace));
      emulators.back()->start();
      CameraInfo ci;
      ci.address = emulators.back()->local_address();
      ci.slots = slots;
      ci.emu = emulators.back().get();
      cams.push_back(ci);
    };
    add(absl::GetFlag(FLAGS_capture), absl::GetFlag(FLAGS_slots));
    if (!absl::GetFlag(FLAGS_second_capture).empty()) {
      add(absl::GetFlag(FLAGS_second_capture), absl::GetFlag(FLAGS_second_slots));
    }
  } else {
    for (const auto& a : split(external, ',')) {
      CameraInfo ci;
      ci.address = a;
      cams.push_back(ci);
    }
  }
  const std::string share = absl::GetFlag(FLAGS_share);
  std::set<std::string> shared_idx;
  for (const auto& s : split(share, ',')) shared_idx.insert(s);
  for (size_t i = 0; i < cams.size(); ++i) {
    cams[i].mac = camera_mac(i);
    cams[i].shared = !no_recorder && (share == "all" || shared_idx.count(std::to_string(i)) > 0);
  }

  // ---- Recorder: server and hub first, then the cameras, then the
  // listener, as src/main.cpp does. ----
  std::shared_ptr<onvif::pullpoint::PullPointHub> hub;
  std::unique_ptr<onvif::pullpoint::PullPointServer> server;
  std::unique_ptr<onvif::OnvifListener> listener;
  std::thread listener_thread;
  if (!no_recorder) {
    onvif::pullpoint::PullPointHub::Options opts;
    opts.state_path = state_dir + "/event-proxy-state.json";
    hub = std::make_shared<onvif::pullpoint::PullPointHub>(opts);
    server = std::make_unique<onvif::pullpoint::PullPointServer>(hub.get());
    if (!server->start(static_cast<uint16_t>(absl::GetFlag(FLAGS_port)), 10)) {
      emit("{\"event\":\"error\",\"error\":\"cannot listen on port " +
           std::to_string(absl::GetFlag(FLAGS_port)) + "\"}");
      return 1;
    }
    hub->start();

    listener = std::make_unique<onvif::OnvifListener>();
    for (const auto& ci : cams) {
      onvif::CameraConfig cfg;
      cfg.ip = ci.address;
      cfg.mac = ci.mac;
      cfg.user = "admin";
      cfg.password = "password";
      cfg.retry_interval_sec = 2;
      cfg.renew_interval_sec = absl::GetFlag(FLAGS_renew_interval_sec);
      cfg.event_proxy = ci.shared;
      hub->register_camera({cfg.ip, cfg.mac, cfg.event_proxy});
      listener->add_camera(cfg);
    }
    hub->set_ready();
    listener->set_raw_response_tap(
        [hub](const onvif::CameraConfig& c, const std::string& xml) {
          hub->on_upstream_response(c.ip, xml);
        });
    listener->set_upstream_state_tap(
        [hub](const onvif::CameraConfig& c, onvif::UpstreamEvent ev, int status,
              const std::string& detail) {
          hub->on_upstream_event(c.ip, ev, status, detail);
        });
    listener->set_heartbeat_suffix_fn(
        [hub](const onvif::CameraConfig& c) { return hub->heartbeat_suffix(c.ip); });
    listener->set_resume_path(state_dir + "/upstream-subscriptions.json");
    onvif::OnvifListener* l = listener.get();
    hub->set_nudge_fn([l](const std::string& ip) { l->nudge(ip); });
    hub->set_resubscribe_fn([l](const std::string& ip) { l->resubscribe(ip); });
    listener_thread = std::thread([l] { l->run([](const onvif::OnvifEvent&) {}); });
  }

  {
    std::string line = "{\"event\":\"ready\",\"pid\":" + std::to_string(getpid()) +
                       ",\"port\":" + std::to_string(server ? server->port() : 0) +
                       ",\"state_dir\":" + json_str(state_dir) + ",\"cameras\":[";
    for (size_t i = 0; i < cams.size(); ++i) {
      if (i) line += ',';
      line += "{\"index\":" + std::to_string(i) + ",\"address\":" + json_str(cams[i].address) +
              ",\"key\":" + json_str(onvif::pullpoint::camera_key_from_ip(cams[i].address)) +
              ",\"mac\":" + json_str(cams[i].mac) +
              ",\"shared\":" + (cams[i].shared ? "true" : "false") +
              ",\"slots\":" + std::to_string(cams[i].slots) + '}';
    }
    emit(line + "]}");
  }

  // ---- Commands until a signal ----
  auto camera_at = [&](const std::string& s) -> DemoCamera* {
    const size_t i = static_cast<size_t>(std::atoi(s.c_str()));
    return i < cams.size() ? cams[i].emu : nullptr;
  };
  auto command = [&](const std::string& line) -> bool {
    const auto w = split(line, ' ');
    if (w.empty()) return true;
    if (w[0] == "quit") return false;
    if (w[0] == "motion" && w.size() == 3) {
      DemoCamera* emu = camera_at(w[1]);
      if (emu == nullptr) {
        emit("{\"cmd\":\"motion\",\"error\":\"no such emulated camera\"}");
        return true;
      }
      const bool on = w[2] == "true" || w[2] == "1";
      const int n = emu->inject_motion(on);
      emit("{\"cmd\":\"motion\",\"camera\":" + w[1] + ",\"state\":" + (on ? "true" : "false") +
           ",\"t\":" + std::to_string(now_ms()) + ",\"queued\":" + std::to_string(n) + "}");
    } else if (w[0] == "stats") {
      std::string out = "{\"cmd\":\"stats\",\"t\":" + std::to_string(now_ms()) +
                        ",\"cameras\":[";
      for (size_t i = 0; i < cams.size(); ++i) {
        if (i) out += ',';
        DemoCamera* e = cams[i].emu;
        out += "{\"index\":" + std::to_string(i);
        if (e != nullptr) {
          out += ",\"live\":" + std::to_string(e->live_subscriptions()) +
                 ",\"creates\":" + std::to_string(e->creates_total()) +
                 ",\"refused\":" + std::to_string(e->creates_refused()) +
                 ",\"unsubscribes\":" + std::to_string(e->unsubscribes_total()) +
                 ",\"renews\":" + std::to_string(e->renews_total()) +
                 ",\"pulls\":" + std::to_string(e->pulls_total()) +
                 ",\"sync_points\":" + std::to_string(e->sync_points_total()) +
                 ",\"requests\":" + std::to_string(e->request_count()) +
                 ",\"by\":" + e->counts_json();
        }
        out += '}';
      }
      emit(out + "]}");
    } else if (w[0] == "requests" && w.size() >= 2) {
      DemoCamera* emu = camera_at(w[1]);
      const size_t from = w.size() >= 3 ? static_cast<size_t>(std::atol(w[2].c_str())) : 0;
      emit("{\"cmd\":\"requests\",\"camera\":" + w[1] + ",\"requests\":" +
           (emu != nullptr ? emu->requests_json(from) : std::string("[]")) + "}");
    } else if (w[0] == "hub") {
      emit("{\"cmd\":\"hub\",\"hub\":" + (hub ? hub->snapshot_json() : std::string("null")) +
           "}");
    } else {
      emit("{\"cmd\":" + json_str(w[0]) + ",\"error\":\"unknown command\"}");
    }
    return true;
  };

  std::string pending;
  bool stdin_open = true;
  bool running = true;
  while (running) {
    struct pollfd fds[2] = {{g_signal_pipe[0], POLLIN, 0}, {0, POLLIN, 0}};
    const int n = poll(fds, stdin_open ? 2 : 1, 1000);
    if (n < 0 && errno != EINTR) break;
    if (n <= 0) continue;
    if (fds[0].revents & POLLIN) break;
    if (stdin_open && (fds[1].revents & (POLLIN | POLLHUP))) {
      char buf[4096];
      const ssize_t got = read(0, buf, sizeof(buf));
      if (got <= 0) {
        stdin_open = false;
        continue;
      }
      pending.append(buf, static_cast<size_t>(got));
      size_t nl;
      while (running && (nl = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, nl);
        pending.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        running = command(line);
      }
    }
  }

  // ---- Shutdown: listener, hub, server (src/main.cpp's order). ----
  const auto t0 = steady_clock::now();
  if (listener) {
    listener->stop();
    if (listener_thread.joinable()) listener_thread.join();
  }
  if (hub) hub->stop();
  if (server) server->stop();
  const auto ms =
      std::chrono::duration_cast<milliseconds>(steady_clock::now() - t0).count();
  server.reset();
  listener.reset();
  hub.reset();
  for (auto& e : emulators) e->stop();
  onvif::global_cleanup();
  emit("{\"event\":\"stopped\",\"ms\":" + std::to_string(ms) + "}");
  return 0;
}
