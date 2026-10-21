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
 * test_pullpoint_hub.cpp
 *
 * PullPointHub, the state machine that shares a camera's event
 * subscription with Protect: camera keys, failure classes, the
 * subscription lifecycle, queues and overflow, adoption, the property
 * cache, hand-back, status probes, callbacks, the state file, blocking
 * pulls and reporting.  A fake clock drives time and hand-made fragments
 * stand in for parsed camera responses.
 */

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "notification_fragments.hpp"
#include "pullpoint_hub.hpp"
#include "pullpoint_hub_internal.hpp"
#include "upstream_event.hpp"

using onvif::UpstreamEvent;
using onvif::pullpoint::camera_key;
using onvif::pullpoint::camera_key_from_ip;
using onvif::pullpoint::classify_failure;
using onvif::pullpoint::Clock;
using onvif::pullpoint::ExtractResult;
using onvif::pullpoint::FailureClass;
using onvif::pullpoint::Fragment;
using onvif::pullpoint::HubCamera;
using onvif::pullpoint::PullPointHub;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool cond, const std::string& label) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << label << '\n';
  }
}

const char kIp[] = "192.168.1.109";
const char kKey[] = "192.168.1.109";
const char kMac[] = "F00000FDAED9";
const char kIp2[] = "10.0.0.7:8000";
const char kKey2[] = "10.0.0.7:8000";
const char kAdoptId[] = "0123456789abcdef0123456789abcdef";
const char kAdoptId2[] = "fedcba9876543210fedcba9876543210";

// ---------------------------------------------------------------------
// Fake clock: both clocks move together, only when told to.
// ---------------------------------------------------------------------
class FakeClock : public Clock {
 public:
  std::chrono::steady_clock::time_point steady_now() const override {
    return std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(steady_ns_.load())));
  }
  std::chrono::system_clock::time_point system_now() const override {
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds(system_ns_.load())));
  }
  template <typename Duration>
  void advance(Duration d) {
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
    steady_ns_ += ns;
    system_ns_ += ns;
  }

 private:
  std::atomic<int64_t> steady_ns_{int64_t{3600} * 1000 * 1000000000LL};
  std::atomic<int64_t> system_ns_{int64_t{1792540800} * 1000000000LL};  // 2026-10-21
};

// ---------------------------------------------------------------------
// Fragment operations: on_upstream_response() receives a token that
// names a prepared ExtractResult.
// ---------------------------------------------------------------------
std::mutex g_feed_mu;
std::map<std::string, ExtractResult> g_feeds;
uint64_t g_feed_seq = 0;

ExtractResult fake_extract(const std::string& xml) {
  std::lock_guard<std::mutex> lk(g_feed_mu);
  auto it = g_feeds.find(xml);
  if (it == g_feeds.end()) return ExtractResult{};
  ExtractResult r = std::move(it->second);
  g_feeds.erase(it);
  return r;
}

std::string fake_initialize(const std::string& xml) { return "init:" + xml; }

// A plain event (no PropertyOperation).
Fragment event(const std::string& xml, bool relevant = true) {
  Fragment f;
  f.xml = xml;
  f.topic = "tns1:RuleEngine/CellMotionDetector/Motion";
  f.protect_relevant = relevant;
  return f;
}

// A property event for @p key.
Fragment prop(const std::string& xml, const std::string& key,
              const std::string& op = "Changed", bool relevant = true) {
  Fragment f = event(xml, relevant);
  f.op = op;
  f.key = key;
  return f;
}

void feed(PullPointHub* hub, const std::string& ip, std::vector<Fragment> frags) {
  std::string token;
  {
    std::lock_guard<std::mutex> lk(g_feed_mu);
    token = "response-" + std::to_string(++g_feed_seq);
    ExtractResult r;
    r.parsed = true;
    r.fragments = std::move(frags);
    g_feeds[token] = std::move(r);
  }
  hub->on_upstream_response(ip, token);
  std::lock_guard<std::mutex> lk(g_feed_mu);
  g_feeds.erase(token);
}

// ---------------------------------------------------------------------
// Log capture
// ---------------------------------------------------------------------
class CaptureSink : public absl::LogSink {
 public:
  void Send(const absl::LogEntry& entry) override {
    std::lock_guard<std::mutex> lk(mu_);
    lines_.emplace_back(entry.text_message());
  }
  size_t count(const std::string& needle) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const std::string& l : lines_) {
      if (l.find(needle) != std::string::npos) ++n;
    }
    return n;
  }
  void clear() {
    std::lock_guard<std::mutex> lk(mu_);
    lines_.clear();
  }

 private:
  std::mutex mu_;
  std::vector<std::string> lines_;
};

CaptureSink* g_logs = nullptr;

// ---------------------------------------------------------------------
// Minimal strict JSON validator.
// ---------------------------------------------------------------------
class JsonCheck {
 public:
  explicit JsonCheck(const std::string& s) : s_(s) {}
  bool valid() {
    ws();
    if (!value(0)) return false;
    ws();
    return i_ == s_.size();
  }

 private:
  void ws() {
    while (i_ < s_.size() &&
           (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) {
      ++i_;
    }
  }
  bool eat(char c) {
    ws();
    if (i_ < s_.size() && s_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }
  bool lit(const char* w) {
    const size_t n = std::strlen(w);
    if (s_.compare(i_, n, w) != 0) return false;
    i_ += n;
    return true;
  }
  bool digits() {
    const size_t start = i_;
    while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
    return i_ > start;
  }
  bool number() {
    if (i_ < s_.size() && s_[i_] == '-') ++i_;
    if (i_ < s_.size() && s_[i_] == '0') {
      ++i_;
    } else if (!digits()) {
      return false;
    }
    if (i_ < s_.size() && s_[i_] == '.') {
      ++i_;
      if (!digits()) return false;
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      if (!digits()) return false;
    }
    return true;
  }
  bool str() {
    if (i_ >= s_.size() || s_[i_] != '"') return false;
    ++i_;
    while (i_ < s_.size()) {
      const unsigned char c = static_cast<unsigned char>(s_[i_++]);
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c != '\\') continue;
      if (i_ >= s_.size()) return false;
      const char e = s_[i_++];
      if (e == 'u') {
        for (int k = 0; k < 4; ++k) {
          if (i_ >= s_.size() || !std::isxdigit(static_cast<unsigned char>(s_[i_++]))) {
            return false;
          }
        }
      } else if (std::strchr("\"\\/bfnrt", e) == nullptr) {
        return false;
      }
    }
    return false;
  }
  bool value(int depth) {
    if (depth > 64) return false;
    ws();
    if (i_ >= s_.size()) return false;
    const char c = s_[i_];
    if (c == '{') {
      ++i_;
      if (eat('}')) return true;
      do {
        ws();
        if (!str() || !eat(':') || !value(depth + 1)) return false;
      } while (eat(','));
      return eat('}');
    }
    if (c == '[') {
      ++i_;
      if (eat(']')) return true;
      do {
        if (!value(depth + 1)) return false;
      } while (eat(','));
      return eat(']');
    }
    if (c == '"') return str();
    if (lit("true") || lit("false") || lit("null")) return true;
    return number();
  }

  const std::string& s_;
  size_t i_ = 0;
};

bool valid_json(const std::string& s) { return JsonCheck(s).valid(); }

// Value of the first integer member @p name after @p from in a JSON text.
int64_t json_int(const std::string& json, const std::string& name, size_t from = 0) {
  const std::string pat = "\"" + name + "\":";
  const size_t at = json.find(pat, from);
  if (at == std::string::npos) return -1;
  return std::strtoll(json.c_str() + at + pat.size(), nullptr, 10);
}

// ---------------------------------------------------------------------
// Test rig
// ---------------------------------------------------------------------
PullPointHub::Options default_options() { return PullPointHub::Options(); }

struct Rig {
  explicit Rig(PullPointHub::Options opts = default_options())
      : hub(std::make_unique<PullPointHub>(std::move(opts), &clock)) {
    // The callbacks read the hub, which would deadlock if they ran with
    // the hub mutex held.
    hub->set_nudge_fn([this](const std::string& ip) {
      const std::string probe = hub->snapshot_json();
      std::lock_guard<std::mutex> lk(mu);
      nudges.push_back(ip);
      if (probe.empty()) nudges.push_back("?");
    });
    hub->set_resubscribe_fn([this](const std::string& ip) {
      const std::string probe = hub->heartbeat_suffix(ip);
      std::lock_guard<std::mutex> lk(mu);
      resubscribes.push_back(ip);
    });
  }

  void add(const std::string& ip, bool proxied, const std::string& mac = "") {
    hub->register_camera(HubCamera{ip, mac, proxied});
  }
  void upstream(UpstreamEvent ev, const std::string& ip = kIp, int status = 200,
                const std::string& detail = "") {
    hub->on_upstream_event(ip, ev, status, detail);
  }
  void subscribed(const std::string& ip = kIp) { upstream(UpstreamEvent::kSubscribed, ip); }
  void pull_ok(const std::string& ip = kIp) { upstream(UpstreamEvent::kPullOk, ip); }

  // Advance @p total in @p step increments, ticking after each; with
  // @p keep_up a successful upstream pull precedes each tick, and a
  // non-empty @p pull_id is pulled by Protect each step.
  void run_for(seconds total, seconds step, bool keep_up, const std::string& pull_id = "") {
    for (seconds t{0}; t < total; t += step) {
      clock.advance(step);
      if (keep_up) pull_ok();
      if (!pull_id.empty()) pull(pull_id);
      hub->tick();
    }
  }

  PullPointHub::PullResult pull(const std::string& id, int limit = 100,
                                size_t budget = 512 * 1024, const std::string& key = kKey) {
    return hub->pull(key, id, milliseconds(0), limit, budget);
  }

  size_t nudge_count() {
    std::lock_guard<std::mutex> lk(mu);
    return nudges.size();
  }
  size_t resubscribe_count() {
    std::lock_guard<std::mutex> lk(mu);
    return resubscribes.size();
  }

  FakeClock clock;
  std::mutex mu;
  std::vector<std::string> nudges;
  std::vector<std::string> resubscribes;
  std::unique_ptr<PullPointHub> hub;
};

// A ready rig with the shared camera kIp registered and subscribed.
std::unique_ptr<Rig> ready_rig(PullPointHub::Options opts = default_options()) {
  auto r = std::make_unique<Rig>(std::move(opts));
  r->add(kIp, true, kMac);
  r->hub->set_ready();
  r->subscribed();
  return r;
}

std::vector<std::string> texts(const PullPointHub::PullResult& res) {
  std::vector<std::string> out;
  for (const auto& m : res.messages) out.push_back(*m);
  return out;
}

std::string join(const std::vector<std::string>& v) {
  std::string out;
  for (const std::string& s : v) out += (out.empty() ? "" : ",") + s;
  return out;
}

bool is_hex_id(const std::string& id) {
  if (id.size() != 32) return false;
  for (char c : id) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

int64_t secs_from_now(const FakeClock& clock, std::chrono::system_clock::time_point t) {
  return std::chrono::duration_cast<seconds>(t - clock.system_now()).count();
}

// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------
void test_camera_key() {
  check(camera_key("Cam.Example.", "80") == "cam.example", "key: lower-case, dot, port 80");
  check(camera_key("10.0.0.5", "8000") == "10.0.0.5:8000", "key: port kept");
  check(camera_key("10.0.0.5", "0") == "10.0.0.5", "key: port 0 dropped");
  check(camera_key("10.0.0.5", "") == "10.0.0.5", "key: empty port");
  check(camera_key("cam..", "") == "cam.", "key: only one trailing dot stripped");
  check(camera_key_from_ip("CAM.Example:8080") == "cam.example:8080", "from_ip: host:port");
  check(camera_key_from_ip("10.0.0.5") == "10.0.0.5", "from_ip: bare host");
  check(camera_key_from_ip("10.0.0.5:80") == "10.0.0.5", "from_ip: port 80 dropped");
  check(camera_key_from_ip("Cam.Local.:8000") == "cam.local:8000", "from_ip: trailing dot");
  check(camera_key_from_ip("fe80::1") == "fe80::1", "from_ip: second colon verbatim");
  check(camera_key("192.168.1.109", "80") == camera_key_from_ip("192.168.1.109"),
        "Protect's key matches the recorder's");
}

void test_classify_failure() {
  check(classify_failure(400, "<ter:BlockedThreadUpperLimit/>") == FailureClass::kSlotLimited,
        "BlockedThreadUpperLimit");
  check(classify_failure(500, "wsrf-rw:SubscribeCreationFailedFault") ==
            FailureClass::kSlotLimited,
        "SubscribeCreationFailed");
  check(classify_failure(500, "TooManySubscriptions") == FailureClass::kSlotLimited,
        "TooManySubscriptions");
  check(classify_failure(400, "MaxNotificationProducers") == FailureClass::kSlotLimited,
        "MaxNotificationProducers");
  check(classify_failure(500, "Reached the Maximum number of subscribers") ==
            FailureClass::kSlotLimited,
        "'maximum'");
  check(classify_failure(500, "subscriber LIMIT reached") == FailureClass::kSlotLimited,
        "'limit'");
  check(classify_failure(503, "") == FailureClass::kSlotLimited, "HTTP 503");
  check(classify_failure(0, "Couldn't connect to server") == FailureClass::kUnreachable,
        "status 0");
  check(classify_failure(0, "") == FailureClass::kUnreachable, "status 0, no detail");
  check(classify_failure(401, "") == FailureClass::kRejected, "401");
  check(classify_failure(400, "<env:Subcode><env:Value>ter:NotAuthorized</env:Value>") ==
            FailureClass::kRejected,
        "400 NotAuthorized");
  check(classify_failure(404, "Not Found") == FailureClass::kRejected, "404");
  check(classify_failure(200, "no SubscriptionReference") == FailureClass::kRejected,
        "200 without address");
  check(std::string(onvif::pullpoint::failure_class_name(FailureClass::kSlotLimited)) ==
            "slot_limited",
        "class name");
}

void test_lifecycle() {
  auto r = ready_rig();
  g_logs->clear();
  auto c = r->hub->create(kKey, std::nullopt);
  check(c.accepted, "lifecycle: create accepted");
  check(is_hex_id(c.id), "lifecycle: id is 32 lower-case hex");
  check(secs_from_now(r->clock, c.termination) == 60, "lifecycle: default TTL 60 s");
  check(c.snapshot == 0, "lifecycle: empty cache, no snapshot");
  check(g_logs->count("Protect subscribed via recorder (pp=" + c.id.substr(0, 8) +
                      ", snapshot=0)") == 1,
        "lifecycle: subscribe logged");
  auto c2 = r->hub->create(kKey, std::nullopt);
  check(c2.accepted && c2.id != c.id, "lifecycle: ids differ");

  feed(r->hub.get(), kIp, {event("<a/>"), event("<b/>")});
  auto p = r->pull(c.id);
  check(p.status == PullPointHub::PullResult::Status::kOk, "lifecycle: pull ok");
  check(join(texts(p)) == "<a/>,<b/>", "lifecycle: messages in order");
  check(secs_from_now(r->clock, p.termination) == 60, "lifecycle: pull termination");
  check(r->pull(c.id).messages.empty(), "lifecycle: queue drained");
  check(join(texts(r->pull(c2.id))) == "<a/>,<b/>", "lifecycle: fan-out to second");

  auto rn = r->hub->renew(kKey, c.id, seconds(10));
  check(rn.ok && secs_from_now(r->clock, rn.termination) == 30, "renew: TTL floor 30 s");
  rn = r->hub->renew(kKey, c.id, seconds(100000));
  check(rn.ok && secs_from_now(r->clock, rn.termination) == 600, "renew: TTL cap 600 s");
  rn = r->hub->renew(kKey, c.id, std::nullopt);
  check(rn.ok && secs_from_now(r->clock, rn.termination) == 60, "renew: default 60 s");
  check(r->hub->sync_point(kKey, c.id), "sync point on a live id");

  r->hub->unsubscribe(kKey, c.id);
  check(g_logs->count("Protect unsubscribed (pp=" + c.id.substr(0, 8)) == 1,
        "unsubscribe logged");
  check(r->pull(c.id).status == PullPointHub::PullResult::Status::kUnknown,
        "pull after unsubscribe: unknown");
  rn = r->hub->renew(kKey, c.id, std::nullopt);
  check(!rn.ok && !rn.reason.empty(), "renew after unsubscribe faults");
  check(!r->hub->sync_point(kKey, c.id), "sync point after unsubscribe fails");
  r->hub->unsubscribe(kKey, c.id);   // repeat is harmless

  check(r->pull(c2.id, 100, 512 * 1024, kKey2).status ==
            PullPointHub::PullResult::Status::kUnknown,
        "pull on another camera's path: unknown");
  check(r->pull("not-an-id").status == PullPointHub::PullResult::Status::kUnknown,
        "malformed id: unknown");
}

void test_create_declines() {
  Rig r;
  r.add(kIp, true, kMac);
  r.add(kIp2, false);
  auto c = r.hub->create(kKey, std::nullopt);
  check(!c.accepted && c.reason == "starting", "decline: not ready");
  std::string why;
  check(!r.hub->status_probe(kKey, &why) && why == "starting", "status no: not ready");
  r.hub->set_ready();
  check(r.hub->ready(), "ready");
  c = r.hub->create("10.9.9.9", std::nullopt);
  check(!c.accepted && c.reason == "unknown camera", "decline: unknown camera");
  c = r.hub->create(kKey2, std::nullopt);
  check(!c.accepted && c.reason == "not shared", "decline: not shared");
  c = r.hub->create(kKey, std::nullopt);
  check(c.accepted, "accepted while upstream not yet up");
  check(r.nudge_count() == 1, "create while not up nudges");
  const std::string snap = r.hub->snapshot_json();
  check(json_int(snap, "declines", snap.find("\"key\":\"" + std::string(kKey2))) == 1 &&
            json_int(snap, "declines", snap.find("\"key\":\"" + std::string(kKey))) == 1,
        "declines counted per camera (the not-ready one included)");
}

void test_paging_and_budget() {
  auto r = ready_rig();
  auto c = r->hub->create(kKey, std::nullopt);
  std::vector<Fragment> frags;
  for (int i = 0; i < 26; ++i) frags.push_back(event("<m" + std::to_string(i) + "/>"));
  feed(r->hub.get(), kIp, frags);
  auto p1 = r->pull(c.id, 10);
  auto p2 = r->pull(c.id, 10);
  auto p3 = r->pull(c.id, 10);
  check(p1.messages.size() == 10 && p2.messages.size() == 10 && p3.messages.size() == 6,
        "MessageLimit pages 26 as 10/10/6");
  check(*p1.messages.front() == "<m0/>" && *p2.messages.front() == "<m10/>" &&
            *p3.messages.back() == "<m25/>",
        "pages keep FIFO order");
  check(r->pull(c.id, 0).messages.empty(), "limit 0 on an empty queue");

  const std::string big(100, 'x');
  feed(r->hub.get(), kIp, {event(big + "1"), event(big + "2"), event(big + "3")});
  auto b1 = r->pull(c.id, 100, 250);
  check(b1.messages.size() == 2, "byte budget 250 takes two 101-byte messages");
  auto b2 = r->pull(c.id, 100, 10);
  check(b2.messages.size() == 1 && *b2.messages[0] == big + "3",
        "a budget smaller than one message still returns one");
  feed(r->hub.get(), kIp, {event("<x/>"), event("<y/>")});
  check(r->pull(c.id, 0).messages.size() == 1, "limit below 1 returns one");
}

void test_fifo() {
  auto r = ready_rig();
  auto a = r->hub->create(kKey, std::nullopt);
  auto b = r->hub->create(kKey, std::nullopt);
  feed(r->hub.get(), kIp, {event("1"), event("2")});
  check(join(texts(r->pull(a.id, 1))) == "1", "fifo: first");
  feed(r->hub.get(), kIp, {event("3")});
  feed(r->hub.get(), kIp, {event("4"), event("5")});
  check(join(texts(r->pull(a.id))) == "2,3,4,5", "fifo: across responses");
  auto pb = r->pull(b.id);
  check(join(texts(pb)) == "1,2,3,4,5", "fifo: independent queue per subscriber");
}

void test_overflow() {
  PullPointHub::Options o;
  o.max_queue_messages = 4;
  auto r = ready_rig(o);
  auto c = r->hub->create(kKey, std::nullopt);
  g_logs->clear();
  feed(r->hub.get(), kIp,
       {event("R1"), event("N1", false), event("N2", false), event("R2"), event("N3", false)});
  feed(r->hub.get(), kIp, {event("R3")});
  feed(r->hub.get(), kIp, {event("R4")});
  feed(r->hub.get(), kIp, {event("R5")});
  check(join(texts(r->pull(c.id))) == "R2,R3,R4,R5",
        "overflow drops messages Protect ignores first, then the oldest");
  const std::string snap = r->hub->snapshot_json();
  check(json_int(snap, "dropped") == 4, "overflow counted on the pull point");
  check(json_int(snap, "drops") == 4, "overflow counted on the camera");
  check(g_logs->count("not pulling fast enough") == 1, "overflow warning once a minute");
  r->clock.advance(seconds(61));
  for (int i = 0; i < 5; ++i) feed(r->hub.get(), kIp, {event("Z")});
  check(g_logs->count("not pulling fast enough") == 2, "overflow warning again later");

  PullPointHub::Options ob;
  ob.max_queue_bytes = 300;
  auto rb = ready_rig(ob);
  auto cb = rb->hub->create(kKey, std::nullopt);
  const std::string big(100, 'b');
  feed(rb->hub.get(), kIp, {event(big + "1"), event(big + "2"), event(big + "3"),
                            event(big + "4")});
  auto pb = rb->pull(cb.id);
  check(pb.messages.size() == 2 && *pb.messages[0] == big + "3",
        "byte cap keeps the newest that fit");
}

void test_ttl_and_expiry() {
  auto r = ready_rig();
  auto c = r->hub->create(kKey, seconds(5));
  check(secs_from_now(r->clock, c.termination) == 30, "create TTL floor");
  c = r->hub->create(kKey, seconds(3600));
  check(secs_from_now(r->clock, c.termination) == 600, "create TTL cap");
  c = r->hub->create(kKey, seconds(120));
  check(secs_from_now(r->clock, c.termination) == 120, "create TTL in range");

  auto d = r->hub->create(kKey, std::nullopt);
  r->clock.advance(seconds(59));
  r->pull(d.id);               // extends to now + 60 s
  r->clock.advance(seconds(59));
  r->hub->tick();
  check(r->hub->snapshot_json().find(d.id.substr(0, 8)) != std::string::npos,
        "pull extends expiry");
  g_logs->clear();
  r->clock.advance(seconds(2));
  r->hub->tick();
  const std::string snap = r->hub->snapshot_json();
  check(snap.find(d.id.substr(0, 8)) == std::string::npos, "expired pull point removed");
  check(json_int(snap, "expiries") >= 1, "expiry counted");
  check(g_logs->count("Protect subscription expired (pp=" + d.id.substr(0, 8)) == 1,
        "expiry logged");
  r->subscribed();
  auto p = r->pull(d.id);
  check(p.status == PullPointHub::PullResult::Status::kOk, "expired id is adopted again");
  const std::string after = r->hub->snapshot_json();
  check(after.find("\"id\":\"" + d.id.substr(0, 8) + "\"") != std::string::npos &&
            after.find("\"adopted\":true") != std::string::npos,
        "re-adopted pull point is marked adopted");
}

void test_tombstones() {
  auto r = ready_rig();
  auto c = r->hub->create(kKey, std::nullopt);
  r->hub->unsubscribe(kKey, c.id);
  r->clock.advance(seconds(599));
  r->hub->tick();
  check(r->pull(c.id).status == PullPointHub::PullResult::Status::kUnknown,
        "tombstone holds for 10 min");
  r->clock.advance(seconds(2));
  r->hub->tick();
  r->subscribed();
  check(r->pull(c.id).status == PullPointHub::PullResult::Status::kOk,
        "tombstone lapses after 10 min");

  // The tombstone list is capped; the oldest go first.
  for (int i = 0; i < 1100; ++i) {
    char id[33];
    std::snprintf(id, sizeof(id), "%032x", i + 1);
    r->hub->unsubscribe(kKey, id);
  }
  char first[33];
  std::snprintf(first, sizeof(first), "%032x", 1);
  char last[33];
  std::snprintf(last, sizeof(last), "%032x", 1100);
  check(r->pull(last).status == PullPointHub::PullResult::Status::kUnknown,
        "newest tombstone kept");
  check(r->pull(first).status == PullPointHub::PullResult::Status::kOk,
        "oldest tombstone dropped over the cap");
}

void test_adoption_states() {
  Rig r;
  r.add(kIp, true, kMac);
  g_logs->clear();
  auto p = r.pull(kAdoptId);
  check(p.status == PullPointHub::PullResult::Status::kOk, "adopt while not ready");
  check(secs_from_now(r.clock, p.termination) == 20, "provisional reports 20 s");
  check(r.hub->snapshot_json().find("\"state\":\"provisional\"") != std::string::npos,
        "adopted while not ready: provisional");
  check(g_logs->count("adopted Protect subscription pp=01234567") == 1, "adoption logged");
  auto rn = r.hub->renew(kKey, kAdoptId, seconds(60));
  check(rn.ok && secs_from_now(r.clock, rn.termination) == 20, "provisional renew: 20 s");
  r.clock.advance(seconds(59));
  r.pull(kAdoptId);
  r.clock.advance(seconds(2));
  r.hub->tick();
  check(r.hub->snapshot_json().find("\"state\":\"draining\"") != std::string::npos,
        "not ready after 60 s: draining");
  rn = r.hub->renew(kKey, kAdoptId, seconds(60));
  check(!rn.ok && rn.reason == "starting", "draining renew faults (starting)");
  r.hub->set_ready();
  rn = r.hub->renew(kKey, kAdoptId, seconds(60));
  check(rn.ok && secs_from_now(r.clock, rn.termination) == 60, "ready and accepting: serving");
  check(g_logs->count("adopted Protect subscription") == 1, "one log per adoption");

  // Ready and accepting with the upstream up: seeded from the cache.
  auto s = ready_rig();
  feed(s->hub.get(), kIp, {prop("P1", "k1", "Initialized"), prop("P2", "k2")});
  auto ps = s->pull(kAdoptId);
  check(join(texts(ps)) == "init:P1,init:P2", "adopted serving pull point gets the snapshot");
  check(s->hub->snapshot_json().find("\"state\":\"serving\"") != std::string::npos,
        "adopted while accepting: serving");
  check(json_int(s->hub->snapshot_json(), "adoptions") == 1, "adoption counted");

  // Ready but not accepting: draining, renew faults with the reason.
  s->add(kIp2, false);
  auto pd = s->pull(kAdoptId2, 100, 512 * 1024, kKey2);
  check(pd.status == PullPointHub::PullResult::Status::kOk &&
            secs_from_now(s->clock, pd.termination) == 20,
        "not shared: adopted, draining reports 20 s");
  rn = s->hub->renew(kKey2, kAdoptId2, seconds(60));
  check(!rn.ok && rn.reason == "not shared", "draining renew: not shared");
  rn = s->hub->renew("10.9.9.9", std::string(28, '0') + "abcd", seconds(60));
  check(!rn.ok && rn.reason == "unknown camera", "renew adopts unknown camera as draining");
  check(json_int(s->hub->snapshot_json(), "unregistered_pull_points") == 1,
        "unregistered camera's pull point reported");
}

void test_snapshot_cache() {
  auto r = ready_rig();
  feed(r->hub.get(), kIp,
       {prop("P1", "k1", "Initialized"), prop("P2", "k2"), event("E1")});
  auto c = r->hub->create(kKey, std::nullopt);
  check(c.snapshot == 2, "snapshot holds properties only");
  check(join(texts(r->pull(c.id))) == "init:P1,init:P2", "snapshot oldest first, Initialized");

  feed(r->hub.get(), kIp, {prop("P1b", "k1")});
  c = r->hub->create(kKey, std::nullopt);
  check(join(texts(r->pull(c.id))) == "init:P2,init:P1b", "update replaces and moves last");

  feed(r->hub.get(), kIp, {prop("D2", "k2", "Deleted")});
  c = r->hub->create(kKey, std::nullopt);
  check(c.snapshot == 1, "Deleted erases the property");

  r->clock.advance(seconds(31));
  const size_t nudges = r->nudge_count();
  c = r->hub->create(kKey, std::nullopt);
  check(c.snapshot == 0, "no snapshot while the upstream is not up");
  check(r->nudge_count() == nudges + 1, "create nudges when not up");
  r->pull_ok();
  c = r->hub->create(kKey, std::nullopt);
  check(c.snapshot == 1, "snapshot again once up");
  check(r->hub->sync_point(kKey, c.id) && r->pull(c.id).messages.size() == 2,
        "sync point re-queues the snapshot");

  r->upstream(UpstreamEvent::kResumed);
  c = r->hub->create(kKey, std::nullopt);
  check(c.snapshot == 1, "resumed keeps the cache");
  r->upstream(UpstreamEvent::kUnsubscribed);
  r->subscribed();
  c = r->hub->create(kKey, std::nullopt);
  check(c.snapshot == 0, "subscribed clears the cache");

  PullPointHub::Options o;
  o.max_cache_entries = 3;
  auto l = ready_rig(o);
  feed(l->hub.get(), kIp,
       {prop("A", "k1"), prop("B", "k2"), prop("C", "k3"), prop("D", "k4")});
  c = l->hub->create(kKey, std::nullopt);
  check(join(texts(l->pull(c.id))) == "init:B,init:C,init:D", "LRU entry cap");
  feed(l->hub.get(), kIp, {prop("B2", "k2"), prop("E", "k5")});
  c = l->hub->create(kKey, std::nullopt);
  check(join(texts(l->pull(c.id))) == "init:D,init:B2,init:E", "LRU evicts least recent");

  PullPointHub::Options ob;
  ob.max_cache_bytes = 250;
  auto b = ready_rig(ob);
  const std::string big(100, 'v');
  feed(b->hub.get(), kIp, {prop(big + "1", "k1"), prop(big + "2", "k2"), prop(big + "3", "k3")});
  c = b->hub->create(kKey, std::nullopt);
  auto pb = b->pull(c.id);
  check(pb.messages.size() == 2 && *pb.messages[0] == "init:" + big + "2",
        "cache byte cap evicts the oldest");

  // Responses for non-shared and unknown cameras are ignored.
  auto q = ready_rig();
  q->add(kIp2, false);
  q->pull(kAdoptId, 100, 512 * 1024, kKey2);
  q->pull(kAdoptId2, 100, 512 * 1024, "10.9.9.9");
  feed(q->hub.get(), kIp2, {event("other")});
  feed(q->hub.get(), "10.9.9.9", {event("unknown")});
  check(q->pull(kAdoptId, 100, 512 * 1024, kKey2).messages.empty(),
        "responses of a non-shared camera ignored");
  check(q->pull(kAdoptId2, 100, 512 * 1024, "10.9.9.9").messages.empty(),
        "responses of an unknown camera ignored");
}

void test_handback_rejected() {
  auto r = std::make_unique<Rig>();
  r->add(kIp, true, kMac);
  r->hub->set_ready();
  auto held = r->hub->create(kKey, std::nullopt);
  g_logs->clear();
  r->upstream(UpstreamEvent::kCreateFailed, kIp, 401, "Unauthorized");
  r->upstream(UpstreamEvent::kCreateFailed, kIp, 400, "ter:NotAuthorized");
  check(r->hub->create(kKey, std::nullopt).accepted, "two rejections: still accepting");
  r->upstream(UpstreamEvent::kCreateFailed, kIp, 404, "Not Found");
  auto c = r->hub->create(kKey, std::nullopt);
  check(!c.accepted && c.reason == "handed back: rejected", "three rejections: handed back");
  check(g_logs->count("handing Protect's event subscription back") == 1,
        "hand-back warning logged");
  auto rn = r->hub->renew(kKey, held.id, std::nullopt);
  check(!rn.ok && rn.reason == "handed back: rejected", "held subscription now drains");
  const std::string snap = r->hub->snapshot_json();
  check(snap.find("\"handed_back\":\"rejected\"") != std::string::npos &&
            snap.find("\"failure_class\":\"rejected\"") != std::string::npos,
        "snapshot shows hand-back and class");
  check(json_int(snap, "handbacks") == 1, "hand-back counted");

  // Another failure class in between resets the run.
  auto m = std::make_unique<Rig>();
  m->add(kIp, true);
  m->hub->set_ready();
  m->upstream(UpstreamEvent::kCreateFailed, kIp, 401, "");
  m->upstream(UpstreamEvent::kCreateFailed, kIp, 401, "");
  m->upstream(UpstreamEvent::kCreateFailed, kIp, 0, "timeout");
  m->upstream(UpstreamEvent::kCreateFailed, kIp, 401, "");
  m->upstream(UpstreamEvent::kCreateFailed, kIp, 401, "");
  check(m->hub->create(kKey, std::nullopt).accepted, "rejections must be consecutive");

  // After the recorder was once up, it takes 10 min of rejections.
  auto t = ready_rig();
  t->upstream(UpstreamEvent::kUnsubscribed);
  for (int i = 0; i < 9; ++i) {
    t->upstream(UpstreamEvent::kCreateFailed, kIp, 401, "");
    t->clock.advance(seconds(60));
    t->hub->tick();
  }
  check(t->hub->create(kKey, std::nullopt).accepted, "9 min of rejections: accepting");
  t->upstream(UpstreamEvent::kCreateFailed, kIp, 401, "");
  t->clock.advance(seconds(60));
  t->hub->tick();
  auto ct = t->hub->create(kKey, std::nullopt);
  check(!ct.accepted && ct.reason == "handed back: rejected", "10 min of rejections");
}

void test_handback_blocked() {
  auto r = std::make_unique<Rig>();
  r->add(kIp, true, kMac);
  r->hub->set_ready();
  auto held = r->hub->create(kKey, std::nullopt);
  for (int i = 0; i < 28; ++i) {
    r->clock.advance(seconds(30));
    r->upstream(UpstreamEvent::kCreateFailed, kIp, 500, "ter:BlockedThreadUpperLimit");
    r->pull(held.id);
    r->hub->tick();
  }
  check(r->hub->create(kKey, std::nullopt).accepted, "14 min not up: accepting");
  check(r->hub->snapshot_json().find("\"slot_limited_age_s\":0") != std::string::npos,
        "slot-limited time recorded");
  g_logs->clear();
  r->run_for(seconds(60), seconds(30), false, held.id);
  auto c = r->hub->create(kKey, std::nullopt);
  check(!c.accepted && c.reason == "handed back: blocked", "15 min not up: handed back");
  check(g_logs->count("(blocked)") == 1, "blocked hand-back logged");
  auto rn = r->hub->renew(kKey, held.id, std::nullopt);
  check(!rn.ok && rn.reason == "handed back: blocked", "renew faults while handed back");
  std::string why;
  check(!r->hub->status_probe(kKey, &why) && why == "handed back: blocked",
        "status no while handed back");

  // Recovery: 5 min continuously up.
  r->subscribed();
  r->run_for(seconds(290), seconds(10), true, held.id);
  check(!r->hub->create(kKey, std::nullopt).accepted, "4 min 50 s up: still handed back");
  r->run_for(seconds(10), seconds(10), true, held.id);
  check(r->hub->create(kKey, std::nullopt).accepted, "5 min up: sharing again");
  check(g_logs->count("sharing events with Protect again") == 1, "recovery logged");
  check(r->hub->renew(kKey, held.id, std::nullopt).ok, "held subscription serves again");
}

void test_handback_silent() {
  PullPointHub::Options o;
  o.silent_history = std::chrono::hours(1);
  auto r = ready_rig(o);
  r->run_for(seconds(29 * 60), seconds(10), true);
  check(r->hub->create(kKey, std::nullopt).accepted, "29 min silent: accepting");
  g_logs->clear();
  r->run_for(seconds(60), seconds(10), true);
  auto c = r->hub->create(kKey, std::nullopt);
  check(!c.accepted && c.reason == "handed back: silent", "30 min silent: handed back");
  check(g_logs->count("(silent)") == 1, "silent hand-back logged");
  r->run_for(seconds(10 * 60), seconds(10), true);
  check(!r->hub->create(kKey, std::nullopt).accepted, "silent needs an event to recover");
  feed(r->hub.get(), kIp, {event("<motion/>")});
  r->run_for(seconds(10), seconds(10), true);
  check(r->hub->create(kKey, std::nullopt).accepted, "event after silent hand-back recovers");

  // A recent event (within silent_history) prevents the silent hand-back.
  auto s = ready_rig(o);
  feed(s->hub.get(), kIp, {event("<motion/>")});
  s->upstream(UpstreamEvent::kUnsubscribed);
  s->subscribed();
  s->run_for(seconds(31 * 60), seconds(10), true);
  check(s->hub->create(kKey, std::nullopt).accepted, "recent event: not silent");
  s->run_for(seconds(30 * 60), seconds(10), true);
  check(!s->hub->create(kKey, std::nullopt).accepted, "event older than history: silent");
}

void test_status_probe() {
  auto r = std::make_unique<Rig>();
  r->add(kIp, true, kMac);
  r->hub->set_ready();
  std::string why;
  check(!r->hub->status_probe(kKey, &why) && why == "camera subscription not up",
        "status needs the upstream up");
  check(!r->hub->status_probe("10.9.9.9", &why) && why == "unknown camera",
        "status: unknown camera");
  r->subscribed();
  g_logs->clear();
  check(r->hub->status_probe(kKey, &why) && why.empty(), "status yes when up");
  check(g_logs->count("Protect moving its subscription from the camera to the recorder") == 1,
        "status yes logged");

  // The yes reserves acceptance for 30 s.
  r->add(kIp, false, kMac);
  check(r->hub->create(kKey, std::nullopt).accepted, "reservation admits the create");
  r->clock.advance(seconds(9));
  r->hub->tick();
  check(r->resubscribe_count() == 0, "no resubscribe before 10 s");
  r->clock.advance(seconds(1));
  r->hub->tick();
  check(r->resubscribe_count() == 1 && r->resubscribes[0] == kIp, "resubscribe 10 s after yes");
  r->clock.advance(seconds(21));
  auto c = r->hub->create(kKey, std::nullopt);
  check(!c.accepted && c.reason == "not shared", "reservation ends after 30 s");

  // At most one scheduled resubscribe per 60 s.
  r->add(kIp, true, kMac);
  r->pull_ok();
  check(r->hub->status_probe(kKey, nullptr), "second yes");
  r->clock.advance(seconds(15));
  r->hub->tick();
  check(r->resubscribe_count() == 1, "second yes within 60 s schedules nothing");
  r->clock.advance(seconds(30));
  r->pull_ok();
  check(r->hub->status_probe(kKey, nullptr), "third yes after 60 s");
  r->clock.advance(seconds(10));
  r->hub->tick();
  check(r->resubscribe_count() == 2, "resubscribe again after 60 s");
  const std::string snap = r->hub->snapshot_json();
  check(json_int(snap, "status_yes") == 3 && json_int(snap, "status_no") == 1,
        "status counters");
}

void test_nudge() {
  auto r = std::make_unique<Rig>();
  r->add(kIp, true, kMac);
  r->hub->set_ready();
  r->upstream(UpstreamEvent::kCreateFailed, kIp, 500, "BlockedThreadUpperLimit");
  check(r->nudge_count() == 0, "no nudge without Protect subscriptions");
  auto c = r->hub->create(kKey, std::nullopt);
  check(r->nudge_count() == 1, "create nudges a down upstream");
  r->clock.advance(seconds(10));
  r->hub->create(kKey, std::nullopt);
  r->upstream(UpstreamEvent::kCreateFailed, kIp, 500, "BlockedThreadUpperLimit");
  check(r->nudge_count() == 1, "nudges limited to one per 30 s");
  r->clock.advance(seconds(21));
  r->upstream(UpstreamEvent::kCreateFailed, kIp, 500, "BlockedThreadUpperLimit");
  check(r->nudge_count() == 2, "upstream failure with subscribers nudges");
  r->clock.advance(seconds(31));
  r->hub->unsubscribe(kKey, c.id);
  r->pull(kAdoptId);
  check(r->nudge_count() == 3, "adoption nudges");
  check(r->nudges.back() == kIp, "nudge names the camera ip");
  r->clock.advance(seconds(31));
  r->subscribed();
  check(r->nudge_count() == 3, "no nudge once up");
}

void test_persistence() {
  const char* tmpdir = std::getenv("TEST_TMPDIR");
  const std::string path = std::string(tmpdir != nullptr ? tmpdir : "/tmp") +
                           "/test_pullpoint_hub_state.json";
  std::remove(path.c_str());
  std::remove((path + ".tmp").c_str());

  PullPointHub::Options o;
  o.state_path = path;
  FakeClock clock;
  {
    PullPointHub hub(o, &clock);
    hub.register_camera(HubCamera{kIp, "f0:00:00:fd:ae:d9", true});
    hub.set_ready();
    hub.on_upstream_event(kIp, UpstreamEvent::kSubscribed, 200, "");
    feed(&hub, kIp, {event("<motion/>")});
    hub.on_upstream_event(kIp, UpstreamEvent::kUnsubscribed, 200, "");
    for (int i = 0; i < 31; ++i) {
      clock.advance(seconds(30));
      hub.on_upstream_event(kIp, UpstreamEvent::kCreateFailed, 500, "BlockedThreadUpperLimit");
      hub.tick();
    }
    check(!hub.create(kKey, std::nullopt).accepted, "persist: handed back before stop");
    std::ifstream before(path);
    std::stringstream early;
    early << before.rdbuf();
    check(early.str().find("\"handed_back\":\"blocked\"") != std::string::npos,
          "tick writes the state file after a hand-back");
    hub.stop();
  }
  struct stat st {};
  check(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "state file mode 0600");
  check(access((path + ".tmp").c_str(), F_OK) != 0, "no temporary file left");
  std::ifstream f(path);
  std::stringstream buf;
  buf << f.rdbuf();
  const std::string text = buf.str();
  check(valid_json(text), "state file is JSON");
  check(text.find("\"F00000FDAED9\"") != std::string::npos, "state keyed by normalised MAC");
  check(text.find("\"handed_back\":\"blocked\"") != std::string::npos, "hand-back persisted");
  check(json_int(text, "last_fragment_ms") > 0 && json_int(text, "slot_limited_ms") > 0 &&
            json_int(text, "handed_back_ms") > 0 && json_int(text, "last_up_ms") > 0,
        "timestamps persisted");

  {
    PullPointHub hub(o, &clock);
    hub.register_camera(HubCamera{kIp, kMac, true});
    hub.set_ready();
    auto c = hub.create(kKey, std::nullopt);
    check(!c.accepted && c.reason == "handed back: blocked", "hand-back survives a restart");
    const std::string snap = hub.snapshot_json();
    check(snap.find("\"last_fragment_age_s\":null") == std::string::npos &&
              snap.find("\"slot_limited_age_s\":null") == std::string::npos,
          "history survives a restart");
    hub.on_upstream_event(kIp, UpstreamEvent::kResumed, 200, "");
    for (int i = 0; i < 31; ++i) {
      clock.advance(seconds(10));
      hub.on_upstream_event(kIp, UpstreamEvent::kPullOk, 200, "");
      hub.tick();
    }
    check(hub.create(kKey, std::nullopt).accepted, "restored hand-back recovers after 5 min up");
  }
  std::ifstream f2(path);
  std::stringstream buf2;
  buf2 << f2.rdbuf();
  check(buf2.str().find("\"handed_back\":\"\"") != std::string::npos,
        "recovery persisted by the destructor");

  // An unreadable file is ignored.
  {
    std::ofstream junk(path, std::ios::trunc);
    junk << "{not json";
  }
  {
    PullPointHub hub(o, &clock);
    hub.register_camera(HubCamera{kIp, kMac, true});
    hub.set_ready();
    check(hub.create(kKey, std::nullopt).accepted, "unreadable state file ignored");
  }
}

std::string state_file(const std::string& name) {
  const char* tmpdir = std::getenv("TEST_TMPDIR");
  const std::string path = std::string(tmpdir != nullptr ? tmpdir : "/tmp") + "/" + name;
  std::remove(path.c_str());
  std::remove((path + ".tmp").c_str());
  return path;
}

std::string read_file(const std::string& path) {
  std::ifstream f(path);
  std::stringstream buf;
  buf << f.rdbuf();
  return buf.str();
}

// The section of a state file that belongs to @p mac.
std::string mac_entry(const std::string& text, const std::string& mac) {
  const size_t at = text.find("\"" + mac + "\":{");
  if (at == std::string::npos) return "";
  return text.substr(at, text.find('}', at) - at + 1);
}

// Timestamps that keep moving forward reach the file while the hub runs.
void test_persistence_progress() {
  const std::string path = state_file("test_pullpoint_hub_progress.json");
  PullPointHub::Options o;
  o.state_path = path;
  FakeClock clock;
  PullPointHub hub(o, &clock);
  hub.register_camera(HubCamera{kIp, kMac, true});
  hub.set_ready();
  hub.on_upstream_event(kIp, UpstreamEvent::kSubscribed, 200, "");
  feed(&hub, kIp, {event("<motion/>")});
  hub.tick();
  const int64_t up0 = json_int(read_file(path), "last_up_ms");
  const int64_t frag0 = json_int(read_file(path), "last_fragment_ms");
  check(up0 > 0 && frag0 > 0, "progress: first change written");
  for (int i = 0; i < 6 * 15; ++i) {
    clock.advance(seconds(10));
    hub.on_upstream_event(kIp, UpstreamEvent::kPullOk, 200, "");
    if (i % 6 == 0) feed(&hub, kIp, {event("<motion/>")});
    hub.tick();
    if (i == 6 * 2) {
      check(json_int(read_file(path), "last_up_ms") == up0,
            "progress: small forward moves are not written every minute");
    }
  }
  const std::string text = read_file(path);
  check(json_int(text, "last_up_ms") - up0 >= 5 * 60 * 1000 &&
            json_int(text, "last_fragment_ms") - frag0 >= 5 * 60 * 1000,
        "progress: timestamps written within 10 min while running");
}

// The persisted history belongs to a camera's MAC, not to its address.
void test_persistence_by_mac() {
  const std::string path = state_file("test_pullpoint_hub_mac.json");
  PullPointHub::Options o;
  o.state_path = path;
  FakeClock clock;
  {
    PullPointHub hub(o, &clock);
    hub.register_camera(HubCamera{kIp, "AA0000000001", true});
    hub.set_ready();
    hub.on_upstream_event(kIp, UpstreamEvent::kSubscribed, 200, "");
    feed(&hub, kIp, {event("<motion/>")});
    // A different camera now answers at the same address.
    hub.register_camera(HubCamera{kIp, "BB0000000002", true});
    hub.stop();
  }
  std::string text = read_file(path);
  check(json_int(mac_entry(text, "AA0000000001"), "last_fragment_ms") > 0,
        "mac change: the old camera's history is kept");
  check(json_int(mac_entry(text, "BB0000000002"), "last_fragment_ms") == 0,
        "mac change: the new camera does not inherit it");

  // A camera that moves to another address takes its history along; the
  // stale address no longer writes under its MAC.
  std::remove(path.c_str());
  {
    PullPointHub hub(o, &clock);
    hub.register_camera(HubCamera{kIp, kMac, true});
    hub.set_ready();
    for (int i = 0; i < 31; ++i) {
      clock.advance(seconds(30));
      hub.tick();
    }
    check(!hub.create(kKey, std::nullopt).accepted, "moved: old address handed back");
    hub.register_camera(HubCamera{kIp2, kMac, true});
    hub.on_upstream_event(kIp2, UpstreamEvent::kSubscribed, 200, "");
    for (int i = 0; i < 31; ++i) {
      clock.advance(seconds(10));
      hub.on_upstream_event(kIp2, UpstreamEvent::kPullOk, 200, "");
      hub.tick();
    }
    check(hub.create(kKey2, std::nullopt).accepted, "moved: new address shares after 5 min up");
    hub.stop();
  }
  text = read_file(path);
  check(text.find("\"handed_back\":\"blocked\"") == std::string::npos &&
            text.find("\"" + std::string(kMac) + "\"") != std::string::npos,
        "moved: the file follows the camera's current address");
}

// Out-of-range numbers in the state file count as never.
void test_persistence_garbage_numbers() {
  const std::string path = state_file("test_pullpoint_hub_garbage.json");
  {
    std::ofstream f(path, std::ios::trunc);
    f << "{\"cameras\":{\"" << kMac << "\":{\"last_up_ms\":1e300,"
      << "\"last_fragment_ms\":-5,\"slot_limited_ms\":-1e300,"
      << "\"handed_back\":\"\",\"handed_back_ms\":0}}}";
  }
  PullPointHub::Options o;
  o.state_path = path;
  FakeClock clock;
  PullPointHub hub(o, &clock);
  hub.register_camera(HubCamera{kIp, kMac, true});
  const std::string snap = hub.snapshot_json();
  check(snap.find("\"last_fragment_age_s\":null") != std::string::npos &&
            snap.find("\"last_up_age_s\":null") != std::string::npos &&
            snap.find("\"slot_limited_age_s\":null") != std::string::npos,
        "garbage numbers in the state file read as never");
}

// A camera that has refused the recorder for want of a free slot is never
// handed back as silent: the recorder holds that slot, so Protect's own
// subscription would be refused.
void test_handback_silent_slot_limited() {
  PullPointHub::Options o;
  o.silent_history = std::chrono::hours(1);

  // Slot-limited before the subscription: shared however long it is silent.
  {
    auto r = std::make_unique<Rig>(o);
    r->add(kIp, true, kMac);
    r->hub->set_ready();
    r->upstream(UpstreamEvent::kCreateFailed, kIp, 400, "ter:BlockedThreadUpperLimit");
    r->subscribed();
    g_logs->clear();
    r->run_for(seconds(2 * 3600), seconds(10), true);
    check(r->hub->create(kKey, std::nullopt).accepted,
          "slot-limited: 2 h silent, still accepting");
    check(g_logs->count("(silent)") == 0, "slot-limited: no silent hand-back logged");
  }

  // A slot fault after a silent hand-back ends it.
  {
    auto r = ready_rig(o);
    r->run_for(seconds(31 * 60), seconds(10), true);
    check(!r->hub->create(kKey, std::nullopt).accepted, "not slot-limited: silent hand-back");
    g_logs->clear();
    r->upstream(UpstreamEvent::kUnsubscribed);
    r->upstream(UpstreamEvent::kCreateFailed, kIp, 400, "ter:BlockedThreadUpperLimit");
    check(r->hub->create(kKey, std::nullopt).accepted, "slot fault ends a silent hand-back");
    check(g_logs->count("sharing events with Protect again") == 1,
          "end of the silent hand-back logged");
    check(r->hub->snapshot_json().find("\"handed_back\":\"\"") != std::string::npos,
          "snapshot: no longer handed back");
  }

  // A saved silent hand-back is dropped at registration once the camera
  // has been slot-limited, and kept otherwise.
  const std::string path = state_file("test_pullpoint_hub_silent_slot.json");
  for (const bool slot_limited : {false, true}) {
    {
      std::ofstream f(path, std::ios::trunc);
      f << "{\"cameras\":{\"" << kMac << "\":{\"last_up_ms\":1792540000000,"
        << "\"last_fragment_ms\":0,\"slot_limited_ms\":"
        << (slot_limited ? "1792540000000" : "0")
        << ",\"handed_back\":\"silent\",\"handed_back_ms\":1792540000000}}}";
    }
    PullPointHub::Options op = o;
    op.state_path = path;
    FakeClock clock;
    g_logs->clear();
    PullPointHub hub(op, &clock);
    hub.register_camera(HubCamera{kIp, kMac, true});
    hub.set_ready();
    const auto c = hub.create(kKey, std::nullopt);
    if (slot_limited) {
      check(c.accepted, "saved silent hand-back of a slot-limited camera: dropped");
      check(g_logs->count("still handed back") == 0 &&
                g_logs->count("sharing events with Protect again") == 1,
            "dropped saved hand-back logged as ended");
    } else {
      check(!c.accepted && c.reason == "handed back: silent",
            "saved silent hand-back of a camera never slot-limited: kept");
    }
    hub.stop();
    check((read_file(path).find("\"handed_back\":\"\"") != std::string::npos) == slot_limited,
          std::string("saved hand-back after restart, slot-limited=") +
              (slot_limited ? "true" : "false"));
  }
}

// Wait until the hub has counted @p n pulls in total for the camera, so a
// pull started on another thread is known to be holding.
bool wait_for_pulls(PullPointHub* hub, int64_t n) {
  for (int i = 0; i < 2000; ++i) {
    if (json_int(hub->snapshot_json(), "pulls") >= n) return true;
    std::this_thread::sleep_for(milliseconds(1));
  }
  return false;
}

int64_t elapsed_ms(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - since)
      .count();
}

void test_blocking_pull() {
  auto r = ready_rig();
  auto c = r->hub->create(kKey, std::nullopt);
  using Status = PullPointHub::PullResult::Status;
  int64_t pulls = 0;

  auto t0 = std::chrono::steady_clock::now();
  auto p = r->hub->pull(kKey, c.id, milliseconds(200), 100, 512 * 1024);
  const int64_t waited = elapsed_ms(t0);
  ++pulls;
  check(p.status == Status::kOk && p.messages.empty() && waited >= 180 && waited < 1000,
        "held pull times out empty");

  // Woken by a publish.
  PullPointHub::PullResult held;
  int64_t held_ms = 0;
  t0 = std::chrono::steady_clock::now();
  std::thread a([&] {
    held = r->hub->pull(kKey, c.id, milliseconds(3000), 100, 512 * 1024);
    held_ms = elapsed_ms(t0);
  });
  check(wait_for_pulls(r->hub.get(), ++pulls), "publish: pull is holding");
  feed(r->hub.get(), kIp, {event("<wake/>")});
  a.join();
  check(held.status == Status::kOk && join(texts(held)) == "<wake/>" && held_ms < 1500,
        "publish wakes a held pull");

  // Pre-empted by a newer pull on the same id.
  t0 = std::chrono::steady_clock::now();
  std::thread b([&] {
    held = r->hub->pull(kKey, c.id, milliseconds(3000), 100, 512 * 1024);
    held_ms = elapsed_ms(t0);
  });
  check(wait_for_pulls(r->hub.get(), ++pulls), "pre-empt: pull is holding");
  auto second = r->hub->pull(kKey, c.id, milliseconds(200), 100, 512 * 1024);
  ++pulls;
  b.join();
  check(held.status == Status::kOk && held.messages.empty() && held_ms < 1500,
        "a second pull pre-empts the held one");
  check(second.status == Status::kOk && second.messages.empty(), "second pull times out");

  // wake_all() releases held pulls; later pulls return at once until start().
  t0 = std::chrono::steady_clock::now();
  std::thread w([&] {
    held = r->hub->pull(kKey, c.id, milliseconds(3000), 100, 512 * 1024);
    held_ms = elapsed_ms(t0);
  });
  check(wait_for_pulls(r->hub.get(), ++pulls), "wake: pull is holding");
  r->hub->wake_all();
  w.join();
  check(held.status == Status::kStopped && held_ms < 1500, "wake_all releases a held pull");
  t0 = std::chrono::steady_clock::now();
  p = r->hub->pull(kKey, c.id, milliseconds(3000), 100, 512 * 1024);
  ++pulls;
  check(p.status == Status::kStopped && elapsed_ms(t0) < 500, "after wake_all: no hold");
  feed(r->hub.get(), kIp, {event("<late/>")});
  p = r->hub->pull(kKey, c.id, milliseconds(3000), 100, 512 * 1024);
  ++pulls;
  check(p.status == Status::kOk && join(texts(p)) == "<late/>",
        "after wake_all: queued messages still delivered");

  r->hub->start();
  t0 = std::chrono::steady_clock::now();
  p = r->hub->pull(kKey, c.id, milliseconds(200), 100, 512 * 1024);
  ++pulls;
  check(p.status == Status::kOk && elapsed_ms(t0) >= 180, "start() restores holding");

  t0 = std::chrono::steady_clock::now();
  std::thread s([&] {
    held = r->hub->pull(kKey, c.id, milliseconds(3000), 100, 512 * 1024);
    held_ms = elapsed_ms(t0);
  });
  check(wait_for_pulls(r->hub.get(), ++pulls), "stop: pull is holding");
  r->hub->stop();
  s.join();
  check(held.status == Status::kStopped && held_ms < 1500, "stop releases a held pull");

  // Unsubscribe while held releases it.
  r->hub->start();
  t0 = std::chrono::steady_clock::now();
  std::thread u([&] {
    held = r->hub->pull(kKey, c.id, milliseconds(3000), 100, 512 * 1024);
    held_ms = elapsed_ms(t0);
  });
  check(wait_for_pulls(r->hub.get(), ++pulls), "unsubscribe: pull is holding");
  r->hub->unsubscribe(kKey, c.id);
  u.join();
  check(held.status == Status::kOk && held.messages.empty() && held_ms < 1500,
        "unsubscribe releases a held pull");
  r->hub->stop();
}

void test_snapshot_json() {
  auto r = ready_rig();
  r->add(kIp2, false);
  r->add("Cam.Example.", true);
  r->upstream(UpstreamEvent::kCreateFailed, "Cam.Example.", 500,
              std::string("quote \" backslash \\ newline \n tab \t ctl \x01 utf8 \xc3\xa9"));
  auto c = r->hub->create(kKey, std::nullopt);
  r->pull(c.id);
  r->pull(kAdoptId, 100, 512 * 1024, "10.9.9.9");
  const std::string snap = r->hub->snapshot_json();
  check(valid_json(snap), "snapshot_json is valid JSON");
  check(snap.rfind("{\"ready\":true,\"cameras\":[", 0) == 0, "snapshot starts with ready");
  check(snap.find("\"key\":\"192.168.1.109\"") != std::string::npos &&
            snap.find("\"key\":\"cam.example\"") != std::string::npos &&
            snap.find("\"key\":\"10.0.0.7:8000\"") != std::string::npos,
        "snapshot lists every camera by key");
  check(snap.find("\"upstream\":\"up\"") != std::string::npos &&
            snap.find("\"upstream\":\"down\"") != std::string::npos &&
            snap.find("\"upstream\":\"unknown\"") != std::string::npos,
        "upstream up/down/unknown");
  check(snap.find("\"failure_class\":\"slot_limited\"") == std::string::npos &&
            snap.find("\"failure_class\":\"rejected\"") != std::string::npos,
        "failure class reported");
  check(snap.find("\"last_fragment_age_s\":null") != std::string::npos, "never is null");
  check(snap.find("\"state\":\"serving\"") != std::string::npos &&
            snap.find("\"id\":\"" + c.id.substr(0, 8) + "\"") != std::string::npos,
        "pull point listed with an 8-digit id");
  check(snap.find(c.id) == std::string::npos, "full ids are not exposed");
  check(json_int(snap, "unregistered_pull_points") == 1, "unregistered pull points counted");
  check(snap.find("\"reason\":\"not shared\"") != std::string::npos, "decline reason shown");

  Rig empty;
  check(valid_json(empty.hub->snapshot_json()), "empty snapshot is valid JSON");
}

void test_heartbeat_suffix() {
  auto r = ready_rig();
  r->add(kIp2, false);
  check(r->hub->heartbeat_suffix(kIp) == " protect_pp=0", "heartbeat: no subscribers");
  check(r->hub->heartbeat_suffix(kIp2).empty(), "heartbeat: not shared");
  check(r->hub->heartbeat_suffix("10.9.9.9").empty(), "heartbeat: unknown");
  auto c = r->hub->create(kKey, std::nullopt);
  r->clock.advance(seconds(5));
  check(r->hub->heartbeat_suffix(kIp) == " protect_pp=1 protect_pull_age=5s",
        "heartbeat: one subscriber, pull age");
  r->pull(c.id);
  r->hub->create(kKey, std::nullopt);
  r->clock.advance(seconds(2));
  check(r->hub->heartbeat_suffix(kIp) == " protect_pp=2 protect_pull_age=2s",
        "heartbeat: newest pull counts");
}

void test_pull_point_caps() {
  PullPointHub::Options o;
  o.max_pull_points_per_camera = 2;
  o.max_pull_points_total = 3;
  auto r = ready_rig(o);
  r->add(kIp2, true);
  r->subscribed(kIp2);
  auto a = r->hub->create(kKey, std::nullopt);
  auto b = r->hub->create(kKey, std::nullopt);
  r->clock.advance(seconds(1));
  r->pull(a.id);
  auto c = r->hub->create(kKey, std::nullopt);
  std::string snap = r->hub->snapshot_json();
  check(snap.find(a.id.substr(0, 8)) != std::string::npos &&
            snap.find(b.id.substr(0, 8)) == std::string::npos &&
            snap.find(c.id.substr(0, 8)) != std::string::npos,
        "per-camera cap evicts the least recently pulled");
  r->clock.advance(seconds(1));
  r->pull(c.id);
  r->clock.advance(seconds(1));
  auto d = r->hub->create(kKey2, std::nullopt);
  r->clock.advance(seconds(1));
  auto e = r->hub->create(kKey2, std::nullopt);
  snap = r->hub->snapshot_json();
  check(snap.find(a.id.substr(0, 8)) == std::string::npos &&
            snap.find(c.id.substr(0, 8)) != std::string::npos &&
            snap.find(d.id.substr(0, 8)) != std::string::npos &&
            snap.find(e.id.substr(0, 8)) != std::string::npos,
        "total cap evicts the least recently pulled of any camera");
}

void test_hourly_summary() {
  auto r = ready_rig();
  r->hub->create(kKey, std::nullopt);
  r->hub->create("10.9.9.9", std::nullopt);
  g_logs->clear();
  r->clock.advance(std::chrono::minutes(59));
  r->hub->tick();
  check(g_logs->count("[event_proxy] last 1h:") == 0, "no summary before an hour");
  r->clock.advance(std::chrono::minutes(1));
  r->hub->tick();
  check(g_logs->count("[event_proxy] last 1h: creates=1 declines=1 status_yes=0") == 1,
        "hourly summary");
  r->clock.advance(std::chrono::hours(1));
  r->hub->tick();
  check(g_logs->count("[event_proxy] last 1h: creates=0 declines=0") == 1,
        "summary counters reset");
}

// Publishers, pullers, the hub thread and readers at once; run under TSan.
void test_concurrency() {
  PullPointHub::Options o;
  o.max_queue_messages = 100000;
  o.max_queue_bytes = 64 * 1024 * 1024;
  auto r = ready_rig(o);
  r->add(kIp2, true);
  r->subscribed(kIp2);
  r->hub->start();
  auto a = r->hub->create(kKey, std::nullopt);
  auto b = r->hub->create(kKey, std::nullopt);
  constexpr int kPerPublisher = 300;
  std::atomic<bool> done{false};
  std::atomic<int> got_a{0};
  std::atomic<int> got_b{0};
  auto puller = [&](const std::string& id, std::atomic<int>* got) {
    const auto deadline = std::chrono::steady_clock::now() + seconds(20);
    while (got->load() < 2 * kPerPublisher && std::chrono::steady_clock::now() < deadline) {
      auto p = r->hub->pull(kKey, id, milliseconds(50), 7, 512 * 1024);
      *got += static_cast<int>(p.messages.size());
    }
  };
  std::thread pa(puller, a.id, &got_a);
  std::thread pb(puller, b.id, &got_b);
  std::thread pub1([&] {
    for (int i = 0; i < kPerPublisher; ++i) feed(r->hub.get(), kIp, {event("one")});
  });
  std::thread pub2([&] {
    for (int i = 0; i < kPerPublisher; ++i) {
      feed(r->hub.get(), kIp, {prop("two", "k" + std::to_string(i % 5))});
    }
  });
  std::thread reader([&] {
    while (!done.load()) {
      (void)r->hub->snapshot_json();
      (void)r->hub->heartbeat_suffix(kIp);
      (void)r->hub->status_probe(kKey, nullptr);
      (void)r->hub->renew(kKey, a.id, std::nullopt);
      r->upstream(UpstreamEvent::kPullOk, kIp2);
      auto c = r->hub->create(kKey2, std::nullopt);
      if (c.accepted) r->hub->unsubscribe(kKey2, c.id);
      r->hub->tick();
      std::this_thread::sleep_for(milliseconds(1));
    }
  });
  pub1.join();
  pub2.join();
  pa.join();
  pb.join();
  done = true;
  reader.join();
  check(got_a.load() == 2 * kPerPublisher && got_b.load() == 2 * kPerPublisher,
        "every message reaches every subscriber under concurrency");
  r->hub->stop();
}

}  // namespace

int main() {
  CaptureSink sink;
  g_logs = &sink;
  absl::AddLogSink(&sink);
  onvif::pullpoint::internal::set_fragment_ops_for_testing({&fake_extract, &fake_initialize});

  test_camera_key();
  test_classify_failure();
  test_lifecycle();
  test_create_declines();
  test_paging_and_budget();
  test_fifo();
  test_overflow();
  test_ttl_and_expiry();
  test_tombstones();
  test_adoption_states();
  test_snapshot_cache();
  test_handback_rejected();
  test_handback_blocked();
  test_handback_silent();
  test_handback_silent_slot_limited();
  test_status_probe();
  test_nudge();
  test_persistence();
  test_persistence_progress();
  test_persistence_by_mac();
  test_persistence_garbage_numbers();
  test_blocking_pull();
  test_snapshot_json();
  test_heartbeat_suffix();
  test_pull_point_caps();
  test_hourly_summary();
  test_concurrency();

  onvif::pullpoint::internal::set_fragment_ops_for_testing({});
  absl::RemoveLogSink(&sink);
  std::cout << "passed: " << g_pass << '\n';
  std::cout << "failed: " << g_fail << '\n';
  return g_fail == 0 ? 0 : 1;
}
