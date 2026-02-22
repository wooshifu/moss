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

// Unified syscall initialization interface
inline bool initialize_architecture_syscalls() noexcept {
  using moss::kernel::arch::is_arm64;
  using moss::kernel::arch::is_riscv;
  using moss::kernel::arch::is_x86_64;
  if constexpr (is_arm64) {
    // ARM64: Set up exception vector table to handle SVC instruction
    // TODO: Set up EL1 exception vector table, point SVC exception to syscall_entry_point
    return true;
  } else if constexpr (is_x86_64) {
    // X86_64: Set up SYSCALL instruction MSR registers
    // TODO: implement initialize_syscall_support() for x86_64
    return true;
  } else if constexpr (is_riscv) {
    // RISC-V: Set up trap vector table to handle ECALL instruction
    // TODO: Set up stvec register to point to syscall_entry_point
    return true;
  } else {
    return false;
  }
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
