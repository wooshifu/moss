// MOSS Kernel main entry point
// System boot entry and global instance management

module moss.kernel;

import moss.logging;

using moss::kernel::u32;
using moss::kernel::u64;
using moss::kernel::usize;

// Bring logging into scope for use in extern "C" and namespace blocks
namespace log = moss::kernel::logging;

namespace moss::kernel {

// Subsystem global instances
containers::ContainerLibrary *g_container_lib = nullptr;
mm::PageTableManager *g_page_table_manager = nullptr;
drivers::DeviceManager *g_device_manager = nullptr;

} // namespace moss::kernel

extern "C" {

[[gnu::weak]] void moss_validation_boot() noexcept {}
[[gnu::weak]] long moss_validation_call([[maybe_unused]] long op, [[maybe_unused]] long arg1,
                                        [[maybe_unused]] long arg2) noexcept {
  return -38; // Native ENOSYS: production images do not implement validation calls.
}

// Kernel main entry (called from boot assembly)
[[noreturn]] void kernel_main(void) noexcept {
  using namespace moss::kernel;

  log::klog::info("=== MOSS kernel main starting ===");
  log::klog::info("detected CPUs: {}", g_num_cpus);

  // Create kernel instance
  log::klog::info("creating kernel instance...");
  auto *kernel = new Kernel();
  if (!kernel) {
    log::klog::panic("kernel instance creation failed, cannot continue");
    while (true) {
      arch::cpu_halt();
    }
  }
  log::klog::info("kernel instance created");

  // Full kernel initialization
  log::klog::info("initializing kernel subsystems...");
  auto init_result = kernel->initialize();
  if (!init_result) {
    log::klog::error("kernel initialization failed");
    while (true) {
      arch::cpu_halt();
    }
  }
  log::klog::info("kernel subsystem initialization complete");

  // Display system info
  log::klog::info("=== kernel system status ===");
  kernel->print_system_info();

  // Subsystem verification
  log::klog::info("=== subsystem verification ===");

  // Memory management
  if (mm::is_memory_system_healthy()) {
    auto pressure = mm::get_memory_pressure();
    if (!pressure) {
      // Readiness is real, but no pressure sampler is implemented yet. Keep
      // those two facts separate in the boot capability report.
      log::klog::info("memory: ready, pressure=unsupported");
    } else {
      const char *level = "unknown";
      switch (*pressure) {
      case mm::MemoryPressure::LOW:
        level = "low";
        break;
      case mm::MemoryPressure::MEDIUM:
        level = "medium";
        break;
      case mm::MemoryPressure::HIGH:
        level = "high";
        break;
      case mm::MemoryPressure::CRITICAL:
        level = "critical";
        break;
      case mm::MemoryPressure::UNKNOWN:
        level = "unknown";
        break;
      default:
        break;
      }
      log::klog::info("memory: ready, pressure={}", level);
    }
  } else {
    log::klog::warn("memory: not ready");
  }

  // Interrupt controller
  if (interrupts::g_gic) {
    log::klog::info("interrupts: GIC initialized");
  } else {
    log::klog::warn("interrupts: GIC not initialized");
  }

  // Timer subsystem
  if (timer::TimerSubsystem::instance().is_initialized()) {
    log::klog::info("timer: initialized");
  } else {
    log::klog::warn("timer: not initialized");
  }

  // Scheduler
  if (process::g_scheduler) {
    log::klog::info("scheduler: CFS ready");
  } else {
    log::klog::error("scheduler: not initialized");
  }

  // Userspace archive validation and init creation happen in Kernel::run().
  // Keep this message scoped to what has actually completed so a missing init
  // cannot appear after a successful whole-kernel boot announcement.
  log::klog::info("MOSS kernel subsystem verification complete");
  log::klog::info("entering userspace bootstrap and scheduling phase...");

  // Start kernel run system (with real task scheduling)
  log::klog::info("starting kernel run system");

  auto run_result = kernel->run();
  if (!run_result) {
    log::klog::panic("kernel run system failed to start");
    while (true) {
      arch::cpu_halt();
    }
  }

  log::klog::panic("kernel main loop exited unexpectedly");
  while (true) {
    arch::cpu_halt();
  }
}

// Legacy extern "C" shim — retained for ABI compatibility with assembly
// code and test harness. New code should import moss.logging instead.
void early_debug_print(const char *message) noexcept { ::moss::kernel::hal::uart::puts(message); }

// Syscall entry
void system_call_handler(void *raw_frame) noexcept {
  using namespace moss::kernel;
  auto &frame = *static_cast<moss::abi::TrapFrame *>(raw_frame);
  auto *thread = process::CfsScheduler::get_current_task();
  auto *previous = thread ? thread->trap_frame : nullptr;
  // A nested trap must restore the previous borrowed frame: this frame lives
  // on the current entry stack and must not escape after assembly returns.
  if (thread) {
    thread->trap_frame = &frame;
  }
  const long nr = static_cast<long>(frame.syscall_number());
  // 511 is the validation-only syscall slot, outside the production table.
  // It must match validation.c; production retains the ENOSYS weak hook.
  const long result =
      nr == 511 ? moss_validation_call(static_cast<long>(frame.argument(0)), static_cast<long>(frame.argument(1)),
                                       static_cast<long>(frame.argument(2)))
                : syscall::SyscallDispatcher::dispatch(
                      nr, static_cast<long>(frame.argument(0)), static_cast<long>(frame.argument(1)),
                      static_cast<long>(frame.argument(2)), static_cast<long>(frame.argument(3)),
                      static_cast<long>(frame.argument(4)), static_cast<long>(frame.argument(5)));
  // Commit the result before delivery saves the interrupted context or sets
  // the handler argument. Assembly restores this frame; it never patches it.
  frame.result() = static_cast<u64>(result);
  if (thread) {
    thread->trap_frame = previous;
  }
}

void user_return_handler(void *raw_frame) noexcept {
  using namespace moss::kernel;
  using namespace moss::kernel::process;
  auto &frame = *static_cast<moss::abi::TrapFrame *>(raw_frame);
  if (!frame.from_user()) {
    return;
  }
  Thread *thread = CfsScheduler::get_current_task();
  if (!thread) {
    return;
  }
  auto *previous = thread->trap_frame;
  thread->trap_frame = &frame;
  if (signal_pending(thread)) {
    const u32 signo = do_signal_checkpoint(thread);
    if (signo) {
      auto proc = g_process_manager ? g_process_manager->find_process(thread->owner_pid) : shared_ptr<Process>{};
      if (proc) {
        // Preserve the shell convention of 128 + signal number for a fatal signal.
        do_exit(thread, moss::move(proc), 128 + static_cast<i32>(signo));
      }
    }
  }
  thread->trap_frame = previous;
}

// IRQ handler called from assembly irq_trampoline.
// Kept minimal — no logging in hot ISR path.
void irq_handler_c(void) noexcept {
  namespace intc_hal = ::moss::kernel::hal::intc;
  namespace timer_hal = ::moss::kernel::hal::timer;

  u64 gicc_base = ::moss::kernel::platform::intc_cpu_base();
  u32 ack_val = intc_hal::ack_irq(gicc_base);
  u32 irq = intc_hal::irq_from_ack(ack_val);

  if (intc_hal::is_spurious(irq)) {
    return;
  }

  // EOI first: tell the GIC we've acknowledged this interrupt so it can
  // deliver the next one.  This is critical because handle_interrupt() may
  // call context_switch(), which suspends the current execution flow.
  // If EOI were after handle_interrupt(), the GIC would block subsequent
  // timer IRQs until the suspended task resumes.
  intc_hal::eoi(gicc_base, ack_val);

  // SGI 0 (Reschedule IPI): another CPU wants us to re-examine our runqueue.
  // Just EOI + return — the interrupted context (WFI or running task) will
  // naturally re-check the runqueue.  No further action needed because:
  //   - If in WFI (idle loop), eret returns to the scheduling loop
  //   - If running a task, the next timer tick will preempt if needed
  constexpr u32 RESCHEDULE_SGI = 0;
  if (irq == RESCHEDULE_SGI) {
    return; // EOI already sent above
  }

  // Firmware/controller dispatch selects the timer ID (GIC and BCM differ).
  // Each CPU has its own banked cntv_cval_el0 compare register.
  // We must reprogram THIS CPU's compare before dispatching, because
  // TimerSubsystem::handle_interrupt() may context_switch and never return.
  if (irq == ::moss::kernel::platform::timer_irq()) {
    timer_hal::ack_interrupt();

    // Reprogram THIS CPU's timer compare for the next tick interval.
    // This ensures the timer keeps firing regardless of what handle_interrupt does.
    auto &ts = ::moss::kernel::timer::TimerSubsystem::instance();
    u64 tick_ns = ::moss::kernel::process::cfs_params::SCHED_LATENCY_NS;
    u64 delta_cycles = ts.clocksource().ns_to_cycles(tick_ns);
    u64 counter_now = timer_hal::read_counter();
    timer_hal::set_compare(counter_now + delta_cycles);

    // Dispatch: CPU 0 uses TimerSubsystem (drives HrTimer queue),
    // secondary CPUs call scheduler_tick() directly.
    u32 cpu = ::moss::kernel::arch::get_current_cpu_id();
    if (cpu == 0) {
      ts.handle_interrupt();
    } else {
      if (::moss::kernel::process::g_scheduler) {
        ::moss::kernel::process::g_scheduler->scheduler_tick();
      }
    }
    return;
  }

  // Non-timer, non-SGI IRQ (e.g. UART SPI 33): dispatch via GIC's
  // registered interrupt handler table.  ACK + EOI already done above.
  if (::moss::kernel::interrupts::g_gic) {
    // Look up the registered handler for this IRQ and call it directly.
    // We cannot call g_gic->handle_interrupt() because it would do its
    // own ACK+EOI (already done above).  Instead, look up and invoke.
    auto desc = ::moss::kernel::interrupts::g_gic->get_interrupt_info(irq);
    if (static_cast<bool>(desc) && desc->handler != nullptr) {
      desc->handler(irq, desc->context);
    }
  }
}

// ============================================================================
// Bridge functions for demand paging
// These are called from page_fault.cpp (mm module) via extern "C" linkage,
// bridging the mm ↔ process module boundary without circular imports.
// ============================================================================

int resolve_current_user_fault(unsigned long long fault_addr, unsigned int access, bool cow_only) noexcept {
  using namespace moss::kernel;

  // The faulting translation belongs to the installed hardware root. Process
  // publication may already point at a newer image on another CPU; resolving
  // into that version would leave the actual faulting page unmapped.
  auto as = process::CfsScheduler::active_address_space();
  if (!as) {
    return 0;
  }

  if (access != static_cast<unsigned int>(mm::UserFaultAccess::Read) &&
      access != static_cast<unsigned int>(mm::UserFaultAccess::Write) &&
      access != static_cast<unsigned int>(mm::UserFaultAccess::Execute)) {
    return 0;
  }
  // Keep the same owning snapshot through VMA lookup, backing reads and PTE
  // publication. Returning a raw root/backing from separate lookups could pair
  // different exec generations or allow the image to die during the fault.
  VirtAddr grown_stack = 0;
  const bool resolved = as->resolve_fault(static_cast<VirtAddr>(fault_addr), static_cast<mm::UserFaultAccess>(access),
                                          cow_only, &grown_stack);
  auto *thread = process::current_thread();
  if (thread && grown_stack != 0) {
    thread->stack_base = grown_stack;
    thread->stack_size = process::user_layout::STACK_TOP - grown_stack;
  }
  return resolved ? 1 : 0;
}

unsigned long long get_current_pgd_phys() noexcept {
  using namespace moss::kernel;

  auto as = process::CfsScheduler::active_address_space();
  if (!as) {
    return 0;
  }
  return static_cast<unsigned long long>(as->pgd_phys);
}

// ============================================================================
// Bridge: terminate current user process and switch to scheduler
// ============================================================================
[[noreturn]] void terminate_current_user_process(int exit_code) noexcept {
  using namespace moss::kernel;

  process::Thread *cur = process::CfsScheduler::get_current_task();
  if (!cur) {
    log::klog::panic("terminate_current_user_process: no current thread");
    while (true) {
      arch::cpu_halt();
    }
  }

  ProcessId pid = cur->owner_pid;
  log::klog::info("terminate_user_process: PID={} TID={} exit_code={}", pid, static_cast<u32>(cur->tid), exit_code);

  auto proc =
      process::g_process_manager ? process::g_process_manager->find_process(pid) : shared_ptr<process::Process>{};

  if (!proc) {
    log::klog::panic("terminate_user_process: process not found PID={}", pid);
    while (true) {
      arch::cpu_halt();
    }
  }

  // Delegate to shared Zombie transition (never returns)
  process::do_exit(cur, moss::move(proc), static_cast<i32>(exit_code));
}

// ============================================================================
// RISC-V 64 trap handlers — called from riscv64_syscall.S dispatch
// ============================================================================
#if defined(MOSS_ARCH_RISCV64) || defined(__riscv) || defined(__riscv__)

void riscv64_software_handler() noexcept {
  // SBI software IPIs share SSIP. Reschedule-only notifications have no TLB
  // request; polling the mailbox is harmless and never takes scheduler locks.
  ::moss::kernel::arch::service_tlb_shootdown();
}

// S-mode timer interrupt handler.
// Reprograms stimecmp via HAL and calls scheduler_tick().
void riscv64_timer_handler() noexcept {
  namespace timer_hal = ::moss::kernel::hal::timer;

  timer_hal::ack_interrupt();

  auto &ts = ::moss::kernel::timer::TimerSubsystem::instance();
  u64 tick_ns = ::moss::kernel::process::cfs_params::SCHED_LATENCY_NS;
  u64 delta_cycles = ts.clocksource().ns_to_cycles(tick_ns);
  u64 counter_now = timer_hal::read_counter();
  timer_hal::set_compare(counter_now + delta_cycles);

  u32 cpu = ::moss::kernel::arch::get_current_cpu_id();
  if (cpu == 0) {
    ts.handle_interrupt();
  } else {
    if (::moss::kernel::process::g_scheduler) {
      ::moss::kernel::process::g_scheduler->scheduler_tick();
    }
  }
}

// S-mode external interrupt handler (PLIC).
// Claims the IRQ, dispatches via GIC, then completes.
void riscv64_external_handler() noexcept {
  if (::moss::kernel::interrupts::g_gic) {
    ::moss::kernel::interrupts::g_gic->handle_interrupt();
  }
}

// Exception handler for non-ecall, non-page-fault RISC-V 64 exceptions.
// Page faults (scause 12/13/15) are handled separately by riscv64_page_fault_handler.
// This handler covers illegal instruction, misaligned access, etc.
// User-mode exceptions terminate the faulting process; kernel-mode exceptions halt.
[[noreturn]] void riscv64_exception_handler(u64 scause, u64 sepc, u64 stval) noexcept {
  namespace log = ::moss::kernel::logging;

  log::klog::error("RISC-V 64 EXCEPTION: scause={:#x} sepc={:#x} stval={:#x}", scause, sepc, stval);

  // Check if the faulting PC is in user space (below kernel base).
  // If so, terminate the user process and let the scheduler continue.
  if (sepc < ::moss::kernel::KERNEL_BASE) {
    log::klog::error("  User-mode exception, terminating process");
    // Legacy fatal-user-fault code: -SIGSEGV (11), not an errno result.
    ::moss::abi::bridge::terminate_current_user_process(-11);
  }

  // Kernel-mode exception — unrecoverable
  log::klog::error("  Kernel-mode exception, halting");
  while (true) {
    asm volatile("wfi");
  }
}

#endif // MOSS_ARCH_RISCV64

} // extern "C"

// Syscall convention info (multi-arch)
namespace moss::kernel::arch::syscall {

void print_syscall_convention() noexcept {
  const auto &conv = get_syscall_convention();

  log::klog::info("=== syscall architecture info ===");
  log::klog::info("arch: {}", conv.arch_name);
  log::klog::info("instruction: {}", conv.syscall_instruction);
  log::klog::info("syscall_nr: {}", conv.syscall_nr_register);
  log::klog::info("return_reg: {}", conv.return_register);

  // Print arg registers — use uart directly for inline list
  hal::uart::puts("[INFO]  arg_regs: ");
  for (int i = 0; i < 6; ++i) {
    hal::uart::puts(conv.arg_registers[i]);
    if (i < 5) {
      hal::uart::puts(", ");
    }
  }
  hal::uart::puts("\n");
  log::klog::info("================================");
}

} // namespace moss::kernel::arch::syscall
