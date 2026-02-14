// MOSS Kernel main entry point
// System boot entry and global instance management

module;

// Architecture detection
#include "arch_detect.h"

// extern "C" declarations in global module fragment
extern "C" {
void early_debug_print(const char *message) noexcept;
void kernel_test_all_subsystems(void) noexcept;
[[noreturn]] void kernel_main(void) noexcept;
[[noreturn]] void kernel_panic_handler(const char *message) noexcept;
const char *get_kernel_version(void) noexcept;
const char *get_build_info(void) noexcept;
long system_call_handler(long syscall_number, long arg0, long arg1,
                         long arg2, long arg3, long arg4, long arg5) noexcept;

// IRQ handler called from assembly irq_trampoline (start_arm64.S)
void irq_handler_c(void) noexcept;
}

module moss.kernel;

import moss.logging;

using moss::kernel::u32;
using moss::kernel::u64;
using moss::kernel::usize;

// Bring logging into scope for use in extern "C" and namespace blocks
namespace log = moss::kernel::logging;

namespace moss::kernel {

// Global instance definitions
Kernel *g_kernel = nullptr;

// Subsystem global instances
containers::ContainerLibrary *g_container_lib = nullptr;
mm::PageTableManager *g_page_table_manager = nullptr;
drivers::DeviceManager *g_device_manager = nullptr;

} // namespace moss::kernel

extern "C" {

// Kernel main entry (called from boot assembly)
[[noreturn]] void kernel_main(void) noexcept {
  using namespace moss::kernel;

  log::klog::info("=== MOSS kernel main starting ===");
  log::klog::info("single-core mode (SMP disabled)");

  // Create kernel instance
  log::klog::info("creating kernel instance...");
  g_kernel = new Kernel();
  if (!g_kernel) {
    log::klog::panic("kernel instance creation failed, cannot continue");
    while (true) { arch::cpu_halt(); }
  }
  log::klog::info("kernel instance created");

  // Full kernel initialization
  log::klog::info("initializing kernel subsystems...");
  auto init_result = g_kernel->initialize();
  if (!init_result) {
    log::klog::error("kernel initialization failed");
    while (true) { arch::cpu_halt(); }
  }
  log::klog::info("kernel subsystem initialization complete");

  // Display system info
  log::klog::info("=== kernel system status ===");
  g_kernel->print_system_info();

  // Subsystem verification
  log::klog::info("=== subsystem verification ===");

  // Memory management
  if (mm::is_memory_system_healthy()) {
    auto pressure = mm::get_memory_pressure();
    const char *level = "unknown";
    switch (pressure) {
      case mm::MemoryPressure::LOW: level = "low"; break;
      case mm::MemoryPressure::MEDIUM: level = "medium"; break;
      case mm::MemoryPressure::HIGH: level = "high"; break;
      case mm::MemoryPressure::CRITICAL: level = "critical"; break;
      default: break;
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

  auto run_result = g_kernel->run();
  if (!run_result) {
    log::klog::panic("kernel run system failed to start");
    while (true) { arch::cpu_halt(); }
  }

  log::klog::panic("kernel main loop exited unexpectedly");
  while (true) { arch::cpu_halt(); }
}

// Kernel panic handler — uses direct UART writes for crash safety.
// Does NOT use the logging module because the system may be in an
// inconsistent state (corrupted heap, invalid stack, etc.).
[[noreturn]] void kernel_panic_handler(const char *message) noexcept {
  ::moss::kernel::arch::disable_all_interrupts();

  const auto &plat = ::moss::fdt::get_platform_info();
  u64 uart_base = (plat.dtb_valid && plat.uart.valid)
                      ? plat.uart.base_addr
                      : ::moss::kernel::platform::uart_base();
  volatile u32 *uart_data = reinterpret_cast<volatile u32 *>(uart_base);
  const char *panic_msg = "\n[PANIC] KERNEL PANIC: ";

  while (*panic_msg) {
    *uart_data = static_cast<u32>(static_cast<unsigned char>(*panic_msg++));
  }

  if (message) {
    while (*message) {
      *uart_data = static_cast<u32>(static_cast<unsigned char>(*message++));
    }
  }

  while (true) {
    ::moss::kernel::arch::cpu_halt();
  }
}

// Legacy extern "C" shim — retained for ABI compatibility with assembly
// code and test harness. New code should import moss.logging instead.
void early_debug_print(const char *message) noexcept {
  ::moss::kernel::hal::uart::puts(message);
}

// Syscall entry
long system_call_handler(long syscall_number, long arg0, long arg1,
                         long arg2, long arg3, long arg4, long arg5) noexcept {
  using namespace moss::kernel;

  if (syscall_number == 0) {
    log::klog::debug("syscall 0 (debug_print)");
    if (arg0 != 0) {
      hal::uart::puts(reinterpret_cast<const char *>(arg0));
    }
  } else if (syscall_number == 1) {
    log::klog::debug("syscall 1 (exit) status={}", arg0);
  } else {
    log::klog::debug("syscall {}", syscall_number);
  }

  return syscall::SyscallDispatcher::dispatch(syscall_number, arg0, arg1, arg2,
                                              arg3, arg4, arg5);
}

const char *get_kernel_version(void) noexcept {
  return "MOSS v1.0.0 - ARM64 Hybrid Kernel";
}

const char *get_build_info(void) noexcept {
  return "clang C++26 - Release Build";
}

// Kernel memory statistics
struct KernelMemoryInfo {
  usize total_memory;
  usize free_memory;
  usize kernel_heap_used;
  usize user_heap_used;
  u32 page_faults;
};

KernelMemoryInfo get_kernel_memory_info(void) noexcept {
  const auto &plat = ::moss::fdt::get_platform_info();
  usize total = (plat.dtb_valid && plat.total_memory_size > 0)
                    ? static_cast<usize>(plat.total_memory_size)
                    : static_cast<usize>(::moss::kernel::platform::ram_size());

  return {.total_memory = total,
          .free_memory = total / 2,
          .kernel_heap_used = 16 * 1024 * 1024,
          .user_heap_used = 0,
          .page_faults = 0};
}

// IRQ handler called from assembly irq_trampoline.
// Kept minimal — no logging in hot ISR path.
static u64 irq_count = 0;

void irq_handler_c(void) noexcept {
  irq_count++;

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
    return;  // EOI already sent above
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
    u64 tick_ns = ::moss::kernel::process::CfsParams::SCHED_LATENCY_NS;
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

  // Non-timer IRQ: pass to TimerSubsystem (legacy path)
  timer_hal::ack_interrupt();
  ::moss::kernel::timer::TimerSubsystem::instance().handle_interrupt();
}

} // extern "C"

// Syscall convention info (multi-arch)
namespace moss::kernel::arch::syscall {

void print_syscall_convention() noexcept {
    const auto& conv = get_syscall_convention();

    log::klog::info("=== syscall architecture info ===");
    log::klog::info("arch: {}", conv.arch_name);
    log::klog::info("instruction: {}", conv.syscall_instruction);
    log::klog::info("syscall_nr: {}", conv.syscall_nr_register);
    log::klog::info("return_reg: {}", conv.return_register);

    // Print arg registers — use uart directly for inline list
    hal::uart::puts("[INFO]  arg_regs: ");
    for (int i = 0; i < 6; ++i) {
        hal::uart::puts(conv.arg_registers[i]);
        if (i < 5) hal::uart::puts(", ");
    }
    hal::uart::puts("\n");
    log::klog::info("================================");
}

} // namespace moss::kernel::arch::syscall
