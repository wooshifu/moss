// src/modules/arch.cppm
export module moss.arch;

import moss.std;
import moss.types;

namespace moss::kernel::arch {

// Architecture identification
export enum class Architecture {
    ARM64,
    X86_64,
    RISCV
};

#if defined(MOSS_ARCH_ARM64)
    export constexpr Architecture CURRENT_ARCH = Architecture::ARM64;
#elif defined(MOSS_ARCH_X86_64)
    export constexpr Architecture CURRENT_ARCH = Architecture::X86_64;
#elif defined(MOSS_ARCH_RISCV)
    export constexpr Architecture CURRENT_ARCH = Architecture::RISCV;
#else
    #error "Unsupported architecture"
#endif

// Memory barrier operations - implemented per architecture
export void memory_barrier() noexcept;
export void read_barrier() noexcept;
export void write_barrier() noexcept;

// CPU operations
export void cpu_yield() noexcept;
export u32 get_current_cpu_id() noexcept;
export u64 get_timestamp_counter() noexcept;

// MMU management
export void setup_kernel_mmu(PhysAddr kernel_pgd_pa) noexcept;
export void flush_tlb() noexcept;
export void flush_tlb_addr(VirtAddr addr) noexcept;

// Debug and panic
export [[noreturn]] void kernel_panic(const char* message) noexcept;

// Context switching (platform specific)
export void switch_to_kernel_stack(void* stack_ptr) noexcept;
export void* get_current_stack_pointer() noexcept;

// Interrupt control
export void enable_interrupts() noexcept;
export void disable_interrupts() noexcept;
export bool interrupts_enabled() noexcept;

// Cache operations
export void invalidate_icache() noexcept;
export void invalidate_dcache() noexcept;
export void clean_dcache() noexcept;
export void flush_dcache() noexcept;

// Architecture-specific initialization
export void early_arch_init() noexcept;
export void arch_init() noexcept;

} // namespace moss::kernel::arch