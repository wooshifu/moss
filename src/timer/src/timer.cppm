// MOSS Timer Subsystem — Module Interface + Implementation
//
// Architecture-independent timer infrastructure:
//   - Clocksource: divide-free cycle-to-nanosecond conversion
//   - HrTimer: individual high-resolution timer instance
//   - TimerSubsystem: singleton manager, sorted queue, ISR dispatch
//
// Consumers (CFS scheduler, future nanosleep, etc.) create HrTimer
// instances and register them with TimerSubsystem.

module;

#include "arch_detect.h"

export module moss.timer;

import moss.std;
import moss.types;
import moss.result;
import moss.hal.timer;
import moss.containers;

export namespace moss::kernel::timer {

using moss::u8;
using moss::u32;
using moss::u64;
using moss::kernel::VoidResult;
using moss::kernel::ErrorCode;

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
  u64 mult_{0};          // Multiply factor for cycles -> ns: ns = (cycles * mult) >> shift
  u64 inv_mult_{0};      // Inverse factor for ns -> cycles: cycles = (ns * inv_mult) >> shift
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

  /// Is the subsystem initialized?
  [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

  /// Statistics.
  struct Stats {
    u64 total_interrupts{0};
    u64 timers_fired{0};
  };
  [[nodiscard]] Stats get_statistics() const noexcept { return stats_; }

  /// Global singleton access.
  static TimerSubsystem& instance() noexcept;

private:
  Clocksource clocksource_;
  HrTimer*    queue_head_{nullptr};   // Sorted by expires_ns (ascending)
  Stats       stats_{};
  bool        initialized_{false};
  containers::IrqSpinLock queue_lock_;  // Protects queue_head_ linked list

  /// Reprogram hardware for next pending expiry.
  void reprogram_next() noexcept;

  /// Insert into sorted queue — caller must already hold queue_lock_.
  void enqueue_locked(HrTimer* timer) noexcept;
};

} // namespace moss::kernel::timer

// ============================================================================
// Implementation (merged from timer.cpp)
// ============================================================================

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
  // 1000000000 << 32 = 0x3B9ACA00_00000000 — fits in u64.
  shift_ = 32;
  u64 ns_per_sec = 1000000000ULL;
  mult_ = (ns_per_sec << shift_) / freq_hz_;

  // Compute inverse: cycles = (ns * inv_mult) >> shift
  // inv_mult = (freq_hz << shift) / 10^9
  inv_mult_ = (freq_hz_ << shift_) / ns_per_sec;

  // Record boot timestamp
  boot_cycles_ = hal::timer::read_counter();

  return VoidResult{};
}

u64 Clocksource::now_ns() const noexcept {
  u64 current = hal::timer::read_counter();
  u64 delta = current - boot_cycles_;
  // Use 128-bit multiply to prevent overflow (u64 * u64 overflows after ~69s at 62MHz)
  return static_cast<u64>(
      (static_cast<__uint128_t>(delta) * mult_) >> shift_);
}

u64 Clocksource::cycles_to_ns(u64 cycles) const noexcept {
  return static_cast<u64>(
      (static_cast<__uint128_t>(cycles) * mult_) >> shift_);
}

u64 Clocksource::ns_to_cycles(u64 ns) const noexcept {
  if (inv_mult_ == 0) return 0;
  // Use multiply-then-shift (no 128-bit division needed — safe in freestanding)
  return static_cast<u64>(
      (static_cast<__uint128_t>(ns) * inv_mult_) >> shift_);
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

void HrTimer::start(u64 abs_expires_ns) noexcept {
  expires_ns_ = abs_expires_ns;
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
  static TimerSubsystem inst;
  return inst;
}

VoidResult TimerSubsystem::initialize() noexcept {
  if (initialized_) {
    return VoidResult{};
  }

  // 1. Initialize clocksource (read frequency, compute mult/shift)
  auto result = clocksource_.initialize();
  if (!result) {
    return result;
  }

  // 2. Enable hardware timer
  hal::timer::enable();

  // 3. Set compare far in the future to avoid spurious interrupt before
  //    any timer is enqueued
  u64 now = hal::timer::read_counter();
  hal::timer::set_compare(now + clocksource_.ns_to_cycles(1000000000ULL));

  initialized_ = true;
  return VoidResult{};
}

void TimerSubsystem::shutdown() noexcept {
  hal::timer::disable();
  initialized_ = false;
}

void TimerSubsystem::enqueue(HrTimer* timer) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(queue_lock_);
  enqueue_locked(timer);
}

void TimerSubsystem::enqueue_locked(HrTimer* timer) noexcept {
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
  containers::LockGuard<containers::IrqSpinLock> guard(queue_lock_);
  if (queue_head_ == nullptr) return;

  if (queue_head_ == timer) {
    queue_head_ = timer->next_;
    timer->next_ = nullptr;
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
  // Note: hardware timer ack is done by the caller (timer_irq_handler)
  // before invoking this method.
  //
  // IMPORTANT: We must NOT hold queue_lock_ across the callback invocation.
  // The callback (e.g. scheduler_tick) may call context_switch(), which
  // suspends the current execution and never returns.  If the LockGuard
  // destructor never runs, queue_lock_ stays locked forever, deadlocking
  // all future timer interrupts on this CPU.
  //
  // Strategy: lock → dequeue + re-enqueue periodic + reprogram → unlock
  //           → fire callback (lock-free) → re-lock for next iteration.

  queue_lock_.lock();

  // 1. Get current time
  u64 now = clocksource_.now_ns();

  // 2. Fire all expired timers
  while (queue_head_ != nullptr && queue_head_->expires_ns_ <= now) {
    HrTimer* expired = queue_head_;
    queue_head_ = expired->next_;
    expired->next_ = nullptr;
    expired->active_ = false;

    stats_.timers_fired++;

    // Re-enqueue periodic timers BEFORE firing (callback may not return).
    if (expired->mode_ == TimerMode::Periodic && expired->interval_ns_ > 0) {
      expired->expires_ns_ += expired->interval_ns_;
      expired->active_ = true;
      enqueue_locked(expired);  // Already holding queue_lock_
    }

    // Reprogram hardware while still holding the lock (ensures consistent
    // queue state for the compare value computation).
    reprogram_next();

    // Release lock BEFORE callback — callback may context_switch and
    // never return, which is fine since we no longer hold the lock.
    queue_lock_.unlock();

    if (expired->callback_) {
      expired->callback_(expired->callback_data_);
    }

    // Re-acquire lock for next iteration
    queue_lock_.lock();

    // Refresh now for next iteration (if callback returned)
    now = clocksource_.now_ns();
  }

  // 3. Final reprogram in case no timers fired or all returned normally
  reprogram_next();

  // 4. Update statistics
  stats_.total_interrupts++;

  queue_lock_.unlock();
}

void TimerSubsystem::reprogram_next() noexcept {
  if (queue_head_ != nullptr) {
    // Convert expires_ns to absolute cycle count for hardware compare:
    //   compare = current_counter + ns_to_cycles(expires_ns - now_ns)
    u64 now = clocksource_.now_ns();
    u64 delta_ns = (queue_head_->expires_ns_ > now)
                       ? (queue_head_->expires_ns_ - now)
                       : 0;
    // Enforce minimum delta to avoid interrupt storm on level-triggered PPI.
    // 100 µs minimum gives the ISR enough time to complete.
    constexpr u64 MIN_DELTA_NS = 100000;  // 100 µs
    if (delta_ns < MIN_DELTA_NS) {
      delta_ns = MIN_DELTA_NS;
    }
    u64 delta_cycles = clocksource_.ns_to_cycles(delta_ns);
    u64 counter_now = hal::timer::read_counter();
    u64 compare = counter_now + delta_cycles;
    hal::timer::set_compare(compare);
  } else {
    // No pending timers — set compare far in the future
    u64 now_counter = hal::timer::read_counter();
    hal::timer::set_compare(now_counter + clocksource_.ns_to_cycles(1000000000ULL));
  }
}

} // namespace moss::kernel::timer
