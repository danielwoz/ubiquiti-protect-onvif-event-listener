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

#include <string>

#include "notification_fragments.hpp"

namespace onvif {
namespace pullpoint {
namespace internal {

// The fragment operations PullPointHub uses: extract_fragments() on each
// upstream PullMessagesResponse and as_initialized() when it replays the
// property cache.  Tests replace them to drive the hub with hand-made
// fragments.
struct FragmentOps {
  ExtractResult (*extract)(const std::string& pull_response_xml) = nullptr;
  std::string (*initialize)(const std::string& fragment_xml) = nullptr;
};

// Replace the process-wide fragment operations.  A null member restores
// the default for that operation.
void set_fragment_ops_for_testing(const FragmentOps& ops);

}  // namespace internal
}  // namespace pullpoint
}  // namespace onvif
