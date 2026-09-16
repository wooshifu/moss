#pragma once

// Included after the timer/types modules by the production validation image.
namespace moss::test::hardware {

inline bool clocksource_high_frequency_regression() noexcept {
  using moss::u64;
  using moss::kernel::timer::Clocksource;
  // Cover the former u64 shift-overflow boundary and the HAL's 100 GHz ceiling.
  constexpr u64 frequencies[] = {1ULL << 32, 100000000000ULL};
  constexpr u64 ns_per_second = 1000000000ULL;
  constexpr u64 ns_per_millisecond = ns_per_second / 1000;
  Clocksource clock;
  if (clock.initialize(0, 0))
    return false; // A zero-frequency counter has no meaningful conversion.

  for (u64 frequency : frequencies) {
    if (!clock.initialize(frequency, 0))
      return false;
    const u64 cycles_per_ms = clock.ns_to_cycles(ns_per_millisecond);
    const u64 expected = frequency / 1000;
    // Truncating Q32 can lose at most one cycle over these <=1 s samples.
    if (cycles_per_ms > expected || expected - cycles_per_ms > 1)
      return false;
    const u64 cycles_per_second = clock.ns_to_cycles(ns_per_second);
    if (cycles_per_second > frequency || frequency - cycles_per_second > 1)
      return false;

    const u64 converted_ns = clock.cycles_to_ns(frequency);
    // The forward multiplier truncates by <1/2^32 ns per cycle. For one
    // second, ceil(frequency/2^32) bounds the accumulated nanosecond error.
    const u64 error_bound = frequency / (1ULL << 32) + (frequency % (1ULL << 32) != 0);
    if (converted_ns > ns_per_second || ns_per_second - converted_ns > error_bound)
      return false;
  }
  return true;
}

inline bool arm64_mmu_granule_regression() noexcept {
#if defined(MOSS_ARCH_ARM64)
  moss::u64 tcr;
  asm volatile("mrs %0, tcr_el1" : "=r"(tcr));
  // Read the boot-programmed register, not only its constant: TG0[15:14]=0
  // and TG1[31:30]=2 are the distinct Arm encodings of 4 KiB granules.
  return ((tcr >> 14) & 3) == 0 && ((tcr >> 30) & 3) == 2;
#else
  return true; // TCR_EL1 exists only on ARM64; clock coverage remains shared.
#endif
}

} // namespace moss::test::hardware
