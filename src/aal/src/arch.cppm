// MOSS Architecture Abstraction Layer (AAL)
//
// Single source of truth for ALL CPU-level inline assembly.  Every kernel
// module that needs barriers, CPU-ID, timestamps, interrupt control, etc.
// should `import moss.arch;` and call these functions instead of writing
// its own `#ifdef / asm volatile` blocks.
//
// Design: all functions are `inline` in the exported interface, so the
// compiler can inline them at every call-site — zero overhead compared to
// the previous copy-paste approach, but with a single point of maintenance.

export module moss.arch;

import moss.std;
import moss.types;

export namespace moss::kernel::arch {

// ============================================================================
// Architecture identification
// ============================================================================
enum class Architecture { ARM64, X86_64, RISCV };

#if defined(MOSS_ARCH_ARM64)
inline constexpr Architecture CURRENT_ARCH = Architecture::ARM64;
#elif defined(MOSS_ARCH_X86_64)
inline constexpr Architecture CURRENT_ARCH = Architecture::X86_64;
#elif defined(MOSS_ARCH_RISCV)
inline constexpr Architecture CURRENT_ARCH = Architecture::RISCV;
#endif

inline constexpr bool is_arm64 = (CURRENT_ARCH == Architecture::ARM64);
inline constexpr bool is_x86_64 = (CURRENT_ARCH == Architecture::X86_64);
inline constexpr bool is_riscv = (CURRENT_ARCH == Architecture::RISCV);

// Maximum supported CPUs (compile-time constant)
inline constexpr u32 MAX_CPUS = 16;

// ============================================================================
// Memory barriers
// ============================================================================

// Full memory barrier (data + instruction ordering)
inline void memory_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dmb sy" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("mfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence rw,rw" ::: "memory");
#endif
}

// Load-acquire barrier
inline void read_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dmb ld" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("lfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence r,r" ::: "memory");
#endif
}

// Store-release barrier
inline void write_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dmb st" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("sfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence w,w" ::: "memory");
#endif
}

// Data synchronisation barrier (stronger than DMB on ARM64)
inline void data_sync_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dsb sy" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("mfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence iorw,iorw" ::: "memory");
#endif
}

// Instruction barrier (pipeline flush)
inline void instruction_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("isb" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("" ::: "memory"); // x86 serialises via CPUID; lightweight barrier here
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence.i" ::: "memory");
#endif
}

// I/O-specific barrier (for MMIO ordering)
inline void io_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dsb sy" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("mfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence iorw,iorw" ::: "memory");
#endif
}

// ============================================================================
// CPU operations
// ============================================================================

// Hint the CPU to yield execution (spin-wait optimisation)
inline void cpu_yield() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("yield" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("pause" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("" ::: "memory"); // no yield hint on RISC-V
#endif
}

// Halt the CPU until the next interrupt
inline void cpu_halt() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("wfi");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("hlt");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("wfi");
#endif
}

// Idle-loop sequence: enable interrupts → wait → disable interrupts
// (used by scheduler idle tasks)
inline void cpu_idle_once() noexcept {
#if defined(MOSS_ARCH_ARM64)
  // Match x86_64 (sti;hlt;cli) and RISC-V (csrsi;wfi;csrci) pattern:
  // enable IRQ → WFI → disable IRQ.
  // Without explicit IRQ enable, WFI returns immediately when DAIF.I=1
  // (IRQs masked), causing a busy-loop that pins host CPU at 100%.
  asm volatile("dsb sy" ::: "memory");
  asm volatile("msr daifclr, #0x2" ::: "memory"); // enable IRQ (clear DAIF.I)
  asm volatile("wfi" ::: "memory");
  asm volatile("msr daifset, #0x2" ::: "memory"); // disable IRQ (set DAIF.I)
  asm volatile("isb" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("sti" ::: "memory");
  asm volatile("hlt" ::: "memory");
  asm volatile("cli" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  // S-mode: use sstatus.SIE (bit 1), not mstatus.MIE (bit 3)
  asm volatile("csrsi sstatus, 0x2" ::: "memory");
  asm volatile("wfi" ::: "memory");
  asm volatile("csrci sstatus, 0x2" ::: "memory");
#endif
}

// Get current CPU ID (topology-level, capped to MAX_CPUS)
[[nodiscard]] inline u32 get_current_cpu_id() noexcept {
#if defined(MOSS_ARCH_ARM64)
  u64 mpidr;
  asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  return static_cast<u32>(mpidr & 0xFF) % MAX_CPUS;
#elif defined(MOSS_ARCH_X86_64)
  u32 eax, ebx, ecx, edx;
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
  return (ebx >> 24) & 0xFF;
#elif defined(MOSS_ARCH_RISCV)
  // S-mode cannot read mhartid; use tp register (set by SBI/bootloader)
  u64 hartid;
  asm volatile("mv %0, tp" : "=r"(hartid));
  return static_cast<u32>(hartid) % MAX_CPUS;
#else
  return 0;
#endif
}

// Read hardware timestamp counter (monotonic, architecture-specific units)
[[nodiscard]] inline u64 get_timestamp_counter() noexcept {
#if defined(MOSS_ARCH_ARM64)
  u64 val;
  asm volatile("mrs %0, cntvct_el0" : "=r"(val));
  return val;
#elif defined(MOSS_ARCH_X86_64)
  u32 lo, hi;
  asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return (static_cast<u64>(hi) << 32) | lo;
#elif defined(MOSS_ARCH_RISCV)
  u64 val;
  asm volatile("rdcycle %0" : "=r"(val));
  return val;
#else
  return 0;
#endif
}

// ============================================================================
// Interrupt control
// ============================================================================

inline void enable_interrupts() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("msr daifclr, #0x2" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("sti" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("csrsi sstatus, 0x2" ::: "memory"); // SIE = bit 1
#endif
}

inline void disable_interrupts() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("msr daifset, #0x2" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("cli" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("csrci sstatus, 0x2" ::: "memory"); // clear SIE
#endif
}

// Disable ALL asynchronous exceptions (IRQ + FIQ + SError)
inline void disable_all_interrupts() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("msr daifset, #0xf" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("cli" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("csrci sstatus, 0x2" ::: "memory"); // clear SIE
#endif
}

[[nodiscard]] inline bool interrupts_enabled() noexcept {
#if defined(MOSS_ARCH_ARM64)
  u64 daif;
  asm volatile("mrs %0, daif" : "=r"(daif));
  return (daif & (1 << 7)) == 0; // IRQ mask bit
#elif defined(MOSS_ARCH_X86_64)
  u64 flags;
  asm volatile("pushfq; pop %0" : "=r"(flags));
  return (flags & (1 << 9)) != 0; // IF flag
#elif defined(MOSS_ARCH_RISCV)
  u64 sstatus;
  asm volatile("csrr %0, sstatus" : "=r"(sstatus));
  return (sstatus & 0x2) != 0; // SIE = bit 1
#else
  return false;
#endif
}

// ============================================================================
// TLB management
// ============================================================================

inline void flush_tlb() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("tlbi vmalle1is" ::: "memory");
  asm volatile("dsb sy");
  asm volatile("isb");
#elif defined(MOSS_ARCH_X86_64)
  // Reload CR3 to flush entire TLB
  u64 cr3;
  asm volatile("mov %%cr3, %0" : "=r"(cr3));
  asm volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("sfence.vma" ::: "memory");
#endif
}

inline void flush_tlb_addr(VirtAddr addr) noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("tlbi vae1is, %0" ::"r"(addr >> 12) : "memory");
  asm volatile("dsb sy");
  asm volatile("isb");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("invlpg (%0)" ::"r"(addr) : "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("sfence.vma %0, zero" ::"r"(addr) : "memory");
#endif
}

// ============================================================================
// Cache operations
// ============================================================================

inline void invalidate_icache() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("ic iallu" ::: "memory");
  asm volatile("dsb sy");
  asm volatile("isb");
#elif defined(MOSS_ARCH_X86_64)
  // x86 has coherent I-cache; compiler barrier suffices
  asm volatile("" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence.i" ::: "memory");
#endif
}

inline void flush_cache_line(VirtAddr addr) noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dc civac, %0" ::"r"(addr) : "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("clflush (%0)" ::"r"(addr) : "memory");
#elif defined(MOSS_ARCH_RISCV)
  (void)addr; // RISC-V cache flush is implementation-specific
#endif
}

// ============================================================================
// Context / stack operations
// ============================================================================

inline void switch_to_kernel_stack(void *stack_ptr) noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("mov sp, %0" ::"r"(stack_ptr) : "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("mov %0, %%rsp" ::"r"(stack_ptr) : "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("mv sp, %0" ::"r"(stack_ptr) : "memory");
#endif
}

[[nodiscard]] inline void *get_current_stack_pointer() noexcept {
  void *sp;
#if defined(MOSS_ARCH_ARM64)
  asm volatile("mov %0, sp" : "=r"(sp));
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("mov %%rsp, %0" : "=r"(sp));
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("mv %0, sp" : "=r"(sp));
#endif
  return sp;
}

// Read frame pointer (for stack traces)
[[nodiscard]] inline u64 get_frame_pointer() noexcept {
  u64 fp = 0;
#if defined(MOSS_ARCH_ARM64)
  asm volatile("mov %0, x29" : "=r"(fp));
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("mov %%rbp, %0" : "=r"(fp));
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("mv %0, s0" : "=r"(fp));
#endif
  return fp;
}

// ============================================================================
// MMU setup (ARM64-specific details; x86_64 and RISC-V will add their own)
// ============================================================================

inline void setup_kernel_mmu(PhysAddr kernel_pgd_pa) noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("msr ttbr1_el1, %0" ::"r"(kernel_pgd_pa));
  asm volatile("isb");
  u64 sctlr;
  asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
  sctlr |= (1 << 0) | (1 << 2) | (1 << 12); // MMU + D-cache + I-cache
  asm volatile("msr sctlr_el1, %0" ::"r"(sctlr));
  asm volatile("isb");
#elif defined(MOSS_ARCH_X86_64)
  (void)kernel_pgd_pa; // TODO: set CR3
#elif defined(MOSS_ARCH_RISCV)
  (void)kernel_pgd_pa; // TODO: set satp
#endif
}

// ============================================================================
// Debug / panic
// ============================================================================

// NOTE: kernel_panic intentionally hardcodes the UART address per architecture
// because this function must work even when PlatformInfo / DTB is corrupted.
// The addresses match QEMU virt defaults and typical bootloader setups.
[[noreturn]] inline void kernel_panic(const char *message) noexcept {
  disable_all_interrupts();

#if defined(MOSS_ARCH_ARM64)
  // PL011 UART at QEMU virt default 0x09000000
  volatile u32 *uart_data = reinterpret_cast<volatile u32 *>(0x09000000ULL);
  volatile u32 *uart_flags = reinterpret_cast<volatile u32 *>(0x09000018ULL);

  auto uart_putc = [&](char c) {
    while (*uart_flags & (1u << 5)) {
    }
    *uart_data = static_cast<u32>(static_cast<unsigned char>(c));
  };
#elif defined(MOSS_ARCH_X86_64)
  // COM1 at I/O port 0x3F8
  auto uart_putc = [](char c) {
    // Wait for TX empty (bit 5 of Line Status Register)
    for (;;) {
      u8 lsr;
      asm volatile("inb %1, %0" : "=a"(lsr) : "Nd"(static_cast<u16>(0x3FD)));
      if (lsr & 0x20)
        break;
    }
    asm volatile("outb %0, %1" ::"a"(static_cast<u8>(c)), "Nd"(static_cast<u16>(0x3F8)));
  };
#elif defined(MOSS_ARCH_RISCV)
  // NS16550 UART at QEMU virt default 0x10000000
  volatile u8 *uart_data = reinterpret_cast<volatile u8 *>(0x10000000ULL);
  volatile u8 *uart_lsr = reinterpret_cast<volatile u8 *>(0x10000005ULL);

  auto uart_putc = [&](char c) {
    while (!(*uart_lsr & 0x20)) {
    }
    *uart_data = static_cast<u8>(c);
  };
#endif

  auto puts = [&](const char *s) {
    if (!s)
      return;
    while (*s) {
      if (*s == '\n')
        uart_putc('\r');
      uart_putc(*s++);
    }
  };

  puts("\r\nKERNEL PANIC: ");
  puts(message);
  puts("\r\n");

  while (true) {
    cpu_halt();
  }
}

// ============================================================================
// Architecture-specific early initialisation
// ============================================================================

inline void early_arch_init() noexcept {
#if defined(MOSS_ARCH_ARM64)
  u64 currentel;
  asm volatile("mrs %0, currentel" : "=r"(currentel));
  currentel = (currentel >> 2) & 3;

  if (currentel == 3) {
    u64 scr = (1 << 0) | (1 << 8) | (1 << 10);
    asm volatile("msr scr_el3, %0" ::"r"(scr));
    u64 spsr = (1 << 0) | (1 << 2) | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9);
    asm volatile("msr spsr_el3, %0" ::"r"(spsr));
  }
#elif defined(MOSS_ARCH_X86_64)
  // x86_64 early init handled by boot code
#elif defined(MOSS_ARCH_RISCV)
  // RISC-V early init handled by boot code
#endif
}

inline void arch_init() noexcept {
#if defined(MOSS_ARCH_ARM64)
  invalidate_icache();
#elif defined(MOSS_ARCH_X86_64)
  // Nothing needed
#elif defined(MOSS_ARCH_RISCV)
  // Nothing needed
#endif
}

} // namespace moss::kernel::arch
