// MOSS Timer Subsystem — Module Interface + Implementation
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
import moss.arch;
import moss.hal.timer;
import moss.hal.intc;
import moss.platform;
import moss.containers;

export namespace moss::kernel::timer {

using moss::u32;
using moss::u64;
using moss::u8;
using moss::kernel::ErrorCode;
using moss::kernel::VoidResult;

// ============================================================================
// Clocksource — divide-free cycle-to-nanosecond conversion
// ============================================================================

class Clocksource {
public:
  Clocksource() noexcept = default;

  /// Initialize from hardware: read frequency, compute mult/shift.
  [[nodiscard]] VoidResult initialize() noexcept;

  /// Initialize from calibrated counter metadata without reading hardware.
  [[nodiscard]] VoidResult initialize(u64 frequency_hz, u64 boot_counter) noexcept;

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

  /// First raw counter value at which now_ns() reaches an absolute deadline.
  [[nodiscard]] u64 deadline_counter(u64 deadline_ns) const noexcept {
    if (!mult_) {
      return 0;
    }
    // Exact bounded inversion, without a freestanding 128-bit division runtime.
    const auto target = static_cast<__uint128_t>(deadline_ns) << shift_;
    u64 low = 0, high = ~u64{0};
    while (low < high) {
      const auto middle = low + (high - low) / 2;
      if (static_cast<__uint128_t>(middle) * mult_ >= target) {
        high = middle;
      } else {
        low = middle + 1;
      }
    }
    return boot_cycles_ + low;
  }

  /// Hardware timer frequency in Hz.
  [[nodiscard]] u64 frequency_hz() const noexcept { return freq_hz_; }

private:
  u64 mult_{0};        // Multiply factor for cycles -> ns: ns = (cycles * mult) >> shift
  u64 inv_mult_{0};    // Inverse factor for ns -> cycles: cycles = (ns * inv_mult) >> shift
  u32 shift_{0};       // Right-shift amount
  u64 freq_hz_{0};     // Raw hardware frequency (Hz)
  u64 boot_cycles_{0}; // Counter value at init time
};

// ============================================================================
// HrTimer — individual high-resolution timer
// ============================================================================

/// Timer callback signature: called from interrupt context.
using TimerCallback = void (*)(void *data) noexcept;

/// Timer mode: one-shot fires once, periodic repeats at interval.
enum class TimerMode : u8 {
  OneShot,
  Periodic,
};

// Forward declaration
class TimerSubsystem;

class HrTimer {
  friend class TimerSubsystem; // TimerSubsystem manages the min-heap

public:
  HrTimer() noexcept = default;

  /// Configure an idle timer before reserve or start.
  void init(TimerMode mode, TimerCallback callback, void *data = nullptr) noexcept;

  /// Hold one queue slot for a one-shot timer without setting an expiry.
  [[nodiscard]] VoidResult reserve() noexcept;

  /// Set or move a one-shot expiry earlier without releasing its queue slot.
  /// A callback already dispatched at the old expiry satisfies the new one.
  [[nodiscard]] VoidResult tighten(u64 earlier_abs_ns) noexcept;

  /// Start with absolute expiry (ns since boot).
  [[nodiscard]] VoidResult start(u64 expires_ns) noexcept;

  /// Start with relative delay from now.
  [[nodiscard]] VoidResult start_relative(u64 delay_ns) noexcept;

  /// Cancel a pending timer.
  void cancel() noexcept;

  /// Cancel and wait for callbacks to finish before freeing the timer/data.
  /// Call from task context, never from this timer's callback.
  void cancel_sync() noexcept;

  /// Query state.
  [[nodiscard]] bool is_active() const noexcept;
  [[nodiscard]] u64 expires_ns() const noexcept { return expires_ns_; }
  [[nodiscard]] TimerMode mode() const noexcept { return mode_; }

private:
  u64 expires_ns_{0};
  u64 interval_ns_{0}; // For periodic timers: repeat interval
  TimerCallback callback_{nullptr};
  void *callback_data_{nullptr};
  TimerMode mode_{TimerMode::OneShot};
  bool active_{false};
  bool reserved_{false};
  bool fired_{false};          // One-shot expiry was extracted for dispatch; guarded by queue_lock_.
  u32 callbacks_in_flight_{0}; // guarded by TimerSubsystem::queue_lock_

  // Heap index: position of this timer in TimerSubsystem's min-heap array.
  // Enables O(log n) cancel/dequeue without linear search.
  // ~0u (UINT32_MAX) means "not in heap".
  u32 heap_index_{~0U};
};

// ============================================================================
// TimerSubsystem — top-level manager (singleton)
// ============================================================================

class TimerSubsystem {
public:
  TimerSubsystem() noexcept = default;
  ~TimerSubsystem() noexcept = default;

  // Non-copyable, non-movable
  TimerSubsystem(const TimerSubsystem &) = delete;
  TimerSubsystem &operator=(const TimerSubsystem &) = delete;

  /// Initialize the entire timer subsystem:
  ///   1. Clocksource init (read frequency, compute mult/shift)
  ///   2. Enable hardware timer
  [[nodiscard]] VoidResult initialize() noexcept;

  /// Shutdown: disable timer.
  void shutdown() noexcept;

  /// Access the clocksource.
  [[nodiscard]] const Clocksource &clocksource() const noexcept { return clocksource_; }

  /// Convenience: current time in nanoseconds since boot.
  [[nodiscard]] u64 now_ns() const noexcept { return clocksource_.now_ns(); }

  /// Insert a timer into the min-heap.
  [[nodiscard]] VoidResult enqueue(HrTimer *timer, u64 expires_ns, u64 interval_ns) noexcept;

  /// Reserve capacity now so a future one-shot expiry can be armed infallibly.
  [[nodiscard]] VoidResult reserve(HrTimer *timer) noexcept;
  [[nodiscard]] VoidResult tighten(HrTimer *timer, u64 earlier_abs_ns) noexcept;

  /// Remove a timer from the min-heap.
  void dequeue(HrTimer *timer) noexcept;
  void cancel_sync(HrTimer *timer) noexcept;
  [[nodiscard]] bool is_active(const HrTimer *timer) const noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(queue_lock_);
    return timer->active_;
  }

  /// Called from the timer interrupt handler.
  void handle_interrupt() noexcept;

  /// CPU 0 IPI hook: apply the latest global heap root to its local timer.
  void reprogram_local() noexcept;

  /// Is the subsystem initialized?
  [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

  /// Check if any timers are pending (for tickless idle decisions).
  [[nodiscard]] bool has_pending_timers() const noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(queue_lock_);
    return heap_size_ > 0;
  }

  /// Statistics.
  struct Stats {
    u64 total_interrupts{0};
    u64 timers_fired{0};
  };
  [[nodiscard]] Stats get_statistics() const noexcept { return stats_; }

  /// Global singleton access.
  static TimerSubsystem &instance() noexcept;

private:
  Clocksource clocksource_;
  Stats stats_{};
  bool initialized_{false};
  mutable containers::IrqSpinLock queue_lock_; // Protects the min-heap and active state

  // ── Min-heap of HrTimer pointers, keyed by expires_ns ──────────
  // O(log n) enqueue/dequeue, O(1) peek (heap_[0] = earliest).
  // Each HrTimer stores its own heap_index_ for O(log n) cancel.
  // 256 bounds fixed queue storage without interrupt-time allocation. Queued
  // timers and unarmed reservations share it; admission returns
  // ResourceExhausted when full. The workload basis for 256 is not recorded,
  // so increasing concurrency requires revisiting this capacity.
  static constexpr u32 MAX_TIMERS = 256;
  HrTimer *heap_[MAX_TIMERS]{};
  u32 heap_size_{0};
  u32 reserved_count_{0}; // Unarmed timers consume the same fixed capacity as heap entries.

  /// Reprogram hardware for next pending expiry.
  void reprogram_next() noexcept;

  /// Insert into min-heap — caller must already hold queue_lock_.
  void enqueue_locked(HrTimer *timer) noexcept;

  // ── Heap operations ────────────────────────────────────────────
  void heap_sift_up(u32 idx) noexcept;
  void heap_sift_down(u32 idx) noexcept;
  void heap_swap(u32 a, u32 b) noexcept;
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
  const u64 frequency_hz = hal::timer::frequency();
  if (frequency_hz == 0) {
    return VoidResult{ErrorCode::InvalidState};
  }
  return initialize(frequency_hz, hal::timer::read_counter());
}

VoidResult Clocksource::initialize(u64 frequency_hz, u64 boot_counter) noexcept {
  // Q32 has 32 fractional bits; 10^9 converts Hz to cycles per nanosecond.
  constexpr u32 shift = 32;
  constexpr u64 ns_per_sec = 1000000000ULL;
  // Reject rates whose integral Q32 inverse would not fit u64; the HAL's
  // supported frequencies are well below this representation limit.
  if (frequency_hz == 0 || frequency_hz / ns_per_sec > (~u64{0} >> shift)) {
    return VoidResult{ErrorCode::InvalidState};
  }
  freq_hz_ = frequency_hz;
  shift_ = shift;

  // Compute mult and shift for: ns = (cycles * mult) >> shift
  // Formula: mult = (10^9 << shift) / freq_hz
  // 1000000000 << 32 = 0x3B9ACA00_00000000 — fits in u64.
  mult_ = (ns_per_sec << shift_) / freq_hz_;

  // floor(freq*2^32/10^9) = (freq/10^9)*2^32 + floor((freq%10^9)*2^32/10^9).
  // Divide the whole units before shifting; the remaining numerator is below
  // 10^9*2^32, so each intermediate fits even at the HAL's 100 GHz ceiling.
  // Avoid >=2^32 Hz overflow without introducing freestanding __udivti3.
  inv_mult_ = ((freq_hz_ / ns_per_sec) << shift_) + ((freq_hz_ % ns_per_sec) << shift_) / ns_per_sec;

  boot_cycles_ = boot_counter;

  return VoidResult{};
}

u64 Clocksource::now_ns() const noexcept {
  u64 current = hal::timer::read_counter();
  u64 delta = current - boot_cycles_;
  // Multiply before shifting in 128 bits: Q32's unshifted product can exceed
  // u64 after only a few seconds even though the final nanosecond value fits.
  return static_cast<u64>((static_cast<__uint128_t>(delta) * mult_) >> shift_);
}

u64 Clocksource::cycles_to_ns(u64 cycles) const noexcept {
  return static_cast<u64>((static_cast<__uint128_t>(cycles) * mult_) >> shift_);
}

u64 Clocksource::ns_to_cycles(u64 ns) const noexcept {
  if (inv_mult_ == 0) {
    return 0;
  }
  // Use multiply-then-shift (no 128-bit division needed — safe in freestanding)
  return static_cast<u64>((static_cast<__uint128_t>(ns) * inv_mult_) >> shift_);
}

// ============================================================================
// HrTimer implementation
// ============================================================================

void HrTimer::init(TimerMode mode, TimerCallback callback, void *data) noexcept {
  mode_ = mode;
  callback_ = callback;
  callback_data_ = data;
  active_ = false;
  heap_index_ = ~0U;
  fired_ = false;
}

VoidResult HrTimer::reserve() noexcept { return TimerSubsystem::instance().reserve(this); }

VoidResult HrTimer::tighten(u64 earlier_abs_ns) noexcept {
  return TimerSubsystem::instance().tighten(this, earlier_abs_ns);
}

VoidResult HrTimer::start(u64 abs_expires_ns) noexcept {
  if (mode_ == TimerMode::Periodic) {
    return VoidResult{ErrorCode::InvalidParameter}; // periodic timers need an interval
  }
  return TimerSubsystem::instance().enqueue(this, abs_expires_ns, 0);
}

VoidResult HrTimer::start_relative(u64 delay_ns) noexcept {
  u64 now = TimerSubsystem::instance().now_ns();
  if (delay_ns > ~u64{0} - now || (mode_ == TimerMode::Periodic && delay_ns == 0)) {
    return VoidResult{ErrorCode::InvalidParameter};
  }
  return TimerSubsystem::instance().enqueue(this, now + delay_ns, mode_ == TimerMode::Periodic ? delay_ns : 0);
}

void HrTimer::cancel() noexcept { TimerSubsystem::instance().dequeue(this); }

void HrTimer::cancel_sync() noexcept { TimerSubsystem::instance().cancel_sync(this); }

bool HrTimer::is_active() const noexcept { return TimerSubsystem::instance().is_active(this); }

// ============================================================================
// TimerSubsystem implementation
// ============================================================================

// Singleton instance
TimerSubsystem &TimerSubsystem::instance() noexcept {
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

  // 3. Park the compare one second (10^9 ns) ahead until work is enqueued.
  // This is a finite quiet interval, not a disabled timer; the empty-queue ISR
  // rearms the same interval. Its exact policy rationale is not recorded.
  u64 now = hal::timer::read_counter();
  hal::timer::set_compare(now + clocksource_.ns_to_cycles(1000000000ULL));

  initialized_ = true;
  return VoidResult{};
}

void TimerSubsystem::shutdown() noexcept {
  hal::timer::disable();
  initialized_ = false;
}

// ── Heap helper operations ──────────────────────────────────────────

void TimerSubsystem::heap_swap(u32 a, u32 b) noexcept {
  HrTimer *tmp = heap_[a];
  heap_[a] = heap_[b];
  heap_[b] = tmp;
  heap_[a]->heap_index_ = a;
  heap_[b]->heap_index_ = b;
}

void TimerSubsystem::heap_sift_up(u32 idx) noexcept {
  while (idx > 0) {
    u32 parent = (idx - 1) / 2;
    if (heap_[idx]->expires_ns_ < heap_[parent]->expires_ns_) {
      heap_swap(idx, parent);
      idx = parent;
    } else {
      break;
    }
  }
}

void TimerSubsystem::heap_sift_down(u32 idx) noexcept {
  while (true) {
    u32 smallest = idx;
    u32 left = 2 * idx + 1;
    u32 right = 2 * idx + 2;

    if (left < heap_size_ && heap_[left]->expires_ns_ < heap_[smallest]->expires_ns_) {
      smallest = left;
    }
    if (right < heap_size_ && heap_[right]->expires_ns_ < heap_[smallest]->expires_ns_) {
      smallest = right;
    }
    if (smallest == idx) {
      break;
    }
    heap_swap(idx, smallest);
    idx = smallest;
  }
}

// ── Enqueue / Dequeue ───────────────────────────────────────────────

VoidResult TimerSubsystem::enqueue(HrTimer *timer, u64 expires_ns, u64 interval_ns) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(queue_lock_);
  if (!initialized_ || timer->active_ || timer->reserved_ || timer->heap_index_ != ~0U ||
      timer->callbacks_in_flight_ != 0) {
    return VoidResult{ErrorCode::InvalidState};
  }
  if (!timer->callback_) {
    return VoidResult{ErrorCode::InvalidParameter};
  }
  if (heap_size_ + reserved_count_ == MAX_TIMERS) {
    return VoidResult{ErrorCode::ResourceExhausted};
  }
  timer->expires_ns_ = expires_ns;
  timer->interval_ns_ = interval_ns;
  timer->active_ = true;
  timer->fired_ = false;
  enqueue_locked(timer);
  return VoidResult{};
}

VoidResult TimerSubsystem::reserve(HrTimer *timer) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(queue_lock_);
  if (!initialized_ || timer->active_ || timer->reserved_ || timer->heap_index_ != ~0U ||
      timer->callbacks_in_flight_ != 0) {
    return VoidResult{ErrorCode::InvalidState};
  }
  if (timer->mode_ != TimerMode::OneShot || !timer->callback_) {
    return VoidResult{ErrorCode::InvalidParameter};
  }
  if (heap_size_ + reserved_count_ == MAX_TIMERS) {
    return VoidResult{ErrorCode::ResourceExhausted};
  }
  timer->reserved_ = true;
  timer->fired_ = false;
  ++reserved_count_;
  return VoidResult{};
}

VoidResult TimerSubsystem::tighten(HrTimer *timer, u64 earlier_abs_ns) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(queue_lock_);
  // IPC uses zero for no deadline. The all-ones timestamp is also reserved:
  // treating it as a finite expiry would occupy a fixed slot indefinitely.
  if (timer->mode_ != TimerMode::OneShot || earlier_abs_ns == 0 || earlier_abs_ns == ~u64{0}) {
    return VoidResult{ErrorCode::InvalidParameter};
  }
  if (timer->reserved_) {
    // The reservation already owns a slot, so concurrent enqueues cannot make
    // this first finite deadline fail for lack of capacity.
    --reserved_count_;
    timer->reserved_ = false;
    timer->expires_ns_ = earlier_abs_ns;
    timer->interval_ns_ = 0;
    timer->active_ = true;
    enqueue_locked(timer);
    return VoidResult{};
  }
  if (timer->active_ && timer->heap_index_ != ~0U) {
    if (earlier_abs_ns < timer->expires_ns_) {
      timer->expires_ns_ = earlier_abs_ns;
      heap_sift_up(timer->heap_index_);
      if (timer->heap_index_ == 0) {
        reprogram_next();
      }
    }
    return VoidResult{};
  }
  // Expiry extraction and callback publication share queue_lock_. Once
  // extracted, the callback will run at or after every earlier deadline.
  return timer->fired_ ? VoidResult{} : VoidResult{ErrorCode::InvalidState};
}

void TimerSubsystem::enqueue_locked(HrTimer *timer) noexcept {
  // Callers hold queue_lock_ and either checked capacity, converted a held
  // reservation, or extracted a periodic timer whose slot is being reused.

  u32 idx = heap_size_;
  heap_[idx] = timer;
  timer->heap_index_ = idx;
  heap_size_++;
  heap_sift_up(idx);

  // Reprogram hardware if the new timer became the earliest (heap root)
  if (timer->heap_index_ == 0) {
    reprogram_next();
  }
}

void TimerSubsystem::dequeue(HrTimer *timer) noexcept {
  containers::LockGuard<containers::IrqSpinLock> guard(queue_lock_);
  timer->active_ = false;
  timer->fired_ = false;
  if (timer->reserved_) {
    timer->reserved_ = false;
    --reserved_count_;
  }
  if (heap_size_ == 0 || timer->heap_index_ == ~0U) {
    return;
  }

  u32 idx = timer->heap_index_;
  if (idx >= heap_size_) {
    return;
  }

  bool was_root = (idx == 0);

  // Move last element to the removed position and shrink heap
  heap_size_--;
  if (idx < heap_size_) {
    heap_[idx] = heap_[heap_size_];
    heap_[idx]->heap_index_ = idx;
    // Restore heap property: sift up or down depending on relative order
    heap_sift_up(idx);
    heap_sift_down(idx);
  }

  timer->heap_index_ = ~0U;

  if (was_root) {
    reprogram_next();
  }
}

void TimerSubsystem::cancel_sync(HrTimer *timer) noexcept {
  dequeue(timer);
  // The callback runs outside queue_lock_; cancellation removes future work
  // but cannot revoke a callback already extracted by another CPU. Waiting
  // without holding the lock lets that callback publish completion safely.
  for (;;) {
    {
      containers::LockGuard<containers::IrqSpinLock> guard(queue_lock_);
      if (timer->callbacks_in_flight_ == 0) {
        return;
      }
    }
    arch::cpu_yield();
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
  // Strategy: lock → extract min + re-enqueue periodic + reprogram → unlock
  //           → fire callback (lock-free) → re-lock for next iteration.

  queue_lock_.lock();

  // 1. Get current time
  u64 now = clocksource_.now_ns();

  // 2. Fire all expired timers (heap root is always the earliest)
  while (heap_size_ > 0 && heap_[0]->expires_ns_ <= now) {
    HrTimer *expired = heap_[0];

    // Remove from heap (extract min)
    heap_size_--;
    if (heap_size_ > 0) {
      heap_[0] = heap_[heap_size_];
      heap_[0]->heap_index_ = 0;
      heap_sift_down(0);
    }
    expired->heap_index_ = ~0U;
    expired->active_ = false;
    expired->fired_ = expired->mode_ == TimerMode::OneShot;

    stats_.timers_fired++;

    // Re-enqueue periodic timers BEFORE firing (callback may not return).
    if (expired->mode_ == TimerMode::Periodic && expired->interval_ns_ > 0) {
      expired->expires_ns_ += expired->interval_ns_;
      expired->active_ = true;
      enqueue_locked(expired); // Already holding queue_lock_
    }

    // Reprogram hardware while still holding the lock (ensures consistent
    // heap state for the compare value computation).
    reprogram_next();

    // Release lock BEFORE callback — callback may context_switch and
    // never return, which is fine since we no longer hold the lock.
    auto callback = expired->callback_;
    void *data = expired->callback_data_;
    ++expired->callbacks_in_flight_;
    queue_lock_.unlock();

    callback(data);

    // Re-acquire lock for next iteration
    queue_lock_.lock();
    --expired->callbacks_in_flight_;

    // Refresh now for next iteration (if callback returned)
    now = clocksource_.now_ns();
  }

  // 3. Final reprogram in case no timers fired or all returned normally
  reprogram_next();

  // 4. Update statistics
  stats_.total_interrupts++;

  queue_lock_.unlock();
}

void TimerSubsystem::reprogram_local() noexcept {
  if (arch::get_current_cpu_id() != 0) {
    return;
  }
  containers::LockGuard<containers::IrqSpinLock> guard(queue_lock_);
  // A reschedule IPI can arrive during boot before the clocksource is ready.
  if (initialized_) {
    reprogram_next();
  }
}

void TimerSubsystem::reprogram_next() noexcept {
  // Only CPU 0 drains the global heap. Hardware compare registers are local
  // to each CPU; writing here on a remote caller would replace that CPU's
  // scheduler tick without making CPU 0 observe the new deadline. SGI 0 is
  // already the reschedule IPI, and its send path waits for delivery only,
  // never for the target handler to acquire queue_lock_.
  if (arch::get_current_cpu_id() != 0) {
    constexpr u32 RESCHEDULE_SGI = 0;
    constexpr u32 BOOT_CPU_MASK = 1U; // CPU 0 owns the global timer heap.
    if (!hal::intc::send_sgi(platform::intc_dist_base(), platform::intc_cpu_base(), RESCHEDULE_SGI, BOOT_CPU_MASK)) {
      arch::kernel_panic("failed to notify timer heap owner");
    }
    return;
  }
  if (heap_size_ > 0) {
    // Convert expires_ns to absolute cycle count for hardware compare:
    //   compare = current_counter + ns_to_cycles(expires_ns - now_ns)
    u64 now = clocksource_.now_ns();
    u64 delta_ns = (heap_[0]->expires_ns_ > now) ? (heap_[0]->expires_ns_ - now) : 0;
    // Enforce minimum delta to avoid interrupt storm on level-triggered PPI.
    // 100 us (10^5 ns) is a latency/coalescing policy, not measured worst-case
    // ISR time. Its tuning basis is not recorded; increasing it delays timers,
    // while reducing it needs interrupt-storm and handler-duration validation.
    constexpr u64 MIN_DELTA_NS = 100000; // 100 µs
    if (delta_ns < MIN_DELTA_NS) {
      delta_ns = MIN_DELTA_NS;
    }
    // The existing 1-second quiet interval also bounds conversion to raw
    // cycles at the HAL's accepted 100 GHz ceiling. Far timers are rechecked
    // once per second; the policy basis for that interval is not recorded.
    constexpr u64 MAX_DELTA_NS = 1000000000ULL;
    if (delta_ns > MAX_DELTA_NS) {
      delta_ns = MAX_DELTA_NS;
    }
    u64 delta_cycles = clocksource_.ns_to_cycles(delta_ns);
    u64 counter_now = hal::timer::read_counter();
    u64 compare = counter_now + delta_cycles;
    hal::timer::set_compare(compare);
  } else {
    // No pending timers: rearm the same one-second quiet interval used at init.
    u64 now_counter = hal::timer::read_counter();
    hal::timer::set_compare(now_counter + clocksource_.ns_to_cycles(1000000000ULL));
  }
}

} // namespace moss::kernel::timer
