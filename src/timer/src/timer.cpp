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
  // 1000000000 << 32 = 0x3B9ACA00_00000000 — fits in u64.
  shift_ = 32;
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

  // 1. Get current time
  u64 now = clocksource_.now_ns();

  // 2. Fire all expired timers
  while (queue_head_ != nullptr && queue_head_->expires_ns_ <= now) {
    HrTimer* expired = queue_head_;
    queue_head_ = expired->next_;
    expired->next_ = nullptr;
    expired->active_ = false;

    stats_.timers_fired++;

    // IMPORTANT: Re-enqueue periodic timers and reprogram hardware
    // BEFORE firing the callback.  The callback (e.g. scheduler_tick)
    // may call context_switch(), which suspends the current execution
    // and never returns.  If we wait until after the callback,
    // the re-enqueue and reprogram_next() will never execute,
    // and the timer stops forever.
    if (expired->mode_ == TimerMode::Periodic && expired->interval_ns_ > 0) {
      expired->expires_ns_ += expired->interval_ns_;
      expired->active_ = true;
      enqueue(expired);
    }

    // Reprogram hardware for the (possibly re-enqueued) next timer.
    // This ensures the hardware compare value is set even if the
    // callback below does context_switch and never returns.
    reprogram_next();

    // Fire callback — may context_switch and not return!
    if (expired->callback_) {
      expired->callback_(expired->callback_data_);
    }

    // Refresh now for next iteration (if callback returned)
    now = clocksource_.now_ns();
  }

  // 3. Final reprogram in case no timers fired or all returned normally
  reprogram_next();

  // 4. Update statistics
  stats_.total_interrupts++;
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
