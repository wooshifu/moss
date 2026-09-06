// MOSS Timer Hardware Abstraction Layer
//
// Provides architecture-specific timer register operations.
//
// What lives here (architecture-specific):
//   - ARM64: Generic Timer (cntvct_el0, cntv_cval_el0, cntv_ctl_el0)
//   - x86_64: Local APIC Timer / TSC [placeholder]
//   - RISC-V: SBI Timer / stimecmp CSR [placeholder]
//
// What stays in timer.cppm (architecture-independent):
//   - Clocksource class (mult/shift conversion)
//   - HrTimer class (timer instances)
//   - TimerSubsystem class (sorted queue, ISR dispatch)

export module moss.hal.timer;

import moss.std;
import moss.types;
import moss.platform;

export namespace moss::kernel::hal::timer {

using moss::u16;
using moss::u32;
using moss::u64;
using moss::u8;

// ============================================================================
// Timer frequency — read from hardware or platform defaults
// ============================================================================

/// Return the hardware timer frequency in Hz.
[[nodiscard]] inline u64 frequency() noexcept {
#if defined(MOSS_ARCH_ARM64)
  // ARM64 Generic Timer: read counter frequency register
  u64 freq;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  return freq;
#elif defined(MOSS_ARCH_X86_64)
  // x86_64: TSC frequency must be calibrated (placeholder: return platform default)
  u64 plat_freq = platform::timer_frequency();
  return plat_freq;
#elif defined(MOSS_ARCH_RISCV)
  // RISC-V: typically from DTB timebase-frequency; use platform default
  u64 plat_freq = platform::timer_frequency();
  return plat_freq;
#endif
}

// ============================================================================
// Counter read — monotonic hardware counter
// ============================================================================

/// Read the current value of the hardware counter.
[[nodiscard]] inline u64 read_counter() noexcept {
#if defined(MOSS_ARCH_ARM64)
  u64 val;
  asm volatile("mrs %0, cntvct_el0" : "=r"(val));
  return val;
#elif defined(MOSS_ARCH_X86_64)
  u32 lo, hi;
  asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return (static_cast<u64>(hi) << 32) | lo;
#elif defined(MOSS_ARCH_RISCV)
  u64 val;
  asm volatile("rdtime %0" : "=r"(val));
  return val;
#endif
}

#if defined(MOSS_ARCH_X86_64)
inline u64 lapic_frequency = 0; // Counter frequency after the configured divide-by-16.

[[nodiscard]] inline bool calibrate() noexcept {
  // Measure TSC and LAPIC against the legacy PIT reference clock. This profile
  // requires a working PIT; no guessed GHz value is used if it is unavailable.
  auto out = [](u16 port, u8 value) { asm volatile("outb %0, %1" ::"a"(value), "Nd"(port)); };
  auto in = [](u16 port) {
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
  };
  auto count = [&] {
    out(0x43, 0);
    u16 low = in(0x40);
    return static_cast<u16>(low | (static_cast<u16>(in(0x40)) << 8));
  };
  auto *initial = reinterpret_cast<volatile u32 *>(platform::intc_dist_base() + 0x380);
  auto *current = reinterpret_cast<volatile u32 *>(platform::intc_dist_base() + 0x390);
  out(0x43, 0x30);
  out(0x40, 0xFF);
  out(0x40, 0xFF);
  u16 first = count(), last = first;
  u64 tsc_start = read_counter();
  *initial = ~0U;
  for (u32 retry = 0; retry < 1000000 && first - last < 20000; ++retry) {
    last = count();
  }
  u64 apic_ticks = ~0U - *current;
  u64 tsc_ticks = read_counter() - tsc_start;
  *initial = 0;
  if (last > first || first - last < 20000 || first - last > 60000 || !tsc_ticks || !apic_ticks) {
    return false;
  }
  u64 reference_ticks = static_cast<u64>(first - last);
  platform::hardware.timebase_frequency = tsc_ticks * 1193182ULL / reference_ticks;
  lapic_frequency = apic_ticks * 1193182ULL / reference_ticks;
  return frequency() >= 1000 && frequency() <= 100000000000ULL && lapic_frequency >= 1000 &&
         lapic_frequency <= 100000000ULL;
}
#endif

// ============================================================================
// Compare value — program next interrupt
// ============================================================================

/// Set the compare register so the timer fires when counter reaches `value`.
inline void set_compare(u64 value) noexcept {
#if defined(MOSS_ARCH_ARM64)
  // ARM64: write virtual timer compare value
  asm volatile("msr cntv_cval_el0, %0" ::"r"(value));
  asm volatile("isb");
#elif defined(MOSS_ARCH_X86_64)
  {
    u64 now = read_counter();
    u64 delta = value > now ? value - now : 1;
    u64 seconds = delta / frequency();
    u64 ticks = seconds > 0xFFFFFFFFULL / lapic_frequency
                    ? 0xFFFFFFFFULL
                    : seconds * lapic_frequency + (delta % frequency()) * lapic_frequency / frequency();
    if (ticks > 0xFFFFFFFFULL) {
      ticks = 0xFFFFFFFFULL;
    }
    *reinterpret_cast<volatile u32 *>(platform::intc_dist_base() + 0x380) = static_cast<u32>(ticks ? ticks : 1);
  }
#elif defined(MOSS_ARCH_RISCV)
  // SBI TIME works with and without the optional Sstc extension.
  register u64 a0 asm("a0") = value;
  register u64 a1 asm("a1") = 0;
  register u64 a6 asm("a6") = 0;
  register u64 a7 asm("a7") = 0x54494D45;
  asm volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory");
#endif
}

// ============================================================================
// Enable / disable timer interrupt
// ============================================================================

/// Unmask the timer interrupt at the hardware level.
inline void enable() noexcept {
#if defined(MOSS_ARCH_ARM64)
  // cntv_ctl_el0: bit0 = ENABLE, bit1 = IMASK (0 = not masked)
  u64 ctl;
  asm volatile("mrs %0, cntv_ctl_el0" : "=r"(ctl));
  ctl |= 1ULL;         // Set ENABLE
  ctl &= ~(1ULL << 1); // Clear IMASK
  asm volatile("msr cntv_ctl_el0, %0" ::"r"(ctl));
  asm volatile("isb");
#elif defined(MOSS_ARCH_X86_64)
  // Unmask LAPIC LVT Timer (clear bit 16 = mask bit)
  {
    auto *lvt_timer = reinterpret_cast<volatile u32 *>(platform::intc_dist_base() + 0x320);
    *lvt_timer &= ~(1U << 16);
  }
#elif defined(MOSS_ARCH_RISCV)
  // Set SIE.STIE (S-mode timer interrupt enable)
  asm volatile("csrs sie, %0" ::"r"(1ULL << 5));
#endif
}

/// Mask the timer interrupt at the hardware level.
inline void disable() noexcept {
#if defined(MOSS_ARCH_ARM64)
  u64 ctl;
  asm volatile("mrs %0, cntv_ctl_el0" : "=r"(ctl));
  ctl |= (1ULL << 1); // Set IMASK
  asm volatile("msr cntv_ctl_el0, %0" ::"r"(ctl));
  asm volatile("isb");
#elif defined(MOSS_ARCH_X86_64)
  // Mask LAPIC LVT Timer (set bit 16 = mask bit)
  {
    auto *lvt_timer = reinterpret_cast<volatile u32 *>(platform::intc_dist_base() + 0x320);
    *lvt_timer |= (1U << 16);
  }
#elif defined(MOSS_ARCH_RISCV)
  // Clear SIE.STIE
  asm volatile("csrc sie, %0" ::"r"(1ULL << 5));
#endif
}

// ============================================================================
// Interrupt acknowledgment
// ============================================================================

/// Acknowledge a pending timer interrupt in the ISR.
/// On ARM64 Generic Timer, the interrupt is cleared by writing a new compare
/// value (or disabling the timer), so ack is essentially a no-op; the caller
/// (TimerSubsystem::handle_interrupt) will set a new compare value.
inline void ack_interrupt() noexcept {
#if defined(MOSS_ARCH_ARM64)
  // ARM64: ISTATUS clears when cntv_cval_el0 > cntvct_el0 or timer disabled.
  // The caller will set a new compare value, which clears ISTATUS.
#elif defined(MOSS_ARCH_X86_64)
  // APIC EOI — handled by intc_hal::eoi, not duplicated here
#elif defined(MOSS_ARCH_RISCV)
  // On RISC-V with Sstc, writing stimecmp clears the pending timer interrupt.
  // No explicit SIP.STIP clear needed — the caller will set a new compare value.
#endif
}

// ============================================================================
// Timer IRQ number query
// ============================================================================

/// Return the IRQ number used by the timer, as configured in PlatformDefaults.
[[nodiscard]] inline u32 irq_number() noexcept { return platform::timer_irq(); }

} // namespace moss::kernel::hal::timer
