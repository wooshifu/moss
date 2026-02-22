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

  volatile u32 *const uart_base;

public:
  EarlyUart() : uart_base(reinterpret_cast<volatile u32 *>(UART_BASE)) {}

  void put_char(char c) const { uart_base[UART_REG_TXDATA / 4] = static_cast<u32>(c); }

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
  u64 hartid;
  asm volatile("csrr %0, mhartid" : "=r"(hartid));
  return static_cast<u32>(hartid & 0xFFFFFFFF);
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

} // namespace moss::boot

// RISCVBootImpl member function implementations
::moss::kernel::VoidResult moss::boot::RISCVBootImpl::hardware_early_init(BootContext &ctx) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::HardwareInit);

  moss::boot::early_print("=== RISC-V Hardware Early Init ===\n");

  ctx.cpu_id = moss::boot::get_current_cpu_id_impl();
  moss::boot::early_print("CPU ID (Hart ID): ");
  moss::boot::early_print_hex(ctx.cpu_id);
  moss::boot::early_print("\n");

  u64 mvendorid, marchid, mimpid;
  asm volatile("csrr %0, mvendorid" : "=r"(mvendorid));
  asm volatile("csrr %0, marchid" : "=r"(marchid));
  asm volatile("csrr %0, mimpid" : "=r"(mimpid));

  moss::boot::early_print("Machine Vendor ID: ");
  moss::boot::early_print_hex(mvendorid);
  moss::boot::early_print("\n");

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
  moss::boot::early_print("TODO: Implement RISC-V page table and MMU setup\n");
  moss::boot::early_print("RISC-V memory management setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCVBootImpl::setup_interrupts_and_exceptions(BootContext &ctx) noexcept {
  (void)ctx;
  moss::boot::update_boot_stage(moss::boot::BootStage::InterruptsExceptions);

  moss::boot::early_print("=== RISC-V Interrupts and Exceptions Setup ===\n");
  moss::boot::early_print("TODO: Implement RISC-V interrupt controller init\n");
  moss::boot::early_print("RISC-V interrupt/exception setup complete\n\n");
  return ::moss::kernel::VoidResult{};
}

::moss::kernel::VoidResult moss::boot::RISCVBootImpl::setup_smp_support(BootContext &ctx) noexcept {
  moss::boot::update_boot_stage(moss::boot::BootStage::SmpSupport);

  moss::boot::early_print("=== RISC-V SMP Support Setup ===\n");
  // Preserve DTB-derived CPU count (set in hardware_early_init); only default to 1
  // if it wasn't set. Once RISC-V SMP boot is implemented, this fallback can be removed.
  if (ctx.total_cpus == 0) {
    ctx.total_cpus = 1;
  }
  moss::boot::early_print("TODO: Implement RISC-V multi-core boot support\n");
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
