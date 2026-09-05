#pragma once

#include "ut_kernel.hpp"

namespace moss::bench {
using Tick = unsigned long long;
using Count = unsigned long;

Tick read_counter() noexcept;
unsigned current_cpu() noexcept;

struct Clock {
  Tick frequency = 0;
  const char *source = "invalid";
  Tick calibration_ticks[3]{};
  Tick reference_ticks[3]{};
  unsigned uncertainty_ppm = 0;
};

struct Context {
  const Clock &clock;
  Count iterations = 0;
  Count capacity = 256;
  unsigned warmup = 5;
  unsigned samples = 30;
  unsigned cpu = 0;
  bool valid = true;
  void (*record)(Tick ticks, Count operations, bool warmup, Tick overhead) = nullptr;

  template <typename Prepare, typename Operation, typename Cleanup>
  void measure_batches(Prepare prepare, Operation operation, Cleanup cleanup) {
    if (!clock.frequency || !capacity || iterations > capacity) {
      valid = false;
      return;
    }
    auto batch = [&](Count count) {
      if (!prepare(count)) {
        (void)cleanup(count);
        valid = false;
        return Tick{0};
      }
      auto before_cpu = current_cpu();
      Tick start = read_counter();
      for (Count i = 0; i < count; ++i) {
        asm volatile("" : "+r"(i) : : "memory");
        operation(i);
      }
      Tick end = read_counter();
      auto after_cpu = current_cpu();
      bool cleaned = cleanup(count);
      valid = valid && cleaned && end > start && before_cpu == cpu && after_cpu == cpu;
      return end - start;
    };
    if (!iterations) {
      iterations = 1;
      while (valid) {
        Tick elapsed = batch(iterations);
        if (elapsed >= clock.frequency / 1000 || iterations >= capacity) {
          break;
        }
        iterations = iterations > capacity / 2 ? capacity : iterations * 2;
      }
    }
    for (unsigned sample = 0; valid && sample < warmup + samples; ++sample) {
      Tick elapsed = batch(iterations);
      Tick start = read_counter();
      for (Count i = 0; i < iterations; ++i) {
        asm volatile("" : "+r"(i) : : "memory");
      }
      Tick overhead = read_counter() - start;
      if (valid && record) {
        record(elapsed, iterations, sample < warmup, overhead);
      }
    }
  }
};

struct Scenario {
  const char *name = nullptr;
  void (*run)(Context &) = nullptr;
};

struct Registry {
  Scenario scenarios[16]{};
  unsigned count = 0;
  const char *error = nullptr;

  bool add(const char *name, void (*run)(Context &)) {
    if (error) {
      return false;
    }
    if (!boost::ut::valid_id(name) || !run) {
      error = "invalid_benchmark";
      return false;
    }
    for (unsigned i = 0; i < count; ++i) {
      if (boost::ut::same_id(name, scenarios[i].name)) {
        error = "duplicate_benchmark";
        return false;
      }
    }
    if (count == 16) {
      error = "benchmark_capacity";
      return false;
    }
    scenarios[count++] = {.name = name, .run = run};
    return true;
  }
};

inline Registry registry{};
inline bool register_benchmark(const char *name, void (*run)(Context &)) { return registry.add(name, run); }
} // namespace moss::bench
