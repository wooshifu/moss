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
#include "validation/exec_control.hpp"
#include "validation/isolation.hpp"
#include "validation/memory_cases.hpp"
#include "validation/memory_internal.hpp"
#include "validation/process_control.hpp"
#include "validation/resources.hpp"
#include "validation/runtime.hpp"
#include "validation/runtime_state.hpp"
#include "validation/smp_cases.hpp"
#include "validation/timer_control.hpp"
#include "validation/uaccess.hpp"
#include "validation_internal.hpp"

#include "validation/signal_control.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;

namespace {
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
u32 console_irq_probe_armed = 0;
u32 console_reader_at_gap = 0;
u32 console_irq_before_lock = 0;
u32 console_irq_cpu = ~u32{0};
#endif
} // namespace

#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
extern "C" void moss_validation_console_before_register() noexcept {
  if (__atomic_load_n(&console_irq_probe_armed, __ATOMIC_ACQUIRE) == 0 || arch::get_current_cpu_id() != 1) {
    return;
  }
  // Hold the empty-check lock until the remote hardware IRQ has reached the
  // same lock. Registration then races only with a handler already in flight.
  __atomic_store_n(&console_reader_at_gap, 1U, __ATOMIC_RELEASE);
  while (__atomic_load_n(&console_irq_before_lock, __ATOMIC_ACQUIRE) == 0 &&
         __atomic_load_n(&console_irq_probe_armed, __ATOMIC_ACQUIRE) != 0) {
    arch::cpu_yield();
  }
}

extern "C" void moss_validation_console_irq_before_lock() noexcept {
  if (__atomic_load_n(&console_irq_probe_armed, __ATOMIC_ACQUIRE) == 0) {
    return;
  }
  __atomic_store_n(&console_irq_cpu, arch::get_current_cpu_id(), __ATOMIC_RELEASE);
  __atomic_store_n(&console_irq_before_lock, 1U, __ATOMIC_RELEASE);
}
#endif

namespace moss::test::validation {
namespace {
ProcessId wait_exit_parent = INVALID_PROCESS_ID;
ProcessId wait_exit_child = INVALID_PROCESS_ID;
u32 wait_exit_phase = 0;
ProcessId cpu_bound_probe_pid = INVALID_PROCESS_ID;
u64 cpu_bound_probe_start = 0;
u64 cpu_bound_probe_end = 0;
u32 cpu_bound_irq_seen = 0;
// Validation control IDs shared with the CPU-bound userspace case.
constexpr long CPU_BOUND_ARM_PROBE = 60;
constexpr long CPU_BOUND_CHECK_PROBE = 61;
constexpr long STOP_STATE_PROBE = 62;
constexpr long STOP_PENDING_PROBE = 63;
} // namespace

extern "C" void moss_validation_user_return(void *raw_frame) noexcept {
  const auto pid = __atomic_load_n(&cpu_bound_probe_pid, __ATOMIC_ACQUIRE);
  if (pid == INVALID_PROCESS_ID || !ut::same_id(active_case, "cpu_bound_irq")) {
    return;
  }
  auto *thread = process::CfsScheduler::get_current_task();
  auto &frame = *static_cast<moss::abi::TrapFrame *>(raw_frame);
  // The child performs no syscall inside this registered PC interval. Seeing
  // its PC here proves that an IRQ used the common user-return checkpoint.
  if (thread && thread->owner_pid == pid && arch::get_current_cpu_id() == 1 && frame.pc >= cpu_bound_probe_start &&
      frame.pc < cpu_bound_probe_end) {
    __atomic_fetch_add(&cpu_bound_irq_seen, 1U, __ATOMIC_RELEASE);
  }
}

extern "C" void moss_validation_wait_before_register(u32 parent_pid, long wait_pid) noexcept {
  if (!ut::same_id(active_case, "wait_registration") || parent_pid != wait_exit_parent || wait_pid <= 1 ||
      wait_pid > static_cast<long>(~ProcessId{0}) || __atomic_load_n(&wait_exit_phase, __ATOMIC_ACQUIRE) != 1) {
    return;
  }
  // The child can leave its gate only after the first Zombie scan missed it.
  wait_exit_child = static_cast<ProcessId>(wait_pid);
  __atomic_store_n(&wait_exit_phase, 2U, __ATOMIC_RELEASE);
  moss::test::validation::wait_for_phase(wait_exit_phase, 3);
}

extern "C" void moss_validation_child_exit_notified(u32 child_pid, u32 parent_pid) noexcept {
  if (ut::same_id(active_case, "wait_registration") && parent_pid == wait_exit_parent && child_pid == wait_exit_child &&
      __atomic_load_n(&wait_exit_phase, __ATOMIC_ACQUIRE) == 2) {
    // The real exit path has published Zombie and attempted wakeup while no
    // waiter exists; release/acquire also makes its status visible to wait4.
    __atomic_store_n(&wait_exit_phase, 3U, __ATOMIC_RELEASE);
  }
}

long signal_control(long op, long arg1, long arg2) {
  if (ut::same_id(selection, "users.signals") && ut::same_id(active_case, "wait_registration")) {
    if (op == 52 && affinity_valid() && arg1 == 0 && g_num_cpus >= 2 &&
        __atomic_load_n(&wait_exit_phase, __ATOMIC_ACQUIRE) == 0) {
      auto parent = process::current_process();
      if (!parent) {
        return 0;
      }
      wait_exit_parent = parent->pid();
      wait_exit_child = INVALID_PROCESS_ID;
      __atomic_store_n(&wait_exit_phase, 1U, __ATOMIC_RELEASE);
      return 1;
    }
    if (op == 53 && arch::get_current_cpu_id() == 1) {
      moss::test::validation::wait_for_phase(wait_exit_phase, 2);
      auto child = process::current_process();
      if (!child || child->pid() != wait_exit_child || child->parent_pid() != wait_exit_parent) {
        return 0;
      }
      return 1;
    }
    if (op == 54 && affinity_valid()) {
      const bool observed =
          __atomic_load_n(&wait_exit_phase, __ATOMIC_ACQUIRE) == 3 && arg1 == static_cast<long>(wait_exit_child);
      __atomic_store_n(&wait_exit_phase, 0U, __ATOMIC_RELEASE);
      wait_exit_parent = INVALID_PROCESS_ID;
      wait_exit_child = INVALID_PROCESS_ID;
      return observed;
    }
  }
  if (op == 55 && ut::same_id(selection, "users.signals") &&
      (ut::same_id(active_case, "wait_interrupted") || ut::same_id(active_case, "wait_restarted")) &&
      arch::get_current_cpu_id() == 1) {
    auto child = process::current_process();
    if (!child || arg1 != static_cast<long>(child->parent_pid()) || arg2 != static_cast<long>(child->pid())) {
      return -1;
    }
    auto parent = process::g_process_manager->find_process(child->parent_pid());
    auto *thread = parent ? parent->get_main_thread() : nullptr;
    if (!thread) {
      return -1;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(thread->sleep_lock);
    auto *frame = thread->trap_frame;
    // Native syscall 13 is waitpid; inspect the real blocked frame rather
    // than treating the child's readiness or a preceding syscall as proof.
    return thread->state == process::ProcessState::Sleeping && thread->sleep_handoff.load() == 0 && frame &&
           frame->syscall_number() == 13 && frame->argument(0) == static_cast<u64>(arg2);
  }
  if (ut::same_id(selection, "users.signals") && ut::same_id(active_case, "cpu_bound_irq")) {
    if (op == CPU_BOUND_ARM_PROBE && arch::get_current_cpu_id() == 1 && arg1 > 0 && arg2 > arg1) {
      auto child = process::current_process();
      if (!child) {
        return -1;
      }
      cpu_bound_probe_start = static_cast<u64>(arg1);
      cpu_bound_probe_end = static_cast<u64>(arg2);
      __atomic_store_n(&cpu_bound_irq_seen, 0U, __ATOMIC_RELEASE);
      __atomic_store_n(&cpu_bound_probe_pid, child->pid(), __ATOMIC_RELEASE);
      return 1;
    }
    if (op == CPU_BOUND_CHECK_PROBE && affinity_valid() && arg1 == static_cast<long>(cpu_bound_probe_pid)) {
      if (__atomic_load_n(&cpu_bound_irq_seen, __ATOMIC_ACQUIRE) < 2) {
        return 0;
      }
      __atomic_store_n(&cpu_bound_probe_pid, INVALID_PROCESS_ID, __ATOMIC_RELEASE);
      return 1;
    }
  }
  if ((op == STOP_STATE_PROBE || op == STOP_PENDING_PROBE) && ut::same_id(selection, "users.signals") &&
      ut::same_id(active_case, "stop_continue")) {
    if (!affinity_valid() || arg1 <= 0 || arg1 > static_cast<long>(~ProcessId{0}) || (arg2 != 0 && arg2 != 1)) {
      return -1;
    }
    auto child = process::g_process_manager->find_process(static_cast<ProcessId>(arg1));
    auto *thread = child ? child->get_main_thread() : nullptr;
    if (!thread) {
      return -1;
    }
    if (op == STOP_PENDING_PROBE) {
      const u64 stop_mask = process::sig::sigmask(process::sig::SIGTSTP);
      const u64 cont_mask = process::sig::sigmask(process::sig::SIGCONT);
      const u64 expected = arg2 == 0 ? stop_mask : cont_mask;
      return (thread->signal_mask & (stop_mask | cont_mask)) == (stop_mask | cont_mask) &&
             (thread->pending_signals & (stop_mask | cont_mask)) == expected;
    }
    // A state label alone is insufficient: the old STOP path marked the
    // thread Stopped while continuing to execute its user return frame.
    if (thread->state != process::ProcessState::Stopped || thread->sleep_handoff.load() != 0 ||
        process::CfsScheduler::get_current_task_on_cpu(1) == thread) {
      return 0;
    }
    return arg2 == 0 || (thread->pending_signals & process::sig::sigmask(process::sig::SIGUSR1)) != 0;
  }
  if (op == 57 && ut::same_id(selection, "users.signals") && ut::same_id(active_case, "console_multi_reader") &&
      affinity_valid()) {
    auto parent = process::current_process();
    if (!parent || arg1 <= 1 || arg2 <= 1 || arg1 > ~ProcessId{0} || arg2 > ~ProcessId{0} || arg1 == arg2) {
      return -1;
    }
    const long pids[] = {arg1, arg2};
    for (long pid : pids) {
      auto child = process::g_process_manager->find_process(static_cast<ProcessId>(pid));
      if (!child || child->parent_pid() != parent->pid()) {
        return -1;
      }
      auto *thread = child->get_main_thread();
      if (!thread) {
        return -1;
      }
      containers::LockGuard<containers::IrqSpinLock> guard(thread->sleep_lock);
      if (thread->state == process::ProcessState::Zombie) {
        return -1;
      }
#if !defined(MOSS_ARCH_RISCV64)
      if (thread->state != process::ProcessState::Sleeping || thread->sleep_handoff.load() != 0) {
        return 0;
      }
#endif
      auto *frame = thread->trap_frame;
      if (!frame || frame->syscall_number() != 32) { // Native read syscall.
        return 0;
      }
    }
    return 1;
  }
#if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X64)
  if (op == 58 && ut::same_id(selection, "users.console_irq") && ut::same_id(active_case, "irq_before_registration") &&
      affinity_valid()) {
    if (arg1 == 0 && arg2 == 0) {
      if (g_num_cpus < 2 || !drivers::console::is_initialized()) {
        return -1;
      }
      __atomic_store_n(&console_reader_at_gap, 0U, __ATOMIC_RELEASE);
      __atomic_store_n(&console_irq_before_lock, 0U, __ATOMIC_RELEASE);
      __atomic_store_n(&console_irq_cpu, ~u32{0}, __ATOMIC_RELEASE);
      __atomic_store_n(&console_irq_probe_armed, 1U, __ATOMIC_RELEASE);
      return 1;
    }
    if (arg1 == -1 && arg2 == 0) {
      __atomic_store_n(&console_irq_probe_armed, 0U, __ATOMIC_RELEASE);
      return __atomic_load_n(&console_reader_at_gap, __ATOMIC_ACQUIRE) == 1 &&
                     __atomic_load_n(&console_irq_before_lock, __ATOMIC_ACQUIRE) == 1 &&
                     __atomic_load_n(&console_irq_cpu, __ATOMIC_ACQUIRE) == 0
                 ? 1
                 : -1;
    }
    auto parent = process::current_process();
    if (!parent || arg1 <= 1 || arg1 > ~ProcessId{0} || arg2 != 0) {
      return -1;
    }
    auto child = process::g_process_manager->find_process(static_cast<ProcessId>(arg1));
    if (!child || child->parent_pid() != parent->pid()) {
      return -1;
    }
    return __atomic_load_n(&console_reader_at_gap, __ATOMIC_ACQUIRE) == 1 ? 1 : 0;
  }
#endif
  if (op == 39 && ut::same_id(selection, "users.signals") &&
      (ut::same_id(active_case, "pipe_interrupted") || ut::same_id(active_case, "pipe_restarted") ||
       ut::same_id(active_case, "pipe_noninterrupting_signals") || ut::same_id(active_case, "console_interrupted") ||
       ut::same_id(active_case, "console_partial_interrupt") || ut::same_id(active_case, "console_restarted"))) {
    if (arg1 == 0 && arg2 == 0) {
      return 1; // The production image's weak hook still returns ENOSYS.
    }
    auto caller = process::current_process();
    if (!caller || arg1 != static_cast<long>(caller->parent_pid()) || arg2 < 0 || arg2 >= vfs::MAX_FDS) {
      return -1;
    }
    auto parent = process::g_process_manager->find_process(caller->parent_pid());
    auto *thread = parent ? parent->get_main_thread() : nullptr;
    if (!thread) {
      return -1;
    }
    containers::LockGuard<containers::IrqSpinLock> guard(thread->sleep_lock);
    bool require_sleep = true;
#if defined(MOSS_ARCH_RISCV64)
    // RV64 still polls console RX; its active read frame is the readiness boundary.
    require_sleep = !ut::same_id(active_case, "console_interrupted") &&
                    !ut::same_id(active_case, "console_partial_interrupt") &&
                    !ut::same_id(active_case, "console_restarted");
#endif
    if (require_sleep && (thread->state != process::ProcessState::Sleeping || thread->sleep_handoff.load() != 0)) {
      return 0;
    }
    // Do not mistake the parent's preparation nanosleep for the intended I/O.
    auto *frame = thread->trap_frame;
    return frame && (frame->syscall_number() == 32 || frame->syscall_number() == 33) &&
           frame->argument(0) == static_cast<u64>(arg2);
  }
  invalid_control();
}
} // namespace moss::test::validation
