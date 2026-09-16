#pragma once

// Included after moss.process/moss.mm and ut_kernel.hpp by the validation TU.
namespace moss::test::scheduler_regression {

// Keep the largest fixture below half of the 16 KiB kernel stack, leaving
// room for the validation framework and the production update call chain.
static_assert(2 * sizeof(kernel::process::Thread) + 2 * sizeof(kernel::process::CfsRunqueue) <=
              static_cast<kernel::usize>(8) * 1024);

// The specified 1024 us period uses exact 1000 ns/us conversion in Moss.
inline constexpr kernel::u64 period_ns = 1024ULL * 1000;
// 32 periods are the specified PELT half-life; 16 Ki-us stays below the
// saturation limit so the initial history's decay remains observable.
inline constexpr kernel::u64 history_us = 16ULL * 1024;

inline void pelt_partitioned_runtime() {
  using namespace kernel;
  process::CfsRunqueue whole_queue, split_queue;
  process::Thread whole(0, 0), split(1, 0);
  whole.se.load_sum = split.se.load_sum = history_us;
  whole.se.util_sum = split.se.util_sum = history_us;
  constexpr u64 elapsed = 32 * period_ns;
  // Four samples per period force the actual update path to carry fractional
  // periods across calls; dropping each remainder prevents any decay.
  constexpr u64 slice = period_ns / 4;
  whole_queue.update_curr_task(&whole, elapsed);
  for (u64 time = 0; time < elapsed; time += slice) {
    split_queue.update_curr_task(&split, slice);
  }
  // Integer decay can lose one us per crossed period when updates are split:
  // 32 periods * 1 us gives the rounding bound. The old missing-phase result
  // differs by about 16 Ki-us, so this bound still independently catches it.
  constexpr u64 rounding_bound = elapsed / period_ns;
  boost::ut::expect(whole.se.load_sum + rounding_bound >= split.se.load_sum &&
                    split.se.load_sum + rounding_bound >= whole.se.load_sum);
  boost::ut::expect(whole.se.util_sum + rounding_bound >= split.se.util_sum &&
                    split.se.util_sum + rounding_bound >= whole.se.util_sum);

  // Half-us samples must carry their ns remainder instead of rounding every
  // call up to one us, which doubles the contribution at this sampling rate.
  constexpr u64 sub_us_slice_ns = 1000 / 2;
  whole_queue.update_curr_task(&whole, period_ns);
  for (u64 time = 0; time < period_ns; time += sub_us_slice_ns) {
    split_queue.update_curr_task(&split, sub_us_slice_ns);
  }
  constexpr u64 final_bound = rounding_bound + 1; // One additional crossed period.
  boost::ut::expect(whole.se.load_sum + final_bound >= split.se.load_sum &&
                    split.se.load_sum + final_bound >= whole.se.load_sum);
  boost::ut::expect(whole.se.util_sum + final_bound >= split.se.util_sum &&
                    split.se.util_sum + final_bound >= whole.se.util_sum);
  boost::ut::expect(whole.se.sum_exec_runtime == elapsed + period_ns &&
                    split.se.sum_exec_runtime == elapsed + period_ns);
}

inline void pelt_half_life() {
  using namespace kernel;
  process::CfsRunqueue seeded_queue, empty_queue;
  process::Thread seeded(0, 0), empty(1, 0);
  seeded.se.load_sum = seeded.se.util_sum = history_us;
  seeded_queue.update_curr_task(&seeded, 32 * period_ns);
  empty_queue.update_curr_task(&empty, 32 * period_ns);
  // Both tasks receive identical running contributions, which cancel when
  // comparing them; only half of the seeded history should remain.
  const u64 remaining_load = seeded.se.load_sum - empty.se.load_sum;
  const u64 remaining_util = seeded.se.util_sum - empty.se.util_sum;
  boost::ut::expect(remaining_load + 1 >= history_us / 2 && remaining_load <= history_us / 2 + 1);
  boost::ut::expect(remaining_util + 1 >= history_us / 2 && remaining_util <= history_us / 2 + 1);
}

inline void pelt_continuous_normalization() {
  using namespace kernel;
  process::CfsRunqueue queue;
  process::Thread current(0, 0);
  // Sixteen half-lives reduce startup bias on the 1024 scale to 1/64 of a
  // unit; an inconsistent divisor remains visible after this warmup.
  for (u64 period = 0; period < static_cast<u64>(32 * 16); ++period) {
    queue.update_curr_task(&current, period_ns);
  }
  boost::ut::expect(current.se.load_avg + 1 >= current.se.weight && current.se.load_avg <= current.se.weight);
  boost::ut::expect(current.se.util_avg + 1 >= 1024 && current.se.util_avg <= 1024);
  // The current quarter-period is elapsed running time. A fixed full-period
  // divisor would falsely treat its unelapsed tail as idle time.
  queue.update_curr_task(&current, period_ns / 4);
  boost::ut::expect(current.se.load_avg + 1 >= current.se.weight && current.se.load_avg <= current.se.weight);
  boost::ut::expect(current.se.util_avg + 1 >= 1024 && current.se.util_avg <= 1024);
}

inline void pelt_large_runtime() {
  using namespace kernel;
  // One 5500 h interval exceeds the old delta*1024 numerator's roughly
  // 5004 h u64 limit. Convert hours using 60 min/h, 60 s/min and 10^9 ns/s.
  constexpr u64 elapsed = 5500ULL * 60 * 60 * 1000000000;
  // Nice 0/19/-20 use weights 1024/15/88761. Seven extra ns makes the
  // non-neutral divisions exercise a nonzero remainder; zero weight retains
  // the existing unweighted fallback. Expected values are independently
  // evaluated floor((elapsed+7)*1024/weight) with arbitrary-precision integers.
  constexpr u32 weights[] = {1024, 15, 88761, 0};
  constexpr i32 nice_values[] = {0, 19, -20, 0};
  constexpr u64 intervals[] = {elapsed, elapsed + 7, elapsed + 7, elapsed};
  constexpr u64 expected[] = {elapsed, 1351680000000000477ULL, 228424645959374ULL, elapsed};
  for (usize i = 0; i < sizeof(weights) / sizeof(weights[0]); ++i) {
    process::CfsRunqueue queue;
    process::Thread current(0, 0);
    current.se.weight = weights[i];
    current.se.nice = nice_values[i];
    queue.update_curr_task(&current, intervals[i]);
    boost::ut::expect(current.se.sum_exec_runtime == intervals[i]);
    boost::ut::expect(current.se.vruntime == expected[i]);
    // Old history vanishes over this many half-lives. Continuous running
    // therefore reaches the task's weight and the full 1024 capacity scale.
    boost::ut::expect(current.se.load_avg == weights[i]);
    boost::ut::expect(current.se.util_avg == 1024);
    if (i == 0) {
      const u64 before_load = current.se.load_sum;
      const u64 before_util = current.se.util_sum;
      // Two 500 ns tails advance exactly one us after the large interval;
      // the first must wait for the second without rounding or losing phase.
      constexpr u64 half_us_ns = 1000 / 2;
      queue.update_curr_task(&current, half_us_ns);
      boost::ut::expect(current.se.load_sum == before_load && current.se.util_sum == before_util);
      queue.update_curr_task(&current, half_us_ns);
      boost::ut::expect(current.se.load_sum == before_load + 1 && current.se.util_sum == before_util + 1);
      boost::ut::expect(current.se.sum_exec_runtime == elapsed + 1000);
      boost::ut::expect(current.se.vruntime == elapsed + 1000);
    }
  }
}

inline void pageblock_units() {
  using namespace kernel;
  // Buddy order counts powers of two in pages; order 9 therefore spans
  // 512 base pages, or one 2 MiB page-table block with the 4 KiB granule.
  boost::ut::expect(mm::PAGEBLOCK_PAGES == (usize{1} << mm::PAGEBLOCK_ORDER));
  boost::ut::expect(mm::PAGEBLOCK_SIZE == mm::PAGEBLOCK_PAGES * kernel::PAGE_SIZE);
  boost::ut::expect(mm::PAGEBLOCK_PAGES == 512 && mm::PAGEBLOCK_SIZE == 2ULL * 1024 * 1024);
}

} // namespace moss::test::scheduler_regression
