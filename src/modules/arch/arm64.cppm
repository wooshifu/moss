// src/modules/arch/arm64.cppm
module;

// ARM64-specific headers and definitions
#if defined(MOSS_ARCH_ARM64)

export module moss.arch.arm64;

import moss.std;
import moss.types;

export namespace moss::kernel::arch::arm64 {

// Memory barriers
inline void memory_barrier() noexcept { asm volatile("dmb sy" ::: "memory"); }

inline void read_barrier() noexcept { asm volatile("dmb ld" ::: "memory"); }

inline void write_barrier() noexcept { asm volatile("dmb st" ::: "memory"); }

// CPU operations
inline void cpu_yield() noexcept { asm volatile("yield" ::: "memory"); }

inline u32 get_current_cpu_id() noexcept {
  u64 mpidr;
  asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  return static_cast<u32>(mpidr & 0xFF);
}

inline u64 get_timestamp_counter() noexcept {
  u64 val;
  asm volatile("mrs %0, cntvct_el0" : "=r"(val));
  return val;
}

// MMU management
inline void setup_kernel_mmu(PhysAddr kernel_pgd_pa) noexcept {
  // Set up TTBR1_EL1 with kernel page tables
  asm volatile("msr ttbr1_el1, %0" ::"r"(kernel_pgd_pa));
  asm volatile("isb");

  // Enable MMU with kernel page tables
  u64 sctlr;
  asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
  sctlr |= (1 << 0);  // Enable MMU
  sctlr |= (1 << 2);  // Enable data cache
  sctlr |= (1 << 12); // Enable instruction cache
  asm volatile("msr sctlr_el1, %0" ::"r"(sctlr));
  asm volatile("isb");
}

inline void flush_tlb() noexcept {
  asm volatile("tlbi vmalle1is" ::: "memory");
  asm volatile("dsb sy");
  asm volatile("isb");
}

inline void flush_tlb_addr(VirtAddr addr) noexcept {
  asm volatile("tlbi vae1is, %0" ::"r"(addr >> 12) : "memory");
  asm volatile("dsb sy");
  asm volatile("isb");
}

// Debug and panic
[[noreturn]] inline void kernel_panic(const char * /*message*/) noexcept {
  // Disable interrupts
  asm volatile("msr daifset, #0xf" ::: "memory");

  // Halt with breakpoint for debugger
  while (true) {
    asm volatile("brk #0");
    asm volatile("wfi");
  }
}

// Context switching
inline void switch_to_kernel_stack(void *stack_ptr) noexcept {
  asm volatile("mov sp, %0" ::"r"(stack_ptr) : "memory");
}

inline void *get_current_stack_pointer() noexcept {
  void *sp;
  asm volatile("mov %0, sp" : "=r"(sp));
  return sp;
}

// Interrupt control
inline void enable_interrupts() noexcept {
  asm volatile("msr daifclr, #0x2" ::: "memory");
}

inline void disable_interrupts() noexcept {
  asm volatile("msr daifset, #0x2" ::: "memory");
}

inline bool interrupts_enabled() noexcept {
  u64 daif;
  asm volatile("mrs %0, daif" : "=r"(daif));
  return (daif & (1 << 7)) == 0;
}

// Cache operations
inline void invalidate_icache() noexcept {
  asm volatile("ic iallu" ::: "memory");
  asm volatile("dsb sy");
  asm volatile("isb");
}

inline void invalidate_dcache() noexcept {
  // Implementation depends on cache levels - simplified version
  asm volatile("dc isw, xzr" ::: "memory");
  asm volatile("dsb sy");
}

inline void clean_dcache() noexcept {
  asm volatile("dc csw, xzr" ::: "memory");
  asm volatile("dsb sy");
}

inline void flush_dcache() noexcept {
  asm volatile("dc cisw, xzr" ::: "memory");
  asm volatile("dsb sy");
}

// Architecture initialization
inline void early_arch_init() noexcept {
  // Set up exception level and basic CPU state
  u64 currentel;
  asm volatile("mrs %0, currentel" : "=r"(currentel));
  currentel = (currentel >> 2) & 3;

  if (currentel == 3) {
    // Drop from EL3 to EL1
    u64 scr = (1 << 0) | // NS bit
              (1 << 8) | // HVC enable
              (1 << 10); // RW bit (AArch64)
    asm volatile("msr scr_el3, %0" ::"r"(scr));

    u64 spsr = (1 << 0) | // M[0] = 1 (EL1h)
               (1 << 2) | // M[2] = 1 (EL1)
               (1 << 6) | // F bit (FIQ masked)
               (1 << 7) | // I bit (IRQ masked)
               (1 << 8) | // A bit (SError masked)
               (1 << 9);  // D bit (Debug masked)
    asm volatile("msr spsr_el3, %0" ::"r"(spsr));
  }
}

inline void arch_init() noexcept {
  // Configure caches, MMU, and other architecture features
  invalidate_icache();
  invalidate_dcache();
}

} // namespace moss::kernel::arch::arm64

#endif // MOSS_ARCH_ARM64