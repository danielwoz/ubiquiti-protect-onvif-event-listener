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

// Tests for the sd-daemon priority prefixes written to a journal stderr.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "absl/log/log.h"
#include "journal_sink.hpp"

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& label) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << label << '\n';
  }
}

static void test_prefix_per_severity() {
  check(onvif::sd_priority_prefix(absl::LogSeverity::kInfo) == "<6>", "INFO is <6>");
  check(onvif::sd_priority_prefix(absl::LogSeverity::kWarning) == "<4>", "WARNING is <4>");
  check(onvif::sd_priority_prefix(absl::LogSeverity::kError) == "<3>", "ERROR is <3>");
  check(onvif::sd_priority_prefix(absl::LogSeverity::kFatal) == "<3>", "FATAL is <3>");
}

static void test_every_line_prefixed() {
  using onvif::with_sd_priority;
  check(with_sd_priority("", absl::LogSeverity::kError).empty(), "empty text stays empty");
  check(with_sd_priority("E1028 a.cpp:1] boom\n", absl::LogSeverity::kError) ==
            "<3>E1028 a.cpp:1] boom\n",
        "one line");
  check(with_sd_priority("trace:\n  frame 1\n\n  frame 2\n", absl::LogSeverity::kFatal) ==
            "<3>trace:\n<3>  frame 1\n<3>\n<3>  frame 2\n",
        "every line of a multi-line message, blank ones too");
  check(with_sd_priority("no newline", absl::LogSeverity::kWarning) == "<4>no newline\n",
        "a missing final newline is added");
}

static void test_journal_stream_match() {
  std::FILE* f = std::tmpfile();
  if (f == nullptr) {
    check(false, "tmpfile");
    return;
  }
  const int fd = fileno(f);
  struct stat st {};
  fstat(fd, &st);
  const std::string dev = std::to_string(static_cast<uint64_t>(st.st_dev));
  const std::string ino = std::to_string(static_cast<uint64_t>(st.st_ino));
  const std::string other = std::to_string(static_cast<uint64_t>(st.st_ino) + 1);

  check(onvif::is_journal_stream(fd, (dev + ":" + ino).c_str()), "matching device:inode");
  check(!onvif::is_journal_stream(fd, (dev + ":" + other).c_str()), "another inode");
  check(!onvif::is_journal_stream(fd, nullptr), "variable unset");
  check(!onvif::is_journal_stream(fd, ""), "variable empty");
  check(!onvif::is_journal_stream(fd, (dev + ino).c_str()), "no colon");
  check(!onvif::is_journal_stream(fd, (dev + ":").c_str()), "no inode");
  check(!onvif::is_journal_stream(fd, (" " + dev + ":" + ino).c_str()), "leading space");
  check(!onvif::is_journal_stream(fd, (dev + ":" + ino + ":1").c_str()), "trailing field");
  check(!onvif::is_journal_stream(-1, (dev + ":" + ino).c_str()), "bad descriptor");
  std::fclose(f);
}

// The sink writes lines at or above its threshold, each with its prefix,
// and a FATAL entry's stack trace in place of the message.
static void test_sink_output() {
  char* buf = nullptr;
  size_t len = 0;
  std::FILE* mem = open_memstream(&buf, &len);
  {
    onvif::JournalStderrSink errors_only(absl::LogSeverityAtLeast::kError, mem);
    LOG(INFO).ToSinkOnly(&errors_only) << "quiet info";
    LOG(WARNING).ToSinkOnly(&errors_only) << "quiet warning";
    LOG(ERROR).ToSinkOnly(&errors_only) << "loud error";
    onvif::JournalStderrSink everything(absl::LogSeverityAtLeast::kInfo, mem);
    LOG(INFO).ToSinkOnly(&everything) << "verbose info";
    LOG(WARNING).ToSinkOnly(&everything) << "verbose warning";
    LOG(INFO).ToSinkOnly(&everything) << "two\nlines";
  }
  std::fclose(mem);
  const std::string out(buf, len);
  std::free(buf);

  check(out.find("quiet") == std::string::npos, "below the threshold is not written");
  check(out.rfind("<3>E", 0) == 0 && out.find("] loud error\n") != std::string::npos,
        "an error starts with <3> and keeps absl's prefix");
  check(out.find("\n<6>I") != std::string::npos &&
            out.find("] verbose info\n") != std::string::npos,
        "info is <6> with a kInfo threshold");
  check(out.find("\n<4>W") != std::string::npos &&
            out.find("] verbose warning\n") != std::string::npos,
        "warning is <4>");
  check(out.find("] two\n<6>lines\n") != std::string::npos,
        "the second line of a message is prefixed too");
}

int main() {
  test_prefix_per_severity();
  test_every_line_prefixed();
  test_journal_stream_match();
  test_sink_output();
  std::cout << "test_journal_sink: " << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
