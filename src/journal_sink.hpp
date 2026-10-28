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

#include <cstdio>
#include <string>

#include "absl/base/log_severity.h"
#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/strings/string_view.h"

namespace onvif {

/// The sd-daemon(3) priority prefix journald reads at the start of a line a
/// service writes to stdout or stderr: "<3>" (err) for ERROR and FATAL,
/// "<4>" (warning) for WARNING and "<6>" (info) for INFO.
absl::string_view sd_priority_prefix(absl::LogSeverity severity);

/// @p text with sd_priority_prefix(@p severity) at the start of every line,
/// each line ending in a newline.  journald stores every line as its own
/// entry, so every line of a multi-line message (a FATAL stack trace)
/// carries the prefix.  Empty @p text gives an empty string.
std::string with_sd_priority(absl::string_view text, absl::LogSeverity severity);

/// True when @p fd is the journal stream named by @p journal_stream, the
/// "<device>:<inode>" value systemd puts in $JOURNAL_STREAM for a service
/// whose stdout or stderr is connected to the journal.  Null, empty and
/// malformed values give false.
bool is_journal_stream(int fd, const char* journal_stream);

/// is_journal_stream() for stderr and this process's $JOURNAL_STREAM.
bool stderr_is_journal();

/// Writes every log line at or above @p threshold to @p out with its
/// sd-daemon priority prefix, so journald records the line's severity as
/// PRIORITY instead of the default (info).  It takes the place of absl's
/// own stderr output, which has no such prefix: register it with
/// absl::AddLogSink() and set absl::SetStderrThreshold() to kInfinity.
class JournalStderrSink : public absl::LogSink {
 public:
  explicit JournalStderrSink(absl::LogSeverityAtLeast threshold,
                             std::FILE* out = stderr)
      : threshold_(threshold), out_(out) {}
  void Send(const absl::LogEntry& entry) override;

 private:
  const absl::LogSeverityAtLeast threshold_;
  std::FILE* const out_;
};

}  // namespace onvif
