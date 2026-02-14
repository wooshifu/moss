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
  u64 mult_{0};          // Multiply factor for cycles -> ns
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

  /// Reprogram hardware for next pending expiry.
  void reprogram_next() noexcept;
};

} // namespace moss::kernel::timer
