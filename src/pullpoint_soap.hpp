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

// Request parsing and response building for PullPointServer: request
// paths, SOAP request bodies, ISO-8601 time values and the SOAP 1.2
// envelopes and faults sent back to Protect.

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace onvif {
namespace pullpoint {
namespace soap {

// ---------------------------------------------------------------------
// Time values
// ---------------------------------------------------------------------

// xs:duration such as "PT60S", "PT1M", "P0DT0H1M0S" or "PT1.5S".  Years
// count as 365 days and months as 30 days.  A leading '-' gives a
// negative duration.  Surrounding whitespace is ignored.  nullopt when
// the text is not a duration.
std::optional<std::chrono::milliseconds> parse_duration(const std::string& text);

// xs:dateTime such as "2026-10-21T20:00:00Z", "2026-10-21T20:00:00.5Z" or
// "2026-10-21T22:00:00+02:00".  A value without a zone is UTC.  Years
// outside 1900..2200 are rejected.  nullopt when the text is not a
// dateTime.
std::optional<std::chrono::system_clock::time_point> parse_date_time(const std::string& text);

// A duration, or a dateTime expressed as its distance from @p now
// (negative when it lies in the past).  nullopt when it is neither.
std::optional<std::chrono::milliseconds> parse_time_offset(
    const std::string& text, std::chrono::system_clock::time_point now);

// UTC with seconds precision: "2026-10-21T20:00:00Z".
std::string format_date_time(std::chrono::system_clock::time_point t);

// ---------------------------------------------------------------------
// Request paths
// ---------------------------------------------------------------------

// Where a request path points, below PullPointServer::kPathPrefix:
//   {h}/{p}/s/{32 lower-case hex}  a pull point (Pull, Renew, ...)
//   {h}/{p}/status                 the migration probe
//   {h}/{p}<anything else>         CreatePullPointSubscription
struct Route {
  enum class Kind { kInvalid, kCreate, kStatus, kSubscription };
  Kind kind = Kind::kInvalid;
  std::string key;     // camera_key() of the decoded host and port
  std::string base;    // prefix + "{h}/{p}" exactly as received
  std::string id;      // kSubscription: the pull point id
  std::string suffix;  // kCreate: Protect's event-service path
  std::string error;   // kInvalid: why the path was refused
};

// @p path is the request path without the query string, still
// percent-encoded.
Route parse_route(const std::string& path);

// Decode %XX escapes.  False on a malformed escape or when the result
// contains a control character.
bool percent_decode(const std::string& in, std::string* out);

// ---------------------------------------------------------------------
// Request bodies
// ---------------------------------------------------------------------

struct Request {
  std::string op;  // local name of the first element child of Body
  // Local name and trimmed text of each child element of the operation,
  // in document order.
  std::vector<std::pair<std::string, std::string>> params;

  // Text of the first parameter called @p name, or null.
  const std::string* param(const char* name) const;
};

// Parse a SOAP request.  Refuses a DOCTYPE, resolves no entities and
// reads nothing from the network.  SOAP headers are ignored.  On failure
// returns false and sets @p error.
bool parse_request(const std::string& body, Request* req, std::string* error);

// ---------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------

// Subcode carried by a fault.
enum class FaultKind {
  kActionNotSupported,  // ter:ActionNotSupported (declines, unknown ops)
  kInvalidArgVal,       // ter:InvalidArgVal (malformed requests)
  kResourceUnknown,     // ter:InvalidArgVal / ter:ResourceUnknown
};

// Escape &, <, >, " and ' for XML text and attribute values.
std::string xml_escape(const std::string& s);

// SOAP 1.2 envelope declaring env, wsa, tev, wsnt, tt and ter, with
// @p action as the wsa:Action header and @p body inside env:Body.
std::string envelope(const std::string& action, const std::string& body);

// SOAP 1.2 Receiver fault whose Reason is "onvif-recorder: <reason>".
std::string fault(FaultKind kind, const std::string& reason);

std::string create_response(const std::string& address,
                            std::chrono::system_clock::time_point current,
                            std::chrono::system_clock::time_point termination);
std::string capabilities_response();
std::string pull_response(std::chrono::system_clock::time_point current,
                          std::chrono::system_clock::time_point termination,
                          const std::vector<std::shared_ptr<const std::string>>& messages);
std::string renew_response(std::chrono::system_clock::time_point current,
                           std::chrono::system_clock::time_point termination);
std::string unsubscribe_response();
std::string sync_point_response();

}  // namespace soap
}  // namespace pullpoint
}  // namespace onvif
