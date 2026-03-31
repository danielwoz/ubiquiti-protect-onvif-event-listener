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

#include "camera_emulators.hpp"

#include <algorithm>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// ============================================================
// Minimal JSON parser for our own raw-log format.
//
// Fields of interest:
//   "camera_ip":       string
//   "soap_action":     string
//   "response_status": number
//   "response":        string (escaped SOAP XML)
// ============================================================
namespace {

// Scan a JSON string value starting just after the opening '"'.
// Advances pos past the closing '"'.
std::string scan_string(const std::string& s, std::size_t& pos) {
  std::string out;
  while (pos < s.size() && s[pos] != '"') {
    if (s[pos] == '\\' && pos + 1 < s.size()) {
      ++pos;
      switch (s[pos]) {
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case '/':  out += '/';  break;
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case 't':  out += '\t'; break;
        case 'u':
          if (pos + 4 < s.size()) {
            unsigned cp = std::stoul(s.substr(pos + 1, 4), nullptr, 16);
            // Only handle ASCII range for our use case
            out += (cp < 0x80) ? static_cast<char>(cp) : '?';
            pos += 4;
          }
          break;
        default:
          out += s[pos];
          break;
      }
    } else {
      out += s[pos];
    }
    ++pos;
  }
  if (pos < s.size()) ++pos;  // consume closing '"'
  return out;
}

struct ParsedEntry {
  std::string camera_ip;
  std::string soap_action;
  int64_t     response_status{0};
  std::string response;
};

ParsedEntry parse_line(const std::string& line) {
  ParsedEntry e;
  std::size_t pos = 0;

  // Find '{'
  while (pos < line.size() && line[pos] != '{') ++pos;
  if (pos >= line.size()) return e;
  ++pos;

  while (pos < line.size() && line[pos] != '}') {
    // Skip commas and spaces between key-value pairs
    while (pos < line.size() &&
           (line[pos] == ',' || line[pos] == ' ')) ++pos;
    if (pos >= line.size() || line[pos] == '}') break;
    if (line[pos] != '"') break;
    ++pos;

    std::string key = scan_string(line, pos);

    // Skip ':'
    while (pos < line.size() && line[pos] != ':') ++pos;
    ++pos;

    if (pos < line.size() && line[pos] == '"') {
      // String value
      ++pos;
      std::string val = scan_string(line, pos);
      if      (key == "camera_ip")   e.camera_ip   = val;
      else if (key == "soap_action") e.soap_action = val;
      else if (key == "response")    e.response    = val;
      // "timestamp", "url", "request" intentionally ignored
    } else {
      // Numeric value
      std::string num;
      while (pos < line.size() &&
             line[pos] != ',' && line[pos] != '}')
        num += line[pos++];
      if (key == "response_status" && !num.empty())
        e.response_status = std::stol(num);
    }
  }
  return e;
}

// Read the first camera_ip value from a JSONL file so emulators can
// pass the correct real IP to rewrite_urls() without hardcoding it.
std::string peek_camera_ip(const std::string& path) {
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto e = parse_line(line);
    if (!e.camera_ip.empty()) return e.camera_ip;
  }
  return "";
}

std::string action_tail(const std::string& soap_action) {
  auto p = soap_action.rfind('/');
  return (p != std::string::npos) ? soap_action.substr(p + 1) : soap_action;
}

}  // namespace

// ============================================================
// make_get_services_response -- declared in camera_emulators.hpp
// ============================================================
std::string make_get_services_response(const std::string& real_ip,
                                        const std::string& alarm_url) {
  std::string resp =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<SOAP-ENV:Envelope"
    "  xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\""
    "  xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\""
    "  xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
    "<SOAP-ENV:Body>"
    "<tds:GetServicesResponse>"
    "<tds:Service>"
    "<tds:Namespace>http://www.onvif.org/ver10/events/wsdl</tds:Namespace>"
    "<tds:XAddr>http://" + real_ip + "/onvif/event_service</tds:XAddr>"
    "<tds:Version>"
    "<tt:Major>2</tt:Major><tt:Minor>60</tt:Minor>"
    "</tds:Version>"
    "</tds:Service>";

  if (!alarm_url.empty()) {
    resp +=
      "<tds:Service>"
      "<tds:Namespace>http://www.ubnt.com/ver10/alarm/wsdl</tds:Namespace>"
      "<tds:XAddr>" + alarm_url + "</tds:XAddr>"
      "<tds:Version>"
      "<tt:Major>1</tt:Major><tt:Minor>0</tt:Minor>"
      "</tds:Version>"
      "</tds:Service>";
  }

  resp +=
    "</tds:GetServicesResponse>"
    "</SOAP-ENV:Body>"
    "</SOAP-ENV:Envelope>";
  return resp;
}

namespace {  // re-open anonymous namespace for emulator implementations

// Advance through a response sequence:
//   - clamp:  clamps at last entry (used for CreatePullPointSubscription / Renew
//             so the last successful 200 keeps being returned)
//   - cycle:  wraps around (used for PullMessages so events repeat indefinitely)
std::pair<int, std::string> next_clamp(
  const std::vector<RecordedExchange>& vec, std::size_t& idx) {
  if (vec.empty()) return {500, ""};
  const auto& ex = vec[std::min(idx, vec.size() - 1)];
  if (idx < vec.size()) ++idx;
  return {ex.status, ex.response};
}

std::pair<int, std::string> next_cycle(
  const std::vector<RecordedExchange>& vec, std::size_t& idx) {
  if (vec.empty()) return {200, ""};
  const auto& ex = vec[idx % vec.size()];
  ++idx;
  return {ex.status, ex.response};
}

}  // anonymous namespace

// ============================================================
// RecordedSession::from_jsonl
// ============================================================
RecordedSession RecordedSession::from_jsonl(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) {
    std::fprintf(stderr, "Fatal: Cannot open raw log: %s\n", path.c_str());
    std::abort();
  }

  RecordedSession session;
  std::string line;

  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto e = parse_line(line);

    RecordedExchange ex{static_cast<int>(e.response_status), e.response};
    const auto tail = action_tail(e.soap_action);

    if      (tail == "CreatePullPointSubscriptionRequest") session.create_sub.push_back(ex);
    else if (tail == "PullMessagesRequest")                session.pull.push_back(ex);
    else if (tail == "RenewRequest")                       session.renew.push_back(ex);
  }

  if (session.create_sub.empty()) {
    std::fprintf(stderr, "Fatal: No CreatePullPointSubscription data in: %s\n",
                 path.c_str());
    std::abort();
  }
  // pull may be empty for cameras that never reach PullMessages (e.g. always-404).

  return session;
}

// ============================================================
// HikvisionCompatibleEmulator
// ============================================================
HikvisionCompatibleEmulator::HikvisionCompatibleEmulator(
    const std::string& jsonl_path)
  : OnvifCameraEmulator(peek_camera_ip(jsonl_path)) {
  session_ = RecordedSession::from_jsonl(jsonl_path);
}

std::pair<int, std::string> HikvisionCompatibleEmulator::handle(
  const std::string& /*path*/,
  const std::string& soap_action,
  const std::string& /*body*/) {
  std::lock_guard<std::mutex> lk(mu_);
  const auto tail = action_tail(soap_action);

  std::pair<int, std::string> resp;
  if      (tail == "GetServicesRequest")
    resp = {200, make_get_services_response(real_ip_, alarm_service_url_)};
  else if (tail == "CreatePullPointSubscriptionRequest")
    resp = next_clamp(session_.create_sub, create_idx_);
  else if (tail == "PullMessagesRequest")
    resp = next_cycle(session_.pull, pull_idx_);
  else if (tail == "RenewRequest")
    resp = next_clamp(session_.renew, renew_idx_);
  else
    resp = {400, ""};

  resp.second = rewrite_urls(resp.second);
  return resp;
}

// ============================================================
// CellMotionCameraEmulator
// ============================================================
CellMotionCameraEmulator::CellMotionCameraEmulator(const std::string& jsonl_path)
  : OnvifCameraEmulator(peek_camera_ip(jsonl_path)) {
  session_ = RecordedSession::from_jsonl(jsonl_path);
}

std::pair<int, std::string> CellMotionCameraEmulator::handle(
  const std::string& /*path*/,
  const std::string& soap_action,
  const std::string& /*body*/) {
  std::lock_guard<std::mutex> lk(mu_);
  const auto tail = action_tail(soap_action);

  std::pair<int, std::string> resp;
  if      (tail == "GetServicesRequest")
    resp = {200, make_get_services_response(real_ip_, alarm_service_url_)};
  else if (tail == "CreatePullPointSubscriptionRequest")
    resp = next_clamp(session_.create_sub, create_idx_);
  else if (tail == "PullMessagesRequest")
    resp = next_cycle(session_.pull, pull_idx_);
  else if (tail == "RenewRequest")
    resp = next_clamp(session_.renew, renew_idx_);
  else
    resp = {400, ""};

  resp.second = rewrite_urls(resp.second);
  return resp;
}

// ============================================================
// ThinginoCameraEmulator
// ============================================================
ThinginoCameraEmulator::ThinginoCameraEmulator(const std::string& jsonl_path)
  : OnvifCameraEmulator(peek_camera_ip(jsonl_path)) {
  session_ = RecordedSession::from_jsonl(jsonl_path);
}

std::pair<int, std::string> ThinginoCameraEmulator::handle(
  const std::string& /*path*/,
  const std::string& soap_action,
  const std::string& /*body*/) {
  std::lock_guard<std::mutex> lk(mu_);
  // GetServices: this camera returns 404 for everything; return without
  // consuming the create_sub replay sequence.
  if (action_tail(soap_action) == "GetServicesRequest")
    return {404, ""};
  return next_clamp(session_.create_sub, create_idx_);
}

// ============================================================
// Html404CameraEmulator
// ============================================================
Html404CameraEmulator::Html404CameraEmulator(const std::string& jsonl_path)
  : OnvifCameraEmulator(peek_camera_ip(jsonl_path)) {
  session_ = RecordedSession::from_jsonl(jsonl_path);
}

std::pair<int, std::string> Html404CameraEmulator::handle(
  const std::string& /*path*/,
  const std::string& soap_action,
  const std::string& /*body*/) {
  std::lock_guard<std::mutex> lk(mu_);
  // GetServices: this camera returns HTML 404 for everything; return without
  // consuming the create_sub replay sequence.
  if (action_tail(soap_action) == "GetServicesRequest") {
    const auto& ex = session_.create_sub.front();
    return {ex.status, ex.response};
  }
  return next_clamp(session_.create_sub, create_idx_);
}

// ============================================================
// DahuaSD4A425DBEmulator
// ============================================================
DahuaSD4A425DBEmulator::DahuaSD4A425DBEmulator(const std::string& jsonl_path)
  : OnvifCameraEmulator(peek_camera_ip(jsonl_path)) {
  session_ = RecordedSession::from_jsonl(jsonl_path);
}

std::pair<int, std::string> DahuaSD4A425DBEmulator::handle(
  const std::string& /*path*/,
  const std::string& soap_action,
  const std::string& /*body*/) {
  std::lock_guard<std::mutex> lk(mu_);
  const auto tail = action_tail(soap_action);

  // CreatePullPointSubscription: clamp -- replays the recorded 400s then
  // stays on the final 200, so the retry-then-succeed path is exercised.
  // PullMessages: cycle -- events repeat indefinitely.
  std::pair<int, std::string> resp;
  if      (tail == "GetServicesRequest")
    resp = {200, make_get_services_response(real_ip_, alarm_service_url_)};
  else if (tail == "CreatePullPointSubscriptionRequest")
    resp = next_clamp(session_.create_sub, create_idx_);
  else if (tail == "PullMessagesRequest")
    resp = next_cycle(session_.pull, pull_idx_);
  else
    resp = {400, ""};

  resp.second = rewrite_urls(resp.second);
  return resp;
}

// ============================================================
// UosEmulator
// ============================================================

UosEmulator::UosEmulator() : OnvifCameraEmulator("uos-emulator") {}

void UosEmulator::set_alarms_json(const std::string& json) {
  std::lock_guard<std::mutex> lk(mu_);
  alarms_json_ = json;
}

std::vector<std::string> UosEmulator::posted_events() const {
  std::lock_guard<std::mutex> lk(mu_);
  return posted_;
}

std::string UosEmulator::base_url() const {
  return "http://127.0.0.1:" + std::to_string(port());
}

std::pair<int, std::string> UosEmulator::handle(
    const std::string& path,
    const std::string& /*soap_action*/,
    const std::string& body) {
  std::lock_guard<std::mutex> lk(mu_);
  if (path == "/api/v1/alarms" && body.empty())
    return {200, alarms_json_};
  if (path == "/api/v1/alarms/events" && !body.empty()) {
    posted_.push_back(body);
    return {200, "{}"};
  }
  return {404, ""};
}
