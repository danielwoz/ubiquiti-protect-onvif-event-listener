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
 * test_pullpoint_e2e.cpp
 *
 * Protect event sharing end to end, in one process: a camera with one
 * event-subscription slot (SlotLimitedCameraEmulator replaying the Dahua
 * PTZ capture), the OnvifListener sharing that camera, the PullPointHub and
 * the PullPointServer, wired the way src/main.cpp wires them, and libcurl
 * clients that talk to the server the way Protect does (Create PT60S,
 * PullMessages PT2S / limit 10 in a loop, Renew, Unsubscribe, no
 * SOAPAction header).
 *
 * Checks:
 *  - the camera never has more than one live subscription and never
 *    refuses a Create;
 *  - every client receives the same fragments in the same order, and the
 *    same ones the recorder took from the camera;
 *  - a client created late first receives the current state as
 *    Initialized messages;
 *  - a pull point faults once it has been unsubscribed;
 *  - the recorder asks the camera for its current state
 *    (SetSynchronizationPoint) once after creating its subscription and
 *    once after resuming it;
 *  - a recorder restart (listener, hub and server stopped and started
 *    again on the same port, resume and state paths) keeps the clients'
 *    pull points working (adopted) with no new Create at the camera and at
 *    most one transport error per client.
 *
 * Usage: test_pullpoint_e2e <capture.jsonl>...  (the Dahua PTZ capture,
 * dahua_dh_sd4a425db_hny_*.jsonl, must be among them)
 */

#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "camera_emulators.hpp"
#include "notification_fragments.hpp"
#include "onvif_listener.hpp"
#include "pullpoint_hub.hpp"
#include "pullpoint_server.hpp"
#include "upstream_event.hpp"

using onvif::pullpoint::HubCamera;
using onvif::pullpoint::PullPointHub;
using onvif::pullpoint::PullPointServer;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::steady_clock;

namespace {

std::atomic<int> g_pass{0};
std::atomic<int> g_fail{0};

void check(bool cond, const std::string& label) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << label << '\n';
  }
}

const char kMac[] = "F00000FDAED9";

bool wait_for(const std::function<bool()>& cond, milliseconds limit) {
  const auto deadline = steady_clock::now() + limit;
  while (steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(milliseconds(50));
  }
  return cond();
}

int count_of(const std::string& s, const std::string& needle) {
  int n = 0;
  for (size_t p = s.find(needle); p != std::string::npos; p = s.find(needle, p + 1)) ++n;
  return n;
}

std::string scratch_dir() {
  const char* base = std::getenv("TEST_TMPDIR");
  std::string dir = std::string(base != nullptr ? base : "/tmp") + "/pullpoint_e2e." +
                    std::to_string(getpid());
  mkdir(dir.c_str(), 0700);
  return dir;
}

// What a fragment is, independent of how it was serialised: topic,
// property operation and property key.
std::string identity(const onvif::pullpoint::Fragment& f) {
  return f.topic + "\x1f" + f.op + "\x1f" + f.key;
}

// ---------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------

struct HttpReply {
  bool transport_error = false;
  long status = 0;  // NOLINT(runtime/int)
  std::string body;
  std::string error;
};

size_t append_body(char* data, size_t size, size_t n, void* out) {
  static_cast<std::string*>(out)->append(data, size * n);
  return size * n;
}

// POST as Protect does: SOAP 1.2 content type, no SOAPAction header.
HttpReply post(const std::string& url, const std::string& body) {
  HttpReply r;
  CURL* c = curl_easy_init();
  if (c == nullptr) {
    r.transport_error = true;
    r.error = "curl_easy_init";
    return r;
  }
  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/soap+xml; charset=utf-8");
  hdrs = curl_slist_append(hdrs, "Expect:");
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));  // NOLINT
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, append_body);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
  curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 10000L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  const CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK) {
    r.transport_error = true;
    r.error = curl_easy_strerror(rc);
  } else {
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
  }
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);
  return r;
}

// Envelope as Protect builds it, with its WS-Security header and the
// WS-Addressing headers its patched client adds, all mustUnderstand.
std::string protect_envelope(const std::string& action, const std::string& to,
                             const std::string& body) {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
         "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Header>"
         "<Security xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-"
         "wssecurity-secext-1.0.xsd\" mustUnderstand=\"1\"><UsernameToken>"
         "<Username>admin</Username><Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
         "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">c2VjcmV0</Password>"
         "<Nonce>bm9uY2U=</Nonce><Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/"
         "oasis-200401-wss-wssecurity-utility-1.0.xsd\">2026-10-21T20:00:00.000Z</Created>"
         "</UsernameToken></Security>"
         "<a:Action xmlns:a=\"http://www.w3.org/2005/08/addressing\" s:mustUnderstand=\"1\">" +
         action + "</a:Action><a:To xmlns:a=\"http://www.w3.org/2005/08/addressing\" "
         "s:mustUnderstand=\"1\">" + to + "</a:To></s:Header><s:Body>" + body +
         "</s:Body></s:Envelope>";
}

const char kEventsNs[] = "http://www.onvif.org/ver10/events/wsdl";
const char kWsnNs[] = "http://docs.oasis-open.org/wsn/b-2";

// ---------------------------------------------------------------------
// A client that subscribes through the recorder the way Protect does.
// ---------------------------------------------------------------------

class ProtectClient {
 public:
  ProtectClient(std::string name, uint16_t server_port, uint16_t camera_port)
      : name_(std::move(name)),
        origin_("http://127.0.0.1:" + std::to_string(server_port)),
        camera_base_("/onvif-recorder/pullpoint/127.0.0.1/" + std::to_string(camera_port)) {}
  ~ProtectClient() { stop_loop(); }

  const std::string& name() const { return name_; }

  // CreatePullPointSubscription; true when the recorder accepted it.
  bool create() {
    const std::string url = origin_ + camera_base_ + "/onvif/event_service";
    const HttpReply r = post(url, protect_envelope(
        std::string(kEventsNs) + "/EventPortType/CreatePullPointSubscriptionRequest", url,
        std::string("<CreatePullPointSubscription xmlns=\"") + kEventsNs +
            "\"><InitialTerminationTime>PT60S</InitialTerminationTime>"
            "</CreatePullPointSubscription>"));
    if (r.transport_error || r.status != 200) {
      std::cerr << name_ << ": Create failed: " << r.status << ' ' << r.error << ' '
                << r.body.substr(0, 300) << '\n';
      return false;
    }
    const std::string open = "Address>";
    const size_t a = r.body.find(open);
    const size_t e = a == std::string::npos ? a : r.body.find('<', a + open.size());
    if (e == std::string::npos) return false;
    const std::string address = r.body.substr(a + open.size(), e - a - open.size());
    if (address.compare(0, origin_.size(), origin_) != 0) return false;
    std::lock_guard<std::mutex> lk(mu_);
    path_ = address.substr(origin_.size());
    return true;
  }

  std::string path() const {
    std::lock_guard<std::mutex> lk(mu_);
    return path_;
  }

  HttpReply pull_once() {
    const std::string url = origin_ + path();
    HttpReply r = post(url, protect_envelope(
        std::string(kEventsNs) + "/PullPointSubscription/PullMessagesRequest", url,
        std::string("<PullMessages xmlns=\"") + kEventsNs +
            "\"><Timeout>PT2S</Timeout><MessageLimit>10</MessageLimit></PullMessages>"));
    std::lock_guard<std::mutex> lk(mu_);
    if (r.transport_error) {
      ++transport_errors_;
    } else if (r.status != 200) {
      ++faults_;
      last_fault_ = r.body;
    } else {
      ++pulls_ok_;
      const auto ex = onvif::pullpoint::extract_fragments(r.body);
      if (!ex.parsed) ++unparsed_;
      for (const auto& f : ex.fragments) {
        xml_.push_back(f.xml);
        ids_.push_back(identity(f));
        ops_.push_back(f.op);
      }
      if (first_pull_ops_.empty() && pulls_ok_ == 1) {
        for (const auto& f : ex.fragments) first_pull_ops_.push_back(f.op + "|" + f.key);
      }
    }
    return r;
  }

  HttpReply renew() {
    const std::string url = origin_ + path();
    HttpReply r = post(url, protect_envelope(
        std::string(kWsnNs) + "/SubscriptionManager/RenewRequest", url,
        std::string("<Renew xmlns=\"") + kWsnNs +
            "\"><TerminationTime>PT60S</TerminationTime></Renew>"));
    std::lock_guard<std::mutex> lk(mu_);
    if (r.transport_error) {
      ++transport_errors_;
    } else if (r.status != 200) {
      ++faults_;
      last_fault_ = r.body;
    } else {
      ++renews_ok_;
    }
    return r;
  }

  HttpReply unsubscribe() {
    const std::string url = origin_ + path();
    return post(url, protect_envelope(
        std::string(kWsnNs) + "/SubscriptionManager/UnsubscribeRequest", url,
        std::string("<Unsubscribe xmlns=\"") + kWsnNs + "\"/>"));
  }

  // Pull in a loop, renewing every 3 s, until stop_loop().  A transport
  // error, on a pull or a renew, waits 2 s before the next request, as
  // Protect's next poll would.
  void start_loop() {
    stop_ = false;
    thread_ = std::thread([this] {
      auto last_renew = steady_clock::now();
      while (!stop_) {
        const HttpReply r = pull_once();
        if (stop_) break;
        if (r.transport_error) {
          sleep_interruptible(milliseconds(2000));
          continue;
        }
        if (r.status != 200) {
          sleep_interruptible(milliseconds(500));
          continue;
        }
        if (steady_clock::now() - last_renew >= seconds(3)) {
          const bool lost = renew().transport_error;
          last_renew = steady_clock::now();
          if (lost) {
            sleep_interruptible(milliseconds(2000));
            continue;
          }
        }
        sleep_interruptible(milliseconds(50));
      }
    });
  }

  void stop_loop() {
    {
      std::lock_guard<std::mutex> lk(stop_mu_);
      stop_ = true;
    }
    stop_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  // Position in the received sequence, for comparing later parts of it.
  size_t received() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ids_.size();
  }
  std::vector<std::string> xml_from(size_t from) const {
    std::lock_guard<std::mutex> lk(mu_);
    return std::vector<std::string>(xml_.begin() + std::min(from, xml_.size()), xml_.end());
  }
  std::vector<std::string> ids_from(size_t from) const {
    std::lock_guard<std::mutex> lk(mu_);
    return std::vector<std::string>(ids_.begin() + std::min(from, ids_.size()), ids_.end());
  }
  std::vector<std::string> first_pull_ops() const {
    std::lock_guard<std::mutex> lk(mu_);
    return first_pull_ops_;
  }
  int initialized_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(std::count(ops_.begin(), ops_.end(), "Initialized"));
  }
  int transport_errors() const {
    std::lock_guard<std::mutex> lk(mu_);
    return transport_errors_;
  }
  int faults() const {
    std::lock_guard<std::mutex> lk(mu_);
    return faults_;
  }
  int pulls_ok() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pulls_ok_;
  }
  int renews_ok() const {
    std::lock_guard<std::mutex> lk(mu_);
    return renews_ok_;
  }
  int unparsed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return unparsed_;
  }
  std::string last_fault() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_fault_;
  }

 private:
  void sleep_interruptible(milliseconds d) {
    std::unique_lock<std::mutex> lk(stop_mu_);
    stop_cv_.wait_for(lk, d, [this] { return stop_.load(); });
  }

  const std::string name_;
  const std::string origin_;
  const std::string camera_base_;

  mutable std::mutex mu_;
  std::string path_;
  std::vector<std::string> xml_;
  std::vector<std::string> ids_;
  std::vector<std::string> ops_;
  std::vector<std::string> first_pull_ops_;
  int transport_errors_ = 0;
  int faults_ = 0;
  int pulls_ok_ = 0;
  int renews_ok_ = 0;
  int unparsed_ = 0;
  std::string last_fault_;

  std::atomic<bool> stop_{false};
  std::mutex stop_mu_;
  std::condition_variable stop_cv_;
  std::thread thread_;
};

// ---------------------------------------------------------------------
// The recorder side, wired as src/main.cpp wires it.
// ---------------------------------------------------------------------

class Recorder {
 public:
  Recorder(onvif::CameraConfig cfg, std::string state_path, std::string resume_path)
      : cfg_(std::move(cfg)),
        state_path_(std::move(state_path)),
        resume_path_(std::move(resume_path)) {}
  ~Recorder() { stop(); }

  // The PullPoint service: hub and server, then the camera list.
  bool start_service(uint16_t port) {
    PullPointHub::Options opts;
    opts.state_path = state_path_;
    opts.max_pull_points_total = onvif::pullpoint::max_pull_points_total_for(1);
    hub_ = std::make_shared<PullPointHub>(opts);
    server_ = std::make_unique<PullPointServer>(hub_.get());
    if (!server_->start(port, 10)) return false;
    hub_->start();
    hub_->register_camera(HubCamera{cfg_.ip, cfg_.mac, cfg_.event_proxy});
    hub_->set_ready();
    return true;
  }

  // The camera subscription, feeding the hub.
  void start_listener() {
    listener_ = std::make_unique<onvif::OnvifListener>();
    listener_->add_camera(cfg_);
    std::shared_ptr<PullPointHub> hub = hub_;
    listener_->set_raw_response_tap(
        [this, hub](const onvif::CameraConfig& c, const std::string& xml) {
          const auto ex = onvif::pullpoint::extract_fragments(xml);
          {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto& f : ex.fragments) upstream_.push_back(identity(f));
          }
          hub->on_upstream_response(c.ip, xml);
        });
    listener_->set_upstream_state_tap(
        [this, hub](const onvif::CameraConfig& c, onvif::UpstreamEvent ev, int status,
                    const std::string& detail) {
          {
            std::lock_guard<std::mutex> lk(mu_);
            states_.push_back(ev);
          }
          hub->on_upstream_event(c.ip, ev, status, detail);
        });
    listener_->set_heartbeat_suffix_fn(
        [hub](const onvif::CameraConfig& c) { return hub->heartbeat_suffix(c.ip); });
    listener_->set_resume_path(resume_path_);
    onvif::OnvifListener* l = listener_.get();
    hub_->set_nudge_fn([l](const std::string& ip) { l->nudge(ip); });
    hub_->set_resubscribe_fn([l](const std::string& ip) { l->resubscribe(ip); });
    run_thread_ = std::thread([this] {
      listener_->run([this](const onvif::OnvifEvent&) { ++events_; });
    });
  }

  // Shutdown in main's order: the listener (which keeps the shared
  // subscription for the next run), then the hub, then the server.
  void stop_listener() {
    if (!listener_) return;
    listener_->stop();
    if (run_thread_.joinable()) run_thread_.join();
  }

  void stop() {
    stop_listener();
    if (hub_) hub_->stop();
    if (server_) server_->stop();
    server_.reset();
    listener_.reset();
    hub_.reset();
  }

  uint16_t port() const { return server_ ? server_->port() : 0; }
  PullPointHub* hub() const { return hub_.get(); }
  std::string snapshot() const { return hub_ ? hub_->snapshot_json() : std::string(); }

  std::vector<std::string> upstream() const {
    std::lock_guard<std::mutex> lk(mu_);
    return upstream_;
  }
  int state_count(onvif::UpstreamEvent ev) const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(std::count(states_.begin(), states_.end(), ev));
  }
  int events() const { return events_.load(); }

 private:
  const onvif::CameraConfig cfg_;
  const std::string state_path_;
  const std::string resume_path_;
  std::shared_ptr<PullPointHub> hub_;
  std::unique_ptr<PullPointServer> server_;
  std::unique_ptr<onvif::OnvifListener> listener_;
  std::thread run_thread_;
  std::atomic<int> events_{0};

  mutable std::mutex mu_;
  std::vector<std::string> upstream_;
  std::vector<onvif::UpstreamEvent> states_;
};

// The slot-limited camera, answering each PullMessages after 200 ms the
// way a camera holds a pull until it has something to say.  Every pull of
// the capture carries messages, so without the pause the replay would
// produce events far faster than Protect's 10-per-pull client reads them.
class PacedCamera : public SlotLimitedCameraEmulator {
 public:
  PacedCamera(const std::string& jsonl_path, int slots)
      : SlotLimitedCameraEmulator(jsonl_path, slots) {}

 protected:
  std::pair<int, std::string> handle(const std::string& path, const std::string& soap_action,
                                     const std::string& body) override {
    if (soap_action.find("PullMessages") != std::string::npos) {
      std::this_thread::sleep_for(milliseconds(200));
    }
    return SlotLimitedCameraEmulator::handle(path, soap_action, body);
  }
};

// Samples the camera's live subscriptions until stopped.
class SlotMonitor {
 public:
  explicit SlotMonitor(const SlotLimitedCameraEmulator* emu) : emu_(emu) {
    thread_ = std::thread([this] {
      while (!stop_) {
        const int n = emu_->live_subscriptions();
        int seen = max_.load();
        while (n > seen && !max_.compare_exchange_weak(seen, n)) {
        }
        std::this_thread::sleep_for(milliseconds(5));
      }
    });
  }
  ~SlotMonitor() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }
  int max() const { return max_.load(); }

 private:
  const SlotLimitedCameraEmulator* emu_;
  std::atomic<bool> stop_{false};
  std::atomic<int> max_{0};
  std::thread thread_;
};

// The hub's cached property count for the only camera in @p snapshot.
int cache_entries(const std::string& snapshot) {
  const std::string tag = "\"cache_entries\":";
  const size_t p = snapshot.find(tag);
  return p == std::string::npos ? -1 : std::atoi(snapshot.c_str() + p + tag.size());
}

bool all_drained(const std::string& snapshot) {
  return count_of(snapshot, "\"queued\":0") == count_of(snapshot, "\"queued\":");
}

bool none_dropped(const std::string& snapshot) {
  return count_of(snapshot, "\"dropped\":0") == count_of(snapshot, "\"dropped\":");
}

std::string join_head(const std::vector<std::string>& v, size_t n) {
  std::string out;
  for (size_t i = 0; i < std::min(n, v.size()); ++i) {
    if (i) out += ", ";
    std::string s = v[i];
    std::replace(s.begin(), s.end(), '\x1f', '|');
    out += s;
  }
  return out;
}

void test_shared_camera(const std::string& capture) {
  const std::string dir = scratch_dir();
  const std::string state_path = dir + "/event-proxy-state.json";
  const std::string resume_path = dir + "/upstream-subscriptions.json";
  std::remove(state_path.c_str());
  std::remove(resume_path.c_str());

  PacedCamera emu(capture, 1);
  emu.start();
  SlotMonitor slots(&emu);

  onvif::CameraConfig cfg;
  cfg.ip = emu.local_address();
  cfg.mac = kMac;
  cfg.user = "admin";
  cfg.password = "password";
  cfg.retry_interval_sec = 1;
  cfg.renew_interval_sec = 5;
  cfg.event_proxy = true;

  // ---- Phase 1: three clients subscribe before the camera subscription
  // exists, so all of them see every fragment the recorder takes in. ----
  auto rec = std::make_unique<Recorder>(cfg, state_path, resume_path);
  check(rec->start_service(0), "the PullPoint server starts on a free port");
  const uint16_t port = rec->port();
  check(port != 0, "the server reports its port");

  ProtectClient a("A", port, emu.port());
  ProtectClient b("B", port, emu.port());
  ProtectClient c("C", port, emu.port());
  check(a.create() && b.create() && c.create(),
        "Creates are accepted while the camera subscription is starting");
  check(a.path() != b.path() && b.path() != c.path() && a.path() != c.path(),
        "each Create gets its own pull point");
  a.start_loop();
  b.start_loop();
  c.start_loop();

  rec->start_listener();
  check(wait_for([&] { return rec->state_count(onvif::UpstreamEvent::kSubscribed) >= 1; },
                 seconds(15)),
        "the recorder subscribes to the camera");
  check(wait_for([&] { return rec->upstream().size() >= 8; }, seconds(60)),
        "the camera delivers at least 8 messages to the recorder");
  // Let the clients pull what has been published so far, and run long
  // enough for the clients' and the recorder's Renews.
  wait_for([&] { return a.received() >= 8 && b.received() >= 8 && c.received() >= 8; },
           seconds(10));
  check(wait_for([&] { return a.renews_ok() >= 1 && b.renews_ok() >= 1 &&
                              c.renews_ok() >= 1 && emu.renews_total() >= 1; },
                 seconds(15)),
        "the clients renew through the recorder and the recorder renews at the camera");

  // ---- A client created now gets the current state first. ----
  const int cache_before = cache_entries(rec->snapshot());
  ProtectClient d("D", port, emu.port());
  check(d.create(), "a late Create is accepted");
  const int cache_after = cache_entries(rec->snapshot());
  d.pull_once();
  const auto d_first = d.first_pull_ops();
  int d_initialized = 0;
  std::set<std::string> d_keys;
  for (const auto& op_key : d_first) {
    if (op_key.compare(0, 12, "Initialized|") == 0) {
      ++d_initialized;
      d_keys.insert(op_key.substr(12));
    }
  }
  check(cache_before >= 1, "the hub holds the camera's current state");
  // The first pull carries at most MessageLimit (10) of them.
  check(d_initialized >= std::min(cache_before, 10) &&
            d_initialized <= std::min(cache_after, 10),
        "the late client gets one Initialized message per cached property (" +
            std::to_string(d_initialized) + ", cache " + std::to_string(cache_before) + ".." +
            std::to_string(cache_after) + ")");
  check(d_initialized >= 1 && !d_first.empty() &&
            d_first.front().compare(0, 12, "Initialized|") == 0,
        "the late client's first pull starts with the current state as Initialized "
        "(got " + std::to_string(d_initialized) + " of " + std::to_string(d_first.size()) +
            ")");
  check(static_cast<int>(d_keys.size()) == d_initialized,
        "the current state holds each property once");
  {
    std::set<std::string> seen_keys;
    for (const auto& id : rec->upstream()) {
      const size_t k = id.rfind('\x1f');
      seen_keys.insert(id.substr(k + 1));
    }
    bool subset = true;
    for (const auto& k : d_keys) subset = subset && seen_keys.count(k) > 0;
    check(subset, "the current state only names properties the camera reported");
  }
  d.start_loop();

  // ---- Unsubscribe ends a pull point for good. ----
  c.stop_loop();
  const size_t c_received = c.received();
  const HttpReply unsub = c.unsubscribe();
  check(!unsub.transport_error && unsub.status == 200, "Unsubscribe answers 200");
  const HttpReply after = c.pull_once();
  check(!after.transport_error && after.status == 500 &&
            after.body.find("Fault") != std::string::npos,
        "a pull on an unsubscribed id faults (HTTP " + std::to_string(after.status) + ")");
  const HttpReply renew_after = c.renew();
  check(!renew_after.transport_error && renew_after.status == 500,
        "a Renew of an unsubscribed id faults");

  // ---- Phase 1 results, once the clients have drained their queues. ----
  wait_for([&] { return a.received() >= c_received + 4; }, seconds(30));
  rec->stop_listener();
  check(wait_for([&] { return all_drained(rec->snapshot()); }, seconds(10)),
        "the clients drain their queues after the listener stops");
  check(none_dropped(rec->snapshot()), "no fragment was dropped: " + rec->snapshot());
  std::this_thread::sleep_for(milliseconds(300));
  const auto upstream1 = rec->upstream();
  const auto a1 = a.ids_from(0);
  const auto b1 = b.ids_from(0);
  const auto c1 = c.ids_from(0);
  const auto a1_xml = a.xml_from(0);
  const auto b1_xml = b.xml_from(0);
  check(a1.size() >= 8, "client A received at least 8 fragments, got " +
                            std::to_string(a1.size()));
  check(a1_xml == b1_xml, "clients A and B receive the same fragments in the same order (" +
                              std::to_string(a1.size()) + " vs " +
                              std::to_string(b1.size()) + ")");
  check(c1.size() >= 4 && c1.size() <= a1.size() &&
            std::equal(c1.begin(), c1.end(), a1.begin()),
        "client C received the same fragments as A until it unsubscribed (" +
            std::to_string(c1.size()) + ")");
  check(a1 == upstream1, "client A received exactly what the recorder took from the camera (" +
                             std::to_string(a1.size()) + " vs " +
                             std::to_string(upstream1.size()) + "): A=[" + join_head(a1, 4) +
                             "] upstream=[" + join_head(upstream1, 4) + "]");
  check(a.faults() == 0 && b.faults() == 0 && d.faults() == 0,
        "no faults before the restart (A " + std::to_string(a.faults()) + ", B " +
            std::to_string(b.faults()) + ", D " + std::to_string(d.faults()) + "): " +
            a.last_fault().substr(0, 300));
  check(a.unparsed() == 0 && b.unparsed() == 0, "every pull response is XML");
  check(a.renews_ok() >= 1 && b.renews_ok() >= 1, "the clients' Renews are answered");

  // ---- Recorder restart: same port, same resume and state paths. ----
  const int creates_before = emu.creates_total();
  const int syncs_before = emu.sync_points_total();
  check(syncs_before == 1,
        "the new camera subscription asks the camera for its current state once (" +
            std::to_string(syncs_before) + ")");
  const int a_err = a.transport_errors();
  const int b_err = b.transport_errors();
  const int d_err = d.transport_errors();
  const size_t a_mark = a.received();
  const size_t b_mark = b.received();
  const size_t d_mark = d.received();
  const auto restart_start = steady_clock::now();
  rec->stop();
  rec.reset();
  check(emu.live_subscriptions() == 1, "the camera subscription stays live across the restart");
  // A new process takes a moment to start; requests in the gap fail.
  std::this_thread::sleep_for(milliseconds(500));
  auto rec2 = std::make_unique<Recorder>(cfg, state_path, resume_path);
  check(rec2->start_service(port), "the restarted server binds the same port");
  const auto gap_ms = std::chrono::duration_cast<milliseconds>(steady_clock::now() -
                                                                restart_start).count();
  std::cout << "  restart gap " << gap_ms << " ms\n";
  // Each client's next pull adopts its pull point.
  check(wait_for([&] { return count_of(rec2->snapshot(), "\"adopted\":true") >= 3; },
                 seconds(10)),
        "the restarted recorder adopts the clients' pull points: " + rec2->snapshot());
  rec2->start_listener();
  check(wait_for([&] { return rec2->state_count(onvif::UpstreamEvent::kResumed) >= 1; },
                 seconds(15)),
        "the restarted recorder resumes the camera subscription");
  check(wait_for([&] { return rec2->upstream().size() >= 6; }, seconds(60)),
        "the camera keeps delivering after the restart");
  const int a_renews = a.renews_ok();
  const int camera_renews = emu.renews_total();
  check(wait_for([&] { return a.renews_ok() > a_renews && emu.renews_total() > camera_renews; },
                 seconds(15)),
        "Renews keep working after the restart");
  wait_for([&] { return a.received() >= a_mark + 6; }, seconds(10));
  const std::string snap2 = rec2->snapshot();
  check(snap2.find("\"state\":\"serving\"") != std::string::npos &&
            snap2.find("\"state\":\"draining\"") == std::string::npos,
        "adopted pull points are served: " + snap2);
  rec2->stop_listener();
  check(wait_for([&] { return all_drained(rec2->snapshot()); }, seconds(10)),
        "the clients drain their queues after the second listener stops");
  check(none_dropped(rec2->snapshot()), "no fragment was dropped after the restart: " +
                                            rec2->snapshot());
  std::this_thread::sleep_for(milliseconds(300));

  const auto upstream2 = rec2->upstream();
  const auto a2 = a.ids_from(a_mark);
  const auto b2_xml = b.xml_from(b_mark);
  const auto d2_xml = d.xml_from(d_mark);
  const auto a2_xml = a.xml_from(a_mark);
  check(rec2->state_count(onvif::UpstreamEvent::kSubscribed) == 0,
        "the restarted recorder does not create a new camera subscription");
  check(emu.creates_total() == creates_before,
        "no new Create at the camera across the restart (" +
            std::to_string(emu.creates_total()) + " vs " + std::to_string(creates_before) +
            ")");
  check(emu.sync_points_total() == syncs_before + 1,
        "the resumed subscription asks the camera for its current state once");
  check(a2.size() >= 6, "client A keeps receiving after the restart (" +
                            std::to_string(a2.size()) + ")");
  check(a2_xml == b2_xml && a2_xml == d2_xml,
        "clients A, B and D receive the same fragments after the restart (" +
            std::to_string(a2_xml.size()) + ", " + std::to_string(b2_xml.size()) + ", " +
            std::to_string(d2_xml.size()) + ")");
  check(a2 == upstream2, "after the restart client A receives exactly what the recorder "
                         "took from the camera (" + std::to_string(a2.size()) + " vs " +
                             std::to_string(upstream2.size()) + ")");
  const int a_gap = a.transport_errors() - a_err;
  const int b_gap = b.transport_errors() - b_err;
  const int d_gap = d.transport_errors() - d_err;
  std::cout << "  transport errors in the gap: A " << a_gap << ", B " << b_gap << ", D "
            << d_gap << '\n';
  check(a_gap <= 1 && b_gap <= 1 && d_gap <= 1,
        "at most one transport error per client during the restart");
  check(a.faults() == 0 && b.faults() == 0 && d.faults() == 0,
        "no faults after the restart: " + a.last_fault().substr(0, 300) +
            b.last_fault().substr(0, 300) + d.last_fault().substr(0, 300));

  a.stop_loop();
  b.stop_loop();
  d.stop_loop();
  for (ProtectClient* cl : {&a, &b, &d}) {
    const HttpReply u = cl->unsubscribe();
    check(!u.transport_error && u.status == 200, cl->name() + ": Unsubscribe answers 200");
  }
  rec2->stop();
  rec2.reset();

  check(slots.max() <= 1, "the camera never has more than one live subscription (max " +
                              std::to_string(slots.max()) + ")");
  check(emu.creates_refused() == 0, "the camera never refuses a Create (" +
                                        std::to_string(emu.creates_refused()) + ")");
  check(emu.creates_total() == 1, "the camera sees exactly one Create in the whole test (" +
                                      std::to_string(emu.creates_total()) + ")");

  // The kept subscription is released like --rollback does.
  const absl::Status rel = onvif::OnvifListener::release_saved_subscriptions(resume_path, {cfg});
  check(rel.ok(), "release_saved_subscriptions: " + std::string(rel.message()));
  check(emu.live_subscriptions() == 0, "the released subscription is gone from the camera");
  emu.stop();
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string capture;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.find("dahua_dh_sd4a425db") != std::string::npos) capture = arg;
  }
  if (capture.empty()) {
    std::cerr << "usage: " << argv[0] << " <capture.jsonl>...  (needs the Dahua PTZ capture "
                 "dahua_dh_sd4a425db_hny_*.jsonl)\n";
    return 2;
  }
  onvif::global_init();
  const auto t0 = steady_clock::now();
  test_shared_camera(capture);
  onvif::global_cleanup();
  std::cout << "shared_camera: "
            << std::chrono::duration_cast<milliseconds>(steady_clock::now() - t0).count()
            << " ms\n";
  std::cout << g_pass.load() << " passed, " << g_fail.load() << " failed\n";
  return g_fail.load() == 0 ? 0 : 1;
}
