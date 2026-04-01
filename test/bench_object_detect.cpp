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

// Single-core throughput benchmark for object_detect::ObjectDetector.
//
// Loads NanoDet-M model files and a JPEG image, then runs inference in a loop
// measuring latency and throughput on a single pinned CPU core.
//
// Usage:
//   bazel run //test:bench_object_detect
//   (args: nanodet_m.param  nanodet_m.bin  image.jpg  [iterations])
//
// Requires NCNN support (WITH_NCNN defined at build time).  Without NCNN the
// detector load will fail and the benchmark will exit with a message.

#include <sched.h>
#include <sys/resource.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../object_detect.hpp"

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
  const auto size = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> buf(static_cast<size_t>(size));
  f.read(reinterpret_cast<char*>(buf.data()), size);
  return buf;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 4) {
    std::cerr << "Usage: " << argv[0]
              << " <nanodet_m.param> <nanodet_m.bin> <image.jpg>"
              << " [iterations]\n";
    return 1;
  }

  const std::string param_path = argv[1];
  const std::string bin_path   = argv[2];
  const std::string img_path   = argv[3];
  const int64_t     iterations = (argc >= 5) ? std::stoll(argv[4]) : 100;

  const auto input = read_file(img_path);
  if (input.empty()) {
    std::cerr << "Failed to read image: " << img_path << "\n";
    return 1;
  }

  auto det_or = object_detect::ObjectDetector::Load(param_path, bin_path);
  if (!det_or.ok()) {
    std::cerr << "Failed to load model: "
              << std::string(det_or.status().message()) << "\n";
    return 1;
  }
  auto& det = *det_or;

  // Warm-up run.
  {
    auto result = det->detect(input);
    if (result) {
      std::fprintf(stderr,
                   "Warmup detection: bbox=(%.3f, %.3f, %.3f, %.3f) class=%d\n",
                   result->bbox.x, result->bbox.y,
                   result->bbox.w, result->bbox.h, result->class_id);
    } else {
      std::fprintf(stderr, "Warmup: no security-relevant detection\n");
    }
  }

  pin_to_core(0);

  using Clock = std::chrono::steady_clock;

  struct rusage ru_before, ru_after;
  getrusage(RUSAGE_SELF, &ru_before);
  const auto wall_before = Clock::now();
  const auto t_start     = Clock::now();

  for (int64_t i = 0; i < iterations; ++i)
    det->detect(input);

  const auto t_end      = Clock::now();
  const auto wall_after = Clock::now();
  getrusage(RUSAGE_SELF, &ru_after);

  const int64_t elapsed_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start)
          .count();
  const double elapsed_ms   = elapsed_ns * 1e-6;
  const double throughput   = iterations / (elapsed_ns * 1e-9);
  const double ms_per_infer = elapsed_ms / static_cast<double>(iterations);

  const double wall_sec =
      std::chrono::duration<double>(wall_after - wall_before).count();
  auto tv_to_sec = [](const struct timeval& tv) {
    return tv.tv_sec + tv.tv_usec * 1e-6;
  };
  const double user_sec  = tv_to_sec(ru_after.ru_utime) -
                           tv_to_sec(ru_before.ru_utime);
  const double sys_sec   = tv_to_sec(ru_after.ru_stime) -
                           tv_to_sec(ru_before.ru_stime);
  const double cpu_user  = user_sec / wall_sec * 100.0;
  const double cpu_sys   = sys_sec  / wall_sec * 100.0;
  const double cpu_total = cpu_user + cpu_sys;

  std::cout << "Param:        " << param_path                             << "\n"
            << "Bin:          " << bin_path                               << "\n"
            << "Image:        " << img_path                               << "\n"
            << "Input size:   " << input.size() << " bytes"              << "\n"
            << "Iterations:   " << iterations                             << "\n"
            << "Wall time:    " << static_cast<int64_t>(elapsed_ms)      << " ms\n"
            << "Throughput:   " << static_cast<int64_t>(throughput)      << " infer/s\n"
            << "Per infer:    " << ms_per_infer                          << " ms\n"
            << "CPU user:     " << cpu_user                              << " %\n"
            << "CPU sys:      " << cpu_sys                               << " %\n"
            << "CPU total:    " << cpu_total                             << " %\n";

  return 0;
}
