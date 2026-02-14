# Timer Subsystem Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement the MOSS timer subsystem with Clocksource, HrTimer, and TimerSubsystem classes, plus a HAL/Timer hardware layer, enabling preemptive scheduling via periodic timer interrupts.

**Architecture:** Four-layer design — HAL/Timer (arch-specific register ops as free functions), Clocksource (divide-free ns conversion), HrTimer (individual timer instances), TimerSubsystem (singleton manager with sorted queue). The CFS scheduler becomes the first hrtimer consumer, replacing its cooperative busy-wait loop with a periodic 6ms timer tick.

**Tech Stack:** C++26 Modules, freestanding environment (no stdlib), inline assembly for ARM64/x86_64/RISC-V timer registers.

---

### Task 1: Extend Platform with TimerDefaults

**Files:**
- Modify: `src/platform/src/platform.cppm`

**Step 1: Add TimerDefaults struct and wire into PlatformDefaults**

Add after `MemoryDefaults` struct (before `PlatformDefaults`):

```cpp
// ============================================================================
// Timer defaults
// ============================================================================
struct TimerDefaults {
  u32 irq;           // Timer IRQ number (PPI for ARM64, etc.)
  u64 frequency;     // Timer frequency in Hz (0 = read from hardware)
};
```

Add `TimerDefaults timer;` field to `PlatformDefaults` struct:

```cpp
struct PlatformDefaults {
  const char     *name;
  UartDefaults    uart;
  IntcDefaults    intc;
  MemoryDefaults  memory;
  TimerDefaults   timer;     // NEW
};
```

Set per-platform timer values in each `DEFAULTS` instance:

| Platform | IRQ | Frequency |
|----------|-----|-----------|
| ARM64 QEMU virt | `27` (PPI #11, virtual timer) | `0` (read `cntfrq_el0` at runtime) |
| x86_64 QEMU | `0` (PIT/APIC placeholder) | `0` (calibrate at runtime) |
| RISC-V QEMU virt | `5` (S-mode timer) | `10000000` (10 MHz from DTB) |

ARM64 example:
```cpp
.timer = {
  .irq       = 27,         // PPI #11 (virtual timer, non-secure EL1)
  .frequency = 0,          // Read from cntfrq_el0 at runtime
},
```

Add convenience accessor after `kernel_virt_base()`:

```cpp
/// Get default timer IRQ number
[[nodiscard]] inline constexpr u32 timer_irq() noexcept {
  return DEFAULTS.timer.irq;
}

/// Get default timer frequency (0 = discover at runtime)
[[nodiscard]] inline constexpr u64 timer_frequency() noexcept {
  return DEFAULTS.timer.frequency;
}
```

**Step 2: Build all presets to verify no regressions**

Run: `uv run build.py`
Expected: All 6 presets pass (the new struct field is additive).

**Step 3: Commit**

```
[platform][timer] add TimerDefaults to PlatformDefaults
```

---

### Task 2: Create HAL/Timer Module

**Files:**
- Create: `src/hal/timer/CMakeLists.txt`
- Create: `src/hal/timer/src/timer_hal.cppm`
- Modify: `src/hal/CMakeLists.txt` — add `add_subdirectory(timer)`

**Step 1: Create CMakeLists.txt**

File: `src/hal/timer/CMakeLists.txt`

```cmake
add_library(moss_hal_timer OBJECT)

target_sources(moss_hal_timer
  PUBLIC FILE_SET CXX_MODULES FILES
    src/timer_hal.cppm
)

target_link_libraries(moss_hal_timer PUBLIC moss_core moss_aal moss_platform)
```

**Step 2: Create timer_hal.cppm**

File: `src/hal/timer/src/timer_hal.cppm`

This module provides architecture-specific timer register operations as free functions, following the same pattern as `moss.hal.intc` and `moss.hal.uart`.

```cpp
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
  // ARM64: write virtual timer compare value, then ensure timer is enabled
  asm volatile("msr cntv_cval_el0, %0" :: "r"(value));
  asm volatile("isb");
#elif defined(MOSS_ARCH_X86_64)
  // APIC Timer: write initial count (placeholder — needs calibration)
  (void)value;
#elif defined(MOSS_ARCH_RISCV)
  // RISC-V: write stimecmp CSR (if available) or SBI call
  // Placeholder — SBI set_timer
  (void)value;
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
  // asm volatile("csrs sie, %0" :: "r"(1ULL << 5));
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
  // asm volatile("csrc sie, %0" :: "r"(1ULL << 5));
#endif
}

// ============================================================================
// Interrupt acknowledgment
// ============================================================================

/// Acknowledge a pending timer interrupt in the ISR.
/// On ARM64 Generic Timer, the interrupt is cleared by writing a new compare
/// value (or disabling the timer), so ack is essentially a no-op; we just
/// ensure ISTATUS is cleared by re-enabling the timer with a future compare.
inline void ack_interrupt() noexcept {
#if defined(MOSS_ARCH_ARM64)
  // ARM64: ISTATUS clears when cntv_cval_el0 > cntvct_el0 or timer disabled.
  // The caller (TimerSubsystem::handle_interrupt) will set a new compare value,
  // which clears ISTATUS. Nothing extra needed here.
#elif defined(MOSS_ARCH_X86_64)
  // APIC EOI — handled by intc_hal::eoi, not duplicated here
#elif defined(MOSS_ARCH_RISCV)
  // Clear SIP.STIP (S-mode timer interrupt pending)
  // asm volatile("csrc sip, %0" :: "r"(1ULL << 5));
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
```

**Step 3: Add subdirectory to hal/CMakeLists.txt**

Append to `src/hal/CMakeLists.txt`:

```cmake
add_subdirectory(timer)
```

**Step 4: Add moss_hal_timer to root CMakeLists.txt link**

In the root `CMakeLists.txt`, add `moss_hal_timer` to `target_link_libraries(moss.elf ...)` list, after `moss_hal_uart`.

**Step 5: Build all presets**

Run: `uv run build.py`
Expected: All 6 presets pass. The HAL module compiles but has no consumers yet.

**Step 6: Commit**

```
[hal][timer] add timer hardware abstraction layer module
```

---

### Task 3: Create Timer Subsystem Module — Clocksource Class

**Files:**
- Create: `src/timer/CMakeLists.txt`
- Create: `src/timer/src/timer.cppm` (module interface — all three classes)
- Create: `src/timer/src/timer.cpp` (module implementation — method bodies)

**Step 1: Create CMakeLists.txt**

File: `src/timer/CMakeLists.txt`

```cmake
add_library(moss_timer OBJECT)

target_sources(moss_timer
  PUBLIC FILE_SET CXX_MODULES FILES
    src/timer.cppm
  PRIVATE
    src/timer.cpp
)

target_link_libraries(moss_timer PRIVATE moss_core moss_aal moss_platform moss_hal_timer)
```

**Step 2: Create timer.cppm — module interface with all class declarations**

File: `src/timer/src/timer.cppm`

This file declares Clocksource, HrTimer, and TimerSubsystem. Only declarations here; implementations go in `timer.cpp`.

```cpp
// MOSS Timer Subsystem — Module Interface
//
// Architecture-independent timer infrastructure:
//   - Clocksource: divide-free cycle-to-nanosecond conversion
//   - HrTimer: individual high-resolution timer instance
//   - TimerSubsystem: singleton manager, sorted queue, ISR dispatch
//
// Consumers (CFS scheduler, future nanosleep, etc.) create HrTimer
// instances and register them with TimerSubsystem.

export module moss.timer;

import moss.std;
import moss.types;
import moss.result;

export namespace moss::kernel::timer {

using moss::u8;
using moss::u32;
using moss::u64;
using moss::kernel::VoidResult;

// ============================================================================
// Clocksource — divide-free cycle-to-nanosecond conversion
// ============================================================================

class Clocksource {
public:
  Clocksource() noexcept = default;

  /// Initialize from hardware: read frequency, compute mult/shift.
  [[nodiscard]] VoidResult initialize() noexcept;

  /// Monotonic time since boot (nanoseconds).
  [[nodiscard]] u64 now_ns() const noexcept;

  /// Monotonic time since boot (microseconds).
  [[nodiscard]] u64 now_us() const noexcept { return now_ns() / 1000; }

  /// Monotonic time since boot (milliseconds).
  [[nodiscard]] u64 now_ms() const noexcept { return now_ns() / 1000000; }

  /// Convert raw cycles to nanoseconds.
  [[nodiscard]] u64 cycles_to_ns(u64 cycles) const noexcept;

  /// Convert nanoseconds to raw cycles.
  [[nodiscard]] u64 ns_to_cycles(u64 ns) const noexcept;

  /// Hardware timer frequency in Hz.
  [[nodiscard]] u64 frequency_hz() const noexcept { return freq_hz_; }

private:
  u64 mult_{0};          // Multiply factor for cycles → ns
  u32 shift_{0};         // Right-shift amount
  u64 freq_hz_{0};       // Raw hardware frequency (Hz)
  u64 boot_cycles_{0};   // Counter value at init time
};

// ============================================================================
// HrTimer — individual high-resolution timer
// ============================================================================

/// Timer callback signature: called from interrupt context.
using TimerCallback = void(*)(void* data) noexcept;

/// Timer mode: one-shot fires once, periodic repeats at interval.
enum class TimerMode : u8 {
  OneShot,
  Periodic,
};

// Forward declaration
class TimerSubsystem;

class HrTimer {
  friend class TimerSubsystem;  // TimerSubsystem manages the sorted list

public:
  HrTimer() noexcept = default;

  /// Configure this timer (must be called before start).
  void init(TimerMode mode, TimerCallback callback,
            void* data = nullptr) noexcept;

  /// Start with absolute expiry (ns since boot).
  void start(u64 expires_ns) noexcept;

  /// Start with relative delay from now.
  void start_relative(u64 delay_ns) noexcept;

  /// Cancel a pending timer.
  void cancel() noexcept;

  /// Query state.
  [[nodiscard]] bool is_active() const noexcept { return active_; }
  [[nodiscard]] u64 expires_ns() const noexcept { return expires_ns_; }
  [[nodiscard]] TimerMode mode() const noexcept { return mode_; }

private:
  u64           expires_ns_{0};
  u64           interval_ns_{0};    // For periodic timers: repeat interval
  TimerCallback callback_{nullptr};
  void*         callback_data_{nullptr};
  TimerMode     mode_{TimerMode::OneShot};
  bool          active_{false};
  HrTimer*      next_{nullptr};     // Sorted linked list linkage
};

// ============================================================================
// TimerSubsystem — top-level manager (singleton)
// ============================================================================

class TimerSubsystem {
public:
  TimerSubsystem() noexcept = default;
  ~TimerSubsystem() noexcept = default;

  // Non-copyable, non-movable
  TimerSubsystem(const TimerSubsystem&) = delete;
  TimerSubsystem& operator=(const TimerSubsystem&) = delete;

  /// Initialize the entire timer subsystem:
  ///   1. Clocksource init (read frequency, compute mult/shift)
  ///   2. Enable hardware timer
  [[nodiscard]] VoidResult initialize() noexcept;

  /// Shutdown: disable timer.
  void shutdown() noexcept;

  /// Access the clocksource.
  [[nodiscard]] const Clocksource& clocksource() const noexcept {
    return clocksource_;
  }

  /// Convenience: current time in nanoseconds since boot.
  [[nodiscard]] u64 now_ns() const noexcept { return clocksource_.now_ns(); }

  /// Insert a timer into the sorted queue.
  void enqueue(HrTimer* timer) noexcept;

  /// Remove a timer from the queue.
  void dequeue(HrTimer* timer) noexcept;

  /// Called from the timer interrupt handler.
  void handle_interrupt() noexcept;

  /// Statistics.
  struct Stats {
    u64 total_interrupts{0};
    u64 timers_fired{0};
    u64 max_latency_ns{0};
  };
  [[nodiscard]] Stats get_statistics() const noexcept { return stats_; }

  /// Global singleton access.
  static TimerSubsystem& instance() noexcept;

private:
  Clocksource clocksource_;
  HrTimer*    queue_head_{nullptr};   // Sorted by expires_ns (ascending)
  Stats       stats_{};
  bool        initialized_{false};

  /// Reprogram hardware for next pending expiry.
  void reprogram_next() noexcept;
};

} // namespace moss::kernel::timer
```

**Step 3: Create timer.cpp — method implementations**

File: `src/timer/src/timer.cpp`

```cpp
// MOSS Timer Subsystem — Module Implementation
//
// Implements Clocksource, HrTimer, and TimerSubsystem methods.
// Uses HAL/Timer for hardware register access.

module;

#include "arch_detect.h"

module moss.timer;

import moss.hal.timer;

namespace moss::kernel::timer {

// ============================================================================
// Clocksource implementation
// ============================================================================

VoidResult Clocksource::initialize() noexcept {
  // Read hardware frequency
  freq_hz_ = hal::timer::frequency();
  if (freq_hz_ == 0) {
    return VoidResult{ErrorCode::InvalidState};
  }

  // Compute mult and shift for: ns = (cycles * mult) >> shift
  // Formula: mult = (10^9 << shift) / freq_hz
  // Choose shift to maximize precision without 64-bit overflow.
  // For frequencies < 4 GHz, shift=32 works well.
  shift_ = 32;

  // Ensure no overflow: (10^9 << 32) fits in u64 if shift <= 34
  // 1000000000 << 32 = 0x3B9ACA00_00000000 — fits in u64
  u64 ns_per_sec = 1000000000ULL;
  mult_ = (ns_per_sec << shift_) / freq_hz_;

  // Record boot timestamp
  boot_cycles_ = hal::timer::read_counter();

  return VoidResult{};
}

u64 Clocksource::now_ns() const noexcept {
  u64 current = hal::timer::read_counter();
  u64 delta = current - boot_cycles_;
  return (delta * mult_) >> shift_;
}

u64 Clocksource::cycles_to_ns(u64 cycles) const noexcept {
  return (cycles * mult_) >> shift_;
}

u64 Clocksource::ns_to_cycles(u64 ns) const noexcept {
  if (mult_ == 0) return 0;
  return (ns << shift_) / mult_;
}

// ============================================================================
// HrTimer implementation
// ============================================================================

void HrTimer::init(TimerMode mode, TimerCallback callback,
                   void* data) noexcept {
  mode_ = mode;
  callback_ = callback;
  callback_data_ = data;
  active_ = false;
  next_ = nullptr;
}

void HrTimer::start(u64 expires_ns) noexcept {
  expires_ns_ = expires_ns;
  if (mode_ == TimerMode::Periodic) {
    // For periodic timers started with absolute expiry,
    // interval must have been set via start_relative previously
    // or manually. If interval is 0, treat as one-shot.
  }
  active_ = true;
  TimerSubsystem::instance().enqueue(this);
}

void HrTimer::start_relative(u64 delay_ns) noexcept {
  u64 now = TimerSubsystem::instance().now_ns();
  expires_ns_ = now + delay_ns;
  if (mode_ == TimerMode::Periodic) {
    interval_ns_ = delay_ns;
  }
  active_ = true;
  TimerSubsystem::instance().enqueue(this);
}

void HrTimer::cancel() noexcept {
  if (active_) {
    TimerSubsystem::instance().dequeue(this);
    active_ = false;
  }
}

// ============================================================================
// TimerSubsystem implementation
// ============================================================================

// Singleton instance
TimerSubsystem& TimerSubsystem::instance() noexcept {
  static TimerSubsystem instance;
  return instance;
}

VoidResult TimerSubsystem::initialize() noexcept {
  if (initialized_) {
    return VoidResult{};
  }

  // 1. Initialize clocksource
  auto result = clocksource_.initialize();
  if (!result) {
    return result;
  }

  // 2. Enable hardware timer
  hal::timer::enable();

  // 3. Program a default tick (no timers yet, but arm the hardware
  //    so the first enqueue() has a working timer)
  // Set compare far in the future to avoid spurious interrupt
  u64 now = hal::timer::read_counter();
  hal::timer::set_compare(now + clocksource_.ns_to_cycles(1000000000ULL)); // 1s

  initialized_ = true;
  return VoidResult{};
}

void TimerSubsystem::shutdown() noexcept {
  hal::timer::disable();
  initialized_ = false;
}

void TimerSubsystem::enqueue(HrTimer* timer) noexcept {
  // Remove from queue first if already enqueued (re-arm case)
  if (timer->next_ != nullptr || timer == queue_head_) {
    dequeue(timer);
  }

  // Insert into sorted position (ascending expires_ns)
  if (queue_head_ == nullptr || timer->expires_ns_ < queue_head_->expires_ns_) {
    // Insert at head
    timer->next_ = queue_head_;
    queue_head_ = timer;
  } else {
    // Walk to find insertion point
    HrTimer* prev = queue_head_;
    while (prev->next_ != nullptr &&
           prev->next_->expires_ns_ <= timer->expires_ns_) {
      prev = prev->next_;
    }
    timer->next_ = prev->next_;
    prev->next_ = timer;
  }

  // Reprogram hardware if the new timer is the earliest
  if (timer == queue_head_) {
    reprogram_next();
  }
}

void TimerSubsystem::dequeue(HrTimer* timer) noexcept {
  if (queue_head_ == nullptr) return;

  if (queue_head_ == timer) {
    queue_head_ = timer->next_;
    timer->next_ = nullptr;
    // Reprogram for new head
    reprogram_next();
    return;
  }

  HrTimer* prev = queue_head_;
  while (prev->next_ != nullptr && prev->next_ != timer) {
    prev = prev->next_;
  }

  if (prev->next_ == timer) {
    prev->next_ = timer->next_;
    timer->next_ = nullptr;
  }
}

void TimerSubsystem::handle_interrupt() noexcept {
  // 1. Ack the hardware interrupt
  hal::timer::ack_interrupt();

  // 2. Get current time
  u64 now = clocksource_.now_ns();

  // 3. Fire all expired timers
  while (queue_head_ != nullptr && queue_head_->expires_ns_ <= now) {
    HrTimer* expired = queue_head_;
    queue_head_ = expired->next_;
    expired->next_ = nullptr;
    expired->active_ = false;

    // Fire callback
    if (expired->callback_) {
      expired->callback_(expired->callback_data_);
    }
    stats_.timers_fired++;

    // Re-enqueue periodic timers
    if (expired->mode_ == TimerMode::Periodic && expired->interval_ns_ > 0) {
      expired->expires_ns_ += expired->interval_ns_;
      expired->active_ = true;
      enqueue(expired);
      // Refresh now — enqueue may have taken time
      now = clocksource_.now_ns();
    }
  }

  // 4. Reprogram hardware for next pending timer
  reprogram_next();

  // 5. Update statistics
  stats_.total_interrupts++;
}

void TimerSubsystem::reprogram_next() noexcept {
  if (queue_head_ != nullptr) {
    u64 target_cycles = clocksource_.ns_to_cycles(queue_head_->expires_ns_)
                        + hal::timer::read_counter()
                        - clocksource_.ns_to_cycles(clocksource_.now_ns());
    // Simplified: convert expires_ns directly to absolute cycle count
    // target = boot_cycles + ns_to_cycles(expires_ns)
    // Since now_ns = cycles_to_ns(counter - boot_cycles),
    // we need: compare = counter_at_boot + ns_to_cycles(expires_ns)
    // But we don't expose boot_cycles_ directly. Use:
    //   compare = current_counter + ns_to_cycles(expires_ns - now_ns)
    u64 now = clocksource_.now_ns();
    u64 delta_ns = (queue_head_->expires_ns_ > now)
                       ? (queue_head_->expires_ns_ - now)
                       : 0;
    u64 delta_cycles = clocksource_.ns_to_cycles(delta_ns);
    u64 compare = hal::timer::read_counter() + delta_cycles;
    hal::timer::set_compare(compare);
  } else {
    // No pending timers — set compare far in the future
    u64 now = hal::timer::read_counter();
    hal::timer::set_compare(now + clocksource_.ns_to_cycles(1000000000ULL));
  }
}

} // namespace moss::kernel::timer
```

**Step 4: Add subdirectory and link in root CMake**

In root `CMakeLists.txt`:
- Add `add_subdirectory(src/timer)` after `add_subdirectory(src/ipc)` (before process)
- Add `moss_timer` to `target_link_libraries(moss.elf ...)` list

**Step 5: Build all presets**

Run: `uv run build.py`
Expected: All 6 presets pass. Timer module compiles but is not yet called from anywhere.

**Step 6: Commit**

```
[timer] add timer subsystem module with Clocksource, HrTimer, TimerSubsystem
```

---

### Task 4: Integrate Timer into Kernel Initialization

**Files:**
- Modify: `src/kernel/src/kernel.cppm` — add `import moss.timer;`, call `TimerSubsystem::instance().initialize()` during kernel init
- Modify: `src/kernel/CMakeLists.txt` — add `moss_timer` to link dependencies

**Step 1: Add import and initialization call**

In `kernel.cppm`:
- Add `import moss.timer;` alongside other imports
- In `Kernel::initialize()`, after GIC init and before scheduler start, add:

```cpp
// Initialize timer subsystem
early_debug_print("正在初始化定时器子系统...\n");
auto timer_result = timer::TimerSubsystem::instance().initialize();
if (!timer_result) {
  early_debug_print("⚠️ 定时器子系统初始化失败\n");
} else {
  early_debug_print("✅ 定时器子系统初始化成功\n");
}
```

In `kernel_main.cpp`, add timer status verification in the status block:

```cpp
// 验证定时器系统状态
auto& timer_sys = timer::TimerSubsystem::instance();
auto timer_stats = timer_sys.get_statistics();
early_debug_print("✅ 定时器子系统: ");
// Print frequency info
early_debug_print("freq=");
// (print clocksource frequency)
early_debug_print(" Hz\n");
```

**Step 2: Add moss_timer to kernel CMakeLists.txt**

Add `moss_timer` and `moss_hal_timer` to the kernel's `target_link_libraries`.

**Step 3: Build all presets**

Run: `uv run build.py`
Expected: All 6 presets pass. Timer initializes during boot on ARM64.

**Step 4: Commit**

```
[kernel][timer] integrate timer subsystem into kernel initialization
```

---

### Task 5: QEMU Verification

**Step 1: Run ARM64 QEMU and verify timer init output**

Run: `./build/arm64-qemu-debug/run_qemu.sh`

Expected output should include:
```
✅ 定时器子系统初始化成功
```

If ARM64 QEMU hangs or panics after timer init, debug by:
1. Check if `cntfrq_el0` returns a valid frequency (QEMU virt: 62500000 Hz = 62.5 MHz)
2. Check if timer enable causes an immediate spurious interrupt
3. Verify GIC has timer IRQ 27 enabled

**Step 2: Verify other presets still build**

Run: `uv run build.py`
Expected: All 6 presets pass.

---

### Task 6 (Future): Scheduler Integration

> **Note:** This task modifies the CFS scheduler's cooperative loop to use the timer.
> It is listed here for completeness but should be done as a separate follow-up
> after verifying the timer subsystem works standalone.

**Files:**
- Modify: `src/process/src/process.cppm` — add HrTimer member to CfsScheduler
- Modify: `src/process/CMakeLists.txt` — add `moss_timer` and `moss_hal_timer` to link

**Summary of changes:**
1. Add `import moss.timer;` to process.cppm
2. Add `timer::HrTimer sched_tick_;` member to CfsScheduler
3. Create `scheduler_tick_callback(void*)` that updates vruntime and checks preemption
4. In `start_scheduling()`, replace the busy-wait loop with:
   ```cpp
   sched_tick_.init(timer::TimerMode::Periodic, scheduler_tick_callback, this);
   sched_tick_.start_relative(CfsParams::SCHED_LATENCY_NS);
   while (true) { arch::cpu_idle_once(); }
   ```
5. Add `need_resched` per-CPU flag checked on interrupt return

This is a significant change to the scheduler and should be thoroughly tested.
