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
  return -38;
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
    const char *level = "unknown";
    switch (pressure) {
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
    default:
      break;
    }
    log::klog::info("memory: healthy, pressure={}", level);
  } else {
    log::klog::warn("memory: unhealthy");
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

  log::klog::info("MOSS kernel init and verification complete");
  log::klog::info("entering task scheduling phase...");

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
long system_call_handler(long syscall_number, long arg0, long arg1, long arg2, long arg3, long arg4, long arg5,
                         long trap_frame) noexcept {
  using namespace moss::kernel;

  // Store trap frame pointer in current thread for signal delivery
  {
    using namespace process;
    Thread *cur = CfsScheduler::get_current_task();
    if (cur != nullptr) {
      cur->trap_frame = static_cast<u64>(trap_frame);
    }
  }

  // Only the dedicated validation image overrides this ENOSYS hook.
  if (syscall_number == 511) {
    return moss_validation_call(arg0, arg1, arg2);
  }

  // syscall 0 = debug_print (raw UART output from userspace)
  if (syscall_number == 0) {
    if (arg0 != 0) {
      hal::uart::puts(reinterpret_cast<const char *>(arg0));
    }
  }

  return syscall::SyscallDispatcher::dispatch(syscall_number, arg0, arg1, arg2, arg3, arg4, arg5);
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

  // Timer PPI (IRQ 27): per-CPU timer interrupt.
  // Each CPU has its own banked cntv_cval_el0 compare register.
  // We must reprogram THIS CPU's compare before dispatching, because
  // TimerSubsystem::handle_interrupt() may context_switch and never return.
  constexpr u32 TIMER_PPI_IRQ = 27;
  if (irq == TIMER_PPI_IRQ) {
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
    const auto *desc = ::moss::kernel::interrupts::g_gic->get_interrupt_info(irq);
    if (desc != nullptr && desc->handler != nullptr) {
      desc->handler(irq, desc->context);
    }
  }
}

// ============================================================================
// Bridge functions for demand paging
// These are called from page_fault.cpp (mm module) via extern "C" linkage,
// bridging the mm ↔ process module boundary without circular imports.
// ============================================================================

int demand_page_lookup(unsigned long long fault_addr, unsigned int *out_flags, const unsigned char **out_backing_data,
                       unsigned long long *out_backing_offset, unsigned long long *out_backing_size,
                       unsigned long long *out_vma_start) noexcept {
  using namespace moss::kernel;

  auto *proc = process::current_process();
  if (!proc || !proc->address_space()) {
    return 0;
  }

  const auto *vma = proc->address_space()->find_vma(static_cast<VirtAddr>(fault_addr));
  if (!vma) {
    return 0;
  }

  *out_flags = vma->flags;
  *out_backing_data = vma->backing_data;
  *out_backing_offset = static_cast<unsigned long long>(vma->backing_offset);
  *out_backing_size = static_cast<unsigned long long>(vma->backing_size);
  *out_vma_start = static_cast<unsigned long long>(vma->start_addr);
  return 1;
}

// Try to grow the user stack VMA downward to cover fault_addr.
// Returns 1 if the stack was successfully extended, 0 otherwise.
// Called from page_fault.cpp when demand_page_lookup fails — the caller
// retries demand_page_lookup after a successful growth so the new region
// gets a demand-zero page as usual.
int try_grow_user_stack(unsigned long long fault_addr) noexcept {
  using namespace moss::kernel;

  auto *proc = process::current_process();
  if (!proc || !proc->address_space()) {
    return 0;
  }
  auto *as = proc->address_space();

  // 1. Find the STACK VMA
  const process::VmaRegion *stack_vma =
      as->vmas.find_if([](const process::VmaRegion &v) { return v.type == process::VmaType::STACK; });
  if (!stack_vma) {
    return 0;
  }

  // 2. fault_addr must be below current stack start but above the max growth limit
  const VirtAddr stack_limit = process::user_layout::STACK_TOP - process::user_layout::STACK_MAX;
  if (fault_addr >= stack_vma->start_addr || fault_addr < stack_limit) {
    return 0;
  }

  // 3. Page-align the new stack bottom downward
  constexpr u64 PG_SIZE = 4096;
  const VirtAddr new_start = fault_addr & ~(PG_SIZE - 1);

  // 4. Extend VMA start_addr (const_cast pattern, same as sys_brk)
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  const_cast<process::VmaRegion *>(stack_vma)->start_addr = new_start;

  // 5. Update thread metadata
  auto *thread = process::current_thread();
  if (thread) {
    thread->stack_base = new_start;
    thread->stack_size = process::user_layout::STACK_TOP - new_start;
  }

  return 1;
}

unsigned long long get_current_pgd_phys() noexcept {
  using namespace moss::kernel;

  auto *proc = process::current_process();
  if (!proc || !proc->address_space()) {
    return 0;
  }
  return static_cast<unsigned long long>(proc->address_space()->pgd_phys);
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

  process::Process *proc = process::g_process_manager ? process::g_process_manager->find_process(pid) : nullptr;

  if (!proc) {
    log::klog::panic("terminate_user_process: process not found PID={}", pid);
    while (true) {
      arch::cpu_halt();
    }
  }

  // Delegate to shared Zombie transition (never returns)
  process::do_exit(cur, proc, static_cast<i32>(exit_code));
}

// ============================================================================
// RISC-V trap handlers — called from riscv_syscall.S dispatch
// ============================================================================
#if defined(MOSS_ARCH_RISCV) || defined(__riscv) || defined(__riscv__)

// S-mode timer interrupt handler.
// Reprograms stimecmp via HAL and calls scheduler_tick().
void riscv_timer_handler() noexcept {
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
void riscv_external_handler() noexcept {
  if (::moss::kernel::interrupts::g_gic) {
    ::moss::kernel::interrupts::g_gic->handle_interrupt();
  }
}

// Exception handler for non-ecall, non-page-fault RISC-V exceptions.
// Page faults (scause 12/13/15) are handled separately by riscv_page_fault_handler.
// This handler covers illegal instruction, misaligned access, etc.
// User-mode exceptions terminate the faulting process; kernel-mode exceptions halt.
[[noreturn]] void riscv_exception_handler(u64 scause, u64 sepc, u64 stval) noexcept {
  namespace log = ::moss::kernel::logging;

  log::klog::error("RISC-V EXCEPTION: scause={:#x} sepc={:#x} stval={:#x}", scause, sepc, stval);

  // Check if the faulting PC is in user space (below kernel base).
  // If so, terminate the user process and let the scheduler continue.
  if (sepc < ::moss::kernel::KERNEL_BASE) {
    log::klog::error("  User-mode exception, terminating process");
    ::moss::abi::bridge::terminate_current_user_process(-11);
  }

  // Kernel-mode exception — unrecoverable
  log::klog::error("  Kernel-mode exception, halting");
  while (true) {
    asm volatile("wfi");
  }
}

#endif // MOSS_ARCH_RISCV

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
