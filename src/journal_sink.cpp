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

#include "journal_sink.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "absl/base/log_severity.h"
#include "absl/log/log_entry.h"
#include "absl/strings/numbers.h"
#include "absl/strings/string_view.h"

namespace onvif {

absl::string_view sd_priority_prefix(absl::LogSeverity severity) {
  if (severity >= absl::LogSeverity::kError) return "<3>";
  if (severity == absl::LogSeverity::kWarning) return "<4>";
  return "<6>";
}

std::string with_sd_priority(absl::string_view text, absl::LogSeverity severity) {
  const absl::string_view prefix = sd_priority_prefix(severity);
  std::string out;
  out.reserve(text.size() + 2 * prefix.size() + 1);
  size_t start = 0;
  while (start < text.size()) {
    const size_t nl = text.find('\n', start);
    const size_t end = nl == absl::string_view::npos ? text.size() : nl;
    out.append(prefix.data(), prefix.size());
    out.append(text.data() + start, end - start);
    out.push_back('\n');
    start = end + 1;
  }
  return out;
}

bool is_journal_stream(int fd, const char* journal_stream) {
  if (journal_stream == nullptr) return false;
  // Decimal digits only: SimpleAtoi alone would also take spaces and '+'.
  auto parse = [](absl::string_view s, uint64_t* v) {
    return !s.empty() && s.find_first_not_of("0123456789") == absl::string_view::npos &&
           absl::SimpleAtoi(s, v);
  };
  const absl::string_view value(journal_stream);
  const size_t colon = value.find(':');
  if (colon == absl::string_view::npos) return false;
  uint64_t dev = 0;
  uint64_t ino = 0;
  if (!parse(value.substr(0, colon), &dev) || !parse(value.substr(colon + 1), &ino))
    return false;
  struct stat st {};
  if (fstat(fd, &st) != 0) return false;
  return static_cast<uint64_t>(st.st_dev) == dev &&
         static_cast<uint64_t>(st.st_ino) == ino;
}

bool stderr_is_journal() {
  return is_journal_stream(STDERR_FILENO, std::getenv("JOURNAL_STREAM"));
}

void JournalStderrSink::Send(const absl::LogEntry& entry) {
  if (entry.log_severity() < threshold_) return;
  // absl sends a FATAL entry twice, the second time with its stack trace;
  // like absl's own stderr output, the second time writes the trace alone.
  const absl::string_view text = entry.stacktrace().empty()
                                     ? entry.text_message_with_prefix_and_newline()
                                     : entry.stacktrace();
  const std::string lines = with_sd_priority(text, entry.log_severity());
  std::fwrite(lines.data(), 1, lines.size(), out_);
  std::fflush(out_);
}

}  // namespace onvif
