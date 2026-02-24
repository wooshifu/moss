// MOSS Platform Support Package (PSP)
//
// Compile-time hardware defaults for each supported platform.
//
// Every kernel consumer that previously hardcoded addresses like 0x09000000
// (PL011 UART) or 0x08000000 (GIC distributor) should now use these
// constants as their fallback when the DTB/FDT runtime discovery is
// unavailable or invalid.
//
// Design:
//   - All constants are `inline constexpr` — zero runtime cost.
//   - One `PlatformDefaults` struct per platform, selected at compile time
//     via arch_detect.h macros.
//   - The DTB/FDT system (moss.fdt) provides *runtime* hardware discovery;
//     this module provides *compile-time* fallback defaults.
//   - Consumer pattern:
//       auto addr = (plat.dtb_valid && plat.uart.valid)
//                       ? plat.uart.base_addr
//                       : platform::defaults::UART_BASE;

export module moss.platform;

import moss.std;
import moss.types;

export namespace moss::kernel::platform {

using moss::u32;
using moss::u64;
using moss::kernel::PhysAddr;
using moss::kernel::VirtAddr;

// ============================================================================
// UART defaults (serial console)
// ============================================================================
struct UartDefaults {
  VirtAddr base;  // Data register address
  VirtAddr flags; // Status/flags register address (0 if N/A)
  u32 irq;        // Default IRQ number
  u32 clock_freq; // Default clock frequency in Hz
};

// ============================================================================
// Interrupt controller defaults
// ============================================================================
struct IntcDefaults {
  VirtAddr dist_base;   // GIC Distributor / PLIC / APIC base
  VirtAddr cpu_base;    // GICv2 CPU Interface (0 if N/A)
  VirtAddr redist_base; // GICv3 Redistributor (0 if N/A or GICv2)
};

// ============================================================================
// Memory layout defaults
// ============================================================================
struct MemoryDefaults {
  PhysAddr ram_base;    // Physical RAM start
  u64 ram_size;         // Default RAM size (bytes)
  VirtAddr kernel_virt; // Kernel virtual base address
};

// ============================================================================
// Timer defaults
// ============================================================================
struct TimerDefaults {
  u32 irq;       // Timer IRQ number (PPI for ARM64, etc.)
  u64 frequency; // Timer frequency in Hz (0 = read from hardware)
};

// ============================================================================
// Aggregate: all platform defaults in one struct
// ============================================================================
struct PlatformDefaults {
  const char *name;
  UartDefaults uart;
  IntcDefaults intc;
  MemoryDefaults memory;
  TimerDefaults timer;
};

// ============================================================================
// Platform definitions — one per target
// ============================================================================

#if defined(MOSS_ARCH_ARM64)
// QEMU virt machine for ARM64
// PL011 UART, GICv2/v3 (auto-detected), RAM at 1GB
inline constexpr PlatformDefaults DEFAULTS{
    .name = "QEMU ARM64 virt",
    .uart =
        {
            .base = 0x09000000,
            .flags = 0x09000018,    // PL011 FR (Flag Register)
            .irq = 33,              // SPI #1 (32 + 1)
            .clock_freq = 24000000, // 24 MHz
        },
    .intc =
        {
            .dist_base = 0x08000000,   // GICD (same for v2 and v3)
            .cpu_base = 0x08010000,    // GICC (GICv2 only)
            .redist_base = 0x080a0000, // GICR (GICv3 only, QEMU virt default)
        },
    .memory =
        {
            .ram_base = 0x40000000,                // 1 GB mark
            .ram_size = 1ULL * 1024 * 1024 * 1024, // 1 GB default
            .kernel_virt = 0xFFFF000000000000ULL,
        },
    .timer =
        {
            .irq = 27,      // PPI #11 (virtual timer, non-secure EL1)
            .frequency = 0, // Read from cntfrq_el0 at runtime
        },
};

#elif defined(MOSS_ARCH_X86_64)
// QEMU virt machine for x86_64
// COM1 serial (I/O port 0x3F8), no MMIO interrupt controller yet, RAM at 0
inline constexpr PlatformDefaults DEFAULTS{
    .name = "QEMU x86_64 virt",
    .uart =
        {
            .base = 0x3F8,         // COM1 data port
            .flags = 0x3FD,        // COM1 Line Status Register
            .irq = 4,              // COM1 IRQ
            .clock_freq = 1843200, // 1.8432 MHz
        },
    .intc =
        {
            .dist_base = 0xFEE00000, // Local APIC MMIO base
            .cpu_base = 0xFEC00000,  // I/O APIC MMIO base
            .redist_base = 0,        // N/A for x86_64
        },
    .memory =
        {
            .ram_base = 0x00100000,           // 1 MB (above real-mode area)
            .ram_size = 255ULL * 1024 * 1024, // 255 MB (256 MB total minus 1 MB base)
            .kernel_virt = 0xFFFF800000000000ULL,
        },
    .timer =
        {
            .irq = 0,       // Local APIC timer (vector, not IRQ line)
            .frequency = 0, // Calibrate at runtime
        },
};

#elif defined(MOSS_ARCH_RISCV)
// QEMU virt machine for RISC-V 64
// NS16550A UART, PLIC, RAM at 0x80000000
inline constexpr PlatformDefaults DEFAULTS{
    .name = "QEMU RISC-V virt",
    .uart =
        {
            .base = 0x10000000,
            .flags = 0x10000005,   // NS16550 LSR
            .irq = 10,             // PLIC IRQ for UART0
            .clock_freq = 3686400, // 3.6864 MHz
        },
    .intc =
        {
            .dist_base = 0x0C000000, // PLIC base
            .cpu_base = 0x0C201000,  // PLIC S-mode hart 0 context (base + 0x200000 + 1*0x1000)
            .redist_base = 0,        // N/A for RISC-V
        },
    .memory =
        {
            .ram_base = 0x80000000,           // 2 GB mark
            .ram_size = 256ULL * 1024 * 1024, // 256 MB default
            .kernel_virt = 0xFFFFFFFF80000000ULL,
        },
    .timer =
        {
            .irq = 5,              // S-mode timer interrupt
            .frequency = 10000000, // 10 MHz (QEMU virt default from DTB)
        },
};
#endif

// ============================================================================
// Convenience accessors — shorthand for common lookups
// ============================================================================

/// Get default UART base address for the current platform
[[nodiscard]] constexpr VirtAddr uart_base() noexcept { return DEFAULTS.uart.base; }

/// Get default GIC/PLIC distributor base address
[[nodiscard]] constexpr VirtAddr intc_dist_base() noexcept { return DEFAULTS.intc.dist_base; }

/// Get default GIC CPU interface base address (0 on non-ARM or GICv3)
[[nodiscard]] constexpr VirtAddr intc_cpu_base() noexcept { return DEFAULTS.intc.cpu_base; }

/// Get default GICv3 Redistributor base address (0 on non-ARM or GICv2)
[[nodiscard]] constexpr VirtAddr intc_redist_base() noexcept { return DEFAULTS.intc.redist_base; }

/// Get default physical RAM start address
[[nodiscard]] constexpr PhysAddr ram_base() noexcept { return DEFAULTS.memory.ram_base; }

/// Get default RAM size in bytes
[[nodiscard]] constexpr u64 ram_size() noexcept { return DEFAULTS.memory.ram_size; }

/// Get kernel virtual base address
[[nodiscard]] constexpr VirtAddr kernel_virt_base() noexcept { return DEFAULTS.memory.kernel_virt; }

/// Get default timer IRQ number
[[nodiscard]] constexpr u32 timer_irq() noexcept { return DEFAULTS.timer.irq; }

/// Get default timer frequency (0 = discover at runtime)
[[nodiscard]] constexpr u64 timer_frequency() noexcept { return DEFAULTS.timer.frequency; }

// ============================================================================
// Helper: resolve DTB value with platform fallback
// ============================================================================

/// Return `dtb_value` if `valid` is true, otherwise the platform fallback.
/// This replaces the repetitive ternary pattern throughout the kernel:
///   (plat.dtb_valid && plat.xxx.valid) ? plat.xxx.addr : 0xHARDCODED
[[nodiscard]] constexpr u64 resolve(bool valid, u64 dtb_value, u64 fallback) noexcept {
  return valid ? dtb_value : fallback;
}

} // namespace moss::kernel::platform
