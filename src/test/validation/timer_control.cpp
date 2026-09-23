import moss.std;
import moss.types;
import moss.arch;
import moss.abi;
import moss.hal.timer;
import moss.boot;
import moss.fdt;
import moss.mm;
import moss.vfs;
import moss.process;
import moss.timer;
import moss.containers;
import moss.smart_ptr;
import moss.hal.uart;
import moss.hal.mmu;
import moss.logging;
import moss.ipc;
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "ipc_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"
#include "validation/core_cases.hpp"
#include "validation/isolation.hpp"
#include "validation/memory_cases.hpp"
#include "validation/memory_internal.hpp"
#include "validation/resources.hpp"
#include "validation/runtime.hpp"
#include "validation/runtime_state.hpp"
#include "validation/smp_cases.hpp"
#include "validation/uaccess.hpp"
#include "validation_internal.hpp"

#include "validation/timer_control.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;

namespace moss::test::validation {
namespace {
struct TimerCapacity {
  // One more than the production timer heap's 256 slots forces exhaustion;
  // the 1000-second delay prevents test timers from expiring during setup.
  timer::HrTimer *timers = new timer::HrTimer[257];
  bool full = false;

  TimerCapacity() {
    if (!ut::expect(timers != nullptr)) {
      return;
    }
    unsigned active = 0;
    for (unsigned i = 0; i < 257; ++i) {
      timers[i].init(timer::TimerMode::OneShot, [](void *) noexcept {});
      auto started = timers[i].start_relative(1000000000000ULL);
      ut::expect(started ? timers[i].is_active()
                         : started.error() == ErrorCode::ResourceExhausted && !timers[i].is_active());
      full = full || (!started && started.error() == ErrorCode::ResourceExhausted);
      active += timers[i].is_active();
    }
    // The production heap has 256 slots, including its scheduler tick.
    ut::expect(full && active < 256);
  }

  ~TimerCapacity() {
    if (!timers) {
      return;
    }
    for (unsigned i = 0; i < 257; ++i) {
      timers[i].cancel_sync();
      ut::expect(!timers[i].is_active());
    }
    delete[] timers;
  }
};
TimerCapacity *sleep_capacity = nullptr;
u64 sleep_capacity_heap_before = 0;

struct TimerCancellation {
  timer::HrTimer pending;
  u32 peers_ready = 0, callback_cpu = ~0U;
  u32 entered = 0, release = 0, callback_returned = 0, cancel_returned = 0, observer_returned = 0;
  bool cancel_ok = false, observer_ok = false;

  static void callback(void *data) noexcept {
    auto &self = *static_cast<TimerCancellation *>(data);
    __atomic_store_n(&self.callback_cpu, arch::get_current_cpu_id(), __ATOMIC_RELAXED);
    __atomic_store_n(&self.entered, 1U, __ATOMIC_RELEASE);
    moss::test::validation::wait_for_phase(self.release, 1);
    __atomic_store_n(&self.callback_returned, 1U, __ATOMIC_RELEASE);
  }

  bool peer(long actor) {
    if (actor < 1 || actor > 2 || arch::get_current_cpu_id() != static_cast<u32>(actor)) {
      return false;
    }
    // The timer queue is global: a timer IRQ must not take over either actor
    // whose progress the deliberately held callback needs. Publish readiness
    // only after masking local IRQs, before the owner arms the real timer.
    const bool restore_irqs = arch::interrupts_enabled();
    arch::disable_interrupts();
    __atomic_fetch_or(&peers_ready, 1U << static_cast<u32>(actor), __ATOMIC_RELEASE);
    moss::test::validation::wait_for_phase(entered, 1);
    if (actor == 1) {
      pending.cancel_sync();
      cancel_ok = __atomic_load_n(&callback_returned, __ATOMIC_ACQUIRE) == 1;
      __atomic_store_n(&cancel_returned, 1U, __ATOMIC_RELEASE);
      if (restore_irqs) {
        arch::enable_interrupts();
      }
      return cancel_ok;
    }
    // A periodic timer remains queued while its callback runs. Inactive thus
    // proves CPU1 has reached cancellation, not merely its pre-call barrier.
    while (pending.is_active()) {
      arch::cpu_yield();
    }
    const auto restarted = pending.start_relative(1000000000ULL);
    observer_ok = !restarted && restarted.error() == ErrorCode::InvalidState;
    // Hold the callback across a bounded observation window. Cancellation may
    // not return during it; the host deadline still bounds every peer barrier.
    auto &clock = timer::TimerSubsystem::instance();
    const u64 until = clock.now_ns() + 2000000ULL;
    while (!__atomic_load_n(&cancel_returned, __ATOMIC_ACQUIRE) && clock.now_ns() < until) {
      arch::cpu_yield();
    }
    observer_ok = observer_ok && !__atomic_load_n(&cancel_returned, __ATOMIC_ACQUIRE);
    __atomic_store_n(&release, 1U, __ATOMIC_RELEASE);
    __atomic_store_n(&observer_returned, 1U, __ATOMIC_RELEASE);
    if (restore_irqs) {
      arch::enable_interrupts();
    }
    return observer_ok;
  }

  bool owner() {
    moss::test::validation::wait_for_phase(peers_ready, 6);
    pending.init(timer::TimerMode::Periodic, callback, this);
    if (!pending.start_relative(1000000ULL)) {
      return false;
    }
    moss::test::validation::wait_for_phase(cancel_returned, 1);
    moss::test::validation::wait_for_phase(observer_returned, 1);
    pending.cancel_sync();
    const u32 cpu = __atomic_load_n(&callback_cpu, __ATOMIC_ACQUIRE);
    return cancel_ok && observer_ok && callback_returned == 1 && !pending.is_active() && cpu != 1 && cpu != 2;
  }
};
TimerCancellation *timer_cancellation = nullptr;
process::Thread *early_sleep_thread = nullptr;
u32 early_sleep_visits = 0;
bool early_sleep_woken = true;

} // namespace

void timer_capacity() {
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  {
    TimerCapacity fixture;
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}

extern "C" void moss_validation_sleep_armed(void *pending) noexcept {
  auto *thread = process::CfsScheduler::get_current_task();
  if (!thread || thread != early_sleep_thread) {
    return;
  }
  ++early_sleep_visits;
  auto &armed = *static_cast<timer::HrTimer *>(pending);
  // Local IRQs are masked by the real sleep syscall. Another CPU must expire
  // its timer before this caller saves its context. The host bounds the wait.
  while (armed.is_active()) {
    arch::cpu_yield();
  }
  armed.cancel_sync();
  early_sleep_woken = early_sleep_woken && !arch::interrupts_enabled() &&
                      (thread->state == process::ProcessState::Ready || thread->sleep_handoff.load() == 2);
}

long timer_control(long op, long arg1, long arg2) {
  if (op == 56 && ut::same_id(selection, "users.timers") && arch::get_current_cpu_id() == 1) {
    const long mode = ut::same_id(active_case, "relative_interrupted")         ? 0
                      : ut::same_id(active_case, "clock_relative_interrupted") ? 1
                      : ut::same_id(active_case, "clock_absolute_interrupted") ? 2
                                                                               : -1;
    auto child = process::current_process();
    if (!child || mode != arg2 || arg1 != static_cast<long>(child->parent_pid())) {
      return -1;
    }
    auto parent = process::g_process_manager->find_process(child->parent_pid());
    auto *thread = parent ? parent->get_main_thread() : nullptr;
    if (!thread) {
      return -1;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(thread->sleep_lock);
    auto *frame = thread->trap_frame;
    // Native syscall numbers 86/87 are nanosleep/clock_nanosleep. Inspect the
    // actual frame after its sleep handoff completes, not a guessed delay.
    return thread->state == process::ProcessState::Sleeping && thread->sleep_handoff.load() == 0 && frame &&
           frame->syscall_number() == (mode == 0 ? 86U : 87U) &&
           (mode == 0 || frame->argument(1) == static_cast<u64>(mode == 2));
  }
  if (ut::same_id(selection, "users.timers") && ut::same_id(active_case, "arm_failure_recovery") && affinity_valid()) {
    if (op == 28 && !sleep_capacity) {
      sleep_capacity_heap_before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
      sleep_capacity = new TimerCapacity();
      return sleep_capacity && sleep_capacity->full;
    }
    if (op == 29 && sleep_capacity) {
      delete sleep_capacity;
      sleep_capacity = nullptr;
      return mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == sleep_capacity_heap_before;
    }
  }
  if (ut::same_id(selection, "users.timers") && ut::same_id(active_case, "early_wakeup")) {
    if (op == 24 && !early_sleep_thread && arch::get_current_cpu_id() == 1) {
      early_sleep_visits = 0;
      early_sleep_woken = true;
      early_sleep_thread = process::CfsScheduler::get_current_task();
      return g_num_cpus >= 2;
    }
    if (op == 25 && early_sleep_thread == process::CfsScheduler::get_current_task()) {
      early_sleep_thread = nullptr;
      return early_sleep_woken && early_sleep_visits == 2;
    }
  }
  if (ut::same_id(selection, "users.timers") && ut::same_id(active_case, "cancel_in_flight")) {
    if (op == 20 && !timer_cancellation && affinity_valid()) {
      timer_cancellation = new TimerCancellation();
      return timer_cancellation && g_num_cpus >= 3;
    }
    if (op == 21 && timer_cancellation) {
      arch::enable_interrupts();
      return timer_cancellation->peer(arg1);
    }
    if (op == 22 && timer_cancellation && affinity_valid()) {
      arch::enable_interrupts();
      return timer_cancellation->owner();
    }
    if (op == 23 && timer_cancellation && affinity_valid()) {
      delete timer_cancellation;
      timer_cancellation = nullptr;
      return 1;
    }
  }
  invalid_control();
}
} // namespace moss::test::validation
