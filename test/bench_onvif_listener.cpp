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

// End-to-end single-core throughput benchmark for OnvifListener.
//
// Pins the whole process to CPU 0, fires a HikvisionCompatibleEmulator that serves a
// synthetic PullMessages loop (10 events per pull), and measures how many
// events/second the full HTTP -> XML-parse -> callback pipeline sustains on
// a single core.
//
// Usage (Bazel):
//   bazel run //test:bench_onvif_listener              # default 50 000 events
//   bazel run //test:bench_onvif_listener -- 100000    # custom event count
//   bazel run //test:bench_onvif_listener -- /path/to/other.jsonl 100000

#include <sched.h>
#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "onvif_listener.hpp"
#include "camera_emulators.hpp"

namespace {

void pin_to_core(int core) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(core, &mask);
  if (sched_setaffinity(0, sizeof(mask), &mask) != 0)
    std::cerr << "Warning: sched_setaffinity failed\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <bench.jsonl> [event_count]\n";
    return 1;
  }
  const std::string jsonl  = argv[1];
  const int64_t    target  = (argc >= 3) ? std::stoll(argv[2]) : 50000;

  // Pin every thread in this process to a single core so the measurement
  // reflects single-core throughput (emulator server, listener, and callback
  // all share one CPU).
  pin_to_core(0);

  onvif::global_init();

  HikvisionCompatibleEmulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "password";
  cfg.retry_interval_sec = 1;

  using Clock = std::chrono::steady_clock;

  std::atomic<int64_t> count{0};
  Clock::time_point    t_start{}, t_end{};
  std::mutex           mu;
  std::condition_variable cv;
  bool done = false;

  onvif::OnvifListener listener;
  listener.add_camera(cfg);

  struct rusage ru_before, ru_after;
  getrusage(RUSAGE_SELF, &ru_before);
  const auto wall_before = Clock::now();

  std::thread t([&] {
    listener.run([&](const onvif::OnvifEvent& /*ev*/) {
      int64_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;
      auto now = Clock::now();
      std::lock_guard<std::mutex> lk(mu);
      if (n == 1)                    t_start = now;
      if (n >= target && !done) {
        t_end = now; done = true; cv.notify_one();
      }
    });
  });

  {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait_for(lk, std::chrono::seconds(300), [&] { return done; });
  }

  listener.stop();
  t.join();
  const auto wall_after = Clock::now();
  getrusage(RUSAGE_SELF, &ru_after);

  const int64_t final_count = count.load();
  const int64_t elapsed_ns  =
    std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
  const double elapsed_ms  = elapsed_ns * 1e-6;
  const double throughput  = final_count / (elapsed_ns * 1e-9);
  const double us_per_ev   = (elapsed_ns / 1e3) / final_count;

  // CPU utilisation over the full listener lifetime (wall_before → wall_after).
  const double wall_sec =
    std::chrono::duration<double>(wall_after - wall_before).count();
  auto tv_to_sec = [](const struct timeval& tv) {
    return tv.tv_sec + tv.tv_usec * 1e-6;
  };
  const double user_sec = tv_to_sec(ru_after.ru_utime) - tv_to_sec(ru_before.ru_utime);
  const double sys_sec  = tv_to_sec(ru_after.ru_stime) - tv_to_sec(ru_before.ru_stime);
  const double cpu_user  = user_sec / wall_sec * 100.0;
  const double cpu_sys   = sys_sec  / wall_sec * 100.0;
  const double cpu_total = cpu_user + cpu_sys;

  std::cout << "Events:       " << final_count                          << "\n"
            << "Wall time:    " << static_cast<int64_t>(elapsed_ms)    << " ms\n"
            << "Throughput:   " << static_cast<int64_t>(throughput)    << " events/s\n"
            << "Per event:    " << us_per_ev                           << " us\n"
            << "CPU user:     " << cpu_user                            << " %\n"
            << "CPU sys:      " << cpu_sys                             << " %\n"
            << "CPU total:    " << cpu_total                           << " %\n";

  onvif::global_cleanup();
  return 0;
}
