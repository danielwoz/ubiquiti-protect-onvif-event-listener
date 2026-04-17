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

#include "log_ring.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

namespace onvif {

LogRing::LogRing() {
  std::memset(buf_, 0, kCapacity);
}

void LogRing::Send(const absl::LogEntry& entry) {
  // Format: "YYYY-MM-DD HH:MM:SS.mmm LEVEL file:line] message\n"
  char ts[32];
  const absl::Time t = entry.timestamp();
  const absl::TimeZone utc = absl::UTCTimeZone();
  const absl::TimeZone::CivilInfo ci = utc.At(t);
  std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                static_cast<int>(ci.cs.year()),
                ci.cs.month(), ci.cs.day(),
                ci.cs.hour(), ci.cs.minute(), ci.cs.second(),
                static_cast<int>(absl::ToInt64Milliseconds(
                    t - absl::FromCivil(ci.cs, utc))));

  const char* sev = "?";
  switch (entry.log_severity()) {
    case absl::LogSeverity::kInfo:    sev = "I"; break;
    case absl::LogSeverity::kWarning: sev = "W"; break;
    case absl::LogSeverity::kError:   sev = "E"; break;
    case absl::LogSeverity::kFatal:   sev = "F"; break;
  }

  absl::string_view msg = entry.text_message_with_prefix_and_newline();

  // Build the line: "ts LEVEL msg"
  // entry.text_message_with_prefix_and_newline() already includes source
  // location and trailing newline from absl, so just prepend timestamp + level.
  char prefix[48];
  int plen = std::snprintf(prefix, sizeof(prefix), "%s %s ", ts, sev);
  if (plen < 0) return;
  size_t prefix_len = static_cast<size_t>(plen);

  std::lock_guard<std::mutex> lk(mu_);

  // Write prefix then message into the ring buffer.
  auto write = [&](const char* data, size_t len) {
    while (len > 0) {
      size_t avail = kCapacity - head_;
      size_t n = (len < avail) ? len : avail;
      std::memcpy(buf_ + head_, data, n);
      head_ += n;
      data += n;
      len -= n;
      if (head_ >= kCapacity) {
        head_ = 0;
        wrapped_ = true;
      }
    }
  };

  write(prefix, prefix_len);
  write(msg.data(), msg.size());
}

std::string LogRing::dump() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::string out;
  if (wrapped_) {
    // Old data is from head_ to end, then 0 to head_.
    out.append(buf_ + head_, kCapacity - head_);
    out.append(buf_, head_);
  } else {
    out.append(buf_, head_);
  }
  return out;
}

}  // namespace onvif
