#pragma once

#include "framework/benchmark.hpp"
#include "validation_internal.hpp"

namespace moss::test::validation {
extern moss::bench::Clock clock_info;
void record_batch(u64 ticks, usize operations, bool warmup, u64 overhead);
void timer_benchmark(moss::bench::Context &context, bool wakeup);
void prepare_clock();
void allocation_benchmark(moss::bench::Context &context, unsigned mode);
void read_benchmark(moss::bench::Context &context);
void register_benchmarks();
void catalog_benchmark();
bool has_selected_benchmark();
long start_benchmark();
long benchmark_control(long op, long arg1, long arg2);
} // namespace moss::test::validation
