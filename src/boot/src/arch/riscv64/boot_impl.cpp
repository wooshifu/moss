/*
 * RISC-V 64 architecture-specific boot implementation - module implementation unit
 * Implements the unified boot interface for RISC-V 64
 */

module;

// Architecture detection (global module fragment)
#ifndef MOSS_ARCH_RISCV64
#define MOSS_ARCH_RISCV64
#endif

extern "C" void riscv64_secondary_start();
extern "C" [[noreturn]] void riscv64_secondary_entry() noexcept;

module moss.boot;

import moss.abi;
import moss.hal.uart;

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

static void early_print(const char *str) { moss::kernel::hal::uart::puts(str); }

static void early_print_hex(u64 value) {
  constexpr char hex_chars[] = "0123456789ABCDEF";
  char buffer[19] = "0x"; // 2 prefix bytes + 16 u64 hex digits + NUL.

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
  return moss::kernel::platform::logical_cpu(hartid);
}

// Boot stage status update
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
  return {.error = static_cast<long>(r_a0), .value = static_cast<long>(r_a1)};
}

// SBI HSM (Hart State Management) extension — EID 0x48534D
static constexpr u64 SBI_EID_HSM = 0x48534D;

static SbiResult sbi_hart_start(u64 hartid, u64 start_addr, u64 opaque) noexcept {
  // HSM FID 0 is hart_start; opaque arrives in secondary a1 (the context pointer).
  return sbi_call(SBI_EID_HSM, 0, hartid, start_addr, opaque);
}

} // namespace moss::boot

// RISCV64BootImpl member function implementations
::moss::kernel::VoidResult moss::boot::RISCV64BootImpl::hardware_early_init(BootContext &ctx) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::HardwareInit);

  moss::boot::early_print("=== RISC-V 64 Hardware Early Init ===\n");

  u64 boot_hart;
  asm volatile("mv %0, tp" : "=r"(boot_hart));
  ctx.cpu_id = moss::boot::get_current_cpu_id_impl();
  moss::boot::early_print("CPU ID (Hart ID): ");
  moss::boot::early_print_hex(ctx.cpu_id);
  moss::boot::early_print("\n");

  // M-mode CSRs (mvendorid, marchid, mimpid) are not accessible from S-mode.
  // Hardware identification requires SBI probe calls or DTB parsing.

  if (!moss::fdt::parse_dtb(ctx.device_tree_ptr)) {
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::InvalidArgument};
  }
  const auto &info = moss::fdt::get_platform_info();
  if (!info.memory_map_valid || !info.intc.valid || !info.cpu_count || info.cpu_count > 16 ||
      !info.timebase_frequency) {
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::InvalidArgument};
  }
  if (!moss::kernel::platform::order_cpus(boot_hart)) {
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::InvalidArgument};
  }
  moss::kernel::platform::hardware.intc.cpu_base = moss::kernel::hal::intc::plic_context_base(0);
  ctx.memory_start = info.total_memory_start;
  ctx.memory_size = info.total_memory_size;
  ctx.kernel_phys_base = reinterpret_cast<PhysAddr>(moss::abi::_start);
  ctx.total_cpus = info.cpu_count;

  ctx.kernel_virt_base = moss::boot::arch_constants::KERNEL_VIRT_BASE;

  moss::boot::early_print("Memory range: ");
  moss::boot::early_print_hex(ctx.memory_start);
  moss::boot::early_print(" - ");
  moss::boot::early_print_hex(ctx.memory_start + ctx.memory_size);
  moss::boot::early_print("\n");

  moss::boot::early_print("RISC-V 64 hardware init complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCV64BootImpl::setup_memory_management(BootContext &ctx) noexcept {
  (void)ctx;
  moss::boot::update_boot_stage(moss::boot::BootStage::MemoryManagement);

  moss::boot::early_print("=== RISC-V 64 Memory Management Setup ===\n");

  // Phase 0: Detect page table mode from DTB mmu-type property.
  // A satp probe needs valid identity-mapped tables because the write changes
  // translation immediately. Firmware mmu-type avoids activating a PPN=0
  // table while executing early boot; this kernel selects only Sv39 or Sv48.
  namespace mmu_hal = ::moss::kernel::hal::mmu;

  {
    // RV64 satp.MODE[63:60] encodes Sv39=8 and Sv48=9; these are hardware
    // encodings, while firmware mmu_levels counts 4 KiB table levels.
    const auto &info = moss::fdt::get_platform_info();
    if (info.mmu_levels >= 4) {
      mmu_hal::g_mmu_mode = mmu_hal::MmuMode::Sv48;
      mmu_hal::g_satp_mode_bits = 9ULL << 60;
    } else {
      mmu_hal::g_mmu_mode = mmu_hal::MmuMode::Sv39;
      mmu_hal::g_satp_mode_bits = 8ULL << 60;
    }
  }
  mmu_hal::init_riscv64_address_layout(mmu_hal::g_mmu_mode);
  // Also update STACK_TOP for the detected mode
  if (mmu_hal::g_mmu_mode == mmu_hal::MmuMode::Sv48) {
    // Match the process module's Sv48 stack policy: 4 GiB below the exclusive
    // user ceiling 2^47, keeping the stack within the lower canonical half.
    ::moss::kernel::process::user_layout::STACK_TOP = 0x00007FFF00000000ULL;
    moss::boot::early_print("  MMU mode: Sv48 (4-level page table)\n");
  } else {
    moss::boot::early_print("  MMU mode: Sv39 (3-level page table)\n");
  }

  // Phase 1: Setup identity-mapped page tables + enable MMU
  // setup_mmu() builds permission-separated blocks/pages, then enable_mmu()
  // writes satp with the detected mode.
  // RISC-V 64 has a single satp register, so identity map and high-half share the root.
  moss::boot::early_print("  Phase 1: page tables + MMU enable\n");
  auto mmu_result = ::moss::kernel::mm::setup_mmu();
  if (!mmu_result) {
    moss::boot::early_print("  MMU setup failed\n");
    return ::moss::kernel::VoidResult{mmu_result.error()};
  }
  moss::boot::early_print("  MMU enabled\n");

  // Phase 2: Initialize PageFrameAllocator (buddy allocator)
  moss::boot::early_print("  Phase 2: PageFrameAllocator\n");
  auto pfa_result = ::moss::kernel::mm::PageFrameAllocator::initialize();
  if (!pfa_result) {
    moss::boot::early_print("  PageFrameAllocator init failed\n");
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::OutOfMemory};
  }

  // Phase 3: Build high-half kernel page table
  // Maps physical 0-4GB at KERNEL_DIRECT_MAP_BASE.
  // Sv39: KERNEL_DIRECT_MAP_BASE = 0xFFFFFFC000000000 (uses root[256..259])
  // Sv48: KERNEL_DIRECT_MAP_BASE = 0xFFFF800000000000 (adds PGD→PUD, like ARM64)
  moss::boot::early_print("  Phase 3: high-half kernel mapping\n");
  auto high_result = ::moss::kernel::mm::PageTableManager::setup_kernel_high_half_tables();
  if (!high_result) {
    moss::boot::early_print("  High-half page table setup failed\n");
    return ::moss::kernel::VoidResult{high_result.error()};
  }

  // Phase 4: Flush TLB to pick up new high-half mappings
  // RISC-V 64 satp already points to the PGD containing both identity + high-half.
  asm volatile("sfence.vma" ::: "memory");
  moss::boot::early_print("  Phase 4: TLB flushed (high-half active)\n");

  // Phase 5: Enable dynamic page table allocation (buddy-backed)
  ::moss::kernel::mm::PageTableManager::enable_dynamic_alloc();
  moss::boot::early_print("  Phase 5: dynamic page table alloc enabled\n");

  // Phase 6: Initialize runtime heap
  VirtAddr heap_start = moss::abi::linker::heap_start();
  // 256 KiB is the initial allocator span within the linker's 8 MiB reserve;
  // its exact workload-sizing rationale is not recorded.
  ::moss::kernel::usize initial_heap_size = 256ULL * 1024;
  auto heap_result = ::moss::kernel::mm::RuntimeHeapAllocator::initialize_heap(heap_start, initial_heap_size);
  if (!heap_result) {
    moss::boot::early_print("  RuntimeHeapAllocator init failed\n");
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::OutOfMemory};
  }

  moss::boot::early_print("RISC-V 64 memory management setup complete (high-half active)\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCV64BootImpl::setup_interrupts_and_exceptions(BootContext &ctx) noexcept {
  (void)ctx;
  moss::boot::update_boot_stage(moss::boot::BootStage::InterruptsExceptions);

  moss::boot::early_print("=== RISC-V 64 Interrupts and Exceptions Setup ===\n");

  // 1. Set stvec to point to the trap handler (direct mode)
  u64 trap_addr = reinterpret_cast<u64>(&moss::abi::syscall_entry_point);
  asm volatile("csrw stvec, %0" ::"r"(trap_addr));
  moss::boot::early_print("  stvec configured\n");

  // 2. Initialize PLIC (Platform Level Interrupt Controller)
  auto *gic = new moss::kernel::interrupts::GenericInterruptController();
  if (!gic) {
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::OutOfMemory};
  }
  {
    VirtAddr plic_base = moss::kernel::platform::intc_dist_base();

    VirtAddr ctx_base = moss::kernel::hal::intc::plic_context_base(0);

    auto result = gic->initialize(plic_base, ctx_base, 0);
    if (!result) {
      delete gic;
      return result;
    }
    moss::boot::g_gic_controller = gic;
    moss::boot::g_gic_hardware_available = true;
    moss::boot::early_print("  PLIC initialized (S-mode context 1)\n");
  }

  // 3. Enable S-mode external interrupt enable (SEIE = bit 9 in sie)
  asm volatile("csrs sie, %0" ::"r"(1ULL << 9));

  moss::boot::early_print("RISC-V 64 interrupt/exception setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCV64BootImpl::setup_smp_support(BootContext &ctx) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::SmpSupport);

  moss::boot::early_print("=== RISC-V 64 SMP Support Setup ===\n");

  // Preserve DTB-derived CPU count; default to 1 if not set.
  if (ctx.total_cpus == 0) {
    ctx.total_cpus = 1;
  }

  if (ctx.total_cpus > moss::kernel::BOOT_MAX_CPUS) {
    return ::moss::kernel::VoidResult{::moss::kernel::ErrorCode::InvalidArgument};
  }
  moss::kernel::g_num_cpus = ctx.total_cpus;

  moss::boot::early_print("RISC-V 64 SMP setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCV64BootImpl::finalize_arch_init(BootContext &ctx) noexcept {
  (void)ctx;
  moss::boot::update_boot_stage(moss::boot::BootStage::ArchFinalize);

  moss::boot::early_print("=== RISC-V 64 Architecture Init Complete ===\n");

  // Mark runtime heap as ready so operator new uses RuntimeHeapAllocator
  moss::abi::entry::mark_runtime_heap_ready();
  moss::boot::early_print("Runtime heap marked ready\n");

  moss::boot::early_print("RISC-V 64 architecture-specific init all complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCV64BootImpl::detect_memory_layout(BootContext &ctx) noexcept {
  (void)ctx;
  return ::moss::kernel::VoidResult{};
}

u32 moss::boot::RISCV64BootImpl::get_current_cpu_id() noexcept { return moss::boot::get_current_cpu_id_impl(); }

[[noreturn]] void moss::boot::RISCV64BootImpl::arch_panic(const char *message) noexcept {
  moss::boot::early_print("\n=== RISC-V 64 PANIC ===\n");
  moss::boot::early_print(message);
  moss::boot::early_print("\n===================\n");

  // Legacy SBI v0.1 EID 8 is shutdown; it is not the modern SRST extension.
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

// === Boot global variables (RISC-V 64 stubs) ===
namespace moss::boot {

moss::kernel::interrupts::GenericInterruptController *g_gic_controller = nullptr;
bool g_gic_hardware_available = false;

// Sixteen CPUs match BOOT_MAX_CPUS; 32 KiB stacks match the other boot paths.
// Page alignment permits page-table protection; the exact stack budget is a
// software policy without recorded maximum-depth measurement.
alignas(4096) static u8 secondary_stacks[16][32768];
struct HartStartContext {
  u64 stack;
  u64 satp;
};
static HartStartContext hart_contexts[16];

void activate_secondary_cpus() noexcept {
  u64 satp;
  asm volatile("csrr %0, satp" : "=r"(satp));
  for (u32 cpu = 1; cpu < moss::kernel::g_num_cpus; ++cpu) {
    hart_contexts[cpu] = {.stack = reinterpret_cast<u64>(&secondary_stacks[cpu][32768]), .satp = satp};
    moss::kernel::arch::memory_barrier();
    auto result =
        sbi_hart_start(moss::kernel::arch::riscv64_hart_id(cpu), reinterpret_cast<u64>(&riscv64_secondary_start),
                       reinterpret_cast<u64>(&hart_contexts[cpu]));
    if (result.error != 0) {
      early_print("SBI HSM start failed\n");
    }
  }
}

} // namespace moss::boot

extern "C" [[noreturn]] void riscv64_secondary_entry() noexcept {
  u64 trap = reinterpret_cast<u64>(&moss::abi::syscall_entry_point);
  asm volatile("csrw stvec, %0; csrw sscratch, zero" ::"r"(trap) : "memory");
  // Enable supervisor software IPIs and timer interrupts on this hart.
  asm volatile("csrs sie, %0" ::"r"((1ULL << 1) | (1ULL << 5)));
  // 100000 is time-counter ticks, not ns; delay=100000/timebase_frequency s.
  // This initial scheduling compare has no recorded tuning rationale.
  moss::kernel::hal::timer::set_compare(moss::kernel::hal::timer::read_counter() + 100000);
  moss::boot::record_cpu_online();
  moss::kernel::process::secondary_cpu_schedule_loop(moss::kernel::arch::get_current_cpu_id());
}
