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

// Single-core throughput benchmark for jpeg_crop::smart_crop().
//
// Loads one or more JPEG files from the testdata directory and measures how
// many frames per second the decode-crop-encode pipeline sustains on a single
// pinned CPU core.
//
// Usage:
//   bazel run //test:bench_jpeg_crop                        # uses bundled testdata
//   bazel run //test:bench_jpeg_crop -- file.jpg [count]    # custom file + iteration count

#include <sched.h>
#include <sys/resource.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../jpeg_crop.hpp"

namespace {

void pin_to_core(int core) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(core, &mask);
  if (sched_setaffinity(0, sizeof(mask), &mask) != 0)
    std::cerr << "Warning: sched_setaffinity failed\n";
}

std::vector<uint8_t> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  auto size = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> buf(static_cast<size_t>(size));
  f.read(reinterpret_cast<char*>(buf.data()), size);
  return buf;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <image.jpg> [iterations]\n";
    return 1;
  }

  const std::string path       = argv[1];
  const int64_t     iterations = (argc >= 3) ? std::stoll(argv[2]) : 1000;

  const auto input = read_file(path);
  if (input.empty()) {
    std::cerr << "Failed to read: " << path << "\n";
    return 1;
  }

  // Validate that the image can be cropped at all before benchmarking.
  {
    auto result = jpeg_crop::smart_crop(input);
    if (result.empty()) {
      std::cerr << "smart_crop returned empty result for " << path << "\n";
      return 1;
    }
    std::fprintf(stderr, "Input: %zu bytes → cropped: %zu bytes\n",
                 input.size(), result.size());
  }

  pin_to_core(0);

  using Clock = std::chrono::steady_clock;

  struct rusage ru_before, ru_after;
  getrusage(RUSAGE_SELF, &ru_before);
  const auto wall_before = Clock::now();
  const auto t_start     = Clock::now();

  for (int64_t i = 0; i < iterations; ++i)
    jpeg_crop::smart_crop(input);

  const auto t_end      = Clock::now();
  const auto wall_after = Clock::now();
  getrusage(RUSAGE_SELF, &ru_after);

  const int64_t elapsed_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
  const double elapsed_ms  = elapsed_ns * 1e-6;
  const double throughput  = iterations / (elapsed_ns * 1e-9);
  const double ms_per_crop = elapsed_ms / static_cast<double>(iterations);

  const double wall_sec =
      std::chrono::duration<double>(wall_after - wall_before).count();
  auto tv_to_sec = [](const struct timeval& tv) {
    return tv.tv_sec + tv.tv_usec * 1e-6;
  };
  const double user_sec  = tv_to_sec(ru_after.ru_utime) - tv_to_sec(ru_before.ru_utime);
  const double sys_sec   = tv_to_sec(ru_after.ru_stime) - tv_to_sec(ru_before.ru_stime);
  const double cpu_user  = user_sec / wall_sec * 100.0;
  const double cpu_sys   = sys_sec  / wall_sec * 100.0;
  const double cpu_total = cpu_user + cpu_sys;

  std::cout << "File:         " << path                                  << "\n"
            << "Input size:   " << input.size() << " bytes"             << "\n"
            << "Iterations:   " << iterations                            << "\n"
            << "Wall time:    " << static_cast<int64_t>(elapsed_ms)     << " ms\n"
            << "Throughput:   " << static_cast<int64_t>(throughput)     << " crops/s\n"
            << "Per crop:     " << ms_per_crop                          << " ms\n"
            << "CPU user:     " << cpu_user                             << " %\n"
            << "CPU sys:      " << cpu_sys                              << " %\n"
            << "CPU total:    " << cpu_total                            << " %\n";

  return 0;
}
