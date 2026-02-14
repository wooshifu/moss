// MOSS Boot Module - Unified multi-architecture boot system
// Merges boot.hpp, boot_interface.hpp, arch_selector.hpp into a single C++26 module.

module;

// Architecture detection
#include "arch_detect.h"

// extern "C" declarations for assembly-callable functions (global module fragment)
extern "C" {
void early_debug_print(const char *message) noexcept;
[[noreturn]] void early_main(void *device_tree_ptr);
void mark_runtime_heap_ready() noexcept;
}

export module moss.boot;

import moss.std;
import moss.types;
import moss.result;
import moss.fdt;
import moss.platform;
import moss.arch;
import moss.mm;
import moss.interrupts;
import moss.hal.intc;
import moss.hal.timer;
import moss.timer;
import moss.process;

// ============================================================================
// boot_interface.hpp content - Unified boot interface
// ============================================================================

export namespace moss::boot {

/// Boot context structure
/// Contains key information passed between boot stages
struct BootContext {
    void *device_tree_ptr;                  // Device tree pointer (ARM64) or boot info
    PhysAddr memory_start;                  // Available physical memory start
    moss::kernel::usize memory_size;        // Available physical memory size
    u32 cpu_id;                             // Current CPU ID
    u32 total_cpus;                         // Total CPU cores in system
    PhysAddr kernel_phys_base;              // Kernel physical base address
    VirtAddr kernel_virt_base;              // Kernel virtual base address
};

/// Architecture-specific boot interface abstract base class
class ArchBootInterface {
public:
    static moss::kernel::VoidResult hardware_early_init(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult setup_memory_management(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult setup_interrupts_and_exceptions(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult setup_smp_support(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult finalize_arch_init(BootContext &ctx) noexcept;

protected:
    static moss::kernel::VoidResult detect_memory_layout(BootContext &ctx) noexcept;
    static u32 get_current_cpu_id() noexcept;
    [[noreturn]] static void arch_panic(const char *message) noexcept;
};

/// Boot stage enumeration
enum class BootStage : u32 {
    PreInit = 0,
    HardwareInit = 1,
    MemoryManagement = 2,
    InterruptsExceptions = 3,
    SmpSupport = 4,
    ArchFinalize = 5,
    SystemInit = 6,
    Complete = 7
};

/// Boot status structure
struct BootStatus {
    BootStage current_stage;
    u32 completed_stages_mask;
    u64 stage_timestamps[8];
    moss::kernel::ErrorCode last_error;
};

// Global boot status (defined in arch-specific implementation)
extern BootStatus g_boot_status;

/// Get current boot status
inline const BootStatus &get_boot_status() noexcept {
    return g_boot_status;
}

/// Update boot stage status
void update_boot_stage(BootStage stage,
                       moss::kernel::ErrorCode error = moss::kernel::ErrorCode::Success) noexcept;

/// Linux-style SMP delayed activation
void activate_secondary_cpus() noexcept;

/// Wait for all CPUs to become active
u32 wait_for_all_cpus_active(u32 timeout_ms = 5000) noexcept;

} // namespace moss::boot

// ============================================================================
// arch_selector.hpp content - Compile-time architecture selection
// ============================================================================

export namespace moss::boot {

// Architecture implementation classes and selection
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)

class ARM64BootImpl : public ArchBootInterface {
public:
    static moss::kernel::VoidResult hardware_early_init(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult setup_memory_management(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult setup_interrupts_and_exceptions(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult setup_smp_support(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult finalize_arch_init(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult detect_memory_layout(BootContext &ctx) noexcept;
    static u32 get_current_cpu_id() noexcept;
    [[noreturn]] static void arch_panic(const char *message) noexcept;
};
using ArchBoot = ARM64BootImpl;

#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X86_64)

class X86_64BootImpl : public ArchBootInterface {
public:
    static moss::kernel::VoidResult hardware_early_init(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult setup_memory_management(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult setup_interrupts_and_exceptions(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult setup_smp_support(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult finalize_arch_init(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult detect_memory_layout(BootContext &ctx) noexcept;
    static u32 get_current_cpu_id() noexcept;
    [[noreturn]] static void arch_panic(const char *message) noexcept;
};
using ArchBoot = X86_64BootImpl;

#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV)

class RISCVBootImpl : public ArchBootInterface {
public:
    static moss::kernel::VoidResult hardware_early_init(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult setup_memory_management(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult setup_interrupts_and_exceptions(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult setup_smp_support(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult finalize_arch_init(BootContext &ctx) noexcept;
    static moss::kernel::VoidResult detect_memory_layout(BootContext &ctx) noexcept;
    static u32 get_current_cpu_id() noexcept;
    [[noreturn]] static void arch_panic(const char *message) noexcept;
};
using ArchBoot = RISCVBootImpl;

#else
#error "Unsupported target architecture"
#endif

/// Compile-time architecture info
struct ArchInfo {
    const char *name;
    u32 id;
    const char *description;
};

/// Get current architecture info
constexpr ArchInfo get_current_arch_info() noexcept {
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
    return ArchInfo{
        .name = "ARM64",
        .id = 1,
        .description = "ARM 64-bit (AArch64) Architecture"};
#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X86_64)
    return ArchInfo{
        .name = "x86_64",
        .id = 2,
        .description = "x86-64 (AMD64) Architecture"};
#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV)
    return ArchInfo{
        .name = "RISC-V",
        .id = 3,
        .description = "RISC-V 64-bit Architecture"};
#endif
}

/// Architecture-specific constants
namespace arch_constants {
constexpr u32 PAGE_SIZE = 4096;
constexpr u32 CACHE_LINE_SIZE = 64;
constexpr u32 STACK_ALIGNMENT = 16;
constexpr VirtAddr KERNEL_VIRT_BASE = ::moss::kernel::platform::kernel_virt_base();
} // namespace arch_constants

} // namespace moss::boot

// ============================================================================
// boot.hpp content - Global hardware instances
// ============================================================================

export namespace moss::boot {

/// Global GIC controller instance (initialized during boot, ARM64 only)
extern moss::kernel::interrupts::GenericInterruptController *g_gic_controller;

/// GIC hardware availability flag
extern bool g_gic_hardware_available;

} // namespace moss::boot
