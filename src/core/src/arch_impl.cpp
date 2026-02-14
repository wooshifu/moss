// src/modules/arch_impl.cpp
module moss.arch;

#if defined(MOSS_ARCH_ARM64)
import moss.arch.arm64;
#endif

namespace moss::kernel::arch {

// Implement interface functions by delegating to architecture-specific code
void memory_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::memory_barrier();
#endif
}

void read_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::read_barrier();
#endif
}

void write_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::write_barrier();
#endif
}

void cpu_yield() noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::cpu_yield();
#endif
}

void cpu_halt() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("wfi");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("hlt");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("wfi");
#endif
}

void instruction_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("isb" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence.i" ::: "memory");
#endif
}

void flush_cache_line(VirtAddr addr) noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dc civac, %0" :: "r"(addr) : "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("clflush (%0)" :: "r"(addr) : "memory");
#elif defined(MOSS_ARCH_RISCV)
  (void)addr; // RISC-V cache flush is implementation-specific
#endif
}

u32 get_current_cpu_id() noexcept {
#if defined(MOSS_ARCH_ARM64)
  return arm64::get_current_cpu_id();
#else
  return 0;
#endif
}

u64 get_timestamp_counter() noexcept {
#if defined(MOSS_ARCH_ARM64)
  return arm64::get_timestamp_counter();
#else
  return 0;
#endif
}

void setup_kernel_mmu(PhysAddr kernel_pgd_pa) noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::setup_kernel_mmu(kernel_pgd_pa);
#else
  (void)kernel_pgd_pa;
#endif
}

void flush_tlb() noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::flush_tlb();
#endif
}

void flush_tlb_addr(VirtAddr addr) noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::flush_tlb_addr(addr);
#else
  (void)addr;
#endif
}

[[noreturn]] void kernel_panic(const char *message) noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::kernel_panic(message);
#else
  // Fallback infinite loop for unsupported architectures
  (void)message; // Suppress unused parameter warning
  while (true) {
  }
#endif
}

void switch_to_kernel_stack(void *stack_ptr) noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::switch_to_kernel_stack(stack_ptr);
#else
  (void)stack_ptr;
#endif
}

void *get_current_stack_pointer() noexcept {
#if defined(MOSS_ARCH_ARM64)
  return arm64::get_current_stack_pointer();
#else
  return nullptr;
#endif
}

void enable_interrupts() noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::enable_interrupts();
#endif
}

void disable_interrupts() noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::disable_interrupts();
#endif
}

bool interrupts_enabled() noexcept {
#if defined(MOSS_ARCH_ARM64)
  return arm64::interrupts_enabled();
#else
  return false;
#endif
}

void invalidate_icache() noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::invalidate_icache();
#endif
}

void invalidate_dcache() noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::invalidate_dcache();
#endif
}

void clean_dcache() noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::clean_dcache();
#endif
}

void flush_dcache() noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::flush_dcache();
#endif
}

void early_arch_init() noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::early_arch_init();
#endif
}

void arch_init() noexcept {
#if defined(MOSS_ARCH_ARM64)
  arm64::arch_init();
#endif
}

} // namespace moss::kernel::arch
