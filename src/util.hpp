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

#include <cstdint>
#include <map>
#include <string>

namespace onvif {
namespace util {

// Current time as milliseconds since Unix epoch.
uint64_t now_ms();

// Current time as ISO-8601 UTC: "YYYY-MM-DDTHH:MM:SSZ"
std::string utc_now_iso8601();

// Current time as ISO-8601 UTC with milliseconds: "YYYY-MM-DDTHH:MM:SS.mmmZ"
std::string utc_now_iso8601_ms();

// Generate a UUID v4 string (e.g. "550e8400-e29b-41d4-a716-446655440000").
std::string generate_uuid();

// Generate a 24-char lowercase hex ID (12 random bytes).
// Protect routes thumbnailIds with length == 24 to its local DB (thumbnails
// table) rather than to the msp media server.
std::string generate_24hex_id();

// JSON-escape a string and wrap it in double quotes.
// Handles \", \\, \n, \r, \t, and control chars < 0x20.
std::string json_str(const std::string& s);

// Serialize a string map as a JSON object: {"key":"value",...}
std::string json_obj(const std::map<std::string, std::string>& m);

}  // namespace util
}  // namespace onvif
