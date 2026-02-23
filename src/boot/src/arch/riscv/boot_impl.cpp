/*
 * RISC-V architecture-specific boot implementation - module implementation unit
 * Implements the unified boot interface for RISC-V
 */

module;

// Architecture detection (global module fragment)
#ifndef MOSS_ARCH_RISCV
#define MOSS_ARCH_RISCV
#endif

module moss.boot;

import moss.abi;

using moss::PhysAddr;
using moss::u16;
using moss::u32;
using moss::u64;
using moss::u8;
using moss::VirtAddr;

namespace moss::boot {

// Global boot status
BootStatus g_boot_status = {.current_stage = BootStage::PreInit,
                            .completed_stages_mask = 0,
                            .stage_timestamps = {0},
                            .last_error = ::moss::kernel::ErrorCode::Success};

// Early UART output (RISC-V specific)
class EarlyUart {
private:
  static constexpr VirtAddr UART_BASE = moss::kernel::platform::uart_base();
  static constexpr u32 UART_REG_TXDATA = 0x00;

  volatile u32 *const uart_base_;

public:
  EarlyUart() : uart_base_(reinterpret_cast<volatile u32 *>(UART_BASE)) {}

  void put_char(char c) const { uart_base_[UART_REG_TXDATA / 4] = static_cast<u32>(c); }

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
  asm volatile("rdtime %0" : "=r"(counter));
  return counter;
}

static u32 get_current_cpu_id_impl() noexcept {
  // S-mode cannot read mhartid (M-mode only).
  // Hart ID is stored in tp register by _start (set from a0 passed by OpenSBI).
  u64 hartid;
  asm volatile("mv %0, tp" : "=r"(hartid));
  return static_cast<u32>(hartid);
}

// Boot stage status update
void update_boot_stage(BootStage stage, ::moss::kernel::ErrorCode error) noexcept {
  g_boot_status.current_stage = stage;
  g_boot_status.last_error = error;

  u32 stage_index = static_cast<u32>(stage);
  if (stage_index < 8) {
    g_boot_status.stage_timestamps[stage_index] = get_timestamp_counter();

    if (error == ::moss::kernel::ErrorCode::Success) {
      g_boot_status.completed_stages_mask |= (1u << stage_index);
    }
  }
}

// =============================================================================
// SBI (Supervisor Binary Interface) call wrapper
// =============================================================================
// OpenSBI provides firmware services via ecall from S-mode.
// Convention: a7 = Extension ID (EID), a6 = Function ID (FID),
//             a0-a5 = arguments.  Returns: a0 = error, a1 = value.
struct SbiResult {
  long error;
  long value;
};

static SbiResult sbi_call(u64 eid, u64 fid, u64 a0 = 0, u64 a1 = 0, u64 a2 = 0) noexcept {
  register u64 r_a0 asm("a0") = a0;
  register u64 r_a1 asm("a1") = a1;
  register u64 r_a2 asm("a2") = a2;
  register u64 r_a6 asm("a6") = fid;
  register u64 r_a7 asm("a7") = eid;
  asm volatile("ecall" : "+r"(r_a0), "+r"(r_a1) : "r"(r_a2), "r"(r_a6), "r"(r_a7) : "memory");
  return {static_cast<long>(r_a0), static_cast<long>(r_a1)};
}

// SBI HSM (Hart State Management) extension — EID 0x48534D
static constexpr u64 SBI_EID_HSM = 0x48534D;

static SbiResult sbi_hart_start(u64 hartid, u64 start_addr, u64 opaque) noexcept {
  return sbi_call(SBI_EID_HSM, 0, hartid, start_addr, opaque);
}

} // namespace moss::boot

// RISCVBootImpl member function implementations
::moss::kernel::VoidResult moss::boot::RISCVBootImpl::hardware_early_init(BootContext &ctx) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::HardwareInit);

  moss::boot::early_print("=== RISC-V Hardware Early Init ===\n");

  ctx.cpu_id = moss::boot::get_current_cpu_id_impl();
  moss::boot::early_print("CPU ID (Hart ID): ");
  moss::boot::early_print_hex(ctx.cpu_id);
  moss::boot::early_print("\n");

  // M-mode CSRs (mvendorid, marchid, mimpid) are not accessible from S-mode.
  // Hardware identification requires SBI probe calls or DTB parsing.

  // --- DTB 解析：从 Device Tree 获取真实硬件拓扑 ---
  // OpenSBI 通过 a1 寄存器传递 DTB 指针，已保存在 ctx.device_tree_ptr 中。
  if (ctx.device_tree_ptr) {
    moss::boot::early_print("DTB pointer: ");
    moss::boot::early_print_hex(reinterpret_cast<u64>(ctx.device_tree_ptr));
    moss::boot::early_print("\n");

    if (moss::fdt::parse_dtb(ctx.device_tree_ptr)) {
      const auto &info = moss::fdt::get_platform_info();

      moss::boot::early_print("DTB parse OK: ");
      moss::boot::early_print_hex(info.cpu_count);
      moss::boot::early_print(" CPUs, memory ");
      moss::boot::early_print_hex(info.total_memory_start);
      moss::boot::early_print(" + ");
      moss::boot::early_print_hex(info.total_memory_size);
      moss::boot::early_print("\n");

      ctx.memory_start = info.total_memory_start;
      ctx.memory_size = info.total_memory_size;
      ctx.kernel_phys_base = info.total_memory_start;
      ctx.total_cpus = info.cpu_count;
    } else {
      moss::boot::early_print("DTB parse failed, using platform defaults\n");
      ctx.memory_start = moss::kernel::platform::ram_base();
      ctx.memory_size = moss::kernel::platform::ram_size();
      ctx.kernel_phys_base = moss::kernel::platform::ram_base();
    }
  } else {
    moss::boot::early_print("No DTB pointer, using platform defaults\n");
    ctx.memory_start = moss::kernel::platform::ram_base();
    ctx.memory_size = moss::kernel::platform::ram_size();
    ctx.kernel_phys_base = moss::kernel::platform::ram_base();
  }

  ctx.kernel_virt_base = moss::boot::arch_constants::KERNEL_VIRT_BASE;

  moss::boot::early_print("Memory range: ");
  moss::boot::early_print_hex(ctx.memory_start);
  moss::boot::early_print(" - ");
  moss::boot::early_print_hex(ctx.memory_start + ctx.memory_size);
  moss::boot::early_print("\n");

  moss::boot::early_print("RISC-V hardware init complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCVBootImpl::setup_memory_management(BootContext &ctx) noexcept {
  (void)ctx;
  moss::boot::update_boot_stage(moss::boot::BootStage::MemoryManagement);

  moss::boot::early_print("=== RISC-V Memory Management Setup ===\n");

  // Skip MMU/page-table setup for now: RISC-V S-mode boots with satp=0
  // (bare/physical addressing).  A proper Sv39/Sv48 identity map requires
  // RISC-V-specific page table entry format; the current setup_mmu() uses
  // ARM64 block descriptors.  PFA and heap work fine without virtual memory.
  moss::boot::early_print("  MMU: skipped (bare mode, satp=0)\n");

  auto pfa_result = ::moss::kernel::mm::PageFrameAllocator::initialize();
  if (!pfa_result) {
    moss::boot::early_print("  WARNING: PageFrameAllocator init failed\n");
  }

  VirtAddr heap_start = moss::abi::linker::heap_start();
  ::moss::kernel::usize initial_heap_size = 256ULL * 1024;
  auto heap_result = ::moss::kernel::mm::RuntimeHeapAllocator::initialize_heap(heap_start, initial_heap_size);
  if (!heap_result) {
    moss::boot::early_print("  WARNING: RuntimeHeapAllocator init failed\n");
  }

  moss::boot::early_print("RISC-V memory management setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCVBootImpl::setup_interrupts_and_exceptions(BootContext &ctx) noexcept {
  (void)ctx;
  moss::boot::update_boot_stage(moss::boot::BootStage::InterruptsExceptions);

  moss::boot::early_print("=== RISC-V Interrupts and Exceptions Setup ===\n");

  // 1. Set stvec to point to the trap handler (direct mode)
  u64 trap_addr = reinterpret_cast<u64>(&moss::abi::syscall_entry_point);
  asm volatile("csrw stvec, %0" ::"r"(trap_addr));
  moss::boot::early_print("  stvec configured\n");

  // 2. Initialize PLIC (Platform Level Interrupt Controller)
  auto *gic = new moss::kernel::interrupts::GenericInterruptController();
  if (gic) {
    VirtAddr plic_base = moss::kernel::platform::intc_dist_base(); // 0x0C000000

    // S-mode context for hart 0: context_id = 1 (context 0 is M-mode)
    // Threshold/claim registers at: plic_base + 0x200000 + context_id * 0x1000
    VirtAddr ctx_base = plic_base + 0x200000 + 0x1000;

    (void)gic->initialize(plic_base, ctx_base, 0);
    moss::boot::g_gic_controller = gic;
    moss::boot::g_gic_hardware_available = true;
    moss::boot::early_print("  PLIC initialized (S-mode context 1)\n");
  }

  // 3. Enable S-mode external interrupt enable (SEIE = bit 9 in sie)
  asm volatile("csrs sie, %0" ::"r"(1ULL << 9));

  moss::boot::early_print("RISC-V interrupt/exception setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCVBootImpl::setup_smp_support(BootContext &ctx) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::SmpSupport);

  moss::boot::early_print("=== RISC-V SMP Support Setup ===\n");

  // Preserve DTB-derived CPU count; default to 1 if not set.
  if (ctx.total_cpus == 0) {
    ctx.total_cpus = 1;
  }

  // Use SBI HSM extension to start secondary harts.
  // Each hart enters _start which parks non-zero harts in WFI.
  // A future secondary_cpu_entry trampoline (like ARM64) is needed
  // for full SMP operation; for now we attempt the HSM call to
  // validate the SBI interface.
  if (ctx.total_cpus > 1) {
    u64 entry = reinterpret_cast<u64>(&moss::abi::_start);
    u32 started = 0;
    for (u32 i = 1; i < ctx.total_cpus; ++i) {
      auto result = moss::boot::sbi_hart_start(i, entry, 0);
      if (result.error == 0) {
        ++started;
      }
    }
    moss::boot::early_print("  SBI HSM: started ");
    moss::boot::early_print_hex(started);
    moss::boot::early_print(" secondary harts\n");
  }

  moss::boot::early_print("RISC-V SMP setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCVBootImpl::finalize_arch_init(BootContext &ctx) noexcept {
  (void)ctx;
  moss::boot::update_boot_stage(moss::boot::BootStage::ArchFinalize);

  moss::boot::early_print("=== RISC-V Architecture Init Complete ===\n");

  // Mark runtime heap as ready so operator new uses RuntimeHeapAllocator
  moss::abi::entry::mark_runtime_heap_ready();
  moss::boot::early_print("Runtime heap marked ready\n");

  moss::boot::early_print("RISC-V architecture-specific init all complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCVBootImpl::detect_memory_layout(BootContext &ctx) noexcept {
  (void)ctx;
  return ::moss::kernel::VoidResult{};
}

u32 moss::boot::RISCVBootImpl::get_current_cpu_id() noexcept { return moss::boot::get_current_cpu_id_impl(); }

[[noreturn]] void moss::boot::RISCVBootImpl::arch_panic(const char *message) noexcept {
  moss::boot::early_print("\n=== RISC-V PANIC ===\n");
  moss::boot::early_print(message);
  moss::boot::early_print("\n===================\n");

  asm volatile("li a7, 0x08\n"
               "li a6, 0x00\n"
               "ecall\n"
               :
               :
               : "a6", "a7");

  while (true) {
    asm volatile("wfi");
  }
}

// === Boot global variables (RISC-V stubs) ===
namespace moss::boot {

moss::kernel::interrupts::GenericInterruptController *g_gic_controller = nullptr;
bool g_gic_hardware_available = false;

void activate_secondary_cpus() noexcept { early_print("[RISC-V] SMP activation not yet implemented\n"); }

u32 wait_for_all_cpus_active([[maybe_unused]] u32 timeout_ms) noexcept { return 1; }

} // namespace moss::boot
