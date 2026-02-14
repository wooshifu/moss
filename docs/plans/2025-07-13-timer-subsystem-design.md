# MOSS Timer Subsystem Design

## Problem

MOSS kernel boots into a cooperative scheduling loop: the CFS scheduler runs
20 synthetic test threads via busy-wait, with no timer interrupt driving
preemption. There is no concept of wall-clock time, no `nanosleep`, and no
clocksource abstraction. The only time primitive is raw
`arch::get_timestamp_counter()` returning CPU cycles with no conversion to
real-world time units.

## Goals

1. **Preemptive scheduling** -- timer interrupt drives CFS tick, replacing
   cooperative busy-wait.
2. **Nanosecond timekeeping** -- monotonic `now_ns()` since boot, using
   divide-free mult/shift conversion.
3. **hrtimer framework** -- register/cancel high-resolution timers with
   callbacks, supporting one-shot and periodic modes.
4. **Multi-architecture** -- ARM64 Generic Timer (primary), x86_64 Local APIC
   Timer and RISC-V SBI Timer (placeholders, compilable stubs).

## Non-Goals

- Wall-clock / RTC / NTP synchronization
- POSIX timer syscalls (`timer_create`, `clock_gettime`) -- deferred to future
  syscall implementation phase
- Dynamic clocksource selection (single source per arch is sufficient)
- Tickless (NO_HZ) idle -- periodic tick is acceptable for now

## Architecture

### Module Hierarchy

```
moss.platform          -- TimerDefaults (IRQ, frequency)
  |
moss.hal.timer         -- hardware register ops (set_compare, enable, ack)
  |
moss.timer             -- Clocksource + HrTimer + TimerSubsystem classes
  |
consumers              -- CFS scheduler tick, future nanosleep, etc.
```

### File Layout

```
src/platform/src/platform.cppm    -- extend: add TimerDefaults
src/hal/timer/CMakeLists.txt       -- new
src/hal/timer/src/timer_hal.cppm   -- new: moss.hal.timer
src/timer/CMakeLists.txt           -- new
src/timer/src/timer.cppm           -- new: moss.timer (interface + implementation)
```

## Detailed Design

### Layer 1: HAL/Timer (`moss.hal.timer`)

Architecture-specific timer register operations. Same pattern as `hal.uart`
and `hal.intc`: per-arch `#if` blocks in a single `.cppm` file. This layer
remains free-function style (thin wrappers over inline assembly), consistent
with the other HAL modules.

```cpp
export namespace moss::kernel::hal::timer {
  u64  frequency() noexcept;           // hardware timer frequency (Hz)
  u64  read_counter() noexcept;        // current counter value
  void set_compare(u64 value) noexcept; // next interrupt compare value
  void enable() noexcept;              // unmask timer interrupt
  void disable() noexcept;             // mask timer interrupt
  void ack_interrupt() noexcept;       // clear pending in ISR
  u32  irq_number() noexcept;          // timer IRQ for GIC/APIC/PLIC
}
```

Per-architecture mapping:

| Op | ARM64 | x86_64 | RISC-V |
|----|-------|--------|--------|
| `frequency()` | `mrs cntfrq_el0` | TSC calibration | DTB `timebase-frequency` |
| `read_counter()` | `mrs cntvct_el0` | `rdtsc` | `rdtime` |
| `set_compare()` | `msr cntv_cval_el0` | APIC initial count | `stimecmp` CSR |
| `enable()` | `cntv_ctl_el0 \|= 1` | unmask APIC LVT | `sie \|= STIE` |
| `ack_interrupt()` | clear ISTATUS | APIC EOI | clear `sip.STIP` |
| IRQ | 27 (PPI) | APIC local | 5 (S-mode) |

### Layer 2: Clocksource Class

Converts raw hardware counter values to nanoseconds without runtime division.

```cpp
export namespace moss::kernel::timer {

class Clocksource {
public:
  Clocksource() noexcept = default;

  // Initialize from hardware (reads frequency, computes mult/shift)
  [[nodiscard]] VoidResult initialize() noexcept;

  // Monotonic time since boot
  [[nodiscard]] u64 now_ns() const noexcept;
  [[nodiscard]] u64 now_us() const noexcept;
  [[nodiscard]] u64 now_ms() const noexcept;

  // Conversion utilities
  [[nodiscard]] u64 cycles_to_ns(u64 cycles) const noexcept;
  [[nodiscard]] u64 ns_to_cycles(u64 ns) const noexcept;

  // Hardware info
  [[nodiscard]] u64 frequency_hz() const noexcept { return freq_hz_; }

private:
  u64 mult_{0};          // multiply factor
  u32 shift_{0};         // right-shift amount
  u64 freq_hz_{0};       // raw hardware frequency
  u64 boot_cycles_{0};   // counter value at init time
};

} // namespace
```

**Key formula**: `ns = ((cycles - boot_cycles) * mult) >> shift`

`mult` and `shift` are pre-computed at init time:
```
mult = (10^9 << shift) / freq_hz
```
where `shift` is chosen to maximize precision without 64-bit overflow
(typically shift=32 for frequencies < 4 GHz).

### Layer 3: HrTimer Class

Individual high-resolution timer instance.

```cpp
export namespace moss::kernel::timer {

// Callback signature
using TimerCallback = void(*)(void* data) noexcept;

enum class TimerMode : u8 {
  OneShot,    // fires once, then deactivates
  Periodic,   // repeats at interval until cancelled
};

class HrTimer {
  friend class TimerSubsystem;   // TimerSubsystem manages the sorted list

public:
  HrTimer() noexcept = default;

  // Configure this timer (must be called before start)
  void init(TimerMode mode, TimerCallback callback,
            void* data = nullptr) noexcept;

  // Start with absolute expiry (ns since boot)
  void start(u64 expires_ns) noexcept;

  // Start with relative delay from now
  void start_relative(u64 delay_ns) noexcept;

  // Cancel a pending timer
  void cancel() noexcept;

  // Query state
  [[nodiscard]] bool is_active() const noexcept { return active_; }
  [[nodiscard]] u64 expires_ns() const noexcept { return expires_ns_; }
  [[nodiscard]] TimerMode mode() const noexcept { return mode_; }

private:
  u64           expires_ns_{0};
  u64           interval_ns_{0};
  TimerCallback callback_{nullptr};
  void*         callback_data_{nullptr};
  TimerMode     mode_{TimerMode::OneShot};
  bool          active_{false};
  HrTimer*      next_{nullptr};    // sorted linked list linkage
};

} // namespace
```

### Layer 4: TimerSubsystem — Top-Level Manager

Owns the clocksource, manages the hrtimer queue, and handles the hardware
interrupt.

```cpp
export namespace moss::kernel::timer {

class TimerSubsystem {
public:
  TimerSubsystem() noexcept = default;
  ~TimerSubsystem() noexcept = default;

  // Non-copyable, non-movable
  TimerSubsystem(const TimerSubsystem&) = delete;
  TimerSubsystem& operator=(const TimerSubsystem&) = delete;

  // Initialize the entire timer subsystem:
  //   1. Clocksource init (read frequency, compute mult/shift)
  //   2. Register timer IRQ handler with GIC
  //   3. Enable hardware timer
  [[nodiscard]] VoidResult initialize() noexcept;

  // Shutdown: disable timer, deregister IRQ
  void shutdown() noexcept;

  // Access the clocksource
  [[nodiscard]] const Clocksource& clocksource() const noexcept {
    return clocksource_;
  }

  // Convenience: current time
  [[nodiscard]] u64 now_ns() const noexcept { return clocksource_.now_ns(); }

  // hrtimer queue management (called by HrTimer::start/cancel)
  void enqueue(HrTimer* timer) noexcept;
  void dequeue(HrTimer* timer) noexcept;

  // Called from interrupt handler
  void handle_interrupt() noexcept;

  // Statistics
  struct Stats {
    u64 total_interrupts;
    u64 timers_fired;
    u64 max_latency_ns;     // worst-case ISR latency
  };
  [[nodiscard]] Stats get_statistics() const noexcept { return stats_; }

  // Global singleton access
  static TimerSubsystem& instance() noexcept;

private:
  Clocksource clocksource_;
  HrTimer*    queue_head_{nullptr};  // sorted by expires_ns
  Stats       stats_{};
  bool        initialized_{false};

  // Reprogram hardware for next expiry
  void reprogram_next() noexcept;
};

} // namespace
```

**Singleton pattern**: `TimerSubsystem::instance()` returns a reference to a
global static instance, consistent with `ContainerLibrary` in the existing
codebase.

**HrTimer methods delegate to the subsystem**:
```cpp
void HrTimer::start(u64 expires_ns) noexcept {
  expires_ns_ = expires_ns;
  active_ = true;
  TimerSubsystem::instance().enqueue(this);
}

void HrTimer::cancel() noexcept {
  if (active_) {
    TimerSubsystem::instance().dequeue(this);
    active_ = false;
  }
}
```

**Data structure**: sorted singly-linked list ordered by `expires_ns`.

- Insert: O(n) walk to find position, n = active timer count.
- Pop expired: O(1) from head.
- Rationale: expected active timer count is small (< 20), no need for
  red-black tree complexity. Can upgrade later if needed.

**Interrupt handler flow** (`TimerSubsystem::handle_interrupt()`):

```
1. hal::timer::ack_interrupt()
2. now = clocksource_.now_ns()
3. while (queue_head_ && queue_head_->expires_ns_ <= now):
     pop head
     head->active_ = false
     head->callback_(head->callback_data_)
     stats_.timers_fired++
     if head->mode_ == Periodic:
       head->expires_ns_ += head->interval_ns_
       head->active_ = true
       enqueue(head)   // re-insert in sorted position
4. reprogram_next()    // set hardware compare for queue_head_
5. stats_.total_interrupts++
```

### Scheduler Integration

The CFS scheduler is the first hrtimer consumer.

1. **New**: `scheduler_tick_callback(void*)` -- called by hrtimer every
   `SCHED_LATENCY_NS` (6 ms):
   - Update current thread's `vruntime`
   - Check `should_preempt()` -> set `need_resched` flag
   - The interrupt return path checks the flag and performs context switch

2. **Modified**: `CfsScheduler::start_scheduling()` -- remove busy-wait loop:
   ```cpp
   // Before: busy-wait with synthetic tasks
   // After:
   void start_scheduling() noexcept {
     sched_tick_.init(TimerMode::Periodic, scheduler_tick_callback, this);
     sched_tick_.start_relative(CfsParams::SCHED_LATENCY_NS);
     while (true) { arch::cpu_idle(); }  // WFI/HLT until interrupt
   }
   ```

3. **New**: `need_resched` per-CPU flag, checked on interrupt return.

4. **New**: `CfsScheduler` holds an `HrTimer sched_tick_` member for the
   periodic scheduler tick.

### Platform Extension

Add `TimerDefaults` to `PlatformDefaults`:

```cpp
struct TimerDefaults {
  u32 irq;         // timer IRQ number
  u64 frequency;   // timer freq Hz (0 = read from hardware)
};
```

| Platform | IRQ | Frequency |
|----------|-----|-----------|
| ARM64 QEMU virt | 27 | 0 (read `cntfrq_el0`) |
| x86_64 QEMU | 0 | 0 (calibrate) |
| RISC-V QEMU virt | 5 | 10000000 (10 MHz from DTB) |

### Class Interaction Diagram

```
 ┌─────────────────────────────────────────────────────┐
 │                   TimerSubsystem                     │
 │  ┌──────────────┐   ┌──────────────────────────┐    │
 │  │ Clocksource  │   │ Sorted HrTimer Queue     │    │
 │  │              │   │  head -> [t1] -> [t2] ... │    │
 │  │ mult_, shift_│   └──────────────────────────┘    │
 │  │ boot_cycles_ │                                    │
 │  └──────────────┘   handle_interrupt()               │
 │                       enqueue() / dequeue()           │
 │                       reprogram_next()                │
 └───────────┬─────────────────────────────────────────┘
             │ uses
 ┌───────────▼─────────────────────────────────────────┐
 │               hal::timer (free functions)             │
 │  frequency() | read_counter() | set_compare()        │
 │  enable() | disable() | ack_interrupt()              │
 └──────────────────────────────────────────────────────┘

 Consumers:
 ┌────────────────────┐   ┌─────────────┐
 │ CfsScheduler       │   │ (future)    │
 │  HrTimer sched_tick│   │  nanosleep  │
 │  periodic 6ms      │   │  watchdog   │
 └────────────────────┘   └─────────────┘
```

## Verification

```bash
# All 6 presets must build
uv run build.py --all

# ARM64 QEMU test: should see periodic scheduler tick messages
./build/arm64-qemu-debug/run_qemu.sh
# Expected output:
#   Timer subsystem initialized: freq=62500000 Hz
#   Clocksource: mult=... shift=...
#   Scheduler tick armed: period=6000000 ns
#   [periodic] sched_tick: vruntime=... need_resched=...
```

## Future Extensions

- `nanosleep` syscall implementation using HrTimer
- `gettimeofday` / `clock_gettime` syscalls using Clocksource
- Per-CPU TimerSubsystem instances for SMP scalability
- Tickless (NO_HZ) idle mode
- Watchdog timer
- Red-black tree upgrade if active timer count exceeds ~50
