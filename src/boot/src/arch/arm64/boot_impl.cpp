/*
 * ARM64 architecture-specific boot implementation - module implementation unit
 * Implements the unified boot interface for ARM64
 */

module;

// PSCI constants (must be in global module fragment as macros)
enum { PSCI_CPU_ON_64 = 0xC4000003 };

// Assembly-callable function forward declaration (defined in this file)
extern "C" [[noreturn]] void secondary_cpu_entry() noexcept;

module moss.boot;

import moss.abi;

// Bring assembly/linker symbols into scope via moss.abi
using moss::abi::_start;
using moss::abi::arm64::cpu_startup_flags;
using moss::abi::arm64::early_uart_lock;
using moss::abi::arm64::exception_vectors;

static void early_uart_lock_acquire() noexcept {
  unsigned int val, status;
  asm volatile("1:\n"
               "   ldxr  %w0, [%2]\n"
               "   cbnz  %w0, 1b\n"
               "   mov   %w0, #1\n"
               "   stxr  %w1, %w0, [%2]\n"
               "   cbnz  %w1, 1b\n"
               "   dmb   sy\n"
               : "=&r"(val), "=&r"(status)
               : "r"(&early_uart_lock)
               : "memory");
}

static void early_uart_lock_release() noexcept {
  asm volatile("dmb sy" ::: "memory");
  early_uart_lock = 0;
}

using moss::i32;
using moss::PhysAddr;
using moss::u16;
using moss::u32;
using moss::u64;
using moss::u8;
using moss::VirtAddr;

// Forward declaration
u32 get_current_cpu_id_impl() noexcept;

// ========================================================================
// SMP startup and CPU detection data structures
// ========================================================================

/// CPU startup control info - aligned with assembly memory layout
struct CpuStartupInfo {
  void (*entry_point)();
  volatile u32 startup_flag;
  u32 reserved;
  u64 stack_pointer;
  u32 cpu_id;
  u32 boot_status;
} __attribute__((packed, aligned(8)));

/// CPU online state enumeration
enum class CpuState : u32 { Offline = 0, Starting = 1, Parked = 2, Active = 3, Online = 4, Failed = 5 };

/// Global CPU topology info
/// All cross-CPU shared fields use __atomic builtins (not volatile) to ensure
/// correct memory ordering on ARM64.  We use raw builtins here because this
/// file runs before moss.containers is fully available.
struct CpuTopology {
  u32 total_cpus;                                   // written only by CPU 0 during init
  u32 online_cpus;                                  // atomic: concurrent inc from secondary CPUs
  u32 cpu_states[moss::kernel::BOOT_MAX_CPUS];      // atomic: each CPU writes its own slot; CPU 0 reads all
  u64 boot_timestamps[moss::kernel::BOOT_MAX_CPUS]; // written once per CPU during init
  bool detection_completed;                         // written by CPU 0, read by others after barrier
};

// Global variable definitions
static CpuTopology g_cpu_topology = {.total_cpus = 1,
                                     .online_cpus = 1,
                                     .cpu_states = {static_cast<u32>(CpuState::Online)},
                                     .boot_timestamps = {0},
                                     .detection_completed = false};

// ========================================================================
// Dynamic CPU detection and startup control functions
// ========================================================================

static u32 probe_available_cpus() noexcept {
  // Priority 1: Use DTB-derived CPU count (set during hardware_early_init)
  const auto &plat = moss::fdt::get_platform_info();
  if (plat.dtb_valid && plat.cpu_count > 0) {
    u32 count = plat.cpu_count;
    // Store the full count in g_num_cpus (runtime, unlimited)
    moss::kernel::g_num_cpus = count;
    // Boot arrays are limited to BOOT_MAX_CPUS
    if (count > moss::kernel::BOOT_MAX_CPUS) {
      count = moss::kernel::BOOT_MAX_CPUS;
    }
    return count;
  }

  // Priority 2: Estimate from linker-allocated stack space
  auto total_stack_size =
      static_cast<u64>(moss::abi::linker::stack_top()) - static_cast<u64>(moss::abi::linker::stack_bottom());
  u32 stack_based = static_cast<u32>(total_stack_size / (32ULL * 1024));
  if (stack_based >= 1 && stack_based <= moss::kernel::BOOT_MAX_CPUS) {
    moss::kernel::g_num_cpus = stack_based;
    return stack_based;
  }

  // Fallback: single core
  moss::kernel::g_num_cpus = 1;
  return 1;
}

static u64 get_timestamp() noexcept {
  u64 count;
  asm volatile("mrs %0, cntvct_el0" : "=r"(count));
  return count;
}

// Helpers for atomic cpu_states[] access (stored as u32, CpuState enum underneath)
static void store_cpu_state(u32 cpu_id, CpuState state) noexcept {
  __atomic_store_n(&g_cpu_topology.cpu_states[cpu_id], static_cast<u32>(state), __ATOMIC_RELEASE);
}

static CpuState load_cpu_state(u32 cpu_id) noexcept {
  return static_cast<CpuState>(__atomic_load_n(&g_cpu_topology.cpu_states[cpu_id], __ATOMIC_ACQUIRE));
}

static void initialize_cpu_startup_info(u32 detected_cpus) noexcept {
  g_cpu_topology.total_cpus = detected_cpus;
  __atomic_store_n(&g_cpu_topology.online_cpus, 1, __ATOMIC_RELAXED);
  g_cpu_topology.detection_completed = true;

  for (u32 cpu = 0; cpu < moss::kernel::BOOT_MAX_CPUS; cpu++) {
    if (cpu == 0) {
      store_cpu_state(cpu, CpuState::Online);
      g_cpu_topology.boot_timestamps[cpu] = get_timestamp();
    } else {
      store_cpu_state(cpu, CpuState::Offline);
      g_cpu_topology.boot_timestamps[cpu] = 0;
    }
  }

  for (u32 cpu = 0; cpu < moss::kernel::BOOT_MAX_CPUS; cpu++) {
    cpu_startup_flags[cpu][0] = 0;
    cpu_startup_flags[cpu][1] = 0;
  }
}

[[maybe_unused]] static bool wait_cpu_parked(u32 cpu_id, u32 timeout_ms) noexcept {
  if (cpu_id >= moss::kernel::BOOT_MAX_CPUS) {
    return false;
  }

  u32 iteration = 0;
  u32 max_iterations = timeout_ms * 10;

  volatile u8 *uart_debug = reinterpret_cast<volatile u8 *>(0x9000000);
  early_uart_lock_acquire();
  uart_debug[0] = 'W';
  uart_debug[0] = '0' + static_cast<u8>(cpu_id % 10);
  uart_debug[0] = 10;
  early_uart_lock_release();

  while (load_cpu_state(cpu_id) != CpuState::Parked) {
    if (iteration >= max_iterations) {
      early_uart_lock_acquire();
      uart_debug[0] = 'T';
      uart_debug[0] = '0' + static_cast<u8>(cpu_id % 10);
      uart_debug[0] = 10;
      early_uart_lock_release();

      store_cpu_state(cpu_id, CpuState::Failed);
      return false;
    }

    for (volatile u32 i = 0; i < 10000; i = i + 1) {
      asm volatile("nop");
    }
    iteration++;

    if (iteration % 1000 == 0) {
      early_uart_lock_acquire();
      uart_debug[0] = 'C';
      uart_debug[0] = '0' + static_cast<u8>(cpu_id % 10);
      uart_debug[0] = '0' + static_cast<u8>(load_cpu_state(cpu_id));
      uart_debug[0] = 10;
      early_uart_lock_release();
    }
  }

  early_uart_lock_acquire();
  uart_debug[0] = 'S';
  uart_debug[0] = '0' + static_cast<u8>(cpu_id % 10);
  uart_debug[0] = 10;
  early_uart_lock_release();

  return true;
}

void mark_cpu_online(u32 cpu_id) noexcept {
  if (cpu_id < moss::kernel::BOOT_MAX_CPUS) {
    store_cpu_state(cpu_id, CpuState::Online);
    g_cpu_topology.boot_timestamps[cpu_id] = 0;
    __atomic_fetch_add(&g_cpu_topology.online_cpus, 1, __ATOMIC_ACQ_REL);
  }
}

void mark_cpu_parked(u32 cpu_id) noexcept {
  if (cpu_id < moss::kernel::BOOT_MAX_CPUS) {
    g_cpu_topology.boot_timestamps[cpu_id] = 0;
    // Release store: makes all prior initialization visible to CPU 0
    store_cpu_state(cpu_id, CpuState::Parked);

    volatile u8 *uart_debug = reinterpret_cast<volatile u8 *>(0x9000000);
    early_uart_lock_acquire();
    uart_debug[0] = 'M';
    uart_debug[0] = '0' + static_cast<u8>(cpu_id % 10);
    uart_debug[0] = 10;
    early_uart_lock_release();
  }
}

void mark_cpu_active(u32 cpu_id) noexcept {
  if (cpu_id < moss::kernel::BOOT_MAX_CPUS) {
    store_cpu_state(cpu_id, CpuState::Active);
  }
}

bool is_cpu_in_state(u32 cpu_id, CpuState expected_state) noexcept {
  if (cpu_id >= moss::kernel::BOOT_MAX_CPUS) {
    return false;
  }
  return load_cpu_state(cpu_id) == expected_state;
}

bool wait_for_cpu_state(u32 cpu_id, CpuState expected_state, u32 timeout_ms) noexcept {
  if (cpu_id >= moss::kernel::BOOT_MAX_CPUS) {
    return false;
  }

  u32 elapsed = 0;
  while (elapsed < timeout_ms) {
    if (load_cpu_state(cpu_id) == expected_state) {
      return true;
    }
    for (volatile u32 i = 0; i < 10000; i = i + 1) {
      asm volatile("nop");
    }
    elapsed += 10;
  }

  return load_cpu_state(cpu_id) == expected_state;
}

[[noreturn]] void cpu_park(u32 cpu_id) noexcept {
  volatile u8 *uart_out = reinterpret_cast<volatile u8 *>(0x9000000);

  early_uart_lock_acquire();
  uart_out[0] = 'P';
  uart_out[0] = 'A';
  uart_out[0] = 'R';
  uart_out[0] = 'K';
  uart_out[0] = '0' + static_cast<u8>(cpu_id % 10);
  uart_out[0] = 10;
  early_uart_lock_release();

  // Directly use store_cpu_state + UART (mark_cpu_parked does UART too,
  // but cpu_park has its own UART output above, so just set state here)
  store_cpu_state(cpu_id, CpuState::Parked);

  while (!is_cpu_in_state(cpu_id, CpuState::Active)) {
    asm volatile("wfi");

    for (volatile u32 i = 0; i < 100; i = i + 1) {
      asm volatile("nop");
    }
  }

  early_uart_lock_acquire();
  uart_out[0] = 'A';
  uart_out[0] = 'C';
  uart_out[0] = 'T';
  uart_out[0] = 'V';
  uart_out[0] = '0' + static_cast<u8>(cpu_id % 10);
  uart_out[0] = 10;

  uart_out[0] = 'W';
  uart_out[0] = 'A';
  uart_out[0] = 'I';
  uart_out[0] = 'T';
  uart_out[0] = '0' + static_cast<u8>(cpu_id % 10);
  uart_out[0] = 10;
  early_uart_lock_release();

  while (true) {
    asm volatile("wfi");

    for (u32 i = 0; i < 1000; i++) {
      asm volatile("nop");
    }
  }
}

// Secondary CPU entry point — park first, then full subsystem initialization
// on activation by CPU 0 (after GIC distributor and timer are ready).
extern "C" [[noreturn]] void secondary_cpu_entry() noexcept {
  // 1. Read CPU ID from hardware
  u64 mpidr;
  asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  u32 cpu_id = static_cast<u32>(mpidr & 0xFF);

  // Minimal UART output (locked)
  volatile u8 *uart_out = reinterpret_cast<volatile u8 *>(0x09000000);
  early_uart_lock_acquire();
  uart_out[0] = 'S';
  uart_out[0] = '0' + static_cast<u8>(cpu_id % 10);
  uart_out[0] = '\n';
  early_uart_lock_release();

  // --- Phase 1: Park and wait for CPU 0 to finish initialization ---
  // Use atomic release store so CPU 0 sees the state transition
  if (cpu_id < moss::kernel::BOOT_MAX_CPUS) {
    store_cpu_state(cpu_id, CpuState::Parked);
  }

  // Wait until CPU 0 marks us as Active (meaning all subsystems are ready)
  while (!is_cpu_in_state(cpu_id, CpuState::Active)) {
    asm volatile("wfe");
  }

  early_uart_lock_acquire();
  uart_out[0] = 'I';
  uart_out[0] = '0' + static_cast<u8>(cpu_id % 10);
  uart_out[0] = '\n';
  early_uart_lock_release();

  // --- Phase 2: Full subsystem initialization (GIC/timer are ready) ---

  // 2. Set exception vectors (same as CPU 0)
  asm volatile("msr vbar_el1, %0" ::"r"(exception_vectors));
  asm volatile("isb");

  // 3. Enable FP/NEON access
  u64 cpacr = (3ULL << 20);
  asm volatile("msr cpacr_el1, %0" ::"r"(cpacr));
  asm volatile("isb");

  // 4. Initialize GIC CPU interface for this CPU
  // GICv3: init_cpu_interface() uses ICC system registers + GICR internally;
  //        the cpu_base argument is ignored (pass 0).
  // GICv2: init_cpu_interface() uses MMIO GICC registers via cpu_base.
  const auto &plat = moss::fdt::get_platform_info();
  moss::kernel::VirtAddr gic_cpu_base = 0;
  if (moss::kernel::hal::intc::g_gic_version != moss::kernel::hal::intc::GicVersion::GICv3) {
    gic_cpu_base = (plat.dtb_valid && plat.intc.valid)
                       ? static_cast<moss::kernel::VirtAddr>(plat.intc.cpu_base)
                       : moss::kernel::platform::intc_cpu_base();
  }
  (void)moss::kernel::hal::intc::init_cpu_interface(gic_cpu_base);

  // Enable timer PPI (IRQ 27) and reschedule SGI (IRQ 0) in per-CPU
  // banked GICD_ISENABLER.  PPI/SGI registers are per-CPU in GICv2.
  moss::kernel::VirtAddr gic_dist_base = (plat.dtb_valid && plat.intc.valid)
                                             ? static_cast<moss::kernel::VirtAddr>(plat.intc.dist_base)
                                             : moss::kernel::platform::intc_dist_base();
  moss::kernel::hal::intc::enable_irq(gic_dist_base, moss::kernel::platform::timer_irq());
  moss::kernel::hal::intc::enable_irq(gic_dist_base, 0); // SGI 0 = Reschedule IPI

  // 5. Enable per-CPU timer and set initial compare for first tick.
  //    Use SCHED_LATENCY_NS (~6ms) so the first scheduler tick fires promptly.
  //    After that, irq_handler_c reprograms the compare register each tick.
  moss::kernel::hal::timer::enable();
  u64 counter_now = moss::kernel::hal::timer::read_counter();
  u64 first_tick_cycles = moss::kernel::timer::TimerSubsystem::instance().clocksource().ns_to_cycles(
      moss::kernel::process::cfs_params::SCHED_LATENCY_NS);
  moss::kernel::hal::timer::set_compare(counter_now + first_tick_cycles);

  // 6. Mark CPU as online (init complete)
  // mark_cpu_online uses __ATOMIC_RELEASE for state + __ATOMIC_ACQ_REL for count
  mark_cpu_online(cpu_id);
  asm volatile("sev" ::: "memory"); // wake CPU 0's wait_for_cpu_state

  early_uart_lock_acquire();
  uart_out[0] = 'R';
  uart_out[0] = '0' + static_cast<u8>(cpu_id % 10);
  uart_out[0] = '\n';
  early_uart_lock_release();

  // 7. Enable IRQs and enter scheduling loop (never returns)
  asm volatile("msr daifclr, #2" ::: "memory");
  moss::kernel::process::secondary_cpu_schedule_loop(cpu_id);
}

// === Linux-style global GIC hardware instances ===

namespace moss::boot {

moss::kernel::interrupts::GenericInterruptController *g_gic_controller = nullptr;
bool g_gic_hardware_available = false;

} // namespace moss::boot

/// Activate parked secondary CPUs: mark Active (to unblock their init),
/// then wait for them to reach Online (init complete, scheduling loop entered).
namespace moss::boot {
void activate_secondary_cpus() noexcept {
  u32 successfully_activated = 0;

  for (u32 cpu_id = 1; cpu_id < g_cpu_topology.total_cpus; ++cpu_id) {
    // Wait for secondary CPU to reach Parked state (PSCI may take time)
    if (!wait_for_cpu_state(cpu_id, CpuState::Parked, 3000)) {
      continue;
    }

    // Unblock secondary CPU from its WFE loop
    // store_cpu_state uses __ATOMIC_RELEASE, so the state is visible
    // before SEV wakes the secondary CPU from WFE.
    mark_cpu_active(cpu_id);
    asm volatile("sev" ::: "memory");

    // Wait for it to finish init and reach Online state
    if (wait_for_cpu_state(cpu_id, CpuState::Online, 3000)) {
      successfully_activated++;
    }
  }

  __atomic_store_n(&g_cpu_topology.online_cpus, 1 + successfully_activated, __ATOMIC_RELAXED);
}

u32 wait_for_all_cpus_active(u32 timeout_ms) noexcept {
  u32 active_count = 1;
  u32 elapsed = 0;

  while (elapsed < timeout_ms) {
    active_count = 1;

    for (u32 cpu_id = 1; cpu_id < g_cpu_topology.total_cpus; ++cpu_id) {
      if (is_cpu_in_state(cpu_id, CpuState::Active) || is_cpu_in_state(cpu_id, CpuState::Online)) {
        active_count++;
      }
    }

    if (active_count >= g_cpu_topology.total_cpus) {
      break;
    }

    for (volatile u32 i = 0; i < 100000; i = i + 1) {
      asm volatile("nop");
    }
    elapsed += 10;
  }

  return active_count;
}

} // namespace moss::boot

// ARM64 PSCI call function
static u64 psci_call(u32 function_id, u64 arg0 = 0, u64 arg1 = 0, u64 arg2 = 0, u64 arg3 = 0) noexcept {
  u64 result;

  asm volatile("mov x0, %1\n"
               "mov x1, %2\n"
               "mov x2, %3\n"
               "mov x3, %4\n"
               "mov x4, %5\n"
               "hvc #0\n"
               "mov %0, x0\n"
               : "=r"(result)
               : "r"(static_cast<u64>(function_id)), "r"(arg0), "r"(arg1), "r"(arg2), "r"(arg3)
               : "x0", "x1", "x2", "x3", "x4", "memory");

  return result;
}

namespace moss::boot {

// Global boot status
BootStatus g_boot_status = {.current_stage = BootStage::PreInit,
                            .completed_stages_mask = 0,
                            .stage_timestamps = {0},
                            .last_error = ::moss::kernel::ErrorCode::Success};

// Early UART output
class EarlyUart {
private:
  static constexpr VirtAddr UART_BASE = moss::kernel::platform::uart_base();
  static constexpr u32 UART_DR = 0x000;
  static constexpr u32 UART_FR = 0x018;
  static constexpr u32 UART_FR_TXFF = (1 << 5);

  volatile u32 *const uart_base_;

public:
  EarlyUart() : uart_base_(reinterpret_cast<volatile u32 *>(UART_BASE)) {}

  void put_char(char c) const {
    while (uart_base_[UART_FR / 4] & UART_FR_TXFF) {
    }
    uart_base_[UART_DR / 4] = static_cast<u32>(c);
  }

  void put_string(const char *str) const {
    while (*str) {
      if (*str == '\n') {
        put_char('\r');
      }
      put_char(*str++);
    }
  }
};

static EarlyUart early_uart;

// RAII guard — holds early_uart_lock for the lifetime of the scope.
// Use to group multiple early_print() calls into one atomic output block.
struct EarlyPrintGuard {
  EarlyPrintGuard() noexcept { early_uart_lock_acquire(); }
  ~EarlyPrintGuard() noexcept { early_uart_lock_release(); }
  EarlyPrintGuard(const EarlyPrintGuard &) = delete;
  auto operator=(const EarlyPrintGuard &) -> EarlyPrintGuard & = delete;
};

// early_print / early_print_hex: raw output, NO lock.
// Caller must hold early_uart_lock (via EarlyPrintGuard) when concurrent
// CPUs may be printing.  Before SMP starts there is no contention.
static void early_print(const char *str) { early_uart.put_string(str); }

static void early_print_hex(u64 value) {
  constexpr char hex_chars[] = "0123456789ABCDEF";
  char buffer[19] = "0x";

  for (int i = 15; i >= 0; i--) {
    buffer[2 + (15 - i)] = hex_chars[(value >> (i * 4)) & 0xF];
  }
  buffer[18] = '\0';

  early_print(buffer);
}

static u64 get_timestamp_counter() noexcept {
  u64 counter;
  asm volatile("mrs %0, cntvct_el0" : "=r"(counter));
  return counter;
}

u32 get_current_cpu_id_impl() noexcept {
  u64 mpidr;
  asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  return static_cast<u32>(mpidr & 0xFF);
}

// Boot stage status update implementation
void update_boot_stage(BootStage stage, ::moss::kernel::ErrorCode error) noexcept {
  g_boot_status.current_stage = stage;
  g_boot_status.last_error = error;

  u32 stage_index = static_cast<u32>(stage);
  if (stage_index < 8) {
    g_boot_status.stage_timestamps[stage_index] = get_timestamp_counter();

    if (error == ::moss::kernel::ErrorCode::Success) {
      g_boot_status.completed_stages_mask |= (1U << stage_index);
    }
  }
}

} // namespace moss::boot

// ARM64BootImpl member function implementations
::moss::kernel::VoidResult moss::boot::ARM64BootImpl::hardware_early_init(BootContext &ctx) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::HardwareInit);

  early_print("=== ARM64 Hardware Early Init ===\n");

  ctx.cpu_id = moss::boot::get_current_cpu_id_impl();
  early_print("CPU ID: ");
  early_print_hex(ctx.cpu_id);
  early_print("\n");

  // --- DTB 解析：从 Device Tree 获取真实硬件拓扑 ---
  // QEMU 通过 x0 寄存器传递 DTB 指针，已保存在 ctx.device_tree_ptr 中。
  // 解析成功后用真实值填充 BootContext，否则回退到硬编码默认值。
  if (ctx.device_tree_ptr) {
    early_print("DTB pointer: ");
    early_print_hex(reinterpret_cast<u64>(ctx.device_tree_ptr));
    early_print("\n");

    if (moss::fdt::parse_dtb(ctx.device_tree_ptr)) {
      const auto &info = moss::fdt::get_platform_info();

      early_print("DTB parse OK: ");
      early_print_hex(info.cpu_count);
      early_print(" CPUs, memory ");
      early_print_hex(info.total_memory_start);
      early_print(" + ");
      early_print_hex(info.total_memory_size);
      early_print("\n");

      ctx.memory_start = info.total_memory_start;
      ctx.memory_size = info.total_memory_size;
      ctx.kernel_phys_base = info.total_memory_start;
      ctx.total_cpus = info.cpu_count;
    } else {
      early_print("DTB parse failed, using platform defaults\n");
      ctx.memory_start = moss::kernel::platform::ram_base();
      ctx.memory_size = moss::kernel::platform::ram_size();
      ctx.kernel_phys_base = moss::kernel::platform::ram_base();
    }
  } else {
    early_print("No DTB pointer, using platform defaults\n");
    ctx.memory_start = moss::kernel::platform::ram_base();
    ctx.memory_size = moss::kernel::platform::ram_size();
    ctx.kernel_phys_base = moss::kernel::platform::ram_base();
  }

  ctx.kernel_virt_base = moss::boot::arch_constants::KERNEL_VIRT_BASE;

  early_print("ARM64 hardware init complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::setup_memory_management(BootContext &ctx) noexcept {
  (void)ctx;
  moss::boot::update_boot_stage(moss::boot::BootStage::MemoryManagement);

  early_print("=== ARM64 Memory Management Setup ===\n");

  // Phase 1: Setup identity-mapped page tables in TTBR0 (existing 4×1GB blocks)
  auto mmu_result = ::moss::kernel::mm::setup_mmu();
  if (!mmu_result) {
    early_print("MMU setup failed\n");
    return ::moss::kernel::VoidResult{mmu_result.error()};
  }

  // Phase 2: Initialize PageFrameAllocator (buddy allocator)
  auto pfa_result = ::moss::kernel::mm::PageFrameAllocator::initialize();
  if (!pfa_result) {
    early_print("Physical page allocator init failed\n");
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::OutOfMemory};
  }

  // Phase 3: Build TTBR1 high-half kernel page table
  // Maps physical 0-4GB at KERNEL_DIRECT_MAP_BASE (0xFFFF800000000000)
  early_print("Building TTBR1 high-half kernel page table...\n");
  auto high_result = ::moss::kernel::mm::PageTableManager::setup_kernel_high_half_tables();
  if (!high_result) {
    early_print("High-half page table setup failed\n");
    return ::moss::kernel::VoidResult{high_result.error()};
  }

  // Phase 4: Activate TTBR1 — write high-half PGD to ttbr1_el1
  PhysAddr high_pgd_pa = ::moss::kernel::mm::PageTableManager::get_physical_address(
      ::moss::kernel::mm::PageTableManager::get_kernel_high_pgd());

  early_print("TTBR1 PGD physical: ");
  early_print_hex(high_pgd_pa);
  early_print("\n");

#if defined(MOSS_ARCH_ARM64)
  // Write TTBR1 with kernel high-half PGD (ASID=0 for kernel)
  asm volatile("msr ttbr1_el1, %0" ::"r"(high_pgd_pa));
  asm volatile("isb" ::: "memory");

  // Full TLB invalidation to ensure new TTBR1 mappings take effect
  asm volatile("tlbi vmalle1" ::: "memory");
  asm volatile("dsb sy" ::: "memory");
  asm volatile("isb" ::: "memory");
#endif

  early_print("TTBR1 high-half mapping active\n");

  // Phase 5: Enable dynamic page table allocation (buddy-backed)
  ::moss::kernel::mm::PageTableManager::enable_dynamic_alloc();

  // Phase 6: Initialize runtime heap
  VirtAddr heap_start = moss::abi::linker::heap_start();
  ::moss::kernel::usize initial_heap_size = 256ULL * 1024;
  auto heap_result = ::moss::kernel::mm::RuntimeHeapAllocator::initialize_heap(heap_start, initial_heap_size);
  if (!heap_result) {
    early_print("Runtime heap allocator init failed\n");
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::OutOfMemory};
  }

  early_print("ARM64 memory management setup complete (high-half active)\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::setup_interrupts_and_exceptions(BootContext &ctx) noexcept {
  (void)ctx;
  moss::boot::update_boot_stage(moss::boot::BootStage::InterruptsExceptions);

  early_print("=== ARM64 Interrupts and Exceptions Setup ===\n");

  early_print("ARM64 GIC hardware init...\n");

  using namespace moss::kernel::interrupts;
  g_gic_controller = new GenericInterruptController();
  if (!g_gic_controller) {
    early_print("GIC controller memory allocation failed\n");
    g_gic_hardware_available = false;
    early_print("System will use IPI proof-of-concept mode\n");
  } else {
    // Resolve GIC addresses from DTB with platform defaults fallback.
    // Determine GIC version hint and choose second_base accordingly:
    //   GICv3: second_base = GICR redistributor base
    //   GICv2: second_base = GICC CPU interface base
    const auto &plat = moss::fdt::get_platform_info();
    moss::kernel::VirtAddr gic_dist_base = (plat.dtb_valid && plat.intc.valid)
                                               ? static_cast<moss::kernel::VirtAddr>(plat.intc.dist_base)
                                               : moss::kernel::platform::intc_dist_base();
    u8 gic_ver = (plat.dtb_valid && plat.intc.valid) ? plat.intc.gic_version : 2;
    moss::kernel::VirtAddr second_base;
    if (gic_ver >= 3) {
      second_base = (plat.dtb_valid && plat.intc.valid && plat.intc.redist_base != 0)
                        ? static_cast<moss::kernel::VirtAddr>(plat.intc.redist_base)
                        : moss::kernel::platform::intc_redist_base();
      early_print("GIC GICD=");
      early_print_hex(gic_dist_base);
      early_print(" GICR=");
      early_print_hex(second_base);
      early_print(" (v3)\n");
    } else {
      second_base = (plat.dtb_valid && plat.intc.valid)
                        ? static_cast<moss::kernel::VirtAddr>(plat.intc.cpu_base)
                        : moss::kernel::platform::intc_cpu_base();
      early_print("GIC GICD=");
      early_print_hex(gic_dist_base);
      early_print(" GICC=");
      early_print_hex(second_base);
      early_print(" (v2)\n");
    }

    auto gic_result = g_gic_controller->initialize(gic_dist_base, second_base, gic_ver);
    if (gic_result) {
      early_print("GIC hardware init success\n");
      early_print("GIC features: SGI 0-15, PPI 16-31, SPI 32+\n");
      g_gic_hardware_available = true;

      early_print("GIC SGI verification...\n");
      early_print("SGI 0-15 available for IPI communication\n");
    } else {
      early_print("GIC hardware init failed\n");
      delete g_gic_controller;
      g_gic_controller = nullptr;
      g_gic_hardware_available = false;
      early_print("System will use IPI proof-of-concept mode\n");
    }
  }

  if (g_gic_hardware_available) {
    early_print("GIC hardware integration success - real hardware IPI available\n");
  } else {
    early_print("GIC hardware unavailable - will use proof-of-concept mode\n");
  }

  early_print("ARM64 interrupt/exception setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::setup_smp_support(BootContext &ctx) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::SmpSupport);

  {
    EarlyPrintGuard g;
    early_print("=== ARM64 SMP Support Setup (Dynamic Detection) ===\n");
  }

  u32 detected_cpus = probe_available_cpus();
  {
    EarlyPrintGuard g;
    early_print("Detected CPU count: ");
    early_print_hex(static_cast<u64>(detected_cpus));
    early_print("\n");
  }

  initialize_cpu_startup_info(detected_cpus);

  if (detected_cpus == 1) {
    {
      EarlyPrintGuard g;
      early_print("Single-core mode\n");
    }
    ctx.total_cpus = 1;
  } else {
    {
      EarlyPrintGuard g;
      early_print("Multi-core boot sequence:\n");
    }

    u32 successful_cpus = 1;

    for (u32 cpu_id = 1; cpu_id < detected_cpus; cpu_id++) {
      {
        EarlyPrintGuard g;
        early_print("   Starting CPU ");
        early_print_hex(static_cast<u64>(cpu_id));
        early_print(" via PSCI...\n");
      }

      // Mark Starting BEFORE PSCI call to avoid race: secondary CPU
      // may reach Parked before we return from PSCI, and we must not
      // overwrite its Parked state with Starting.
      store_cpu_state(cpu_id, CpuState::Starting);

      cpu_startup_flags[cpu_id][0] = reinterpret_cast<u64>(secondary_cpu_entry);
      cpu_startup_flags[cpu_id][1] = 1;

      asm volatile("dmb sy" ::: "memory");
      asm volatile("dsb sy" ::: "memory");

      asm volatile("dc civac, %0" : : "r"(&cpu_startup_flags[cpu_id][0]) : "memory");
      asm volatile("dc civac, %0" : : "r"(&cpu_startup_flags[cpu_id][1]) : "memory");
      asm volatile("dsb sy" ::: "memory");

      asm volatile("sev" ::: "memory");

      u64 target_mpidr = static_cast<u64>(cpu_id);
      u64 entry_addr = reinterpret_cast<u64>(_start);
      u64 context_id = static_cast<u64>(cpu_id);

      {
        EarlyPrintGuard g;
        early_print("   PSCI_CPU_ON: target=");
        early_print_hex(target_mpidr);
        early_print(" entry=");
        early_print_hex(entry_addr);
        early_print("\n");
      }

      u64 psci_result = psci_call(PSCI_CPU_ON_64, target_mpidr, entry_addr, context_id);

      {
        EarlyPrintGuard g;
        early_print("   PSCI result: ");
        early_print_hex(psci_result);
        early_print(psci_result == 0 ? " (success)\n" : " (failed)\n");
      }

      if (psci_result != 0) {
        continue;
      }

      // Wait for this CPU to reach Parked before starting the next.
      // Serializes boot so each CPU's debug output ([N], CPUN:S, DN, SN)
      // appears cleanly between its own PSCI result and the next CPU.
      if (!wait_for_cpu_state(cpu_id, CpuState::Parked, 5000)) {
        {
          EarlyPrintGuard g;
          early_print("   Warning: CPU ");
          early_print_hex(static_cast<u64>(cpu_id));
          early_print(" did not park in time\n");
        }
      }
    }

    // Count successfully parked CPUs
    successful_cpus = 1;
    for (u32 cpu_id = 1; cpu_id < detected_cpus; cpu_id++) {
      if (load_cpu_state(cpu_id) == CpuState::Parked) {
        successful_cpus++;
      }
    }
    ctx.total_cpus = successful_cpus;

    if (successful_cpus > 1) {
      {
        EarlyPrintGuard g;
        early_print("SMP boot complete: ");
        early_print_hex(static_cast<u64>(successful_cpus));
        early_print(" CPUs online\n");
      }
    } else {
      {
        EarlyPrintGuard g;
        early_print("Secondary CPU startup failed, fallback to single-core mode\n");
      }
      ctx.total_cpus = 1;
    }
  }

  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::finalize_arch_init(BootContext & /* ctx */) noexcept {
  {
    EarlyPrintGuard g;
    early_print("=== ARM64 Architecture Init Complete ===\n");
  }

  // Mark runtime heap as ready so operator new uses RuntimeHeapAllocator
  // instead of the 64KB early static buffer
  moss::abi::entry::mark_runtime_heap_ready();
  {
    EarlyPrintGuard g;
    early_print("Runtime heap marked ready\n");
  }

  {
    EarlyPrintGuard g;
    early_print("ARM64 architecture-specific init all complete\n\n");
  }
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::ARM64BootImpl::detect_memory_layout(BootContext &ctx) noexcept {
  (void)ctx;
  return ::moss::kernel::VoidResult{};
}

u32 moss::boot::ARM64BootImpl::get_current_cpu_id() noexcept { return moss::boot::get_current_cpu_id_impl(); }

[[noreturn]] void moss::boot::ARM64BootImpl::arch_panic(const char *message) noexcept {
  early_print("\n=== ARM64 PANIC ===\n");
  early_print(message);
  early_print("\n==================\n");

  asm volatile("movz x0, #0x0008, lsl #0\n"
               "movk x0, #0x8400, lsl #16\n"
               "smc #0\n"
               :
               :
               : "x0");

  while (true) {
    asm volatile("wfi");
  }
}
