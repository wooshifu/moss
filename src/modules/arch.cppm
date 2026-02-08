// src/modules/arch.cppm
export module moss.arch;

import moss.std;
import moss.types;

export namespace moss::kernel::arch {

// Architecture identification
enum class Architecture {
    ARM64,
    X86_64,
    RISCV
};

#if defined(MOSS_ARCH_ARM64)
    constexpr Architecture CURRENT_ARCH = Architecture::ARM64;
#elif defined(MOSS_ARCH_X86_64)
    constexpr Architecture CURRENT_ARCH = Architecture::X86_64;
#elif defined(MOSS_ARCH_RISCV)
    constexpr Architecture CURRENT_ARCH = Architecture::RISCV;
#else
    #error "Unsupported architecture"
#endif

// Memory barrier operations - implemented per architecture
void memory_barrier() noexcept;
void read_barrier() noexcept;
void write_barrier() noexcept;

// CPU operations
void cpu_yield() noexcept;
u32 get_current_cpu_id() noexcept;
u64 get_timestamp_counter() noexcept;

// MMU management
void setup_kernel_mmu(PhysAddr kernel_pgd_pa) noexcept;
void flush_tlb() noexcept;
void flush_tlb_addr(VirtAddr addr) noexcept;

// Debug and panic
[[noreturn]] void kernel_panic(const char* message) noexcept;

// Context switching (platform specific)
void switch_to_kernel_stack(void* stack_ptr) noexcept;
void* get_current_stack_pointer() noexcept;

// Interrupt control
void enable_interrupts() noexcept;
void disable_interrupts() noexcept;
bool interrupts_enabled() noexcept;

// Cache operations
void invalidate_icache() noexcept;
void invalidate_dcache() noexcept;
void clean_dcache() noexcept;
void flush_dcache() noexcept;

// Architecture-specific initialization
void early_arch_init() noexcept;
void arch_init() noexcept;

} // namespace moss::kernel::arch