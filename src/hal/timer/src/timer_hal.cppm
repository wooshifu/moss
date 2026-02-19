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

module;

#include "arch_detect.h"

export module moss.hal.timer;

import moss.std;
import moss.types;
import moss.platform;

export namespace moss::kernel::hal::timer {

using moss::u32;
using moss::u64;

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
  return (plat_freq != 0) ? plat_freq : 1000000000ULL; // fallback 1 GHz
#elif defined(MOSS_ARCH_RISCV)
  // RISC-V: typically from DTB timebase-frequency; use platform default
  u64 plat_freq = platform::timer_frequency();
  return (plat_freq != 0) ? plat_freq : 10000000ULL; // fallback 10 MHz
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

// ============================================================================
// Compare value — program next interrupt
// ============================================================================

/// Set the compare register so the timer fires when counter reaches `value`.
inline void set_compare(u64 value) noexcept {
#if defined(MOSS_ARCH_ARM64)
  // ARM64: write virtual timer compare value
  asm volatile("msr cntv_cval_el0, %0" :: "r"(value));
  asm volatile("isb");
#elif defined(MOSS_ARCH_X86_64)
  // APIC Timer: write initial count (placeholder — needs calibration)
  (void)value;
#elif defined(MOSS_ARCH_RISCV)
  // RISC-V: write stimecmp CSR (Sstc extension)
  asm volatile("csrw stimecmp, %0" :: "r"(value));
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
  ctl |= 1ULL;           // Set ENABLE
  ctl &= ~(1ULL << 1);   // Clear IMASK
  asm volatile("msr cntv_ctl_el0, %0" :: "r"(ctl));
  asm volatile("isb");
#elif defined(MOSS_ARCH_X86_64)
  // Unmask APIC LVT timer entry (placeholder)
#elif defined(MOSS_ARCH_RISCV)
  // Set SIE.STIE (S-mode timer interrupt enable)
  asm volatile("csrs sie, %0" :: "r"(1ULL << 5));
#endif
}

/// Mask the timer interrupt at the hardware level.
inline void disable() noexcept {
#if defined(MOSS_ARCH_ARM64)
  u64 ctl;
  asm volatile("mrs %0, cntv_ctl_el0" : "=r"(ctl));
  ctl |= (1ULL << 1);    // Set IMASK
  asm volatile("msr cntv_ctl_el0, %0" :: "r"(ctl));
  asm volatile("isb");
#elif defined(MOSS_ARCH_X86_64)
  // Mask APIC LVT timer entry (placeholder)
#elif defined(MOSS_ARCH_RISCV)
  // Clear SIE.STIE
  asm volatile("csrc sie, %0" :: "r"(1ULL << 5));
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
[[nodiscard]] inline u32 irq_number() noexcept {
  return platform::timer_irq();
}

} // namespace moss::kernel::hal::timer
