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

#include "pullpoint_server.hpp"

#include <arpa/inet.h>
#include <libxml/parser.h>
#include <microhttpd.h>
#include <netinet/in.h>

#if MHD_VERSION < 0x00097100
typedef int MHD_Result;
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include "absl/log/log.h"
#include "pullpoint_hub.hpp"
#include "pullpoint_soap.hpp"

namespace onvif {
namespace pullpoint {

namespace {

using std::chrono::milliseconds;
using std::chrono::system_clock;
using soap::FaultKind;

constexpr size_t kMaxRequestBytes = 64 * 1024;
constexpr size_t kMaxResponseBytes = 512 * 1024;
// Room kept in a PullMessages response for the envelope around the
// messages, so the whole response stays within kMaxResponseBytes.
constexpr size_t kEnvelopeReserve = 4 * 1024;
constexpr int kMaxMessageLimit = 100;
constexpr milliseconds kDefaultPullTimeout{2000};
// Longest a PullMessages is held.  Protect gives up on the recorder 4 s
// after sending a request, and messages in an answer that arrives after
// that are lost (they have left the queue), so the hold leaves Protect a
// wide margin even when its event loop is slow to read the answer.  A
// message that arrives after the hold goes out with Protect's next poll.
constexpr milliseconds kMaxPullWait{1000};

constexpr unsigned int kConnectionLimit = 32;
constexpr unsigned int kConnectionTimeoutSec = 15;
constexpr size_t kConnectionMemoryLimit = 128 * 1024;
constexpr milliseconds kBindRetryInterval{500};
// stop() lets answers that are already on their way finish for this long
// before the listener and its connections are closed.
constexpr milliseconds kDrainLimit{500};

constexpr unsigned int kStatusOk = 200;
constexpr unsigned int kStatusMethodNotAllowed = 405;
constexpr unsigned int kStatusFault = 500;

constexpr std::chrono::seconds kLogInterval{60};
constexpr size_t kMaxLoggedKeys = 256;
constexpr size_t kMaxLoggedText = 200;

constexpr char kContentType[] = "application/soap+xml; charset=utf-8";

struct Reply {
  unsigned int status = kStatusOk;
  std::string body;
};

Reply ok(std::string body) { return Reply{kStatusOk, std::move(body)}; }

Reply failure(FaultKind kind, const std::string& reason,
              unsigned int status = kStatusFault) {
  return Reply{status, soap::fault(kind, reason)};
}

// A request's body, collected across libmicrohttpd's upload callbacks.
struct RequestState {
  std::string body;
  bool too_big = false;
};

// Request text for a log line or a fault reason: at most kMaxLoggedText
// bytes, every byte outside printable ASCII replaced by '?', so the
// fault stays well-formed UTF-8 XML whatever the client sent.
std::string clip(const std::string& s) {
  const size_t n = std::min(s.size(), kMaxLoggedText);
  std::string out;
  out.reserve(n + 3);
  for (size_t i = 0; i < n; ++i) {
    const unsigned char ch = static_cast<unsigned char>(s[i]);
    out.push_back(ch >= 0x20 && ch < 0x7f ? static_cast<char>(ch) : '?');
  }
  if (s.size() > n) out += "...";
  return out;
}

bool starts_with_ci(const std::string& s, const char* prefix) {
  const size_t n = std::strlen(prefix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) != prefix[i]) return false;
  }
  return true;
}

// Request path from the request target, dropping the scheme and
// authority of an absolute-form target.
std::string request_path(const char* url) {
  std::string path = url != nullptr ? url : "";
  for (const char* scheme : {"http://", "https://"}) {
    if (starts_with_ci(path, scheme)) {
      const size_t slash = path.find('/', std::strlen(scheme));
      return slash == std::string::npos ? std::string("/") : path.substr(slash);
    }
  }
  return path;
}

// The request target is passed to the handler still percent-encoded, so
// the camera part of the path can be echoed back exactly as Protect sent
// it and decoded segment by segment.
size_t keep_escapes(void* /*cls*/, struct MHD_Connection* /*conn*/, char* s) {
  return std::strlen(s);
}

std::optional<int> parse_int(const std::string& text) {
  if (text.empty()) return std::nullopt;
  errno = 0;
  char* end = nullptr;
  const long v = std::strtol(text.c_str(), &end, 10);  // NOLINT(runtime/int)
  if (errno != 0 || end == text.c_str() || *end != '\0') return std::nullopt;
  if (v < -1000000 || v > 1000000) return v < 0 ? -1000000 : 1000000;
  return static_cast<int>(v);
}

MHD_Result queue_reply(struct MHD_Connection* conn, const Reply& reply) {
  struct MHD_Response* resp = MHD_create_response_from_buffer(
      reply.body.size(), const_cast<char*>(reply.body.data()), MHD_RESPMEM_MUST_COPY);
  if (resp == nullptr) return MHD_NO;
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, kContentType);
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONNECTION, "close");
  if (reply.status == kStatusMethodNotAllowed) {
    MHD_add_response_header(resp, MHD_HTTP_HEADER_ALLOW, "POST");
  }
  const MHD_Result ret = MHD_queue_response(conn, reply.status, resp);
  MHD_destroy_response(resp);
  return ret;
}

}  // namespace

// ---------------------------------------------------------------------
// PullPointServer::Impl
// ---------------------------------------------------------------------

struct PullPointServer::Impl {
  explicit Impl(PullPointHub* h) : hub(h) {}

  PullPointHub* const hub;

  std::mutex lifecycle_mu;               // serialises start() and stop()
  struct MHD_Daemon* daemon = nullptr;   // guarded by lifecycle_mu
  std::atomic<bool> is_running{false};
  std::atomic<uint16_t> bound_port{0};

  // Requests between their first callback and their completion.
  std::mutex flight_mu;
  std::condition_variable flight_cv;
  int in_flight = 0;

  std::mutex log_mu;
  std::set<std::string> logged_service_paths;  // camera keys
  std::map<std::string, std::chrono::steady_clock::time_point> last_log;

  void begin_request() {
    std::lock_guard<std::mutex> lk(flight_mu);
    ++in_flight;
  }

  void end_request() {
    std::lock_guard<std::mutex> lk(flight_mu);
    if (--in_flight == 0) flight_cv.notify_all();
  }

  // Wait until no request is being handled or answered, at most @p limit.
  void drain(milliseconds limit) {
    std::unique_lock<std::mutex> lk(flight_mu);
    flight_cv.wait_for(lk, limit, [this] { return in_flight == 0; });
  }

  // True at most once per kLogInterval for each @p topic.
  bool log_due(const std::string& topic) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(log_mu);
    auto it = last_log.find(topic);
    if (it != last_log.end() && now - it->second < kLogInterval) return false;
    last_log[topic] = now;
    return true;
  }

  void note_bad_request(const std::string& path, const std::string& reason) {
    if (!log_due("bad request")) return;
    LOG(WARNING) << "[event_proxy] refused a request for " << clip(path) << ": " << reason;
  }

  void note_service_path(const soap::Route& route) {
    std::lock_guard<std::mutex> lk(log_mu);
    if (logged_service_paths.size() >= kMaxLoggedKeys) return;
    if (!logged_service_paths.insert(route.key).second) return;
    LOG(INFO) << "[event_proxy] " << route.key << " Protect's event service path is "
              << (route.suffix.empty() ? std::string("(none)") : clip(route.suffix));
  }

  // A requested lifetime: nullopt (the hub's default) when absent or
  // unreadable, zero when it lies in the past.
  std::optional<std::chrono::seconds> lifetime(const std::string* text,
                                               system_clock::time_point now) {
    if (text == nullptr || text->empty()) return std::nullopt;
    const std::optional<milliseconds> ms = soap::parse_time_offset(*text, now);
    if (!ms) {
      if (log_due("time value")) {
        LOG(INFO) << "[event_proxy] unreadable time value '" << clip(*text)
                  << "', using the default";
      }
      return std::nullopt;
    }
    if (ms->count() <= 0) return std::chrono::seconds(0);
    return std::chrono::seconds((ms->count() + 999) / 1000);
  }

  Reply handle(const char* method, const char* url, const RequestState& rs);
  Reply create(const soap::Route& route, const soap::Request& req);
  Reply status(const soap::Route& route);
  Reply subscription(const soap::Route& route, const soap::Request& req);

  static MHD_Result on_request(void* cls, struct MHD_Connection* conn, const char* url,
                               const char* method, const char* version,
                               const char* upload_data, size_t* upload_data_size,
                               void** con_cls);
  static void on_completed(void* cls, struct MHD_Connection* conn, void** con_cls,
                           enum MHD_RequestTerminationCode toe);
  static void on_mhd_log(void* cls, const char* fmt, va_list ap);
};

Reply PullPointServer::Impl::handle(const char* method, const char* url,
                                    const RequestState& rs) {
  const std::string path = request_path(url);
  const std::string verb = method != nullptr ? method : "";
  if (verb != "POST") {
    note_bad_request(path, "method " + clip(verb));
    return failure(FaultKind::kInvalidArgVal, clip(verb) + " is not supported, use POST",
                   kStatusMethodNotAllowed);
  }
  if (rs.too_big) {
    note_bad_request(path, "body larger than 64 KiB");
    return failure(FaultKind::kInvalidArgVal, "request larger than 64 KiB");
  }
  const soap::Route route = soap::parse_route(path);
  if (route.kind == soap::Route::Kind::kInvalid) {
    note_bad_request(path, route.error);
    return failure(FaultKind::kInvalidArgVal, route.error);
  }
  soap::Request req;
  std::string error;
  if (!soap::parse_request(rs.body, &req, &error)) {
    note_bad_request(path, error);
    return failure(FaultKind::kInvalidArgVal, error);
  }
  if (hub == nullptr) return failure(FaultKind::kActionNotSupported, "not available");
  switch (route.kind) {
    case soap::Route::Kind::kCreate:
      if (req.op == "CreatePullPointSubscription") return create(route, req);
      break;
    case soap::Route::Kind::kStatus:
      if (req.op == "GetServiceCapabilities") return status(route);
      break;
    case soap::Route::Kind::kSubscription:
      return subscription(route, req);
    case soap::Route::Kind::kInvalid:
      break;
  }
  return failure(FaultKind::kActionNotSupported, clip(req.op) + " is not supported here");
}

Reply PullPointServer::Impl::create(const soap::Route& route, const soap::Request& req) {
  if (req.param("Filter") != nullptr || req.param("SubscriptionPolicy") != nullptr) {
    return failure(FaultKind::kActionNotSupported,
                   "filters and subscription policies are not supported");
  }
  note_service_path(route);
  const auto ttl = lifetime(req.param("InitialTerminationTime"), system_clock::now());
  const PullPointHub::CreateResult cr = hub->create(route.key, ttl);
  if (!cr.accepted) return failure(FaultKind::kActionNotSupported, cr.reason);
  const std::string address = "http://127.0.0.1:" + std::to_string(bound_port.load()) +
                              route.base + "/s/" + cr.id;
  return ok(soap::create_response(address, system_clock::now(), cr.termination));
}

Reply PullPointServer::Impl::status(const soap::Route& route) {
  std::string reason;
  if (hub->status_probe(route.key, &reason)) return ok(soap::capabilities_response());
  return failure(FaultKind::kActionNotSupported, reason.empty() ? "not accepting" : reason);
}

Reply PullPointServer::Impl::subscription(const soap::Route& route, const soap::Request& req) {
  if (req.op == "PullMessages") {
    milliseconds timeout = kDefaultPullTimeout;
    const std::string* timeout_text = req.param("Timeout");
    if (timeout_text != nullptr && !timeout_text->empty()) {
      const auto ms = soap::parse_time_offset(*timeout_text, system_clock::now());
      if (ms) {
        timeout = std::max(*ms, milliseconds::zero());
      } else if (log_due("time value")) {
        LOG(INFO) << "[event_proxy] unreadable PullMessages Timeout '" << clip(*timeout_text)
                  << "', using PT2S";
      }
    }
    int limit = kMaxMessageLimit;
    const std::string* limit_text = req.param("MessageLimit");
    if (limit_text != nullptr) {
      const std::optional<int> v = parse_int(*limit_text);
      if (v) limit = std::clamp(*v, 1, kMaxMessageLimit);
    }
    const PullPointHub::PullResult pr = hub->pull(route.key, route.id,
                                                  std::min(timeout, kMaxPullWait), limit,
                                                  kMaxResponseBytes - kEnvelopeReserve);
    if (pr.status == PullPointHub::PullResult::Status::kUnknown) {
      return failure(FaultKind::kResourceUnknown, "unknown subscription");
    }
    // kOk, or kStopped (shutting down): both are an ordinary answer.
    const system_clock::time_point now = system_clock::now();
    const system_clock::time_point termination =
        pr.termination == system_clock::time_point() ? now : pr.termination;
    return ok(soap::pull_response(now, termination, pr.messages));
  }
  if (req.op == "Renew") {
    const auto ttl = lifetime(req.param("TerminationTime"), system_clock::now());
    const PullPointHub::RenewResult rr = hub->renew(route.key, route.id, ttl);
    if (!rr.ok) {
      return failure(FaultKind::kActionNotSupported,
                     rr.reason.empty() ? "renew refused" : rr.reason);
    }
    return ok(soap::renew_response(system_clock::now(), rr.termination));
  }
  if (req.op == "Unsubscribe") {
    hub->unsubscribe(route.key, route.id);
    return ok(soap::unsubscribe_response());
  }
  if (req.op == "SetSynchronizationPoint") {
    if (!hub->sync_point(route.key, route.id)) {
      return failure(FaultKind::kResourceUnknown, "unknown subscription");
    }
    return ok(soap::sync_point_response());
  }
  return failure(FaultKind::kActionNotSupported, clip(req.op) + " is not supported here");
}

MHD_Result PullPointServer::Impl::on_request(void* cls, struct MHD_Connection* conn,
                                             const char* url, const char* method,
                                             const char* /*version*/, const char* upload_data,
                                             size_t* upload_data_size, void** con_cls) {
  auto* impl = static_cast<Impl*>(cls);
  // libmicrohttpd calls once when the headers are in, once per chunk of
  // body, and once more when the body is complete.
  if (*con_cls == nullptr) {
    *con_cls = new RequestState();
    impl->begin_request();
    return MHD_YES;
  }
  auto* rs = static_cast<RequestState*>(*con_cls);
  if (*upload_data_size > 0) {
    if (!rs->too_big) {
      if (rs->body.size() + *upload_data_size > kMaxRequestBytes) {
        // Keep reading so the answer is not lost to a reset, but drop it.
        rs->too_big = true;
        std::string().swap(rs->body);
      } else {
        rs->body.append(upload_data, *upload_data_size);
      }
    }
    *upload_data_size = 0;
    return MHD_YES;
  }
  return queue_reply(conn, impl->handle(method, url, *rs));
}

void PullPointServer::Impl::on_completed(void* cls, struct MHD_Connection* /*conn*/,
                                         void** con_cls,
                                         enum MHD_RequestTerminationCode /*toe*/) {
  auto* rs = static_cast<RequestState*>(*con_cls);
  if (rs == nullptr) return;
  delete rs;
  *con_cls = nullptr;
  static_cast<Impl*>(cls)->end_request();
}

void PullPointServer::Impl::on_mhd_log(void* cls, const char* fmt, va_list ap) {
  char buf[512];
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  std::string line(buf);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  if (static_cast<Impl*>(cls)->log_due("http")) {
    LOG(WARNING) << "[event_proxy] http: " << line;
  }
}

// ---------------------------------------------------------------------
// PullPointServer
// ---------------------------------------------------------------------

PullPointServer::PullPointServer(PullPointHub* hub) : impl_(new Impl(hub)) {}

PullPointServer::~PullPointServer() { stop(); }

bool PullPointServer::start(uint16_t port, int attempts) {
  std::lock_guard<std::mutex> lk(impl_->lifecycle_mu);
  if (impl_->daemon != nullptr) return true;
  // libxml2 must be initialised before connection threads parse requests.
  xmlInitParser();

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  const unsigned int flags = MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION |
                             MHD_USE_POLL | MHD_USE_ERROR_LOG;
  const int tries = std::max(attempts, 1);
  for (int i = 0; i < tries && impl_->daemon == nullptr; ++i) {
    if (i > 0) std::this_thread::sleep_for(kBindRetryInterval);
    // The logger comes first so it also receives messages about the
    // options that follow it.
    impl_->daemon = MHD_start_daemon(
        flags, port, nullptr, nullptr, &Impl::on_request, impl_.get(),
        MHD_OPTION_EXTERNAL_LOGGER, &Impl::on_mhd_log, impl_.get(),
        MHD_OPTION_SOCK_ADDR, reinterpret_cast<struct sockaddr*>(&addr),
        MHD_OPTION_CONNECTION_LIMIT, kConnectionLimit,
        MHD_OPTION_CONNECTION_TIMEOUT, kConnectionTimeoutSec,
        MHD_OPTION_CONNECTION_MEMORY_LIMIT, kConnectionMemoryLimit,
        MHD_OPTION_NOTIFY_COMPLETED, &Impl::on_completed, impl_.get(),
        MHD_OPTION_UNESCAPE_CALLBACK, &keep_escapes, nullptr,
        MHD_OPTION_END);
  }
  if (impl_->daemon == nullptr) {
    LOG(WARNING) << "[event_proxy] cannot listen on 127.0.0.1:" << port << " after " << tries
                 << " attempt(s)";
    return false;
  }
  const union MHD_DaemonInfo* info =
      MHD_get_daemon_info(impl_->daemon, MHD_DAEMON_INFO_BIND_PORT);
  impl_->bound_port = info != nullptr ? info->port : port;
  impl_->is_running = true;
  LOG(INFO) << "[event_proxy] PullPoint service listening on 127.0.0.1:"
            << impl_->bound_port.load();
  return true;
}

void PullPointServer::stop() {
  std::lock_guard<std::mutex> lk(impl_->lifecycle_mu);
  if (impl_->daemon == nullptr) return;
  impl_->is_running = false;
  // Held pulls answer at once (empty), and their answers go out before
  // the connections are closed.
  if (impl_->hub != nullptr) impl_->hub->wake_all();
  impl_->drain(kDrainLimit);
  MHD_stop_daemon(impl_->daemon);
  impl_->daemon = nullptr;
  LOG(INFO) << "[event_proxy] PullPoint service on 127.0.0.1:" << impl_->bound_port.load()
            << " stopped";
}

bool PullPointServer::running() const { return impl_->is_running.load(); }

uint16_t PullPointServer::port() const { return impl_->bound_port.load(); }

}  // namespace pullpoint
}  // namespace onvif
