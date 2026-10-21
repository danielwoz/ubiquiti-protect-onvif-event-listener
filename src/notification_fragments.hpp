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

#include <cstddef>
#include <string>
#include <vector>

namespace onvif {
namespace pullpoint {

// Largest fragment passed on to Protect.  Bigger messages first lose their
// ElementItem payloads (images, which Protect never reads); a message that
// is still too big is dropped.
constexpr size_t kMaxFragmentBytes = 64 * 1024;

// One wsnt:NotificationMessage copied out of a camera's
// PullMessagesResponse as a standalone element that declares every
// namespace prefix it uses, including prefixes inside Topic text.
struct Fragment {
  std::string xml;     // serialised <…NotificationMessage> element
  std::string topic;   // trimmed Topic text
  // PropertyOperation of the inner Message ("Initialized", "Changed",
  // "Deleted"), empty when the message is not a property event.
  std::string op;
  // Identity of the property the message describes: topic, then the
  // sorted Source SimpleItems, then the sorted Key SimpleItems.  Empty
  // when op is empty.
  std::string key;
  // Topic is one Protect reads: motion (MotionAlarm, CellMotionDetector,
  // MotionRegionDetector) or digital input (DigitalInput, AlarmIn).
  bool protect_relevant = false;
};

struct ExtractResult {
  bool parsed = false;               // the response was XML
  std::vector<Fragment> fragments;   // in document order
  size_t stripped = 0;               // fragments that lost ElementItems
  size_t oversize_dropped = 0;       // fragments dropped as too big
};

// Copy every NotificationMessage (matched by local name, in any
// namespace) out of a PullMessagesResponse.
ExtractResult extract_fragments(const std::string& pull_response_xml);

// Return @p fragment_xml with PropertyOperation="Initialized" on its
// Message element, for replaying current state to a new subscriber.
// Returns the input unchanged when it has no PropertyOperation.
std::string as_initialized(const std::string& fragment_xml);

}  // namespace pullpoint
}  // namespace onvif
