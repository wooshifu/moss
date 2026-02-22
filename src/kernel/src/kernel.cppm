// MOSS Kernel Module - Primary Interface (re-export hub)
// All declarations live in partition files; this module re-exports them
// and provides the global module fragment needed by implementation .cpp files.

module;

// extern "C" declarations (global module fragment)
// Kept here so that implementation .cpp files (which `module moss.kernel;`)
// can see these symbols without their own GMF.
extern "C" {
void kernel_test_all_subsystems(void) noexcept;
void early_debug_print(const char *message) noexcept;

// Syscall arch assembly function declarations
void syscall_entry_point() noexcept;
long system_call_handler(long syscall_number, long arg0, long arg1,
                         long arg2, long arg3, long arg4, long arg5) noexcept;

// Embedded user program symbols (ARM64 only, linked from arm64_user_program.S)
#if defined(MOSS_ARCH_ARM64)
extern char _user_program_start[];
extern char _user_program_end[];
#endif
}

// Forward declaration for syscall_return (needs SyscallContext which is defined later)
// We declare the raw extern "C" here; the typed version is inside the module.
extern "C" void syscall_return(void *context) noexcept;

export module moss.kernel;

// All external module imports (available to implementation units)
import moss.std;
import moss.types;
import moss.result;
import moss.smart_ptr;
import moss.arch;
import moss.platform;
import moss.hal.uart;
import moss.hal.intc;
import moss.hal.timer;
import moss.containers;
import moss.mm;
import moss.interrupts;
import moss.drivers;
import moss.fdt;
import moss.initramfs;
import moss.ipc;
import moss.process;
import moss.timer;
import moss.logging;
import moss.boot;

// Re-export all partitions
export import :elf;
export import :syscall_table;
export import :syscall_arch;
export import :main;
