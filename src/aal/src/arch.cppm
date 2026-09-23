// MOSS Architecture Abstraction Layer (AAL)
//
// Single source of truth for ALL CPU-level inline assembly.  Every kernel
// module that needs barriers, CPU-ID, timestamps, interrupt control, etc.
// should `import moss.arch;` and call these functions instead of writing
// its own `#ifdef / asm volatile` blocks.
//
// CPU primitives are inline; the synchronous software TLB protocol lives in
// tlb.cpp so its shared state and ordering have one implementation.

export module moss.arch;

import moss.std;
import moss.types;
import moss.platform;
import moss.hal.uart;

export namespace moss::kernel::arch {

// ============================================================================
// Architecture identification
// ============================================================================
enum class Architecture { ARM64, X64, RISCV64 };

#if defined(MOSS_ARCH_ARM64)
inline constexpr Architecture CURRENT_ARCH = Architecture::ARM64;
#elif defined(MOSS_ARCH_X64)
inline constexpr Architecture CURRENT_ARCH = Architecture::X64;
#elif defined(MOSS_ARCH_RISCV64)
inline constexpr Architecture CURRENT_ARCH = Architecture::RISCV64;
#endif

inline constexpr bool is_arm64 = (CURRENT_ARCH == Architecture::ARM64);
inline constexpr bool is_x64 = (CURRENT_ARCH == Architecture::X64);
inline constexpr bool is_riscv64 = (CURRENT_ARCH == Architecture::RISCV64);

// ============================================================================
// Memory barriers
// ============================================================================

// Full data-memory ordering barrier. Instruction publication needs the
// architecture's instruction/cache synchronization as well.
inline void memory_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dmb sy" ::: "memory");
#elif defined(MOSS_ARCH_X64)
  asm volatile("mfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV64)
  asm volatile("fence rw,rw" ::: "memory");
#endif
}

// Load-acquire barrier
inline void read_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dmb ld" ::: "memory");
#elif defined(MOSS_ARCH_X64)
  asm volatile("lfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV64)
  asm volatile("fence r,r" ::: "memory");
#endif
}

// Store-release barrier
inline void write_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dmb st" ::: "memory");
#elif defined(MOSS_ARCH_X64)
  asm volatile("sfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV64)
  asm volatile("fence w,w" ::: "memory");
#endif
}

// Data synchronisation barrier (stronger than DMB on ARM64)
inline void data_sync_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dsb sy" ::: "memory");
#elif defined(MOSS_ARCH_X64)
  asm volatile("mfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV64)
  asm volatile("fence iorw,iorw" ::: "memory");
#endif
}

// Instruction barrier (pipeline flush)
inline void instruction_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("isb" ::: "memory");
#elif defined(MOSS_ARCH_X64)
  asm volatile("" ::: "memory"); // Compiler barrier only; this does not execute serializing CPUID.
#elif defined(MOSS_ARCH_RISCV64)
  asm volatile("fence.i" ::: "memory");
#endif
}

// I/O-specific barrier (for MMIO ordering)
inline void io_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dsb sy" ::: "memory");
#elif defined(MOSS_ARCH_X64)
  asm volatile("mfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV64)
  asm volatile("fence iorw,iorw" ::: "memory");
#endif
}

// ============================================================================
// CPU operations
// ============================================================================

// No allocation, locks or scheduling: safe even for an IRQ-masked lock waiter.
// ARM64 uses native TLBI broadcasts and does not need this software mailbox.
void service_tlb_shootdown() noexcept;

// Hint the CPU to yield execution (spin-wait optimisation)
// This never schedules a thread or waits for an event; spin loops must keep
// checking their condition and cannot rely on a matching SEV notification.
inline void cpu_yield() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("yield" ::: "memory");
#elif defined(MOSS_ARCH_X64)
  service_tlb_shootdown();
  asm volatile("pause" ::: "memory");
#elif defined(MOSS_ARCH_RISCV64)
  service_tlb_shootdown();
  asm volatile("" ::: "memory"); // no yield hint on RISC-V 64
#endif
}

// Halt the CPU until the next interrupt
inline void cpu_halt() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("wfi");
#elif defined(MOSS_ARCH_X64)
  asm volatile("hlt");
#elif defined(MOSS_ARCH_RISCV64)
  asm volatile("wfi");
#endif
}

// Idle-loop sequence: enable interrupts → wait → disable interrupts
// (used by scheduler idle tasks)
inline void cpu_idle_once() noexcept {
#if defined(MOSS_ARCH_ARM64)
  // Match x64 (sti;hlt;cli) and RISC-V 64 (csrsi;wfi;csrci) pattern:
  // enable IRQ → WFI → disable IRQ.
  // Enable IRQ delivery so wakeups can run their handlers; keep the caller's
  // scheduler bookkeeping after this sequence protected by masking again.
  asm volatile("dsb sy" ::: "memory");
  asm volatile("msr daifclr, #0x2" ::: "memory"); // enable IRQ (clear DAIF.I)
  asm volatile("wfi" ::: "memory");
  asm volatile("msr daifset, #0x2" ::: "memory"); // disable IRQ (set DAIF.I)
  asm volatile("isb" ::: "memory");
#elif defined(MOSS_ARCH_X64)
  asm volatile("sti" ::: "memory");
  asm volatile("hlt" ::: "memory");
  asm volatile("cli" ::: "memory");
#elif defined(MOSS_ARCH_RISCV64)
  // S-mode: use sstatus.SIE (bit 1), not mstatus.MIE (bit 3)
  asm volatile("csrsi sstatus, 0x2" ::: "memory");
  asm volatile("wfi" ::: "memory");
  asm volatile("csrci sstatus, 0x2" ::: "memory");
#endif
}

// Translate firmware hardware IDs into logical per-CPU indices. Missing IDs
// return platform's out-of-range sentinel; callers must still bounds-check.
#if defined(MOSS_ARCH_RISCV64)
[[nodiscard]] inline u64 riscv64_hart_id(u32 logical) noexcept { return platform::hardware.cpus[logical].hardware_id; }
inline void set_user_kernel_stack(u64 top) noexcept {
  // The caller must keep IRQs masked until the S-mode switch or user return
  // consumes this value: nonzero sscratch selects a user-origin trap stack.
  // Reserve 16 bytes at stack top for the saved kernel tp slot plus alignment;
  // riscv64_syscall.S reads the slot before building the native TrapFrame.
  u64 hart;
  asm volatile("mv %0, tp" : "=r"(hart));
  *reinterpret_cast<u64 *>(top - 16) = hart;
  asm volatile("csrw sscratch, %0" ::"r"(top - 16) : "memory");
}
#endif
[[nodiscard]] inline u32 get_current_cpu_id() noexcept {
#if defined(MOSS_ARCH_ARM64)
  u64 mpidr;
  asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  // Retain MPIDR Aff3[39:32] and Aff2:Aff0[23:0], excluding status/reserved bits.
  return platform::logical_cpu(mpidr & 0xFF00FFFFFFULL);
#elif defined(MOSS_ARCH_X64)
  u32 eax, ebx, ecx, edx;
  // CPUID leaf 1 EBX[31:24] is the legacy initial APIC ID (x2APIC is unsupported).
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
  return platform::logical_cpu((ebx >> 24) & 0xFF);
#elif defined(MOSS_ARCH_RISCV64)
  // S-mode cannot read mhartid; use tp register (set by SBI/bootloader)
  u64 hartid;
  asm volatile("mv %0, tp" : "=r"(hartid));
  return platform::logical_cpu(hartid);
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
#elif defined(MOSS_ARCH_X64)
  u32 lo, hi;
  asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return (static_cast<u64>(hi) << 32) | lo;
#elif defined(MOSS_ARCH_RISCV64)
  // Use rdtime instead of rdcycle: cycle counter may be disabled in S-mode
  // (requires mcounteren.CY). rdtime also requires firmware to grant counter
  // access or emulate it; the boot profile supplies that firmware contract.
  u64 val;
  asm volatile("rdtime %0" : "=r"(val));
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
#elif defined(MOSS_ARCH_X64)
  asm volatile("sti" ::: "memory");
#elif defined(MOSS_ARCH_RISCV64)
  asm volatile("csrsi sstatus, 0x2" ::: "memory"); // SIE = bit 1
#endif
}

inline void disable_interrupts() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("msr daifset, #0x2" ::: "memory");
#elif defined(MOSS_ARCH_X64)
  asm volatile("cli" ::: "memory");
#elif defined(MOSS_ARCH_RISCV64)
  asm volatile("csrci sstatus, 0x2" ::: "memory"); // clear SIE
#endif
}

// Disable ALL asynchronous exceptions (IRQ + FIQ + SError)
inline void disable_all_interrupts() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("msr daifset, #0xf" ::: "memory");
#elif defined(MOSS_ARCH_X64)
  asm volatile("cli" ::: "memory");
#elif defined(MOSS_ARCH_RISCV64)
  asm volatile("csrci sstatus, 0x2" ::: "memory"); // clear SIE
#endif
}

// Firmware or ACPI owns the whole-machine transition. Do not depend on the
// failed supervisor or its services to complete this path.
[[noreturn]] inline void system_reset() noexcept {
  disable_all_interrupts();
#if defined(MOSS_ARCH_ARM64)
  if (platform::hardware.psci_valid) {
    // PSCI SYSTEM_RESET, Arm DEN0022 function ID 0x84000009.
    register u64 x0 asm("x0") = 0x84000009;
    register u64 x1 asm("x1") = 0;
    register u64 x2 asm("x2") = 0;
    register u64 x3 asm("x3") = 0;
    if (platform::hardware.psci_smc) {
      asm volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    } else {
      asm volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    }
  }
#elif defined(MOSS_ARCH_X64)
  if (platform::hardware.acpi_reset_valid) {
    // ACPI 6.5 reset register: write the FADT's RESET_VALUE to RESET_REG.
    asm volatile("outb %0, %1" ::"a"(platform::hardware.acpi_reset_value), "Nd"(platform::hardware.acpi_reset_port)
                 : "memory");
  }
#elif defined(MOSS_ARCH_RISCV64)
  // RISC-V SBI SRST: EID "SRST", FID 0, cold reboot type 1, reason 0.
  register u64 a0 asm("a0") = 1;
  register u64 a1 asm("a1") = 0;
  register u64 a6 asm("a6") = 0;
  register u64 a7 asm("a7") = 0x53525354;
  asm volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory");
#endif
  // Firmware must not return from a successful reset. Masked-IRQ halt is the
  // bounded local fallback if the platform has no reset path or one fails.
  for (;;) {
    cpu_halt();
  }
}

[[nodiscard]] inline bool interrupts_enabled() noexcept {
#if defined(MOSS_ARCH_ARM64)
  u64 daif;
  asm volatile("mrs %0, daif" : "=r"(daif));
  return (daif & (1 << 7)) == 0; // IRQ mask bit
#elif defined(MOSS_ARCH_X64)
  u64 flags;
  asm volatile("pushfq; pop %0" : "=r"(flags));
  return (flags & (1 << 9)) != 0; // IF flag
#elif defined(MOSS_ARCH_RISCV64)
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

// SGIs 0..7 are reserved for the generic IPI classes. x64 routes SGIs at
// vector 64+id (above exceptions/legacy IRQs), so shootdown uses vector 72.
inline constexpr u32 TLB_SHOOTDOWN_SGI = 8;
inline constexpr u32 X64_TLB_SHOOTDOWN_VECTOR = 64 + TLB_SHOOTDOWN_SGI;
using TlbNotifier = bool (*)(u32 cpu) noexcept;
// Boot calls this on each CPU only after its MMU, trap entry and IPI transport
// are ready. Joining serializes with requests; hot-unplug is not supported.
void register_tlb_cpu(TlbNotifier notify) noexcept;
// PTE stores precede publication; return means every participating CPU has
// invalidated. Callers still own VM locking and address-space/root lifetime.
void synchronize_tlb(VirtAddr addr, bool full) noexcept;

inline void flush_tlb() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dsb ishst" ::: "memory");
  asm volatile("tlbi vmalle1is" ::: "memory");
  asm volatile("dsb sy" ::: "memory");
  asm volatile("isb" ::: "memory");
#else
  synchronize_tlb(0, true);
#endif
}

// Complete per-address invalidation after publishing PTE stores. ARM64
// broadcasts within the inner-shareable domain; x64/RV64 wait for remote
// software acknowledgements before callers may reclaim old backing storage.
inline void flush_tlb_addr(VirtAddr addr) noexcept {
#if defined(MOSS_ARCH_ARM64)
  // This interface has no ASID, and the changed root need not be active on
  // this CPU. VAAE1IS covers all ASIDs and walk levels on the sharing CPUs;
  // VAE1IS/VALE1IS with a bare page number would target only ASID 0.
  // Encode exactly VA[55:12] (44 bits). Canonical high bits must not leak into
  // TTL/RES0 fields; the 12-bit shift corresponds to the 4 KiB page granule.
  const u64 operand = (addr >> 12) & ((u64{1} << 44) - 1);
  asm volatile("dsb ishst" ::: "memory");
  asm volatile("tlbi vaae1is, %0" ::"r"(operand) : "memory");
  // Completion precedes releasing frames or publishing a replacement mapping.
  asm volatile("dsb ish" ::: "memory");
  asm volatile("isb" ::: "memory");
#else
  synchronize_tlb(addr, false);
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
#elif defined(MOSS_ARCH_X64)
  // x86 has coherent I-cache; compiler barrier suffices
  asm volatile("" ::: "memory");
#elif defined(MOSS_ARCH_RISCV64)
  asm volatile("fence.i" ::: "memory");
#endif
}

inline void flush_cache_line(VirtAddr addr) noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dc civac, %0" ::"r"(addr) : "memory");
#elif defined(MOSS_ARCH_X64)
  asm volatile("clflush (%0)" ::"r"(addr) : "memory");
#elif defined(MOSS_ARCH_RISCV64)
  (void)addr; // RISC-V 64 cache flush is implementation-specific
#endif
}

// ============================================================================
// Context / stack operations
// ============================================================================

inline void switch_to_kernel_stack(void *stack_ptr) noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("mov sp, %0" ::"r"(stack_ptr) : "memory");
#elif defined(MOSS_ARCH_X64)
  asm volatile("mov %0, %%rsp" ::"r"(stack_ptr) : "memory");
#elif defined(MOSS_ARCH_RISCV64)
  asm volatile("mv sp, %0" ::"r"(stack_ptr) : "memory");
#endif
}

[[nodiscard]] inline void *get_current_stack_pointer() noexcept {
  void *sp;
#if defined(MOSS_ARCH_ARM64)
  asm volatile("mov %0, sp" : "=r"(sp));
#elif defined(MOSS_ARCH_X64)
  asm volatile("mov %%rsp, %0" : "=r"(sp));
#elif defined(MOSS_ARCH_RISCV64)
  asm volatile("mv %0, sp" : "=r"(sp));
#endif
  return sp;
}

// Read frame pointer (for stack traces)
[[nodiscard]] inline u64 get_frame_pointer() noexcept {
  u64 fp = 0;
#if defined(MOSS_ARCH_ARM64)
  asm volatile("mov %0, x29" : "=r"(fp));
#elif defined(MOSS_ARCH_X64)
  asm volatile("mov %%rbp, %0" : "=r"(fp));
#elif defined(MOSS_ARCH_RISCV64)
  asm volatile("mv %0, s0" : "=r"(fp));
#endif
  return fp;
}

// ============================================================================
// MMU setup (ARM64-specific details; x64 and RISC-V 64 will add their own)
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
#elif defined(MOSS_ARCH_X64)
  // x64: load PML4 (page table root) physical address into CR3.
  // In long mode the MMU is always enabled; writing CR3 activates the new page tables.
  asm volatile("mov %0, %%cr3" ::"r"(kernel_pgd_pa) : "memory");
#elif defined(MOSS_ARCH_RISCV64)
  // RISC-V 64: satp = MODE(runtime Sv39/Sv48) | PPN(kernel_pgd_pa >> 12)
  // Read current satp to preserve MODE bits (set by detect_mmu_mode at boot).
  u64 current_satp;
  asm volatile("csrr %0, satp" : "=r"(current_satp));
  u64 mode_bits = current_satp & (0xFULL << 60);
  // If MMU not yet enabled (mode=0), default to Sv39.
  if (mode_bits == 0) {
    // RV64 satp.MODE=8 is Sv39; MODE occupies bits [63:60].
    mode_bits = 8ULL << 60;
  }
  u64 satp_val = mode_bits | ((kernel_pgd_pa >> 12) & 0x00000FFFFFFFFFFFULL);
  asm volatile("csrw satp, %0" ::"r"(satp_val) : "memory");
  asm volatile("sfence.vma" ::: "memory");
#endif
}

// ============================================================================
// Debug / panic
// ============================================================================

// Panic uses the discovered console without taking a possibly held TX lock.
// Before discovery there may be no console; never probe a guessed MMIO address.
[[noreturn]] inline void kernel_panic(const char *message) noexcept {
  disable_all_interrupts();
  auto puts = [](const char *text) {
    if (!text) {
      return;
    }
    while (*text) {
      if (*text == '\n') {
        hal::uart::putc_unlocked('\r');
      }
      hal::uart::putc_unlocked(*text++);
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
    // SCR.NS/HCE/RW (bits 0/8/10) select non-secure AArch64 and permit HVC.
    u64 scr = (1 << 0) | (1 << 8) | (1 << 10);
    asm volatile("msr scr_el3, %0" ::"r"(scr));
    // SPSR.M=5 selects EL1h; bits 6..9 mask FIQ/IRQ/SError/debug during setup.
    u64 spsr = (1 << 0) | (1 << 2) | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9);
    asm volatile("msr spsr_el3, %0" ::"r"(spsr));
  }
#elif defined(MOSS_ARCH_X64)
  // x64 early init handled by boot code
#elif defined(MOSS_ARCH_RISCV64)
  // RISC-V 64 early init handled by boot code
#endif
}

inline void arch_init() noexcept {
#if defined(MOSS_ARCH_ARM64)
  invalidate_icache();
#elif defined(MOSS_ARCH_X64)
  // Nothing needed
#elif defined(MOSS_ARCH_RISCV64)
  // Nothing needed
#endif
}

} // namespace moss::kernel::arch
