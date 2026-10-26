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

#include "pullpoint_hub.hpp"

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/base/log_severity.h"
#include "absl/log/globals.h"
#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "notification_fragments.hpp"
#include "pullpoint_hub_internal.hpp"
#include "util.hpp"

namespace onvif {
namespace pullpoint {

namespace {

using SteadyTime = std::chrono::steady_clock::time_point;
using SysTime = std::chrono::system_clock::time_point;
using std::chrono::seconds;

constexpr seconds kDefaultTtl{60};
constexpr seconds kMinTtl{30};
constexpr seconds kMaxTtl{600};
// Every pull keeps its pull point for at least this long.
constexpr seconds kPullExtension{60};
// TerminationTime reported for draining and provisional pull points.
// Protect reads TerminationTime only from Create and Renew answers, so it
// renews (and is steered) about 10 s after a provisional pull point's
// Renew; a serving pull point that turns draining is steered only at its
// next scheduled Renew, up to about 55 s later.
constexpr seconds kSteerTtl{20};
constexpr seconds kResubscribeInterval{60};
constexpr seconds kPersistInterval{60};
// Timestamps that only move forward by less than this are written with
// the next other change, at stop(), or once this long after the last
// write, not every minute.
constexpr seconds kPersistGranularity{600};
constexpr int64_t kPersistGranularityMs = kPersistGranularity.count() * 1000;
constexpr seconds kDeclineLogInterval{300};
constexpr seconds kOverflowLogInterval{60};
constexpr seconds kEvictionWarnInterval{600};
constexpr seconds kMoveWarnInterval{600};
constexpr std::chrono::hours kSummaryInterval{1};
constexpr size_t kMaxTombstones = 1024;
constexpr size_t kMaxDetailBytes = 300;
constexpr size_t kIdLength = 32;
constexpr size_t kMaxDeclineLogKeys = 1024;

class RealClock : public Clock {
 public:
  std::chrono::steady_clock::time_point steady_now() const override {
    return std::chrono::steady_clock::now();
  }
  std::chrono::system_clock::time_point system_now() const override {
    return std::chrono::system_clock::now();
  }
};

using ExtractFn = ExtractResult (*)(const std::string&);
using InitializeFn = std::string (*)(const std::string&);
std::atomic<ExtractFn> g_extract{&extract_fragments};
std::atomic<InitializeFn> g_initialize{&as_initialized};

std::string to_lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

bool contains_ci(const std::string& haystack, const char* needle) {
  return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

// Upper-case hex digits only, so "f0:00:00:fd:ae:d9" and "F00000FDAED9"
// name the same camera.
std::string normalize_mac(const std::string& mac) {
  std::string out;
  for (char ch : mac) {
    if (std::isxdigit(static_cast<unsigned char>(ch))) {
      out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
  }
  return out;
}

bool valid_id(const std::string& id) {
  if (id.size() != kIdLength) return false;
  for (char ch : id) {
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
  }
  return true;
}

// 32 lower-case hex digits from 128 random bits.  getrandom() cannot fail
// for lack of file descriptors, unlike std::random_device, which would end
// the process in this exception-free build.
std::string random_id() {
  unsigned char bytes[kIdLength / 2];
  size_t got = 0;
  while (got < sizeof(bytes)) {
    const ssize_t n = getrandom(bytes + got, sizeof(bytes) - got, 0);
    if (n > 0) {
      got += static_cast<size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  if (got < sizeof(bytes)) {
    // No getrandom() (kernel before 3.17): ids stay unique, but not
    // unpredictable.
    static std::atomic<uint64_t> counter{0};
    uint64_t mix[2] = {
        static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()),
        counter.fetch_add(1) ^ (static_cast<uint64_t>(getpid()) << 32)};
    std::memcpy(bytes, mix, sizeof(bytes));
  }
  static const char kHex[] = "0123456789abcdef";
  std::string id;
  id.reserve(kIdLength);
  for (unsigned char b : bytes) {
    id.push_back(kHex[b >> 4]);
    id.push_back(kHex[b & 0xF]);
  }
  return id;
}

int64_t to_ms(SysTime t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

template <typename Duration>
int64_t whole_seconds(Duration d) {
  const int64_t s = std::chrono::duration_cast<seconds>(d).count();
  return s < 0 ? 0 : s;
}

std::string id8(const std::string& id) { return id.substr(0, 8); }

std::string truncate(const std::string& s, size_t n) {
  return s.size() <= n ? s : s.substr(0, n);
}

seconds clamp_ttl(std::optional<seconds> ttl) {
  if (!ttl) return kDefaultTtl;
  return std::clamp(*ttl, kMinTtl, kMaxTtl);
}

// Replace @p path with @p data through a temporary file, fsync and
// rename(), so a crash never leaves a truncated file.  Mode 0600.
bool write_file_atomic(const std::string& path, const std::string& data) {
  const std::string tmp = path + ".tmp";
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = true;
  size_t off = 0;
  while (ok && off < data.size()) {
    const ssize_t n = write(fd, data.data() + off, data.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      ok = false;
    } else {
      off += static_cast<size_t>(n);
    }
  }
  ok = ok && fchmod(fd, 0600) == 0;
  ok = ok && fsync(fd) == 0;
  ok = close(fd) == 0 && ok;
  ok = ok && std::rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok) {
    const int saved = errno;
    unlink(tmp.c_str());
    errno = saved;
  }
  return ok;
}

// ---------------------------------------------------------------------
// Minimal JSON reader for the hub's own state file.
// ---------------------------------------------------------------------
struct JsonValue {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool boolean = false;
  double number = 0;
  std::string text;
  std::vector<JsonValue> items;
  std::vector<std::pair<std::string, JsonValue>> members;

  const JsonValue* member(const std::string& name) const {
    for (const auto& m : members) {
      if (m.first == name) return &m.second;
    }
    return nullptr;
  }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& text) : s_(text) {}

  bool parse(JsonValue* out) {
    if (!parse_value(out, 0)) return false;
    skip_ws();
    return pos_ == s_.size();
  }

 private:
  static constexpr int kMaxDepth = 32;

  void skip_ws() {
    while (pos_ < s_.size() &&
           (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool consume(char ch) {
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == ch) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool literal(const char* word) {
    const size_t n = std::strlen(word);
    if (s_.compare(pos_, n, word) != 0) return false;
    pos_ += n;
    return true;
  }

  bool parse_value(JsonValue* v, int depth) {
    if (depth > kMaxDepth) return false;
    skip_ws();
    if (pos_ >= s_.size()) return false;
    const char ch = s_[pos_];
    if (ch == '{') return parse_object(v, depth);
    if (ch == '[') return parse_array(v, depth);
    if (ch == '"') {
      v->type = JsonValue::Type::kString;
      return parse_string(&v->text);
    }
    if (literal("true")) {
      v->type = JsonValue::Type::kBool;
      v->boolean = true;
      return true;
    }
    if (literal("false")) {
      v->type = JsonValue::Type::kBool;
      return true;
    }
    if (literal("null")) return true;
    return parse_number(v);
  }

  bool parse_object(JsonValue* v, int depth) {
    v->type = JsonValue::Type::kObject;
    ++pos_;
    if (consume('}')) return true;
    do {
      skip_ws();
      std::string name;
      if (!parse_string(&name) || !consume(':')) return false;
      JsonValue child;
      if (!parse_value(&child, depth + 1)) return false;
      v->members.emplace_back(std::move(name), std::move(child));
    } while (consume(','));
    return consume('}');
  }

  bool parse_array(JsonValue* v, int depth) {
    v->type = JsonValue::Type::kArray;
    ++pos_;
    if (consume(']')) return true;
    do {
      JsonValue child;
      if (!parse_value(&child, depth + 1)) return false;
      v->items.push_back(std::move(child));
    } while (consume(','));
    return consume(']');
  }

  static void append_utf8(uint32_t cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool parse_string(std::string* out) {
    if (pos_ >= s_.size() || s_[pos_] != '"') return false;
    ++pos_;
    while (pos_ < s_.size()) {
      const char ch = s_[pos_++];
      if (ch == '"') return true;
      if (static_cast<unsigned char>(ch) < 0x20) return false;
      if (ch != '\\') {
        out->push_back(ch);
        continue;
      }
      if (pos_ >= s_.size()) return false;
      const char esc = s_[pos_++];
      switch (esc) {
        case '"': case '\\': case '/': out->push_back(esc); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          if (pos_ + 4 > s_.size()) return false;
          uint32_t cp = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') {
              cp |= static_cast<uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              cp |= static_cast<uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              cp |= static_cast<uint32_t>(h - 'A' + 10);
            } else {
              return false;
            }
          }
          append_utf8(cp, out);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  bool parse_number(JsonValue* v) {
    const size_t start = pos_;
    while (pos_ < s_.size() &&
           (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '-' ||
            s_[pos_] == '+' || s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E')) {
      ++pos_;
    }
    if (pos_ == start) return false;
    const std::string token = s_.substr(start, pos_ - start);
    char* end = nullptr;
    v->number = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size()) return false;
    v->type = JsonValue::Type::kNumber;
    return true;
  }

  const std::string& s_;
  size_t pos_ = 0;
};

// ---------------------------------------------------------------------
// Hub state
// ---------------------------------------------------------------------
struct Message {
  std::shared_ptr<const std::string> xml;
  bool relevant = false;
};

struct Counters {
  uint64_t creates = 0;
  uint64_t declines = 0;
  uint64_t status_yes = 0;
  uint64_t status_no = 0;
  uint64_t renews = 0;
  uint64_t renew_faults = 0;
  uint64_t pulls = 0;
  uint64_t msgs_out = 0;
  uint64_t drops = 0;
  uint64_t adoptions = 0;
  uint64_t expiries = 0;
  uint64_t evictions = 0;
  uint64_t handbacks = 0;
  uint64_t nudges = 0;
  uint64_t resubscribes = 0;
  uint64_t fragments = 0;
  uint64_t oversize_dropped = 0;
};

// Latest message per property (PropertyOperation events), replayed as
// Initialized to each new subscriber.
struct CacheEntry {
  std::string key;
  std::shared_ptr<const std::string> xml;
  bool relevant = false;
};

// Per-camera history kept in Options::state_path, by MAC.  Times are Unix
// milliseconds, 0 for never.
struct Persisted {
  int64_t last_up_ms = 0;
  int64_t last_fragment_ms = 0;
  int64_t slot_limited_ms = 0;
  std::string handed_back;   // reason, empty when sharing
  int64_t handed_back_ms = 0;
};

enum class PpState { kServing, kDraining, kProvisional };

const char* pp_state_name(PpState s) {
  switch (s) {
    case PpState::kServing:     return "serving";
    case PpState::kDraining:    return "draining";
    case PpState::kProvisional: return "provisional";
  }
  return "unknown";
}

// One of Protect's subscriptions served by the hub.
struct PullPoint {
  std::string id;
  std::string key;
  bool adopted = false;
  bool removed = false;     // unsubscribed, expired or evicted
  SteadyTime created;
  SteadyTime last_pull;
  SteadyTime expiry;
  std::deque<Message> queue;
  size_t queue_bytes = 0;
  uint64_t delivered = 0;
  uint64_t dropped = 0;
  uint64_t pull_seq = 0;    // a newer pull pre-empts a held one
  absl::CondVar cv;         // queue, pull_seq, removed or hub wake changed
};

struct CameraState {
  std::string key;
  std::string ip;
  std::string mac;
  bool proxied = false;

  // The recorder's own subscription.
  bool seen_upstream = false;      // any upstream event since start
  bool subscribed = false;
  SteadyTime subscribed_at;
  SteadyTime last_ok_at;
  uint64_t fragments_since_sub = 0;
  int consecutive_rejected = 0;
  std::optional<SteadyTime> rejected_since;
  bool ever_up = false;
  bool up = false;                 // as of the last refresh_up()
  SteadyTime up_since;
  SteadyTime not_up_since;
  FailureClass failure = FailureClass::kNone;
  std::string failure_detail;

  Persisted p;
  bool fragment_since_handback = false;

  // Protect's side.
  std::optional<SteadyTime> reserved_until;
  std::optional<SteadyTime> resubscribe_due;
  std::optional<SteadyTime> resubscribe_planned_at;
  std::optional<SteadyTime> nudged_at;
  std::optional<SteadyTime> overflow_logged_at;
  std::optional<SteadyTime> move_warned_at;
  uint64_t moves_since_warning = 0;

  // Property cache, most recently updated first.
  std::list<CacheEntry> cache;
  std::unordered_map<std::string, std::list<CacheEntry>::iterator> cache_index;
  size_t cache_bytes = 0;

  Counters counters;
};

// Callbacks collected under the hub lock and run after it is released.
struct Actions {
  std::vector<std::string> nudges;
  std::vector<std::string> resubscribes;
  std::function<void(const std::string&)> nudge_fn;
  std::function<void(const std::string&)> resubscribe_fn;

  bool empty() const { return nudges.empty() && resubscribes.empty(); }
  void run() const {
    for (const std::string& ip : nudges) {
      if (nudge_fn) nudge_fn(ip);
    }
    for (const std::string& ip : resubscribes) {
      if (resubscribe_fn) resubscribe_fn(ip);
    }
  }
};

void write_counters(std::ostringstream* o, const Counters& c) {
  *o << "{\"creates\":" << c.creates << ",\"declines\":" << c.declines
     << ",\"status_yes\":" << c.status_yes << ",\"status_no\":" << c.status_no
     << ",\"renews\":" << c.renews << ",\"renew_faults\":" << c.renew_faults
     << ",\"pulls\":" << c.pulls << ",\"msgs_out\":" << c.msgs_out
     << ",\"drops\":" << c.drops << ",\"adoptions\":" << c.adoptions
     << ",\"expiries\":" << c.expiries << ",\"evictions\":" << c.evictions
     << ",\"handbacks\":" << c.handbacks
     << ",\"nudges\":" << c.nudges << ",\"resubscribes\":" << c.resubscribes
     << ",\"fragments\":" << c.fragments
     << ",\"oversize_dropped\":" << c.oversize_dropped << '}';
}

}  // namespace

// ---------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------
std::string camera_key(const std::string& host, const std::string& port) {
  std::string key = to_lower(host);
  if (!key.empty() && key.back() == '.') key.pop_back();
  if (!port.empty() && port != "80" && port != "0") key += ":" + port;
  return key;
}

std::string camera_key_from_ip(const std::string& ip) {
  const size_t colon = ip.find(':');
  if (colon == std::string::npos) return camera_key(ip, "");
  if (ip.find(':', colon + 1) != std::string::npos) return ip;
  return camera_key(ip.substr(0, colon), ip.substr(colon + 1));
}

const char* failure_class_name(FailureClass c) {
  switch (c) {
    case FailureClass::kNone:        return "none";
    case FailureClass::kSlotLimited: return "slot_limited";
    case FailureClass::kUnreachable: return "unreachable";
    case FailureClass::kRejected:    return "rejected";
  }
  return "unknown";
}

FailureClass classify_failure(int http_status, const std::string& detail) {
  if (http_status == 0) return FailureClass::kUnreachable;
  if (http_status == 401 || contains_ci(detail, "NotAuthorized")) {
    return FailureClass::kRejected;
  }
  if (http_status == 503) return FailureClass::kSlotLimited;
  static const char* const kSlotHints[] = {
      "BlockedThreadUpperLimit", "SubscribeCreationFailed", "TooManySubscriptions",
      "MaxNotificationProducers", "maximum", "limit",
  };
  for (const char* hint : kSlotHints) {
    if (contains_ci(detail, hint)) return FailureClass::kSlotLimited;
  }
  return FailureClass::kRejected;
}

Clock* real_clock() {
  static RealClock* const clock = new RealClock();
  return clock;
}

size_t max_pull_points_total_for(size_t shared_cameras) {
  constexpr size_t kMinTotal = 64;
  constexpr size_t kPerCamera = 4;
  return std::max(kMinTotal, kPerCamera * shared_cameras);
}

void WarningSink::Send(const absl::LogEntry& entry) {
  if (entry.log_severity() != absl::LogSeverity::kWarning) return;
  if (entry.log_severity() >= absl::StderrThreshold()) return;   // printed already
  if (!absl::StartsWith(entry.text_message(), "[event_proxy]")) return;
  const absl::string_view line = entry.text_message_with_prefix_and_newline();
  std::fwrite(line.data(), 1, line.size(), out_);
  std::fflush(out_);
}

namespace internal {

void set_fragment_ops_for_testing(const FragmentOps& ops) {
  g_extract.store(ops.extract ? ops.extract : &extract_fragments);
  g_initialize.store(ops.initialize ? ops.initialize : &as_initialized);
}

}  // namespace internal

// ---------------------------------------------------------------------
// PullPointHub::Impl
// ---------------------------------------------------------------------
struct PullPointHub::Impl {
  Impl(Options o, Clock* c)
      : opts(std::move(o)), clock(c != nullptr ? c : real_clock()) {
    started_at = clock->steady_now();
    summary_at = started_at;
  }

  const Options opts;
  Clock* const clock;

  mutable absl::Mutex mu;
  bool ready = false;
  bool woken = false;            // wake_all()/stop(): pulls return at once
  bool thread_stop = false;
  bool not_ready_warned = false;
  SteadyTime started_at;
  SteadyTime summary_at;
  std::map<std::string, CameraState> cameras;   // by camera key
  std::unordered_map<std::string, std::shared_ptr<PullPoint>> pull_points;
  std::unordered_set<std::string> tombstones;
  std::deque<std::pair<std::string, SteadyTime>> tombstone_order;
  std::map<std::string, SteadyTime> decline_logged;
  std::map<std::string, Persisted> loaded;      // state file, by MAC
  // Adoptions for cameras not registered yet, by camera key; counted on
  // the camera once it is registered.
  std::map<std::string, uint64_t> early_adoptions;
  std::optional<SteadyTime> eviction_warned_at;
  Counters hourly;
  std::function<void(const std::string&)> nudge_fn;
  std::function<void(const std::string&)> resubscribe_fn;
  absl::CondVar thread_cv;
  std::thread thread;

  // Serialises state-file writers; taken before mu, never while holding it.
  absl::Mutex persist_mu;
  bool changed = false;    // persisted values differ from the file
  bool material = false;   // a change that is written within a minute
  std::optional<SteadyTime> persisted_at;

  // ---- helpers (mu held) ---------------------------------------------
  int64_t now_ms() const { return to_ms(clock->system_now()); }

  SysTime to_system(SteadyTime t, SteadyTime now) const {
    return clock->system_now() +
           std::chrono::duration_cast<std::chrono::system_clock::duration>(t - now);
  }

  CameraState* camera_by_key(const std::string& key) {
    auto it = cameras.find(key);
    return it == cameras.end() ? nullptr : &it->second;
  }

  CameraState* camera_by_ip(const std::string& ip) {
    return camera_by_key(camera_key_from_ip(ip));
  }

  static std::string label(const CameraState* c, const std::string& key) {
    return c != nullptr ? c->ip : key;
  }

  void count(CameraState* c, uint64_t Counters::*field, uint64_t n = 1) {
    if (c != nullptr) c->counters.*field += n;
    hourly.*field += n;
  }

  // Record a persisted timestamp.  Small forward moves are batched.
  void touch_ms(int64_t* field, int64_t value) {
    if (*field == value) return;
    const int64_t delta = value - *field;
    const bool big = *field == 0 || value == 0 || delta < 0 || delta >= kPersistGranularityMs;
    *field = value;
    changed = true;
    if (big) material = true;
  }

  // Why Creates for @p c are declined; empty when accepting.
  std::string decline_reason(const CameraState* c) const {
    if (!ready) return "starting";
    if (c == nullptr) return "unknown camera";
    if (!c->proxied) return "not shared";
    if (!c->p.handed_back.empty()) return "handed back: " + c->p.handed_back;
    return "";
  }

  bool reserved(const CameraState& c, SteadyTime now) const {
    return c.reserved_until && now < *c.reserved_until;
  }

  bool is_up(const CameraState& c, SteadyTime now) const {
    if (!c.subscribed) return false;
    return now - std::max(c.subscribed_at, c.last_ok_at) <= opts.up_window;
  }

  // Track the start and end of continuous Up periods.
  void refresh_up(CameraState* c, SteadyTime now) {
    const bool up = is_up(*c, now);
    if (up && !c->up) {
      c->up_since = now;
      c->ever_up = true;
    } else if (!up && c->up) {
      // Up ended when the last success aged out of the window.
      SteadyTime end = now;
      if (c->subscribed) {
        end = std::min(now, std::max(c->subscribed_at, c->last_ok_at) + opts.up_window);
      }
      c->not_up_since = end;
    }
    c->up = up;
    if (up) touch_ms(&c->p.last_up_ms, now_ms());
  }

  PpState state_of(const PullPoint& pp, SteadyTime now) {
    if (!ready) {
      return now - started_at < opts.not_ready_limit ? PpState::kProvisional
                                                     : PpState::kDraining;
    }
    const CameraState* c = camera_by_key(pp.key);
    if (c != nullptr && (decline_reason(c).empty() || reserved(*c, now))) {
      return PpState::kServing;
    }
    return PpState::kDraining;
  }

  SysTime termination_of(const PullPoint& pp, SteadyTime now) {
    if (state_of(pp, now) == PpState::kServing) return to_system(pp.expiry, now);
    return clock->system_now() + kSteerTtl;
  }

  void prune_tombstones(SteadyTime now) {
    while (!tombstone_order.empty() &&
           (now - tombstone_order.front().second >= opts.tombstone_ttl ||
            tombstone_order.size() > kMaxTombstones)) {
      tombstones.erase(tombstone_order.front().first);
      tombstone_order.pop_front();
    }
  }

  bool tombstoned(const std::string& id, SteadyTime now) {
    prune_tombstones(now);
    return tombstones.count(id) > 0;
  }

  void tombstone(const std::string& id, SteadyTime now) {
    if (tombstones.insert(id).second) tombstone_order.emplace_back(id, now);
    prune_tombstones(now);
  }

  void maybe_nudge(CameraState* c, SteadyTime now, Actions* act) {
    if (c->nudged_at && now - *c->nudged_at < opts.nudge_interval) return;
    c->nudged_at = now;
    ++c->counters.nudges;
    act->nudges.push_back(c->ip);
  }

  void log_decline(const CameraState* c, const std::string& key, const char* what,
                   const std::string& reason, SteadyTime now) {
    const std::string k = key + "|" + what + "|" + reason;
    auto it = decline_logged.find(k);
    if (it != decline_logged.end() && now - it->second < kDeclineLogInterval) return;
    if (decline_logged.size() >= kMaxDeclineLogKeys) decline_logged.clear();
    decline_logged[k] = now;
    // Declines for a shared camera send Protect to the camera itself.
    LOG(LEVEL(c != nullptr && c->proxied ? absl::LogSeverity::kWarning
                                         : absl::LogSeverity::kInfo))
        << "[event_proxy] " << label(c, key) << " declined Protect's " << what << ": "
        << reason;
  }

  // Append to a pull point's queue.  Over the caps, the oldest message
  // Protect does not read goes first, else the oldest.
  void append(PullPoint* pp, const Message& m, CameraState* c, SteadyTime now) {
    pp->queue.push_back(m);
    pp->queue_bytes += m.xml->size();
    uint64_t dropped = 0;
    while (!pp->queue.empty() && (pp->queue.size() > opts.max_queue_messages ||
                                  pp->queue_bytes > opts.max_queue_bytes)) {
      auto victim = std::find_if(pp->queue.begin(), pp->queue.end(),
                                 [](const Message& q) { return !q.relevant; });
      if (victim == pp->queue.end()) victim = pp->queue.begin();
      pp->queue_bytes -= victim->xml->size();
      pp->queue.erase(victim);
      ++dropped;
    }
    if (dropped == 0) return;
    pp->dropped += dropped;
    count(c, &Counters::drops, dropped);
    if (c != nullptr &&
        (!c->overflow_logged_at || now - *c->overflow_logged_at >= kOverflowLogInterval)) {
      c->overflow_logged_at = now;
      LOG(WARNING) << "[event_proxy] " << c->ip << " Protect is not pulling fast enough;"
                   << " dropped " << dropped << " queued event(s) (pp=" << id8(pp->id)
                   << ", " << pp->dropped << " so far)";
    }
  }

  // Queue the property cache, as Initialized messages, oldest first.
  size_t seed(PullPoint* pp, CameraState* c, SteadyTime now) {
    const InitializeFn initialize = g_initialize.load();
    size_t n = 0;
    for (auto it = c->cache.rbegin(); it != c->cache.rend(); ++it) {
      auto xml = std::make_shared<const std::string>(initialize(*it->xml));
      append(pp, Message{std::move(xml), it->relevant}, c, now);
      ++n;
    }
    if (n > 0) pp->cv.SignalAll();
    return n;
  }

  static size_t entry_bytes(const CacheEntry& e) { return e.key.size() + e.xml->size(); }

  void erase_cache_entry(CameraState* c, std::list<CacheEntry>::iterator it) {
    c->cache_bytes -= entry_bytes(*it);
    c->cache_index.erase(it->key);
    c->cache.erase(it);
  }

  void clear_cache(CameraState* c) {
    c->cache.clear();
    c->cache_index.clear();
    c->cache_bytes = 0;
  }

  void store_cache(CameraState* c, const Fragment& f,
                   const std::shared_ptr<const std::string>& xml) {
    auto found = c->cache_index.find(f.key);
    if (found != c->cache_index.end()) erase_cache_entry(c, found->second);
    if (f.op == "Deleted") return;
    c->cache.push_front(CacheEntry{f.key, xml, f.protect_relevant});
    c->cache_index[f.key] = c->cache.begin();
    c->cache_bytes += entry_bytes(c->cache.front());
    while (!c->cache.empty() && (c->cache.size() > opts.max_cache_entries ||
                                 c->cache_bytes > opts.max_cache_bytes)) {
      erase_cache_entry(c, std::prev(c->cache.end()));
    }
  }

  void remove_pull_point(const std::string& id) {
    auto it = pull_points.find(id);
    if (it == pull_points.end()) return;
    it->second->removed = true;
    it->second->cv.SignalAll();
    pull_points.erase(it);
  }

  size_t pull_point_count(const std::string& key) const {
    size_t n = 0;
    for (const auto& kv : pull_points) {
      if (kv.second->key == key) ++n;
    }
    return n;
  }

  // Remove the least recently pulled pull point (of @p key when given).
  // Evictions over the total cap are warned about every
  // kEvictionWarnInterval: they mean the cap is too small for the cameras
  // Protect pulls through the recorder.
  bool evict_one(const std::string* key, SteadyTime now) {
    std::shared_ptr<PullPoint> victim;
    for (const auto& kv : pull_points) {
      if (key != nullptr && kv.second->key != *key) continue;
      if (!victim || kv.second->last_pull < victim->last_pull) victim = kv.second;
    }
    if (!victim) return false;
    CameraState* c = camera_by_key(victim->key);
    count(c, &Counters::evictions);
    const bool warn = key == nullptr && (!eviction_warned_at ||
                                         now - *eviction_warned_at >= kEvictionWarnInterval);
    if (warn) eviction_warned_at = now;
    LOG(LEVEL(warn ? absl::LogSeverity::kWarning : absl::LogSeverity::kInfo))
        << "[event_proxy] " << label(c, victim->key) << " dropped Protect subscription pp="
        << id8(victim->id) << " (pulled " << whole_seconds(now - victim->last_pull)
        << "s ago; too many subscriptions"
        << (key == nullptr ? ", over " + std::to_string(opts.max_pull_points_total) + " in total"
                           : std::string(" for this camera"))
        << ')';
    remove_pull_point(victim->id);
    return true;
  }

  std::shared_ptr<PullPoint> add_pull_point(const std::string& key, const std::string& id,
                                            bool adopted, SteadyTime now, seconds ttl) {
    while (pull_point_count(key) >= opts.max_pull_points_per_camera && evict_one(&key, now)) {
    }
    while (pull_points.size() >= opts.max_pull_points_total && evict_one(nullptr, now)) {
    }
    auto pp = std::make_shared<PullPoint>();
    pp->id = id;
    pp->key = key;
    pp->adopted = adopted;
    pp->created = now;
    pp->last_pull = now;
    pp->expiry = now + ttl;
    pull_points[id] = pp;
    return pp;
  }

  std::shared_ptr<PullPoint> adopt(const std::string& key, const std::string& id,
                                   SteadyTime now, Actions* act) {
    auto pp = add_pull_point(key, id, /*adopted=*/true, now, kDefaultTtl);
    CameraState* c = camera_by_key(key);
    if (c != nullptr) refresh_up(c, now);
    const PpState st = state_of(*pp, now);
    size_t snapshot = 0;
    if (st == PpState::kServing && c != nullptr && c->up) snapshot = seed(pp.get(), c, now);
    if (c != nullptr && c->proxied && !c->up) maybe_nudge(c, now, act);
    count(c, &Counters::adoptions);
    if (c == nullptr) {
      if (early_adoptions.size() >= kMaxDeclineLogKeys) early_adoptions.clear();
      ++early_adoptions[key];
    }
    LOG(INFO) << "[event_proxy] " << label(c, key) << " adopted Protect subscription pp="
              << id8(id) << " (" << pp_state_name(st) << ", snapshot=" << snapshot << ')';
    return pp;
  }

  // The pull point for @p id, adopted when unknown.  Null when the id is
  // malformed, tombstoned, or belongs to another camera.
  std::shared_ptr<PullPoint> find_or_adopt(const std::string& key, const std::string& id,
                                           SteadyTime now, Actions* act) {
    if (!valid_id(id) || tombstoned(id, now)) return nullptr;
    auto it = pull_points.find(id);
    if (it != pull_points.end()) return it->second->key == key ? it->second : nullptr;
    return adopt(key, id, now, act);
  }

  void enter_handback(CameraState* c, const std::string& reason, const std::string& why) {
    c->p.handed_back = reason;
    c->p.handed_back_ms = now_ms();
    c->fragment_since_handback = false;
    changed = material = true;
    count(c, &Counters::handbacks);
    LOG(WARNING) << "[event_proxy] " << c->ip
                 << " handing Protect's event subscription back to the camera ("
                 << reason << "): " << why;
  }

  void leave_handback(CameraState* c, const std::string& why) {
    LOG(WARNING) << "[event_proxy] " << c->ip << " sharing events with Protect again " << why
                 << " (was handed back: " << c->p.handed_back << ')';
    c->p.handed_back.clear();
    c->p.handed_back_ms = 0;
    changed = material = true;
  }

  // A camera that has ever refused the recorder a subscription for want
  // of a free slot may allow only one subscriber.  The recorder holds that
  // slot while the camera is silent, so handing Protect back to it would
  // only get Protect's own subscription refused.
  static bool silent_handback_allowed(const Persisted& p) { return p.slot_limited_ms == 0; }

  // Ends a Silent hand-back that no longer applies.  True when it did.
  bool drop_void_handback(CameraState* c) {
    if (c->p.handed_back != "silent" || silent_handback_allowed(c->p)) return false;
    leave_handback(c, "because the camera has refused subscriptions for want of a free slot");
    return true;
  }

  // Hand-back rules for a shared camera:
  // * rejected: rejected_attempts consecutive Rejected Creates and never Up
  //   since start, or Rejected Creates for rejected_for;
  // * blocked: not Up for blocked_after;
  // * silent: Up for silent_after on the current subscription with no
  //   fragment since it, and none within silent_history; never for a camera
  //   that has reported SlotLimited (silent_handback_allowed()).
  // A hand-back ends after recover_after continuously Up (silent also
  // needs a fragment since the hand-back), or at once for a silent one
  // that no longer applies.
  void evaluate_handback(CameraState* c, SteadyTime now) {
    if (!c->proxied) return;
    if (!c->p.handed_back.empty()) {
      if (drop_void_handback(c)) return;
      if (c->up && now - c->up_since >= opts.recover_after &&
          (c->p.handed_back != "silent" || c->fragment_since_handback)) {
        leave_handback(c, "after " + std::to_string(whole_seconds(now - c->up_since) / 60) +
                              " min up");
      }
      return;
    }
    const std::string failure = c->failure == FailureClass::kNone
                                    ? std::string()
                                    : std::string(failure_class_name(c->failure)) + ": " +
                                          c->failure_detail;
    if (!c->ever_up && c->consecutive_rejected >= opts.rejected_attempts) {
      enter_handback(c, "rejected",
                     std::to_string(c->consecutive_rejected) +
                         " subscription attempts rejected (" + failure + ")");
    } else if (c->rejected_since && now - *c->rejected_since >= opts.rejected_for) {
      enter_handback(c, "rejected",
                     "subscriptions rejected for " +
                         std::to_string(whole_seconds(now - *c->rejected_since) / 60) +
                         " min (" + failure + ")");
    } else if (!c->up && now - c->not_up_since >= opts.blocked_after) {
      enter_handback(c, "blocked",
                     "no working camera subscription for " +
                         std::to_string(whole_seconds(now - c->not_up_since) / 60) + " min" +
                         (failure.empty() ? "" : " (" + failure + ")"));
    } else if (c->up && c->fragments_since_sub == 0 && silent_handback_allowed(c->p) &&
               now - std::max(c->up_since, c->subscribed_at) >= opts.silent_after &&
               (c->p.last_fragment_ms == 0 ||
                now_ms() - c->p.last_fragment_ms >
                    std::chrono::duration_cast<std::chrono::milliseconds>(opts.silent_history)
                        .count())) {
      enter_handback(c, "silent",
                     "no events from the camera for " +
                         std::to_string(
                             whole_seconds(now - std::max(c->up_since, c->subscribed_at)) / 60) +
                         " min since subscribing");
    }
  }

  void publish(const std::string& ip, ExtractResult* r) {
    absl::MutexLock lk(&mu);
    CameraState* c = camera_by_ip(ip);
    if (c == nullptr || !c->proxied) return;
    const SteadyTime now = clock->steady_now();
    count(c, &Counters::oversize_dropped, r->oversize_dropped);
    if (r->fragments.empty()) return;
    std::vector<Message> msgs;
    msgs.reserve(r->fragments.size());
    for (Fragment& f : r->fragments) {
      auto xml = std::make_shared<const std::string>(std::move(f.xml));
      if (!f.op.empty() && !f.key.empty()) store_cache(c, f, xml);
      msgs.push_back(Message{std::move(xml), f.protect_relevant});
    }
    c->fragments_since_sub += msgs.size();
    count(c, &Counters::fragments, msgs.size());
    touch_ms(&c->p.last_fragment_ms, now_ms());
    if (!c->p.handed_back.empty()) c->fragment_since_handback = true;
    for (auto& kv : pull_points) {
      PullPoint* pp = kv.second.get();
      if (pp->key != c->key) continue;
      for (const Message& m : msgs) append(pp, m, c, now);
      pp->cv.SignalAll();
    }
  }

  void wake_pulls() {
    for (auto& kv : pull_points) kv.second->cv.SignalAll();
  }

  void take_fns(Actions* act) const {
    if (act->empty()) return;
    act->nudge_fn = nudge_fn;
    act->resubscribe_fn = resubscribe_fn;
  }

  // ---- persistence ---------------------------------------------------
  std::string state_json() const {
    std::map<std::string, Persisted> all = loaded;
    for (const auto& kv : cameras) {
      if (!kv.second.mac.empty()) all[kv.second.mac] = kv.second.p;
    }
    std::ostringstream o;
    o << "{\"cameras\":{";
    bool first = true;
    for (const auto& kv : all) {
      if (!first) o << ',';
      first = false;
      const Persisted& p = kv.second;
      o << '\n' << util::json_str(kv.first) << ":{\"last_up_ms\":" << p.last_up_ms
        << ",\"last_fragment_ms\":" << p.last_fragment_ms
        << ",\"slot_limited_ms\":" << p.slot_limited_ms
        << ",\"handed_back\":" << util::json_str(p.handed_back)
        << ",\"handed_back_ms\":" << p.handed_back_ms << '}';
    }
    o << "\n}}\n";
    return o.str();
  }

  void load_state() {
    if (opts.state_path.empty()) return;
    std::ifstream f(opts.state_path);
    if (!f.is_open()) return;
    std::stringstream buf;
    buf << f.rdbuf();
    const std::string text = buf.str();
    JsonValue doc;
    JsonParser parser(text);
    const JsonValue* cams = nullptr;
    if (parser.parse(&doc)) cams = doc.member("cameras");
    if (cams == nullptr || cams->type != JsonValue::Type::kObject) {
      LOG(WARNING) << "[event_proxy] ignoring unreadable state file " << opts.state_path;
      return;
    }
    // Unix milliseconds; negative or absurdly large values count as never.
    auto num = [](const JsonValue& obj, const char* name) -> int64_t {
      const JsonValue* v = obj.member(name);
      if (v == nullptr || v->type != JsonValue::Type::kNumber) return 0;
      if (!(v->number >= 0 && v->number <= 9e15)) return 0;
      return static_cast<int64_t>(v->number);
    };
    for (const auto& kv : cams->members) {
      const std::string mac = normalize_mac(kv.first);
      if (mac.empty() || kv.second.type != JsonValue::Type::kObject) continue;
      Persisted p;
      p.last_up_ms = num(kv.second, "last_up_ms");
      p.last_fragment_ms = num(kv.second, "last_fragment_ms");
      p.slot_limited_ms = num(kv.second, "slot_limited_ms");
      p.handed_back_ms = num(kv.second, "handed_back_ms");
      const JsonValue* hb = kv.second.member("handed_back");
      if (hb != nullptr && hb->type == JsonValue::Type::kString) p.handed_back = hb->text;
      loaded[mac] = p;
    }
  }

  // Write the state file when due: a material change at most once a
  // minute, any other change once kPersistGranularity has passed since
  // the last write.  With @p force, whenever anything changed.
  void persist(bool force) {
    if (opts.state_path.empty()) return;
    absl::MutexLock plk(&persist_mu);
    std::string json;
    {
      absl::MutexLock lk(&mu);
      const SteadyTime now = clock->steady_now();
      if (!changed) return;
      const auto since = persisted_at ? now - *persisted_at : SteadyTime::duration::max();
      if (!force && !(material && since >= kPersistInterval) && since < kPersistGranularity) {
        return;
      }
      json = state_json();
      changed = material = false;
      persisted_at = now;
    }
    if (!write_file_atomic(opts.state_path, json)) {
      LOG_EVERY_N_SEC(WARNING, 3600) << "[event_proxy] cannot write " << opts.state_path
                                     << ": " << std::strerror(errno);
      absl::MutexLock lk(&mu);
      changed = material = true;
    }
  }

  void thread_main(PullPointHub* hub) {
    while (true) {
      {
        absl::MutexLock lk(&mu);
        if (thread_stop) return;
        thread_cv.WaitWithTimeout(&mu, absl::Seconds(1));
        if (thread_stop) return;
      }
      hub->tick();
    }
  }
};

// ---------------------------------------------------------------------
// PullPointHub
// ---------------------------------------------------------------------
PullPointHub::PullPointHub(Options opts, Clock* clock)
    : impl_(new Impl(std::move(opts), clock)) {
  impl_->load_state();
}

PullPointHub::~PullPointHub() { stop(); }

void PullPointHub::register_camera(const HubCamera& cam) {
  absl::MutexLock lk(&impl_->mu);
  Impl& m = *impl_;
  const SteadyTime now = m.clock->steady_now();
  const std::string key = camera_key_from_ip(cam.ip);
  auto ins = m.cameras.try_emplace(key);
  CameraState& c = ins.first->second;
  if (ins.second) {
    auto early = m.early_adoptions.find(key);
    if (early != m.early_adoptions.end()) {
      c.counters.adoptions += early->second;
      m.early_adoptions.erase(early);
    }
  }
  const bool was_proxied = c.proxied;
  c.key = key;
  c.ip = cam.ip;
  c.proxied = cam.proxied;
  const std::string mac = normalize_mac(cam.mac);
  if (!mac.empty() && mac != c.mac) {
    // The persisted history belongs to the camera, not its address: keep
    // the previous camera's, and take this camera's from the address it
    // was registered at before (which then no longer writes it).
    if (!c.mac.empty()) {
      m.loaded[c.mac] = c.p;
      m.changed = m.material = true;
    }
    for (auto& kv : m.cameras) {
      if (&kv.second == &c || kv.second.mac != mac) continue;
      m.loaded[mac] = kv.second.p;
      kv.second.mac.clear();
      m.changed = m.material = true;
    }
    c.mac = mac;
    auto saved = m.loaded.find(mac);
    c.p = saved != m.loaded.end() ? saved->second : Persisted();
    c.fragment_since_handback = false;
    if (!c.p.handed_back.empty() && c.proxied && !m.drop_void_handback(&c)) {
      LOG(WARNING) << "[event_proxy] " << c.ip << " still handed back to the camera ("
                   << c.p.handed_back << ") until the recorder's subscription is up for "
                   << whole_seconds(m.opts.recover_after) / 60 << " min";
    }
  }
  if ((ins.second || (c.proxied && !was_proxied)) && !c.up) c.not_up_since = now;
}

void PullPointHub::set_ready() {
  absl::MutexLock lk(&impl_->mu);
  if (impl_->ready) return;
  impl_->ready = true;
  size_t shared = 0;
  for (const auto& kv : impl_->cameras) {
    if (kv.second.proxied) ++shared;
  }
  LOG(INFO) << "[event_proxy] ready: " << impl_->cameras.size() << " camera(s), " << shared
            << " shared with Protect, up to " << impl_->opts.max_pull_points_total
            << " Protect subscriptions";
}

bool PullPointHub::ready() const {
  absl::MutexLock lk(&impl_->mu);
  return impl_->ready;
}

void PullPointHub::set_nudge_fn(std::function<void(const std::string& ip)> fn) {
  absl::MutexLock lk(&impl_->mu);
  impl_->nudge_fn = std::move(fn);
}

void PullPointHub::set_resubscribe_fn(std::function<void(const std::string& ip)> fn) {
  absl::MutexLock lk(&impl_->mu);
  impl_->resubscribe_fn = std::move(fn);
}

void PullPointHub::on_upstream_response(const std::string& ip, const std::string& xml) {
  {
    absl::MutexLock lk(&impl_->mu);
    const CameraState* c = impl_->camera_by_ip(ip);
    if (c == nullptr || !c->proxied) return;
  }
  // Parsing runs without the lock so Protect's requests are not held up.
  const ExtractFn extract = g_extract.load();
  ExtractResult r = extract(xml);
  impl_->publish(ip, &r);
}

void PullPointHub::on_upstream_event(const std::string& ip, UpstreamEvent ev,
                                     int http_status, const std::string& detail) {
  Actions act;
  {
    absl::MutexLock lk(&impl_->mu);
    Impl& m = *impl_;
    CameraState* c = m.camera_by_ip(ip);
    if (c == nullptr) return;
    const SteadyTime now = m.clock->steady_now();
    m.refresh_up(c, now);
    c->seen_upstream = true;
    switch (ev) {
      case UpstreamEvent::kSubscribed:
      case UpstreamEvent::kResumed:
        // A new subscription starts a new event stream, whose Initialized
        // messages rebuild the cache.  A resumed one continues the old
        // stream, so the cache stays valid; the listener's
        // SetSynchronizationPoint after the resume refreshes it.
        if (ev == UpstreamEvent::kSubscribed) m.clear_cache(c);
        c->subscribed = true;
        c->subscribed_at = now;
        c->last_ok_at = now;
        c->fragments_since_sub = 0;
        c->consecutive_rejected = 0;
        c->rejected_since.reset();
        c->failure = FailureClass::kNone;
        c->failure_detail.clear();
        break;
      case UpstreamEvent::kPullOk:
        if (!c->subscribed) {
          c->subscribed = true;
          c->subscribed_at = now;
        }
        c->last_ok_at = now;
        break;
      case UpstreamEvent::kCreateFailed: {
        c->subscribed = false;
        const FailureClass fc = classify_failure(http_status, detail);
        c->failure = fc;
        c->failure_detail = truncate(detail, kMaxDetailBytes);
        if (fc == FailureClass::kRejected) {
          ++c->consecutive_rejected;
          if (!c->rejected_since) c->rejected_since = now;
        } else {
          c->consecutive_rejected = 0;
          c->rejected_since.reset();
        }
        if (fc == FailureClass::kSlotLimited) m.touch_ms(&c->p.slot_limited_ms, m.now_ms());
        break;
      }
      case UpstreamEvent::kUnsubscribed:
        c->subscribed = false;
        break;
      case UpstreamEvent::kPullFailed:
      case UpstreamEvent::kRenewFailed:
        break;
    }
    m.refresh_up(c, now);
    m.evaluate_handback(c, now);
    if (!c->up && m.pull_point_count(c->key) > 0) m.maybe_nudge(c, now, &act);
    m.take_fns(&act);
  }
  act.run();
}

PullPointHub::CreateResult PullPointHub::create(const std::string& key,
                                                std::optional<std::chrono::seconds> ttl) {
  CreateResult res;
  Actions act;
  {
    absl::MutexLock lk(&impl_->mu);
    Impl& m = *impl_;
    const SteadyTime now = m.clock->steady_now();
    CameraState* c = m.camera_by_key(key);
    if (c != nullptr) m.refresh_up(c, now);
    const std::string reason = m.decline_reason(c);
    if (!reason.empty() && !(c != nullptr && m.reserved(*c, now))) {
      res.reason = reason;
      m.count(c, &Counters::declines);
      m.log_decline(c, key, "subscription", reason, now);
    } else {
      const seconds life = clamp_ttl(ttl);
      std::string id;
      do {
        id = random_id();
      } while (m.pull_points.count(id) > 0 || m.tombstones.count(id) > 0);
      auto pp = m.add_pull_point(key, id, /*adopted=*/false, now, life);
      if (c->up) {
        res.snapshot = m.seed(pp.get(), c, now);
      } else {
        m.maybe_nudge(c, now, &act);
      }
      m.count(c, &Counters::creates);
      res.accepted = true;
      res.id = id;
      res.termination = m.to_system(pp->expiry, now);
      LOG(INFO) << "[event_proxy] " << c->ip << " Protect subscribed via recorder (pp="
                << id8(id) << ", snapshot=" << res.snapshot << ')';
    }
    m.take_fns(&act);
  }
  act.run();
  return res;
}

bool PullPointHub::status_probe(const std::string& key, std::string* reason) {
  bool yes = false;
  std::string why;
  {
    absl::MutexLock lk(&impl_->mu);
    Impl& m = *impl_;
    const SteadyTime now = m.clock->steady_now();
    CameraState* c = m.camera_by_key(key);
    if (c != nullptr) m.refresh_up(c, now);
    why = m.decline_reason(c);
    if (why.empty() && !c->up) why = "camera subscription not up";
    if (why.empty()) {
      yes = true;
      c->reserved_until = now + m.opts.reserve_after_status;
      // Protect's Unsubscribe of its direct subscription can end the
      // recorder's on cameras with a fixed subscription path.
      if (!c->resubscribe_planned_at ||
          now - *c->resubscribe_planned_at >= kResubscribeInterval) {
        c->resubscribe_planned_at = now;
        c->resubscribe_due = now + m.opts.resubscribe_after_status;
      }
      m.count(c, &Counters::status_yes);
      // A warning per camera every kMoveWarnInterval, INFO in between, so
      // a camera that keeps moving back is visible without flooding the
      // journal.
      const bool warn = !c->move_warned_at || now - *c->move_warned_at >= kMoveWarnInterval;
      LOG(LEVEL(warn ? absl::LogSeverity::kWarning : absl::LogSeverity::kInfo))
          << "[event_proxy] " << c->ip
          << " Protect moving its subscription from the camera to the recorder"
          << (warn && c->moves_since_warning > 0
                  ? " (" + std::to_string(c->moves_since_warning) +
                        " more since the last warning)"
                  : std::string());
      if (warn) {
        c->move_warned_at = now;
        c->moves_since_warning = 0;
      } else {
        ++c->moves_since_warning;
      }
    } else {
      m.count(c, &Counters::status_no);
    }
  }
  if (reason != nullptr) *reason = why;
  return yes;
}

PullPointHub::PullResult PullPointHub::pull(const std::string& key, const std::string& id,
                                            std::chrono::milliseconds wait, int limit,
                                            size_t byte_budget) {
  PullResult res;
  Actions act;
  {
    absl::MutexLock lk(&impl_->mu);
    Impl& m = *impl_;
    SteadyTime now = m.clock->steady_now();
    std::shared_ptr<PullPoint> pp = m.find_or_adopt(key, id, now, &act);
    if (!pp) {
      res.status = PullResult::Status::kUnknown;
      res.termination = m.clock->system_now();
    } else {
      CameraState* c = m.camera_by_key(key);
      pp->last_pull = now;
      pp->expiry = std::max(pp->expiry, now + kPullExtension);
      const uint64_t seq = ++pp->pull_seq;
      pp->cv.SignalAll();   // an older held pull on this id returns empty
      m.count(c, &Counters::pulls);
      if (pp->queue.empty() && !m.woken && wait > std::chrono::milliseconds::zero()) {
        // Measured on the steady clock, so a wall-clock step cannot
        // stretch a held pull.
        const auto deadline = std::chrono::steady_clock::now() + wait;
        while (pp->queue.empty() && !pp->removed && pp->pull_seq == seq && !m.woken) {
          const auto left = deadline - std::chrono::steady_clock::now();
          if (left <= std::chrono::steady_clock::duration::zero()) break;
          pp->cv.WaitWithTimeout(&m.mu, absl::FromChrono(left));
        }
      }
      if (!pp->removed && pp->pull_seq == seq && !pp->queue.empty()) {
        const size_t max_n = static_cast<size_t>(std::max(limit, 1));
        size_t bytes = 0;
        while (!pp->queue.empty() && res.messages.size() < max_n) {
          const size_t sz = pp->queue.front().xml->size();
          if (!res.messages.empty() && bytes + sz > byte_budget) break;
          bytes += sz;
          pp->queue_bytes -= sz;
          res.messages.push_back(std::move(pp->queue.front().xml));
          pp->queue.pop_front();
        }
        pp->delivered += res.messages.size();
        m.count(c, &Counters::msgs_out, res.messages.size());
      } else if (m.woken) {
        res.status = PullResult::Status::kStopped;
      }
      now = m.clock->steady_now();
      res.termination = m.termination_of(*pp, now);
    }
    m.take_fns(&act);
  }
  act.run();
  return res;
}

PullPointHub::RenewResult PullPointHub::renew(const std::string& key, const std::string& id,
                                              std::optional<std::chrono::seconds> ttl) {
  RenewResult res;
  Actions act;
  {
    absl::MutexLock lk(&impl_->mu);
    Impl& m = *impl_;
    const SteadyTime now = m.clock->steady_now();
    std::shared_ptr<PullPoint> pp = m.find_or_adopt(key, id, now, &act);
    CameraState* c = m.camera_by_key(key);
    if (!pp) {
      res.reason = "unknown subscription";
      m.count(c, &Counters::renew_faults);
    } else {
      switch (m.state_of(*pp, now)) {
        case PpState::kServing: {
          const seconds life = clamp_ttl(ttl);
          pp->expiry = now + life;
          res.ok = true;
          res.termination = m.to_system(pp->expiry, now);
          m.count(c, &Counters::renews);
          break;
        }
        case PpState::kProvisional:
          pp->expiry = std::max(pp->expiry, now + kSteerTtl);
          res.ok = true;
          res.termination = m.clock->system_now() + kSteerTtl;
          m.count(c, &Counters::renews);
          break;
        case PpState::kDraining:
          res.reason = m.decline_reason(c);
          if (res.reason.empty()) res.reason = "draining";
          m.count(c, &Counters::renew_faults);
          m.log_decline(c, key, "renew", res.reason, now);
          break;
      }
    }
    m.take_fns(&act);
  }
  act.run();
  return res;
}

void PullPointHub::unsubscribe(const std::string& key, const std::string& id) {
  absl::MutexLock lk(&impl_->mu);
  Impl& m = *impl_;
  if (!valid_id(id)) return;
  const SteadyTime now = m.clock->steady_now();
  auto it = m.pull_points.find(id);
  if (it != m.pull_points.end()) {
    if (it->second->key != key) return;
    LOG(INFO) << "[event_proxy] " << Impl::label(m.camera_by_key(key), key)
              << " Protect unsubscribed (pp=" << id8(id) << ')';
    m.remove_pull_point(id);
  }
  m.tombstone(id, now);
}

bool PullPointHub::sync_point(const std::string& key, const std::string& id) {
  Actions act;
  bool ok = false;
  {
    absl::MutexLock lk(&impl_->mu);
    Impl& m = *impl_;
    const SteadyTime now = m.clock->steady_now();
    std::shared_ptr<PullPoint> pp = m.find_or_adopt(key, id, now, &act);
    if (pp) {
      ok = true;
      CameraState* c = m.camera_by_key(key);
      if (c != nullptr) {
        m.refresh_up(c, now);
        if (c->up) m.seed(pp.get(), c, now);
      }
    }
    m.take_fns(&act);
  }
  act.run();
  return ok;
}

void PullPointHub::tick() {
  Actions act;
  {
    absl::MutexLock lk(&impl_->mu);
    Impl& m = *impl_;
    const SteadyTime now = m.clock->steady_now();

    // A pull point outlives the TerminationTime it reported by up to
    // pull_point_grace after its last pull: Protect sometimes pauses
    // polling for about a minute and then pulls the same id again.
    std::vector<std::shared_ptr<PullPoint>> expired;
    for (const auto& kv : m.pull_points) {
      const PullPoint& pp = *kv.second;
      if (now > std::max(pp.expiry, pp.last_pull + m.opts.pull_point_grace)) {
        expired.push_back(kv.second);
      }
    }
    for (const auto& pp : expired) {
      CameraState* c = m.camera_by_key(pp->key);
      m.count(c, &Counters::expiries);
      LOG(INFO) << "[event_proxy] " << Impl::label(c, pp->key)
                << " Protect subscription expired (pp=" << id8(pp->id) << ", last pull "
                << whole_seconds(now - pp->last_pull) << "s ago)";
      m.remove_pull_point(pp->id);
    }
    m.prune_tombstones(now);

    for (auto& kv : m.cameras) {
      CameraState* c = &kv.second;
      m.refresh_up(c, now);
      m.evaluate_handback(c, now);
      if (c->resubscribe_due && now >= *c->resubscribe_due) {
        c->resubscribe_due.reset();
        ++c->counters.resubscribes;
        LOG(INFO) << "[event_proxy] " << c->ip
                  << " renewing the camera subscription after Protect moved to the recorder";
        act.resubscribes.push_back(c->ip);
      }
    }

    if (!m.ready && !m.not_ready_warned && now - m.started_at >= m.opts.not_ready_limit) {
      m.not_ready_warned = true;
      LOG(WARNING) << "[event_proxy] camera list still not loaded "
                   << whole_seconds(now - m.started_at)
                   << "s after start; Protect subscriptions kept meanwhile are handed back";
    }

    if (now - m.summary_at >= kSummaryInterval) {
      const Counters& h = m.hourly;
      LOG(INFO) << "[event_proxy] last 1h: creates=" << h.creates
                << " declines=" << h.declines << " status_yes=" << h.status_yes
                << " status_no=" << h.status_no << " renews=" << h.renews
                << " renew_faults=" << h.renew_faults << " pulls=" << h.pulls
                << " msgs_out=" << h.msgs_out << " drops=" << h.drops
                << " adoptions=" << h.adoptions << " expiries=" << h.expiries
                << " handbacks=" << h.handbacks << " evictions=" << h.evictions;
      m.hourly = Counters();
      m.summary_at = now;
    }
    m.take_fns(&act);
  }
  act.run();
  impl_->persist(false);
}

void PullPointHub::start() {
  absl::MutexLock lk(&impl_->mu);
  impl_->woken = false;
  impl_->started_at = impl_->clock->steady_now();
  if (impl_->thread.joinable()) return;
  impl_->thread_stop = false;
  impl_->thread = std::thread([this] { impl_->thread_main(this); });
}

void PullPointHub::wake_all() {
  absl::MutexLock lk(&impl_->mu);
  impl_->woken = true;
  impl_->wake_pulls();
}

void PullPointHub::stop() {
  std::thread t;
  {
    absl::MutexLock lk(&impl_->mu);
    impl_->woken = true;
    impl_->thread_stop = true;
    impl_->thread_cv.SignalAll();
    impl_->wake_pulls();
    t = std::move(impl_->thread);
  }
  if (t.joinable()) t.join();
  impl_->persist(true);
}

std::string PullPointHub::snapshot_json() const {
  absl::MutexLock lk(&impl_->mu);
  Impl& m = *impl_;
  const SteadyTime now = m.clock->steady_now();
  const int64_t now_ms = m.now_ms();
  auto age_or_null = [now_ms](int64_t ms) {
    return ms == 0 ? std::string("null") : std::to_string(std::max<int64_t>(0, now_ms - ms) / 1000);
  };
  std::ostringstream o;
  o << "{\"ready\":" << (m.ready ? "true" : "false") << ",\"cameras\":[";
  bool first_cam = true;
  size_t registered_pps = 0;
  for (auto& kv : m.cameras) {
    CameraState& c = kv.second;
    const bool up = m.is_up(c, now);
    const std::string reason = m.decline_reason(&c);
    if (!first_cam) o << ',';
    first_cam = false;
    o << "{\"key\":" << util::json_str(c.key) << ",\"ip\":" << util::json_str(c.ip)
      << ",\"mac\":" << util::json_str(c.mac) << ",\"proxied\":" << (c.proxied ? "true" : "false")
      << ",\"accepting\":" << (reason.empty() ? "true" : "false")
      << ",\"reason\":" << util::json_str(reason) << ",\"upstream\":\""
      << (up ? "up" : (c.seen_upstream ? "down" : "unknown")) << '"'
      << ",\"failure_class\":\"" << failure_class_name(c.failure) << '"'
      << ",\"failure_detail\":" << util::json_str(c.failure_detail)
      << ",\"subscribed_age_s\":"
      << (c.subscribed ? std::to_string(whole_seconds(now - c.subscribed_at)) : "null")
      << ",\"last_fragment_age_s\":" << age_or_null(c.p.last_fragment_ms)
      << ",\"last_up_age_s\":" << (up ? std::string("0") : age_or_null(c.p.last_up_ms))
      << ",\"slot_limited_age_s\":" << age_or_null(c.p.slot_limited_ms)
      << ",\"handed_back\":" << util::json_str(c.p.handed_back)
      << ",\"cache_entries\":" << c.cache.size() << ",\"pull_points\":[";
    bool first_pp = true;
    for (const auto& pkv : m.pull_points) {
      const PullPoint& pp = *pkv.second;
      if (pp.key != c.key) continue;
      ++registered_pps;
      if (!first_pp) o << ',';
      first_pp = false;
      o << "{\"id\":" << util::json_str(id8(pp.id))
        << ",\"age_s\":" << whole_seconds(now - pp.created)
        << ",\"last_pull_age_s\":" << whole_seconds(now - pp.last_pull)
        << ",\"queued\":" << pp.queue.size() << ",\"delivered\":" << pp.delivered
        << ",\"dropped\":" << pp.dropped << ",\"state\":\""
        << pp_state_name(m.state_of(pp, now)) << "\",\"adopted\":"
        << (pp.adopted ? "true" : "false") << '}';
    }
    o << "],\"counters\":";
    write_counters(&o, c.counters);
    o << '}';
  }
  o << "],\"unregistered_pull_points\":" << (m.pull_points.size() - registered_pps) << '}';
  return o.str();
}

std::string PullPointHub::heartbeat_suffix(const std::string& ip) const {
  absl::MutexLock lk(&impl_->mu);
  Impl& m = *impl_;
  const CameraState* c = m.camera_by_ip(ip);
  if (c == nullptr || !c->proxied) return "";
  size_t n = 0;
  SteadyTime latest;
  for (const auto& kv : m.pull_points) {
    if (kv.second->key != c->key) continue;
    if (n == 0 || kv.second->last_pull > latest) latest = kv.second->last_pull;
    ++n;
  }
  if (n == 0) return " protect_pp=0";
  return " protect_pp=" + std::to_string(n) + " protect_pull_age=" +
         std::to_string(whole_seconds(m.clock->steady_now() - latest)) + "s";
}

}  // namespace pullpoint
}  // namespace onvif
