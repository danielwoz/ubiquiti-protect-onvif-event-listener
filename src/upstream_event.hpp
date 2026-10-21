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

namespace onvif {

// State changes of the recorder's own event subscription to a camera,
// reported by OnvifListener and consumed by the PullPoint hub that shares
// the subscription with Protect.
enum class UpstreamEvent {
  kSubscribed,    // a new subscription was created
  kResumed,       // a subscription saved at the last stop was taken over
  kPullOk,        // PullMessages answered 200
  kPullFailed,    // PullMessages failed (transport error or non-200)
  kCreateFailed,  // CreatePullPointSubscription failed
  kRenewFailed,   // Renew failed (transport error or non-200)
  kUnsubscribed,  // the worker stopped using the subscription
};

inline const char* upstream_event_name(UpstreamEvent ev) {
  switch (ev) {
    case UpstreamEvent::kSubscribed:   return "subscribed";
    case UpstreamEvent::kResumed:      return "resumed";
    case UpstreamEvent::kPullOk:       return "pull_ok";
    case UpstreamEvent::kPullFailed:   return "pull_failed";
    case UpstreamEvent::kCreateFailed: return "create_failed";
    case UpstreamEvent::kRenewFailed:  return "renew_failed";
    case UpstreamEvent::kUnsubscribed: return "unsubscribed";
  }
  return "unknown";
}

}  // namespace onvif
