/*
 * Unified boot entry file - module implementation unit
 * Provides unified boot flow control for all architectures
 */

module;

// Architecture detection
#include "arch_detect.h"

#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
#define MOSS_CURRENT_ARCH "ARM64"
#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X86_64)
#define MOSS_CURRENT_ARCH "x86_64"
#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV)
#define MOSS_CURRENT_ARCH "RISC-V"
#endif

// extern "C" declarations (global module fragment)
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
extern "C" {
[[noreturn]] void early_main(void *device_tree_ptr);
}
#endif

extern "C" {
void kernel_main(void) noexcept;
}

module moss.boot;

using moss::u32;
using moss::u64;
using moss::VirtAddr;

namespace moss::boot {

// Early boot print function (architecture-independent)
static void boot_print(const char *message) {
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
    // ARM64 uses UART
    static constexpr VirtAddr UART_BASE = ::moss::kernel::platform::uart_base();
    volatile u32 *uart_base = reinterpret_cast<volatile u32 *>(UART_BASE);
    const char *p = message;
    while (*p) {
        if (*p == '\n') {
            // Wait for FIFO not full
            while (uart_base[0x018 / 4] & (1 << 5)) {
            }
            uart_base[0x000 / 4] = '\r';
        }
        // Wait for FIFO not full
        while (uart_base[0x018 / 4] & (1 << 5)) {
        }
        uart_base[0x000 / 4] = *p++;
    }
#else
    (void)message;
#endif
}

/// Unified boot main function
/// All architectures go through this unified entry
extern "C" [[noreturn]] void unified_boot_main(void *device_tree_ptr) {
    boot_print("\n=== Moss Multi-arch Unified Boot System ===\n");
    boot_print("Target arch: ");
    boot_print(MOSS_CURRENT_ARCH);
    boot_print("\n");

    // Initialize boot context
    BootContext ctx{
        .device_tree_ptr = device_tree_ptr,
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

    // Stage 4: SMP support (temporarily skipped, single-core first)
    boot_print("Stage 4: SMP support setup (temporarily skipped)\n");
    ctx.total_cpus = 1;
    boot_print("Single-core mode: 1 CPU\n");

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
    boot_print("Launching MOSS kernel main...\n");

    // Mark boot complete
    update_boot_stage(BootStage::Complete);

    // Call kernel main
    kernel_main();

    // If kernel_main returns, something is wrong
    ArchBoot::arch_panic("Kernel main returned unexpectedly");
}

} // namespace moss::boot

// C entry point, called by architecture assembly code
extern "C" [[noreturn]] void early_main(void *device_tree_ptr) {
    // Directly call unified boot main
    moss::boot::unified_boot_main(device_tree_ptr);
}
