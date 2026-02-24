/*
 * Unified boot entry file - module implementation unit
 * Provides unified boot flow control for all architectures
 */

module;

#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
#define MOSS_CURRENT_ARCH "ARM64"
#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X86_64)
#define MOSS_CURRENT_ARCH "x86_64"
#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV)
#define MOSS_CURRENT_ARCH "RISC-V"
#endif

module moss.boot;

import moss.abi;

using moss::u32;
using moss::u64;
using moss::VirtAddr;

namespace moss::boot {

// Early boot print function (architecture-independent)
// Uses direct hardware access — no heap, no modules, safe before runtime init.
//
// ARM64:  PL011 UART (MMIO) with SMP-safe spinlock
// x86_64: COM1 serial port (I/O 0x3F8)
// RISC-V: NS16550 UART (MMIO 0x10000000)
static void boot_print(const char *message) {
  if (!message) {
    return;
  }

#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
  // Acquire moss::abi::arm64::early_uart_lock (test-and-set spinlock via LDXR/STXR)
  {
    unsigned int val, status;
    asm volatile("1:\n"
                 "   ldxr  %w0, [%2]\n"
                 "   cbnz  %w0, 1b\n"
                 "   mov   %w0, #1\n"
                 "   stxr  %w1, %w0, [%2]\n"
                 "   cbnz  %w1, 1b\n"
                 "   dmb   sy\n"
                 : "=&r"(val), "=&r"(status)
                 : "r"(&moss::abi::arm64::early_uart_lock)
                 : "memory");
  }

  static constexpr VirtAddr UART_BASE = ::moss::kernel::platform::uart_base();
  volatile u32 *uart_base = reinterpret_cast<volatile u32 *>(UART_BASE);
  const char *p = message;
  while (*p) {
    if (*p == '\n') {
      while (uart_base[0x018 / 4] & (1 << 5)) {
      }
      uart_base[0x000 / 4] = '\r';
    }
    while (uart_base[0x018 / 4] & (1 << 5)) {
    }
    uart_base[0x000 / 4] = *p++;
  }

  asm volatile("dmb sy" ::: "memory");
  moss::abi::arm64::early_uart_lock = 0;

#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X86_64)
  // COM1 serial port: data at 0x3F8, Line Status Register at 0x3FD
  const char *p = message;
  while (*p) {
    if (*p == '\n') {
      // Wait for THR empty (bit 5 of LSR)
      u8 lsr;
      do {
        asm volatile("inb %1, %0" : "=a"(lsr) : "Nd"(static_cast<u16>(0x3FD)));
      } while (!(lsr & 0x20));
      asm volatile("outb %0, %1" ::"a"(static_cast<u8>('\r')), "Nd"(static_cast<u16>(0x3F8)));
    }
    u8 lsr;
    do {
      asm volatile("inb %1, %0" : "=a"(lsr) : "Nd"(static_cast<u16>(0x3FD)));
    } while (!(lsr & 0x20));
    asm volatile("outb %0, %1" ::"a"(static_cast<u8>(*p)), "Nd"(static_cast<u16>(0x3F8)));
    ++p;
  }

#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV)
  // NS16550 UART: data register at MMIO base 0x10000000
  static constexpr VirtAddr UART_BASE = ::moss::kernel::platform::uart_base();
  volatile u32 *uart_data = reinterpret_cast<volatile u32 *>(UART_BASE);
  const char *p = message;
  while (*p) {
    if (*p == '\n') {
      *uart_data = static_cast<u32>('\r');
    }
    *uart_data = static_cast<u32>(static_cast<unsigned char>(*p));
    ++p;
  }
#endif
}

/// Unified boot main function
/// All architectures go through this unified entry
[[noreturn]] void unified_boot_main(void *device_tree_ptr) {
  boot_print("\n=== Moss Multi-arch Unified Boot System ===\n");
  boot_print("Target arch: ");
  boot_print(MOSS_CURRENT_ARCH);
  boot_print("\n");

  // Initialize boot context
  BootContext ctx{.device_tree_ptr = device_tree_ptr,
                  .memory_start = 0,
                  .memory_size = 0,
                  .cpu_id = 0,
                  .total_cpus = 1,
                  .kernel_phys_base = 0,
                  .kernel_virt_base = 0};

  boot_print("Boot context initialized\n\n");

  // Execute architecture-specific standardized boot sequence
  boot_print("Starting standardized boot sequence...\n");

  // Stage 1: Hardware early init
  boot_print("Stage 1: Hardware early init\n");
  auto hw_result = ArchBoot::hardware_early_init(ctx);
  if (!hw_result) {
    boot_print("Error: Hardware init failed\n");
    ArchBoot::arch_panic("Hardware initialization failed");
  }

  // Stage 2: Memory management setup
  boot_print("Stage 2: Memory management setup\n");
  auto mem_result = ArchBoot::setup_memory_management(ctx);
  if (!mem_result) {
    boot_print("Error: Memory management setup failed\n");
    ArchBoot::arch_panic("Memory management setup failed");
  }

  // Stage 3: Interrupts and exceptions setup
  boot_print("Stage 3: Interrupts and exceptions setup\n");
  auto int_result = ArchBoot::setup_interrupts_and_exceptions(ctx);
  if (!int_result) {
    boot_print("Error: Interrupt/exception setup failed\n");
    ArchBoot::arch_panic("Interrupt/exception setup failed");
  }

  // Stage 4: SMP support — boot secondary CPUs via PSCI
  boot_print("Stage 4: SMP support setup\n");
  auto smp_result = ArchBoot::setup_smp_support(ctx);
  if (!smp_result) {
    boot_print("Warning: SMP setup failed, falling back to single-core\n");
    ctx.total_cpus = 1;
  }
  boot_print("Stage 4: SMP support setup complete\n");

  // Stage 5: Architecture finalization
  boot_print("Stage 5: Architecture init complete\n");

  auto finalize_result = ArchBoot::finalize_arch_init(ctx);
  if (!finalize_result) {
    boot_print("Error: Architecture finalization failed\n");
    ArchBoot::arch_panic("Architecture finalization failed");
  }

  // Update boot status
  update_boot_stage(BootStage::SystemInit);

  boot_print("=== Architecture-specific boot complete ===\n");
  boot_print("Handing off to architecture-independent system init...\n\n");

  // Hand off to architecture-independent system init
  // Run C++ global constructors (.init_array) before kernel_main.
  // In freestanding environments there is no CRT to do this automatically.
  // Run C++ global constructors (.init_array) before kernel_main.
  // In freestanding environments there is no CRT to do this automatically.
  moss::abi::linker::call_global_constructors();

  boot_print("Launching MOSS kernel main...\n");

  // Mark boot complete
  update_boot_stage(BootStage::Complete);

  // Call kernel main (never returns)
  moss::abi::entry::kernel_main();
}

} // namespace moss::boot

// C entry point, called by architecture assembly code
extern "C" [[noreturn]] void early_main(void *device_tree_ptr) {
  // Directly call unified boot main
  moss::boot::unified_boot_main(device_tree_ptr);
}
