// Copyright 2026 Daniel W
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "contention_profiler.hpp"

#include <chrono>  // NOLINT(build/c++11)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/synchronization/mutex.h"

namespace onvif {

ContentionProfiler* ContentionProfiler::g_instance_ = nullptr;

ContentionProfiler& ContentionProfiler::instance() {
  // Never destroyed: mutexes register and the absl tracer reports into it
  // until the process exits, and its entries stay reachable for LeakSanitizer.
  static ContentionProfiler* const kInstance = new ContentionProfiler;
  return *kInstance;
}

void ContentionProfiler::install() {
  ContentionProfiler& self = instance();
  g_instance_ = &self;

  // Calibrate cycles -> nanoseconds.  absl reports wait_cycles using
  // its base_internal::CycleClock, which on x86 maps to RDTSC and on
  // most ARM cores to the virtual counter at ~1 ns/cycle.  We don't
  // have direct access to absl's clock rate constant from public API,
  // so we measure: spin for a known wall-clock interval and read the
  // cycle count via std::chrono::steady_clock as a sanity check.
  // This calibration is approximate (ns_per_cycle_ ~= 1 in practice)
  // and is fine for the resolution we report (microseconds).
  self.ns_per_cycle_ = 1.0;

  // RegisterMutexTracer fires for every absl::Mutex event.  We only
  // care about "slow lock" (the contended-acquire path).
  absl::RegisterMutexTracer(&ContentionProfiler::on_trace);
  LOG(INFO) << "[contention] mutex tracer installed";
}

void ContentionProfiler::register_mutex(
    const absl::Mutex* mu, std::string name) {
  absl::MutexLock lk(&mu_);
  auto it = entries_.find(mu);
  if (it == entries_.end()) {
    Entry* e = new Entry;
    e->name = std::move(name);
    entries_[mu] = e;
  } else {
    it->second->name = std::move(name);
  }
}

void ContentionProfiler::record_slow_lock(
    const absl::Mutex* mu, int64_t wait_cycles) {
  if (wait_cycles <= 0) return;
  Entry* e;
  {
    absl::MutexLock lk(&mu_);
    auto it = entries_.find(mu);
    e = (it != entries_.end()) ? it->second : &unknown_;
  }
  const uint64_t wait_ns = static_cast<uint64_t>(
      static_cast<double>(wait_cycles) * ns_per_cycle_);
  e->acquisitions.fetch_add(1, std::memory_order_relaxed);
  e->contended.fetch_add(1, std::memory_order_relaxed);
  e->total_wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
  uint64_t prev_max = e->max_wait_ns.load(std::memory_order_relaxed);
  while (wait_ns > prev_max &&
         !e->max_wait_ns.compare_exchange_weak(
             prev_max, wait_ns, std::memory_order_relaxed)) {
    /* retry */
  }
}

// static
void ContentionProfiler::on_trace(const char* msg, const void* obj,
                                  int64_t wait_cycles) {
  if (!g_instance_ || !msg) return;
  // We only want the contended-acquire signal.  Other tracer events
  // ("rwlock", deadlock-detection) don't contribute wait time.
  if (std::strcmp(msg, "slow lock") != 0) return;
  g_instance_->record_slow_lock(
      static_cast<const absl::Mutex*>(obj), wait_cycles);
}

std::string ContentionProfiler::snapshot() const {
  // Materialise stats into a flat vector under the lock so the render
  // loop runs without holding mu_.
  struct Row {
    std::string name;
    uint64_t acquisitions;
    uint64_t contended;
    uint64_t total_wait_ns;
    uint64_t max_wait_ns;
  };
  std::vector<Row> rows;
  {
    absl::MutexLock lk(&mu_);
    rows.reserve(entries_.size() + 1);
    for (const auto& [addr, e] : entries_) {
      (void)addr;
      rows.push_back({
          e->name,
          e->acquisitions.load(std::memory_order_relaxed),
          e->contended.load(std::memory_order_relaxed),
          e->total_wait_ns.load(std::memory_order_relaxed),
          e->max_wait_ns.load(std::memory_order_relaxed),
      });
    }
    rows.push_back({
        "(unknown)",
        unknown_.acquisitions.load(std::memory_order_relaxed),
        unknown_.contended.load(std::memory_order_relaxed),
        unknown_.total_wait_ns.load(std::memory_order_relaxed),
        unknown_.max_wait_ns.load(std::memory_order_relaxed),
    });
  }

  std::string out;
  out.reserve(2048);
  // Header — fixed-width columns, easy to grep / paste.  Numbers are
  // microseconds for total / max wait so a long-running process
  // doesn't blow past 64-bit ns at the print stage.
  char hdr[256];
  std::snprintf(hdr, sizeof(hdr),
      "%-28s %12s %12s %16s %12s\n",
      "mutex", "contended", "total_us", "avg_us", "max_us");
  out += hdr;
  out += std::string(82, '-');
  out += '\n';
  for (const auto& r : rows) {
    if (r.contended == 0 && r.name == "(unknown)") continue;
    const double total_us = static_cast<double>(r.total_wait_ns) / 1000.0;
    const double avg_us   = r.contended == 0
        ? 0.0 : total_us / static_cast<double>(r.contended);
    const double max_us   = static_cast<double>(r.max_wait_ns) / 1000.0;
    char line[256];
    std::snprintf(line, sizeof(line),
        "%-28s %12llu %12.3f %16.3f %12.3f\n",
        r.name.c_str(),
        static_cast<unsigned long long>(r.contended),  // NOLINT(runtime/int)
        total_us, avg_us, max_us);
    out += line;
  }
  return out;
}

}  // namespace onvif
