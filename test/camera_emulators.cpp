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
#include <chrono>
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

// SOAP 1.2 SetSynchronizationPointResponse.
std::string sync_point_response() {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
         "<SOAP-ENV:Envelope"
         " xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\""
         " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\">"
         "<SOAP-ENV:Body><tev:SetSynchronizationPointResponse/></SOAP-ENV:Body>"
         "</SOAP-ENV:Envelope>";
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
// HikvisionPullPointNotAuthorizedEmulator
// ============================================================
HikvisionPullPointNotAuthorizedEmulator::
HikvisionPullPointNotAuthorizedEmulator(const std::string& jsonl_path)
  : OnvifCameraEmulator(peek_camera_ip(jsonl_path)) {
  session_ = RecordedSession::from_jsonl(jsonl_path);
}

std::pair<int, std::string>
HikvisionPullPointNotAuthorizedEmulator::handle(
  const std::string& /*path*/,
  const std::string& soap_action,
  const std::string& /*body*/) {
  std::lock_guard<std::mutex> lk(mu_);
  const auto tail = action_tail(soap_action);

  // GetServices: succeeds, advertising the events service (matching the
  // real camera's behaviour with the web-UI admin credentials).
  // CreatePullPointSubscription: replays the recorded 400 / NotAuthorized
  // fault.  The listener is expected to give up gracefully after
  // max_consecutive_failures attempts.
  std::pair<int, std::string> resp;
  if      (tail == "GetServicesRequest")
    resp = {200, make_get_services_response(real_ip_, alarm_service_url_)};
  else if (tail == "CreatePullPointSubscriptionRequest")
    resp = next_clamp(session_.create_sub, create_idx_);
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
// AxisReferenceParamsEmulator
// ============================================================
AxisReferenceParamsEmulator::AxisReferenceParamsEmulator()
  : OnvifCameraEmulator("192.168.100.200"),
    token_("urn:uuid:axis-ref-params-test-00000000") {}

std::pair<int, std::string> AxisReferenceParamsEmulator::handle(
    const std::string& /*path*/,
    const std::string& soap_action,
    const std::string& body) {
  std::lock_guard<std::mutex> lk(mu_);
  const auto tail = action_tail(soap_action);

  if (tail == "GetServicesRequest") {
    // Advertise /onvif/services as the single event XAddr (Axis style).
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
      "<tds:XAddr>http://" + real_ip_ + "/onvif/services</tds:XAddr>"
      "<tds:Version>"
      "<tt:Major>2</tt:Major><tt:Minor>60</tt:Minor>"
      "</tds:Version>"
      "</tds:Service>"
      "</tds:GetServicesResponse>"
      "</SOAP-ENV:Body>"
      "</SOAP-ENV:Envelope>";
    return {200, rewrite_urls(resp)};
  }

  if (tail == "CreatePullPointSubscriptionRequest") {
    subscribed_ = true;
    // SubscriptionReference/Address = /onvif/services (same endpoint).
    // ReferenceParameters carry the subscription token that the listener
    // must forward verbatim in all subsequent PullMessages and Renew calls.
    std::string resp =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<SOAP-ENV:Envelope"
      "  xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\""
      "  xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
      "  xmlns:wsa5=\"http://www.w3.org/2005/08/addressing\""
      "  xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\">"
      "<SOAP-ENV:Body>"
      "<tev:CreatePullPointSubscriptionResponse>"
      "<tev:SubscriptionReference>"
      "<wsa5:Address>http://" + real_ip_ + "/onvif/services</wsa5:Address>"
      "<wsa5:ReferenceParameters>"
      "<wsnt:Identifier>" + token_ + "</wsnt:Identifier>"
      "</wsa5:ReferenceParameters>"
      "</tev:SubscriptionReference>"
      "<wsnt:CurrentTime>2026-01-01T00:00:00Z</wsnt:CurrentTime>"
      "<wsnt:TerminationTime>2026-01-01T00:02:00Z</wsnt:TerminationTime>"
      "</tev:CreatePullPointSubscriptionResponse>"
      "</SOAP-ENV:Body>"
      "</SOAP-ENV:Envelope>";
    return {200, rewrite_urls(resp)};
  }

  // For PullMessages and Renew, validate that the token was forwarded.
  const bool token_present = body.find(token_) != std::string::npos;
  if (!token_present) {
    // Axis returns HTTP 400 ter:InvalidArgs when ReferenceParameters are missing.
    return {400,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<SOAP-ENV:Envelope"
      "  xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\""
      "  xmlns:ter=\"http://www.onvif.org/ver10/error\">"
      "<SOAP-ENV:Body>"
      "<SOAP-ENV:Fault>"
      "<SOAP-ENV:Code><SOAP-ENV:Value>SOAP-ENV:Sender</SOAP-ENV:Value></SOAP-ENV:Code>"
      "<SOAP-ENV:Reason><SOAP-ENV:Text>ter:InvalidArgs</SOAP-ENV:Text></SOAP-ENV:Reason>"
      "</SOAP-ENV:Fault>"
      "</SOAP-ENV:Body>"
      "</SOAP-ENV:Envelope>"};
  }

  if (tail == "RenewRequest") {
    return {200,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<SOAP-ENV:Envelope"
      "  xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\""
      "  xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\">"
      "<SOAP-ENV:Body>"
      "<wsnt:RenewResponse>"
      "<wsnt:TerminationTime>2026-01-01T00:04:00Z</wsnt:TerminationTime>"
      "</wsnt:RenewResponse>"
      "</SOAP-ENV:Body>"
      "</SOAP-ENV:Envelope>"};
  }

  if (tail == "PullMessagesRequest") {
    // Return one CellMotionDetector/Motion Changed event.
    return {200,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<SOAP-ENV:Envelope"
      "  xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\""
      "  xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
      "  xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
      "  xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
      "<SOAP-ENV:Body>"
      "<tev:PullMessagesResponse>"
      "<tev:CurrentTime>2026-01-01T00:00:00Z</tev:CurrentTime>"
      "<tev:TerminationTime>2026-01-01T00:02:00Z</tev:TerminationTime>"
      "<wsnt:NotificationMessage>"
      "<wsnt:Topic"
      "  Dialect=\"http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet\">"
      "tns1:RuleEngine/CellMotionDetector/Motion"
      "</wsnt:Topic>"
      "<wsnt:Message>"
      "<tt:Message UtcTime=\"2026-01-01T00:00:01Z\""
      "            PropertyOperation=\"Changed\">"
      "<tt:Source>"
      "<tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"1\"/>"
      "<tt:SimpleItem Name=\"Rule\" Value=\"AxisMotionRule\"/>"
      "</tt:Source>"
      "<tt:Data>"
      "<tt:SimpleItem Name=\"IsMotion\" Value=\"true\"/>"
      "</tt:Data>"
      "</tt:Message>"
      "</wsnt:Message>"
      "</wsnt:NotificationMessage>"
      "</tev:PullMessagesResponse>"
      "</SOAP-ENV:Body>"
      "</SOAP-ENV:Envelope>"};
  }

  return {400, ""};
}

// ============================================================
// ReolinkCameraEmulator
// ============================================================

// Extract the first raw GetServices response from a JSONL file.
static std::string extract_get_services(const std::string& path) {
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto e = parse_line(line);
    if (action_tail(e.soap_action) == "GetServicesRequest" &&
        !e.response.empty())
      return e.response;
  }
  return "";
}

ReolinkCameraEmulator::ReolinkCameraEmulator(const std::string& jsonl_path)
  : OnvifCameraEmulator(peek_camera_ip(jsonl_path)) {
  session_ = RecordedSession::from_jsonl(jsonl_path);
  raw_get_services_ = extract_get_services(jsonl_path);
}

std::pair<int, std::string> ReolinkCameraEmulator::handle(
  const std::string& /*path*/,
  const std::string& soap_action,
  const std::string& /*body*/) {
  std::lock_guard<std::mutex> lk(mu_);
  const auto tail = action_tail(soap_action);

  std::pair<int, std::string> resp;
  if (tail == "GetServicesRequest") {
    // Serve the raw GetServices response (preserving the tad: namespace bug).
    resp = {200, raw_get_services_};
  } else if (tail == "CreatePullPointSubscriptionRequest") {
    resp = next_clamp(session_.create_sub, create_idx_);
  } else if (tail == "PullMessagesRequest") {
    resp = next_cycle(session_.pull, pull_idx_);
  } else if (tail == "RenewRequest") {
    resp = next_clamp(session_.renew, renew_idx_);
  } else {
    resp = {400, ""};
  }

  resp.second = rewrite_urls(resp.second);
  return resp;
}

// ============================================================
// ReplayCameraEmulator
// ============================================================

ReplayCameraEmulator::ReplayCameraEmulator(const std::string& jsonl_path)
  : OnvifCameraEmulator(peek_camera_ip(jsonl_path)) {
  session_ = RecordedSession::from_jsonl(jsonl_path);
}

std::pair<int, std::string> ReplayCameraEmulator::handle(
  const std::string& /*path*/,
  const std::string& soap_action,
  const std::string& /*body*/) {
  std::lock_guard<std::mutex> lk(mu_);
  const auto tail = action_tail(soap_action);

  std::pair<int, std::string> resp;
  if (tail == "CreatePullPointSubscriptionRequest") {
    resp = next_clamp(session_.create_sub, create_idx_);
  } else if (tail == "PullMessagesRequest") {
    resp = next_cycle(session_.pull, pull_idx_);
  } else if (tail == "RenewRequest") {
    resp = next_clamp(session_.renew, renew_idx_);
  } else if (tail == "SetSynchronizationPointRequest") {
    resp = {200, sync_point_response()};
  } else {
    resp = {400, ""};
  }

  resp.second = rewrite_urls(resp.second);
  return resp;
}

// ============================================================
// SlotLimitedCameraEmulator
// ============================================================

namespace {

// SOAP 1.2 envelope around @p body.
std::string slot_envelope(const std::string& body) {
  return std::string(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<SOAP-ENV:Envelope"
      " xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\""
      " xmlns:wsa5=\"http://www.w3.org/2005/08/addressing\""
      " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
      " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
      " xmlns:ter=\"http://www.onvif.org/ver10/error\">"
      "<SOAP-ENV:Body>") + body + "</SOAP-ENV:Body></SOAP-ENV:Envelope>";
}

std::string slot_fault(const std::string& subcode, const std::string& text) {
  return slot_envelope(
      "<SOAP-ENV:Fault><SOAP-ENV:Code>"
      "<SOAP-ENV:Value>SOAP-ENV:Sender</SOAP-ENV:Value>"
      "<SOAP-ENV:Subcode><SOAP-ENV:Value>" + subcode +
      "</SOAP-ENV:Value></SOAP-ENV:Subcode></SOAP-ENV:Code>"
      "<SOAP-ENV:Reason><SOAP-ENV:Text xml:lang=\"en\">" + text +
      "</SOAP-ENV:Text></SOAP-ENV:Reason></SOAP-ENV:Fault>");
}

// Seconds in the first ...TerminationTime element of @p body when it holds
// an xs:duration such as PT120S, PT2M or PT1H; @p fallback otherwise.
int termination_seconds(const std::string& body, int fallback) {
  const std::string tag = "TerminationTime>";
  const auto p = body.find(tag);
  if (p == std::string::npos) return fallback;
  const auto start = p + tag.size();
  const auto end = body.find('<', start);
  if (end == std::string::npos) return fallback;
  std::string v = body.substr(start, end - start);
  v.erase(0, v.find_first_not_of(" \t\r\n"));
  const auto t = v.find('T');
  if (v.empty() || v[0] != 'P' || t == std::string::npos) return fallback;
  int total = 0;
  int num = -1;
  for (std::size_t i = t + 1; i < v.size(); ++i) {
    const char c = v[i];
    if (c >= '0' && c <= '9') {
      num = (num < 0 ? 0 : num * 10) + (c - '0');
    } else if (c == '.') {
      // Fractional seconds: keep the whole part.
      while (i + 1 < v.size() && v[i + 1] >= '0' && v[i + 1] <= '9') ++i;
    } else if (num >= 0 && (c == 'H' || c == 'M' || c == 'S')) {
      total += num * (c == 'H' ? 3600 : c == 'M' ? 60 : 1);
      num = -1;
    } else {
      break;
    }
  }
  return total > 0 ? total : fallback;
}

}  // namespace

SlotLimitedCameraEmulator::SlotLimitedCameraEmulator(
    const std::string& jsonl_path, int slots)
  : OnvifCameraEmulator(peek_camera_ip(jsonl_path)), slots_(slots) {
  session_ = RecordedSession::from_jsonl(jsonl_path);
  for (const auto& ex : session_.create_sub) {
    if (ex.status != 200 && fault_body_.empty() &&
        ex.response.find("BlockedThreadUpperLimit") != std::string::npos) {
      fault_status_ = ex.status;
      fault_body_   = ex.response;
    }
    if (ex.status == 200 && create_template_.empty() &&
        ex.response.find("Address>") != std::string::npos)
      create_template_ = ex.response;
  }
  if (fault_body_.empty())
    fault_body_ = slot_fault("ter:BlockedThreadUpperLimit", "");
  if (create_template_.empty()) {
    create_template_ = slot_envelope(
      "<tev:CreatePullPointSubscriptionResponse><tev:SubscriptionReference>"
      "<wsa5:Address>http://" + real_ip_ + "/onvif/Events/pullSubmanager"
      "</wsa5:Address></tev:SubscriptionReference>"
      "<wsnt:CurrentTime>2026-01-01T00:00:00Z</wsnt:CurrentTime>"
      "<wsnt:TerminationTime>2026-01-01T00:02:00Z</wsnt:TerminationTime>"
      "</tev:CreatePullPointSubscriptionResponse>");
  }
}

void SlotLimitedCameraEmulator::expire_locked() {
  const auto now = Clock::now();
  for (auto it = live_.begin(); it != live_.end();) {
    if (it->second <= now)
      it = live_.erase(it);
    else
      ++it;
  }
}

std::string SlotLimitedCameraEmulator::create_response(
    const std::string& path) const {
  std::string resp = create_template_;
  const auto a = resp.find("Address>");
  const auto start = a + std::string("Address>").size();
  const auto end = resp.find('<', start);
  resp.replace(start, end - start,
               "http://127.0.0.1:" + std::to_string(port()) + path);
  return rewrite_urls(resp);
}

std::pair<int, std::string> SlotLimitedCameraEmulator::handle(
    const std::string& path,
    const std::string& soap_action,
    const std::string& body) {
  std::lock_guard<std::mutex> lk(mu_);
  expire_locked();
  const auto tail = action_tail(soap_action);

  if (tail == "GetServicesRequest")
    return {200, rewrite_urls(make_get_services_response(real_ip_,
                                                         alarm_service_url_))};

  if (tail == "CreatePullPointSubscriptionRequest") {
    ++creates_total_;
    if (static_cast<int>(live_.size()) >= slots_) {
      ++creates_refused_;
      return {fault_status_, rewrite_urls(fault_body_)};
    }
    const std::string sub_path =
        "/onvif/Events/pullSubmanager/" + std::to_string(next_id_++);
    live_[sub_path] =
        Clock::now() + std::chrono::seconds(termination_seconds(body, 60));
    return {200, create_response(sub_path)};
  }

  const auto live = live_.find(path);
  if (tail == "UnsubscribeRequest") {
    ++unsubscribes_total_;
    if (live == live_.end())
      return {400, slot_fault("ter:InvalidArgVal", "unknown subscription")};
    live_.erase(live);
    return {200, slot_envelope("<wsnt:UnsubscribeResponse/>")};
  }
  if (tail == "RenewRequest") {
    ++renews_total_;
    if (live == live_.end())
      return {400, slot_fault("ter:InvalidArgVal", "unknown subscription")};
    if (fail_renews_)
      return {400, slot_fault("ter:InvalidArgVal", "renew refused")};
    live->second =
        Clock::now() + std::chrono::seconds(termination_seconds(body, 60));
    return {200, slot_envelope(
                     "<wsnt:RenewResponse>"
                     "<wsnt:TerminationTime>2026-01-01T00:02:00Z"
                     "</wsnt:TerminationTime>"
                     "<wsnt:CurrentTime>2026-01-01T00:00:00Z</wsnt:CurrentTime>"
                     "</wsnt:RenewResponse>")};
  }
  if (tail == "PullMessagesRequest") {
    if (live == live_.end())
      return {400, slot_fault("ter:InvalidArgVal", "unknown subscription")};
    ++pulls_total_;
    auto resp = next_cycle(session_.pull, pull_idx_);
    resp.second = rewrite_urls(resp.second);
    return resp;
  }
  if (tail == "SetSynchronizationPointRequest") {
    if (live == live_.end())
      return {400, slot_fault("ter:InvalidArgVal", "unknown subscription")};
    ++sync_points_total_;
    return {200, sync_point_response()};
  }
  return {400, ""};
}

int SlotLimitedCameraEmulator::live_subscriptions() const {
  std::lock_guard<std::mutex> lk(mu_);
  const auto now = Clock::now();
  int n = 0;
  for (const auto& [path, until] : live_)
    if (until > now) ++n;
  return n;
}

int SlotLimitedCameraEmulator::creates_total() const {
  std::lock_guard<std::mutex> lk(mu_);
  return creates_total_;
}

int SlotLimitedCameraEmulator::creates_refused() const {
  std::lock_guard<std::mutex> lk(mu_);
  return creates_refused_;
}

int SlotLimitedCameraEmulator::unsubscribes_total() const {
  std::lock_guard<std::mutex> lk(mu_);
  return unsubscribes_total_;
}

int SlotLimitedCameraEmulator::renews_total() const {
  std::lock_guard<std::mutex> lk(mu_);
  return renews_total_;
}

int SlotLimitedCameraEmulator::pulls_total() const {
  std::lock_guard<std::mutex> lk(mu_);
  return pulls_total_;
}

int SlotLimitedCameraEmulator::sync_points_total() const {
  std::lock_guard<std::mutex> lk(mu_);
  return sync_points_total_;
}

void SlotLimitedCameraEmulator::set_fail_renews(bool fail) {
  std::lock_guard<std::mutex> lk(mu_);
  fail_renews_ = fail;
}

void SlotLimitedCameraEmulator::set_slots(int slots) {
  std::lock_guard<std::mutex> lk(mu_);
  slots_ = slots;
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
  // Protect 7.3 hides /internal/ automation routes from any request that
  // carries user credentials (requireUcoreIpc), answering 404.
  if (path.rfind("/internal/", 0) == 0 && !request_header("X-UserId").empty()) {
    ++rejected_internal_with_user_;
    return {404, ""};
  }
  if (path == "/api/automations" && body.empty())
    return {200, alarms_json_};
  // Match POST /api/automations/{id}/run
  if (path.rfind("/api/automations/", 0) == 0 &&
      path.size() > 18 && path.find("/run") != std::string::npos) {
    if (global_alarm_manager_) {
      return {400, "{\"error\":\"This operation is not available when "
                   "global alarm manager is enabled\",\"name\":"
                   "\"BAD_REQUEST\"}"};
    }
    posted_.push_back(path);
    return {200, "{}"};
  }
  // UOS external automation manager (the thumbnail-capable path).
  if (path == "/internal/automationManager/external/change") {
    register_bodies_.push_back(body);
    return {200, "{}"};
  }
  if (path == "/internal/automationManager/external/actions/notify") {
    notify_bodies_.push_back(body);
    return {notify_status_, notify_status_ == 200 ? "{}" : ""};
  }
  return {404, ""};
}

std::vector<std::string> UosEmulator::notify_bodies() const {
  std::lock_guard<std::mutex> lk(mu_);
  return notify_bodies_;
}

std::vector<std::string> UosEmulator::register_bodies() const {
  std::lock_guard<std::mutex> lk(mu_);
  return register_bodies_;
}

std::vector<std::string> UosEmulator::triggered_automations() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<std::string> out = posted_;
  out.insert(out.end(), notify_bodies_.begin(), notify_bodies_.end());
  return out;
}

void UosEmulator::set_notify_status(int code) {
  std::lock_guard<std::mutex> lk(mu_);
  notify_status_ = code;
}

void UosEmulator::set_global_alarm_manager(bool on) {
  std::lock_guard<std::mutex> lk(mu_);
  global_alarm_manager_ = on;
}

int UosEmulator::rejected_internal_with_user() const {
  std::lock_guard<std::mutex> lk(mu_);
  return rejected_internal_with_user_;
}
