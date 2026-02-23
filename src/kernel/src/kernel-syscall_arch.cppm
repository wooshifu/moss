// MOSS Kernel Module - Syscall Architecture Partition
// Architecture-specific syscall initialization, conventions, and context.

export module moss.kernel:syscall_arch;

import moss.std;
import moss.types;
import moss.arch;
import moss.abi;

export namespace moss::kernel::arch::syscall {

// Syscall context structure (forward declaration, used as opaque pointer in assembly)
struct SyscallContext;

// Syscall return handler (wraps the assembly function from moss.abi)
inline void do_syscall_return(SyscallContext *context) noexcept {
  moss::abi::syscall_return(static_cast<void *>(context));
}

// Unified syscall initialization interface.
// ARM64: VBAR_EL1 is set in start_arm64.S; no runtime init needed here.
// x86_64: Write LSTAR, STAR, SFMASK, EFER MSRs for SYSCALL instruction.
// RISC-V: Write stvec CSR to point to the trap handler.
//
// Uses #if instead of if constexpr because inline asm constraints are
// validated at parse time regardless of constexpr branch elimination.
inline bool initialize_architecture_syscalls() noexcept {
#if defined(MOSS_ARCH_ARM64)
  // VBAR_EL1 already configured in start_arm64.S during early boot.
  return true;
#elif defined(MOSS_ARCH_X86_64)
  // syscall_entry_point is declared in moss.abi
  u64 lstar = reinterpret_cast<u64>(&moss::abi::syscall_entry_point);
  u32 lo = static_cast<u32>(lstar);
  u32 hi = static_cast<u32>(lstar >> 32);

  // LSTAR MSR (0xC0000082) = syscall entry point address
  asm volatile("wrmsr" ::"c"(0xC0000082U), "a"(lo), "d"(hi));

  // STAR MSR (0xC0000081): kernel CS=0x08 in [47:32], SYSRET base in [63:48]
  u32 star_hi = (0x0008U) | (0x0010U << 16);
  asm volatile("wrmsr" ::"c"(0xC0000081U), "a"(0U), "d"(star_hi));

  // SFMASK MSR (0xC0000084) = RFLAGS bits to clear on SYSCALL (IF, DF, TF)
  asm volatile("wrmsr" ::"c"(0xC0000084U), "a"(0x700U), "d"(0U));

  // Enable SCE (System Call Enable) in EFER MSR (0xC0000080)
  u32 efer_lo = 0;
  u32 efer_hi = 0;
  asm volatile("rdmsr" : "=a"(efer_lo), "=d"(efer_hi) : "c"(0xC0000080U));
  efer_lo |= 1U; // SCE = bit 0
  asm volatile("wrmsr" ::"c"(0xC0000080U), "a"(efer_lo), "d"(efer_hi));

  return true;
#elif defined(MOSS_ARCH_RISCV)
  // Set stvec to point to syscall_entry_point (direct mode)
  u64 addr = reinterpret_cast<u64>(&moss::abi::syscall_entry_point);
  asm volatile("csrw stvec, %0" ::"r"(addr));
  return true;
#else
  return false;
#endif
}

// Architecture-specific syscall convention information
struct SyscallConvention {
  const char *arch_name;           // Architecture name
  const char *syscall_instruction; // Syscall instruction
  const char *syscall_nr_register; // Syscall number register
  const char *return_register;     // Return value register
  const char *arg_registers[6];    // Argument register list
};

inline const SyscallConvention &get_syscall_convention() noexcept {
  using moss::kernel::arch::is_arm64;
  using moss::kernel::arch::is_riscv;
  using moss::kernel::arch::is_x86_64;
  if constexpr (is_arm64) {
    static const SyscallConvention conv = {.arch_name = "ARM64",
                                           .syscall_instruction = "SVC",
                                           .syscall_nr_register = "x8",
                                           .return_register = "x0",
                                           .arg_registers = {"x0", "x1", "x2", "x3", "x4", "x5"}};
    return conv;
  } else if constexpr (is_x86_64) {
    static const SyscallConvention conv = {.arch_name = "x86_64",
                                           .syscall_instruction = "SYSCALL",
                                           .syscall_nr_register = "rax",
                                           .return_register = "rax",
                                           .arg_registers = {"rdi", "rsi", "rdx", "r10", "r8", "r9"}};
    return conv;
  } else if constexpr (is_riscv) {
    static const SyscallConvention conv = {.arch_name = "RISC-V",
                                           .syscall_instruction = "ECALL",
                                           .syscall_nr_register = "a7",
                                           .return_register = "a0",
                                           .arg_registers = {"a0", "a1", "a2", "a3", "a4", "a5"}};
    return conv;
  } else {
    static const SyscallConvention conv = {
        .arch_name = "Unknown",
        .syscall_instruction = "Unknown",
        .syscall_nr_register = "Unknown",
        .return_register = "Unknown",
        .arg_registers = {"Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown"}};
    return conv;
  }
}

// Debug: print current architecture's syscall convention
void print_syscall_convention() noexcept;

} // namespace moss::kernel::arch::syscall
