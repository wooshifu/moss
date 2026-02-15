// MOSS Kernel Module - ELF Loader, Syscall Table, Syscall Arch, Kernel Main
// Merges kernel_main.hpp, syscall_table.hpp, elf_loader.hpp, syscall_arch.hpp
// into a single C++26 module.

module;

// Architecture detection
#include "arch_detect.h"

// extern "C" declarations (global module fragment)
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
import moss.ipc;
import moss.process;
import moss.timer;
import moss.logging;
import moss.boot;

// ============================================================================
// Section 1: ELF types and ElfLoader (from elf_loader.hpp)
// ============================================================================
export namespace moss::kernel::elf {

// ELF file header constants
inline constexpr u32 ELF_MAGIC = 0x464C457F;  // "\x7FELF"
inline constexpr u8  ELF_CLASS_64 = 2;         // 64-bit ELF
inline constexpr u8  ELF_DATA_LSB = 1;         // Little-endian byte order
inline constexpr u8  ELF_VERSION = 1;          // ELF version 1

// Supported architectures
inline constexpr u16 EM_NONE = 0;       // Unspecified
inline constexpr u16 EM_X86_64 = 62;    // AMD64/x86_64
inline constexpr u16 EM_AARCH64 = 183;  // ARM64/AArch64
inline constexpr u16 EM_RISCV = 243;    // RISC-V

// File types
inline constexpr u16 ET_NONE = 0;  // Unknown type
inline constexpr u16 ET_REL = 1;   // Relocatable file
inline constexpr u16 ET_EXEC = 2;  // Executable file
inline constexpr u16 ET_DYN = 3;   // Shared object

// Program header types
inline constexpr u32 PT_NULL = 0;             // Unused
inline constexpr u32 PT_LOAD = 1;             // Loadable segment
inline constexpr u32 PT_DYNAMIC = 2;          // Dynamic linking info
inline constexpr u32 PT_INTERP = 3;           // Interpreter info
inline constexpr u32 PT_NOTE = 4;             // Auxiliary info
inline constexpr u32 PT_SHLIB = 5;            // Reserved
inline constexpr u32 PT_PHDR = 6;             // Program header table itself
inline constexpr u32 PT_TLS = 7;              // Thread-local storage

// Program header flags
inline constexpr u32 PF_X = 0x1;  // Executable
inline constexpr u32 PF_W = 0x2;  // Writable
inline constexpr u32 PF_R = 0x4;  // Readable

// 64-bit ELF file header
struct [[gnu::packed]] ElfHeader {
    u8  e_ident[16];     // ELF identification
    u16 e_type;          // File type
    u16 e_machine;       // Target architecture
    u32 e_version;       // File version
    u64 e_entry;         // Entry point virtual address
    u64 e_phoff;         // Program header table offset
    u64 e_shoff;         // Section header table offset
    u32 e_flags;         // Processor-specific flags
    u16 e_ehsize;        // ELF header size
    u16 e_phentsize;     // Program header table entry size
    u16 e_phnum;         // Program header table entry count
    u16 e_shentsize;     // Section header table entry size
    u16 e_shnum;         // Section header table entry count
    u16 e_shstrndx;      // Section name string table index
};

// 64-bit program header table entry
struct [[gnu::packed]] ProgramHeader {
    u32 p_type;          // Segment type
    u32 p_flags;         // Segment flags
    u64 p_offset;        // Segment offset in file
    u64 p_vaddr;         // Segment virtual address
    u64 p_paddr;         // Segment physical address (usually ignored)
    u64 p_filesz;        // Segment size in file
    u64 p_memsz;         // Segment size in memory
    u64 p_align;         // Segment alignment
};

// ELF load result information
struct LoadedProgram {
    VirtAddr entry_point;       // Program entry point
    VirtAddr base_address;      // Program base address
    VirtAddr stack_top;         // User stack top
    VirtAddr heap_start;        // Heap start address
    usize total_size;           // Total program memory size
    u32 load_segments;          // Number of loaded segments
};

// Memory segment information
struct MemorySegment {
    VirtAddr vaddr;             // Virtual address
    usize size;                 // Segment size
    u32 flags;                  // Access permission flags
    u32 type;                   // Segment type
};

// ELF loader class
class ElfLoader {
public:
    ElfLoader() noexcept = default;
    ~ElfLoader() noexcept = default;

    // Non-copyable, non-movable
    ElfLoader(const ElfLoader&) = delete;
    ElfLoader& operator=(const ElfLoader&) = delete;
    ElfLoader(ElfLoader&&) = delete;
    ElfLoader& operator=(ElfLoader&&) = delete;

    // Validate ELF file format
    [[nodiscard]] static VoidResult validate_elf_header(const ElfHeader* header) noexcept;

    // Check architecture compatibility
    [[nodiscard]] static VoidResult check_architecture_compatibility(u16 e_machine) noexcept;

    // Parse program header table
    [[nodiscard]] static VoidResult parse_program_headers(
        const u8* elf_data, const ElfHeader* header,
        MemorySegment* segments, u32 max_segments, u32* segment_count) noexcept;

    // Calculate program memory layout
    [[nodiscard]] static VoidResult calculate_memory_layout(
        const MemorySegment* segments, u32 segment_count,
        VirtAddr* base_address, usize* total_size) noexcept;

    // Allocate user address space
    [[nodiscard]] static VoidResult allocate_user_address_space(
        VirtAddr base_address, usize total_size, VirtAddr* allocated_base) noexcept;

    // Map ELF segments to memory
    [[nodiscard]] static VoidResult map_elf_segments(
        const u8* elf_data, const MemorySegment* segments, u32 segment_count,
        VirtAddr base_address) noexcept;

    // Set up user stack
    [[nodiscard]] static VoidResult setup_user_stack(
        VirtAddr* stack_top, usize stack_size = 8 * 1024 * 1024) noexcept; // Default 8MB stack

    // Set up user heap
    [[nodiscard]] static VoidResult setup_user_heap(
        VirtAddr stack_top, VirtAddr* heap_start) noexcept;

    // Main load function: load program from ELF data in memory
    [[nodiscard]] static Result<LoadedProgram> load_elf_from_memory(
        const u8* elf_data, usize elf_size) noexcept;

    // Main load function: load program from file (future, needs filesystem support)
    [[nodiscard]] static Result<LoadedProgram> load_elf_from_file(
        const char* filename) noexcept;

    // Debug: print ELF file information
    static void print_elf_info(const ElfHeader* header) noexcept;

    // Debug: print program header information
    static void print_program_headers(const u8* elf_data, const ElfHeader* header) noexcept;

    // Debug: print load result
    static void print_loaded_program(const LoadedProgram& program) noexcept;

private:
    // Internal helper: page alignment
    [[nodiscard]] static VirtAddr align_to_page(VirtAddr addr) noexcept {
        constexpr usize PAGE_SIZE_LOCAL = 4096;
        return (addr + PAGE_SIZE_LOCAL - 1) & ~(PAGE_SIZE_LOCAL - 1);
    }

    // Internal helper: check address range
    [[nodiscard]] static bool is_valid_user_address(VirtAddr addr) noexcept {
        // User address space: 0x00000000_00000000 to 0x00007FFF_FFFFFFFF (128TB)
        return addr < 0x0000800000000000ULL;
    }

    // Internal helper: permission flag conversion
    [[nodiscard]] static u32 elf_flags_to_memory_flags(u32 elf_flags) noexcept;
};

// ELF loader statistics
struct ElfLoaderStats {
    u64 total_loads;             // Total load count
    u64 successful_loads;        // Successful load count
    u64 failed_loads;            // Failed load count
    u64 bytes_loaded;            // Total bytes loaded
    u64 memory_allocated;        // Total memory allocated
};

// Global ELF loader statistics
extern ElfLoaderStats g_elf_loader_stats;

// ELF load error codes
enum class ElfLoadError : u32 {
    InvalidMagic = 1,            // Invalid ELF magic number
    InvalidClass = 2,            // Unsupported ELF class
    InvalidEndian = 3,           // Unsupported byte order
    InvalidVersion = 4,          // Unsupported ELF version
    UnsupportedArch = 5,         // Unsupported architecture
    InvalidFileType = 6,         // Invalid file type
    InvalidProgramHeaders = 7,   // Invalid program headers
    MemoryAllocationFailed = 8,  // Memory allocation failed
    AddressSpaceExhausted = 9,   // Address space exhausted
    InvalidSegment = 10,         // Invalid memory segment
    MappingFailed = 11,          // Memory mapping failed
};

} // namespace moss::kernel::elf

// ============================================================================
// Section 2: Syscall table types (from syscall_table.hpp)
// ============================================================================
export namespace moss::kernel::syscall {

// Syscall number enumeration - grouped by functionality
enum class SyscallNumber : long {
    // === Basic syscalls (0-9) ===
    SYS_DEBUG_PRINT = 0,
    SYS_EXIT = 1,
    SYS_GETPID = 2,
    SYS_GETPPID = 3,
    SYS_GETUID = 4,
    SYS_GETGID = 5,
    SYS_GETEUID = 6,
    SYS_GETEGID = 7,
    SYS_SETSID = 8,
    SYS_GETPGID = 9,

    // === Process management (10-29) ===
    SYS_FORK = 10,
    SYS_EXECVE = 11,
    SYS_WAIT4 = 12,
    SYS_WAITPID = 13,
    SYS_KILL = 14,
    SYS_SIGACTION = 15,
    SYS_SIGPROCMASK = 16,
    SYS_SIGRETURN = 17,
    SYS_PAUSE = 18,
    SYS_ALARM = 19,
    SYS_SETPGID = 20,
    SYS_SETUID = 21,
    SYS_SETGID = 22,
    SYS_SETEUID = 23,
    SYS_SETEGID = 24,
    SYS_GETPGRP = 25,
    SYS_SETPGRP = 26,
    SYS_GETSID = 27,
    SYS_NICE = 28,
    SYS_GETPRIORITY = 29,

    // === Filesystem operations (30-59) ===
    SYS_OPEN = 30,
    SYS_CLOSE = 31,
    SYS_READ = 32,
    SYS_WRITE = 33,
    SYS_LSEEK = 34,
    SYS_STAT = 35,
    SYS_FSTAT = 36,
    SYS_LSTAT = 37,
    SYS_ACCESS = 38,
    SYS_CHMOD = 39,
    SYS_CHOWN = 40,
    SYS_UMASK = 41,
    SYS_DUP = 42,
    SYS_DUP2 = 43,
    SYS_PIPE = 44,
    SYS_MKDIR = 45,
    SYS_RMDIR = 46,
    SYS_LINK = 47,
    SYS_UNLINK = 48,
    SYS_SYMLINK = 49,
    SYS_READLINK = 50,
    SYS_CHDIR = 51,
    SYS_GETCWD = 52,
    SYS_RENAME = 53,
    SYS_TRUNCATE = 54,
    SYS_FTRUNCATE = 55,
    SYS_FSYNC = 56,
    SYS_FDATASYNC = 57,
    SYS_SYNC = 58,
    SYS_MOUNT = 59,

    // === Memory management (60-79) ===
    SYS_MMAP = 60,
    SYS_MUNMAP = 61,
    SYS_MPROTECT = 62,
    SYS_MLOCK = 63,
    SYS_MUNLOCK = 64,
    SYS_MLOCKALL = 65,
    SYS_MUNLOCKALL = 66,
    SYS_MADVISE = 67,
    SYS_MSYNC = 68,
    SYS_BRK = 69,
    SYS_SBRK = 70,
    SYS_MREMAP = 71,
    SYS_MINCORE = 72,
    SYS_MMAP2 = 73,
    SYS_REMAP_FILE_PAGES = 74,
    SYS_MBIND = 75,
    SYS_GET_MEMPOLICY = 76,
    SYS_SET_MEMPOLICY = 77,
    SYS_MIGRATE_PAGES = 78,
    SYS_MOVE_PAGES = 79,

    // === Time and timers (80-89) ===
    SYS_TIME = 80,
    SYS_GETTIMEOFDAY = 81,
    SYS_SETTIMEOFDAY = 82,
    SYS_CLOCK_GETTIME = 83,
    SYS_CLOCK_SETTIME = 84,
    SYS_CLOCK_GETRES = 85,
    SYS_NANOSLEEP = 86,
    SYS_TIMER_CREATE = 87,
    SYS_TIMER_SETTIME = 88,
    SYS_TIMER_GETTIME = 89,

    // === Network communication (90-109) ===
    SYS_SOCKET = 90,
    SYS_BIND = 91,
    SYS_LISTEN = 92,
    SYS_ACCEPT = 93,
    SYS_CONNECT = 94,
    SYS_SEND = 95,
    SYS_RECV = 96,
    SYS_SENDTO = 97,
    SYS_RECVFROM = 98,
    SYS_SHUTDOWN = 99,
    SYS_SETSOCKOPT = 100,
    SYS_GETSOCKOPT = 101,
    SYS_GETSOCKNAME = 102,
    SYS_GETPEERNAME = 103,
    SYS_SOCKETPAIR = 104,
    SYS_SENDMSG = 105,
    SYS_RECVMSG = 106,
    SYS_SELECT = 107,
    SYS_POLL = 108,
    SYS_EPOLL_CREATE = 109,

    // === System information and control (110-129) ===
    SYS_UNAME = 110,
    SYS_SYSINFO = 111,
    SYS_GETRLIMIT = 112,
    SYS_SETRLIMIT = 113,
    SYS_GETRUSAGE = 114,
    SYS_TIMES = 115,
    SYS_PTRACE = 116,
    SYS_SYSLOG = 117,
    SYS_REBOOT = 118,
    SYS_SETHOSTNAME = 119,
    SYS_GETHOSTNAME = 120,
    SYS_SETDOMAINNAME = 121,
    SYS_GETDOMAINNAME = 122,
    SYS_IOPL = 123,
    SYS_IOPERM = 124,
    SYS_SYSCTL = 125,
    SYS_ARCH_PRCTL = 126,
    SYS_PRCTL = 127,
    SYS_CAPGET = 128,
    SYS_CAPSET = 129,

    // Total syscall count marker
    MAX_SYSCALL = 130
};

// Syscall handler function type
using SyscallHandler = long(*)(long arg0, long arg1, long arg2,
                               long arg3, long arg4, long arg5) noexcept;

// Syscall descriptor
struct SyscallDescriptor {
    const char* name;            // Syscall name
    SyscallHandler handler;      // Handler function pointer
    u8 arg_count;                // Argument count
    bool implemented;            // Whether implemented
    const char* description;     // Functionality description

    constexpr SyscallDescriptor() noexcept
        : name(nullptr), handler(nullptr), arg_count(0),
          implemented(false), description(nullptr) {}

    constexpr SyscallDescriptor(const char* n, SyscallHandler h, u8 argc,
                                bool impl, const char* desc) noexcept
        : name(n), handler(h), arg_count(argc),
          implemented(impl), description(desc) {}
};

// Syscall handler function declarations
namespace handlers {
    // Basic syscalls
    long sys_debug_print(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_exit(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_getpid(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_getppid(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_getuid(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_getgid(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

    // Process management - framework implementation
    long sys_fork(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_execve(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_wait4(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_kill(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

    // Filesystem - framework implementation
    long sys_open(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_close(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_read(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_write(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

    // Memory management
    long sys_mmap(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_munmap(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_mprotect(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_brk(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

    // Network communication - framework implementation
    long sys_socket(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_bind(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_listen(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_accept(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

    // Default handler for unimplemented syscalls
    long sys_not_implemented(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
}

// Global syscall table
extern const SyscallDescriptor SYSCALL_TABLE[static_cast<int>(SyscallNumber::MAX_SYSCALL)];

// Syscall dispatcher
class SyscallDispatcher {
public:
    // Dispatch a syscall
    static long dispatch(long syscall_number, long arg0, long arg1, long arg2,
                         long arg3, long arg4, long arg5) noexcept;

    // Get syscall information
    static const SyscallDescriptor* get_syscall_info(long syscall_number) noexcept;

    // Check whether a syscall is implemented
    static bool is_implemented(long syscall_number) noexcept;

    // Validate syscall number
    static bool is_valid_syscall(long syscall_number) noexcept;

    // Get syscall statistics
    static void get_syscall_stats(u64* total_calls, u64* implemented_calls) noexcept;

    // Debug: print all implemented syscalls
    static void print_implemented_syscalls() noexcept;
};

// Syscall statistics
struct SyscallStats {
    u64 total_syscalls;          // Total invocation count
    u64 successful_syscalls;     // Successful invocation count
    u64 failed_syscalls;         // Failed invocation count
    u64 unimplemented_syscalls;  // Unimplemented invocation count
    u64 invalid_syscalls;        // Invalid invocation count
};

// Global syscall statistics
extern SyscallStats g_syscall_stats;

} // namespace moss::kernel::syscall

// ============================================================================
// Section 3: Syscall arch types (from syscall_arch.hpp)
// ============================================================================
export namespace moss::kernel::arch::syscall {

// Syscall context structure (forward declaration, used as opaque pointer in assembly)
struct SyscallContext;

// Syscall return handler (wraps the extern "C" declaration from global fragment)
inline void do_syscall_return(SyscallContext* context) noexcept {
    ::syscall_return(static_cast<void*>(context));
}

// Unified syscall initialization interface
inline bool initialize_architecture_syscalls() noexcept {
#if defined(MOSS_ARCH_ARM64)
    // ARM64: Set up exception vector table to handle SVC instruction
    // TODO: Set up EL1 exception vector table, point SVC exception to syscall_entry_point
    return true;

#elif defined(MOSS_ARCH_X86_64)
    // X86_64: Set up SYSCALL instruction MSR registers
    // TODO: implement initialize_syscall_support() for x86_64
    return true;

#elif defined(MOSS_ARCH_RISCV)
    // RISC-V: Set up trap vector table to handle ECALL instruction
    // TODO: Set up stvec register to point to syscall_entry_point
    return true;

#else
    // Unsupported architecture
    return false;
#endif
}

// Architecture-specific syscall convention information
struct SyscallConvention {
    const char* arch_name;               // Architecture name
    const char* syscall_instruction;     // Syscall instruction
    const char* syscall_nr_register;     // Syscall number register
    const char* return_register;         // Return value register
    const char* arg_registers[6];        // Argument register list
};

inline const SyscallConvention& get_syscall_convention() noexcept {
#if defined(MOSS_ARCH_ARM64)
    static const SyscallConvention conv = {
        .arch_name = "ARM64",
        .syscall_instruction = "SVC",
        .syscall_nr_register = "x8",
        .return_register = "x0",
        .arg_registers = {"x0", "x1", "x2", "x3", "x4", "x5"}
    };
    return conv;

#elif defined(MOSS_ARCH_X86_64)
    static const SyscallConvention conv = {
        .arch_name = "x86_64",
        .syscall_instruction = "SYSCALL",
        .syscall_nr_register = "rax",
        .return_register = "rax",
        .arg_registers = {"rdi", "rsi", "rdx", "r10", "r8", "r9"}
    };
    return conv;

#elif defined(MOSS_ARCH_RISCV)
    static const SyscallConvention conv = {
        .arch_name = "RISC-V",
        .syscall_instruction = "ECALL",
        .syscall_nr_register = "a7",
        .return_register = "a0",
        .arg_registers = {"a0", "a1", "a2", "a3", "a4", "a5"}
    };
    return conv;

#else
    static const SyscallConvention conv = {
        .arch_name = "Unknown",
        .syscall_instruction = "Unknown",
        .syscall_nr_register = "Unknown",
        .return_register = "Unknown",
        .arg_registers = {"Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown"}
    };
    return conv;
#endif
}

// Debug: print current architecture's syscall convention
void print_syscall_convention() noexcept;

} // namespace moss::kernel::arch::syscall

// ============================================================================
// Section 4: Kernel class, SubsystemState, BootPhase, KernelStats
//            (from kernel_main.hpp)
// ============================================================================

export namespace moss::kernel {

namespace log = moss::kernel::logging;

// Kernel subsystem state
enum class SubsystemState : u8 {
  Uninitialized = 0,
  Initializing = 1,
  Active = 2,
  Error = 3
};

// Kernel boot phase
enum class BootPhase : u8 {
  EarlyInit = 0,      // Early initialization (after assembly)
  MemoryInit = 1,     // Memory management initialization
  SchedulerInit = 2,  // Scheduler initialization
  IpcInit = 3,        // IPC system initialization
  DeviceInit = 4,     // Device management initialization
  ServiceInit = 5,    // System service startup
  UserInit = 6,       // User-space initialization
  Completed = 7       // Boot completed
};

// Kernel statistics
struct KernelStats {
  u64 boot_time;           // Boot time
  u64 uptime;              // Uptime
  u64 total_memory;        // Total memory
  u64 free_memory;         // Free memory
  u32 active_processes;    // Active process count
  u32 total_threads;       // Total thread count
  u64 context_switches;    // Context switch count
  u64 interrupts_handled;  // Interrupts handled
  u64 ipc_messages;        // IPC message count
  u32 registered_devices;  // Registered device count
};

// Kernel main class
class Kernel {
private:
  // Boot state
  BootPhase current_phase_;
  [[maybe_unused]] SubsystemState subsystem_states_[8]; // Per-subsystem states

  // Core subsystem instances
  containers::ContainerLibrary *container_lib_;
  mm::PageTableManager *page_table_manager_;
  process::ProcessManager *process_manager_;
  process::CfsScheduler *scheduler_;
  process::LoadBalancer *load_balancer_;
  ipc::SharedMemoryManager *shared_memory_manager_;
  ipc::IpcManager *ipc_manager_;
  interrupts::GenericInterruptController *gic_;
  drivers::DeviceManager *device_manager_;

  // Boot time recording
  u64 boot_start_time_;
  u64 phase_start_times_[8];

  // Kernel configuration
  struct KernelConfig {
    bool enable_smp;              // Enable multi-core support
    bool enable_preemption;       // Enable preemptive scheduling
    u32 max_processes;            // Maximum process count
    u32 max_threads_per_process;  // Maximum threads per process
    usize kernel_heap_size;       // Kernel heap size
    bool enable_debug_output;     // Enable debug output
    u32 scheduler_timeslice_ms;   // Scheduling timeslice (milliseconds)
  } config_;

public:
  Kernel() noexcept
      : current_phase_(BootPhase::EarlyInit),
        subsystem_states_{SubsystemState::Uninitialized},
        container_lib_(nullptr), page_table_manager_(nullptr),
        process_manager_(nullptr), scheduler_(nullptr), load_balancer_(nullptr),
        shared_memory_manager_(nullptr), ipc_manager_(nullptr), gic_(nullptr),
        device_manager_(nullptr),
        boot_start_time_(0), phase_start_times_{0} {
    // Initialize kernel configuration
    config_ = {.enable_smp = true,
               .enable_preemption = true,
               .max_processes = 256,
               .max_threads_per_process = 16,
               .kernel_heap_size = 16 * 1024 * 1024, // 16MB
               .enable_debug_output = true,
               .scheduler_timeslice_ms = 10};
  }

  ~Kernel() noexcept { shutdown(); }

  // Non-copyable, non-movable
  Kernel(const Kernel&) = delete;
  Kernel& operator=(const Kernel&) = delete;
  Kernel(Kernel&&) = delete;
  Kernel& operator=(Kernel&&) = delete;

  // Kernel initialization main entry
  [[nodiscard]] VoidResult initialize() noexcept {
    boot_start_time_ = get_current_time();

    print_banner();

    // Initialize kernel phase by phase
    auto result = initialize_phase_by_phase();
    if (!result) {
      kernel_panic("Kernel initialization failed", result.error());
    }

    current_phase_ = BootPhase::Completed;

    u64 boot_time = get_current_time() - boot_start_time_;
    log::klog::info("MOSS kernel boot completed (time: {} cycles)", boot_time);

    return VoidResult{};
  }

  // Kernel main loop
  [[nodiscard]] VoidResult run() noexcept {
    log::klog::info("MOSS kernel starting...");

    // Enable interrupts
    enable_interrupts();

    // Create initial user process
    auto init_result = create_init_process();
    if (!init_result) {
      kernel_panic("Failed to create init process", init_result.error());
    }

    // Enter scheduling loop (start_scheduling is [[noreturn]])
    log::klog::info("Entering scheduling loop");
    scheduler_->start_scheduling();
  }

  // Kernel shutdown
  void shutdown() noexcept {
    log::klog::info("MOSS kernel shutting down...");

    // Shutdown subsystems in reverse order
    if (device_manager_) {
      (void)device_manager_->suspend_all_devices();
      delete device_manager_;
      device_manager_ = nullptr;
    }

    if (ipc_manager_) {
      delete ipc_manager_;
      ipc_manager_ = nullptr;
    }

    if (shared_memory_manager_) {
      delete shared_memory_manager_;
      shared_memory_manager_ = nullptr;
    }

    if (scheduler_) {
      delete scheduler_;
      scheduler_ = nullptr;
    }

    if (process_manager_) {
      delete process_manager_;
      process_manager_ = nullptr;
    }

    if (page_table_manager_) {
      delete page_table_manager_;
      page_table_manager_ = nullptr;
    }

    if (container_lib_) {
      containers::ContainerLibrary::cleanup();
    }

    // Shutdown memory management system last
    log::klog::info("Shutting down unified memory management system...");
    mm::shutdown_kernel_memory();

    log::klog::info("MOSS kernel shutdown complete");
  }

  // Get kernel statistics
  [[nodiscard]] KernelStats get_statistics() const noexcept {
    KernelStats stats = {};

    stats.boot_time = boot_start_time_;
    stats.uptime = get_current_time() - boot_start_time_;

    if (process_manager_) {
      // stats.active_processes = process_manager_->get_process_count();
    }

    if (gic_) {
      auto gic_stats = gic_->get_statistics();
      stats.interrupts_handled = gic_stats.total_interrupts;
    }

    if (ipc_manager_) {
      auto ipc_stats = ipc_manager_->get_statistics();
      stats.ipc_messages = ipc_stats.messages_processed;
    }

    if (device_manager_) {
      auto dev_stats = device_manager_->get_statistics();
      stats.registered_devices = dev_stats.total_devices;
    }

    return stats;
  }

  // Kernel debug interface
  void print_system_info() const noexcept {
    log::klog::info("=== MOSS Kernel System Info ===");
    log::klog::info("Boot phase: {}", static_cast<int>(current_phase_));
    log::klog::info("SMP support: {}", config_.enable_smp ? "enabled" : "disabled");
    log::klog::info("Preemptive scheduling: {}", config_.enable_preemption ? "enabled" : "disabled");

    auto stats = get_statistics();
    log::klog::info("Uptime: {} cycles", stats.uptime);
    log::klog::info("Active processes: {}", stats.active_processes);
    log::klog::info("Interrupts handled: {}", stats.interrupts_handled);
    log::klog::info("IPC messages: {}", stats.ipc_messages);
    log::klog::info("Registered devices: {}", stats.registered_devices);
    log::klog::info("===============================");
  }

private:
  // Phase-by-phase initialization
  [[nodiscard]] VoidResult initialize_phase_by_phase() noexcept {
    const char *phase_names[] = {"Early init", "Memory management", "Scheduler",
                                 "IPC system", "Device management", "System services",
                                 "User-space", "Complete"};

    for (int phase = 0; phase < 7; ++phase) {
      current_phase_ = static_cast<BootPhase>(phase);
      phase_start_times_[phase] = get_current_time();

      log::klog::info("Phase {}: {}", phase, phase_names[phase]);

      VoidResult result = VoidResult{ErrorCode::NotSupported};

      switch (current_phase_) {
      case BootPhase::EarlyInit:
        result = initialize_early();
        break;
      case BootPhase::MemoryInit:
        result = initialize_memory();
        break;
      case BootPhase::SchedulerInit:
        result = initialize_scheduler();
        break;
      case BootPhase::IpcInit:
        result = initialize_ipc();
        break;
      case BootPhase::DeviceInit:
        result = initialize_devices();
        break;
      case BootPhase::ServiceInit:
        result = initialize_services();
        break;
      case BootPhase::UserInit:
        result = initialize_userspace();
        break;
      case BootPhase::Completed:
        result = VoidResult{};
        break;
      default:
        break;
      }

      if (!result) {
        log::klog::error("Phase {} failed: {}", phase,
                        static_cast<int>(result.error()));
        return result;
      }

      u64 phase_time = get_current_time() - phase_start_times_[phase];
      log::klog::info("Phase {} complete (time: {} cycles)", phase, phase_time);
    }

    return VoidResult{};
  }

  // Early initialization
  [[nodiscard]] VoidResult initialize_early() noexcept {
    // Initialize container library
    if (!containers::ContainerLibrary::initialize()) {
      return VoidResult{ErrorCode::InternalError};
    }
    container_lib_ =
        nullptr; // ContainerLibrary is a singleton/static, no instance needed

    // Initialize timer subsystem (clocksource + hardware timer)
    log::klog::info("Initializing timer subsystem...");
    auto timer_result = timer::TimerSubsystem::instance().initialize();
    if (!timer_result) {
      log::klog::warn("Timer subsystem init failed (non-fatal)");
      // Non-fatal: kernel can operate without timer, just no preemption
    } else {
      auto freq = timer::TimerSubsystem::instance().clocksource().frequency_hz();
      log::klog::info("Timer subsystem initialized: freq={} Hz", freq);
    }

    return VoidResult{};
  }

  // Memory management initialization
  [[nodiscard]] VoidResult initialize_memory() noexcept {
    log::klog::info("Initializing unified memory management system...");

    // Initialize unified memory management system first
    if (!mm::initialize_kernel_memory()) {
      log::klog::error("Unified memory management system initialization failed");
      return VoidResult{ErrorCode::InternalError};
    }
    log::klog::info("Unified memory management system initialized");

    // Create page table manager (now uses new memory management system)
    page_table_manager_ = new mm::PageTableManager();
    if (!page_table_manager_) {
      log::klog::error("PageTableManager creation failed");
      mm::shutdown_kernel_memory();
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Initialize page table manager (use current page table)
    auto init_result = page_table_manager_->initialize_from_current();
    if (!init_result) {
      log::klog::error("PageTableManager initialization failed");
      delete page_table_manager_;
      page_table_manager_ = nullptr;
      mm::shutdown_kernel_memory();
      return init_result;
    }

    // Check memory system health
    if (!mm::is_memory_system_healthy()) {
      log::klog::warn("Memory system status abnormal");
    }

    // Print memory system information
    auto pressure = mm::get_memory_pressure();
    const char* pressure_str = "UNKNOWN";
    switch (pressure) {
      case mm::MemoryPressure::LOW: pressure_str = "LOW"; break;
      case mm::MemoryPressure::MEDIUM: pressure_str = "MEDIUM"; break;
      case mm::MemoryPressure::HIGH: pressure_str = "HIGH"; break;
      case mm::MemoryPressure::CRITICAL: pressure_str = "CRITICAL"; break;
      default: pressure_str = "UNKNOWN"; break;
    }
    log::klog::info("Memory pressure level: {}", pressure_str);

    return VoidResult{};
  }

  // Scheduler initialization
  [[nodiscard]] VoidResult initialize_scheduler() noexcept {
    // Create process manager
    process_manager_ = new process::ProcessManager();
    if (!process_manager_) {
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Set global process manager pointer
    ::moss::kernel::process::g_process_manager = process_manager_;

    // Create CFS scheduler
    scheduler_ = new process::CfsScheduler();
    if (!scheduler_) {
      delete process_manager_;
      process_manager_ = nullptr;
      ::moss::kernel::process::g_process_manager = nullptr;
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Set global scheduler pointer
    ::moss::kernel::process::g_scheduler = scheduler_;

    // Create load balancer
    load_balancer_ = new process::LoadBalancer();
    ::moss::kernel::process::g_load_balancer = load_balancer_;
    if (!load_balancer_) {
      delete scheduler_;
      delete process_manager_;
      scheduler_ = nullptr;
      process_manager_ = nullptr;
      ::moss::kernel::process::g_scheduler = nullptr;
      ::moss::kernel::process::g_load_balancer = nullptr;
      ::moss::kernel::process::g_process_manager = nullptr;
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Linux-style SMP delayed activation: activate secondary CPUs after scheduler is ready
    if (config_.enable_smp) {
      log::klog::info("Scheduler ready, activating parked secondary CPUs...");

      // Activate all parked secondary CPUs
      moss::boot::activate_secondary_cpus();

      // Wait for secondary CPUs to complete activation
      u32 active_cpus = moss::boot::wait_for_all_cpus_active(5000);

      log::klog::info("CPU activation complete: {} CPUs active", active_cpus);

      if (active_cpus > 1) {
        log::klog::info("Linux-style multi-CPU scheduler started successfully!");
      } else {
        log::klog::info("Falling back to single-core mode");
      }
    }

    return VoidResult{};
  }

  // IPC system initialization
  [[nodiscard]] VoidResult initialize_ipc() noexcept {
    // Create shared memory manager
    shared_memory_manager_ = new ipc::SharedMemoryManager();
    if (!shared_memory_manager_) {
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Set global shared memory manager pointer
    ::moss::kernel::ipc::g_shared_memory_manager = shared_memory_manager_;

    // Create IPC manager
    ipc_manager_ = new ipc::IpcManager(shared_memory_manager_);
    if (!ipc_manager_) {
      delete shared_memory_manager_;
      shared_memory_manager_ = nullptr;
      ::moss::kernel::ipc::g_shared_memory_manager = nullptr;
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Set global IPC manager pointer
    ::moss::kernel::ipc::g_ipc_manager = ipc_manager_;

    return VoidResult{};
  }

  // Device management initialization
  [[nodiscard]] VoidResult initialize_devices() noexcept {
    // Create GIC interrupt controller
    gic_ = new interrupts::GenericInterruptController();
    if (!gic_) {
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // 从 DTB 解析结果获取 GIC 地址，若 DTB 无效则回退到 QEMU virt 默认值
    const auto &plat = ::moss::fdt::get_platform_info();
    VirtAddr gic_dist_base = (plat.dtb_valid && plat.intc.valid)
                                 ? static_cast<VirtAddr>(plat.intc.dist_base)
                                 : platform::intc_dist_base();
    VirtAddr gic_cpu_base = (plat.dtb_valid && plat.intc.valid)
                                ? static_cast<VirtAddr>(plat.intc.cpu_base)
                                : platform::intc_cpu_base();

    auto gic_result = gic_->initialize(gic_dist_base, gic_cpu_base);
    if (!gic_result) {
      delete gic_;
      gic_ = nullptr;
      return gic_result;
    }

    // Set global GIC pointer so other subsystems can access it
    ::moss::kernel::interrupts::g_gic = gic_;

    // Create device manager
    device_manager_ = new drivers::DeviceManager();
    if (!device_manager_) {
      delete gic_;
      gic_ = nullptr;
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Initialize multi-architecture syscall support
    log::klog::info("Initializing multi-architecture syscall support...");
    if (!arch::syscall::initialize_architecture_syscalls()) {
        log::klog::error("Syscall architecture initialization failed");
        return VoidResult{ErrorCode::NotSupported};
    }
    log::klog::info("Syscall architecture initialization succeeded");

    // Print syscall architecture information
    arch::syscall::print_syscall_convention();

    return VoidResult{};
  }

  // System service initialization
  [[nodiscard]] VoidResult initialize_services() noexcept {
    // Kernel service threads can be started here
    // e.g., memory reclamation, timer services, etc.
    return VoidResult{};
  }

  // User-space initialization
  [[nodiscard]] VoidResult initialize_userspace() noexcept {
    // Prepare user-space environment
    // Set up user-mode page tables, load initial programs, etc.
    return VoidResult{};
  }

  // Create initial user-mode process (TID=1000, the "init" process).
  //
  // The scheduler's context_switch_to_task() recognises TID=1000 and calls
  // switch_to_user(), which performs an eret to EL0. The user program is
  // embedded directly in the kernel binary (arm64_user_program.S).
  [[nodiscard]] VoidResult create_init_process() noexcept {
#if defined(MOSS_ARCH_ARM64)
    using namespace process;

    log::klog::info("creating init user process (PID=1, TID=1000)");

    // Step 1: Create process
    if (!process_manager_) {
      log::klog::error("process manager not initialized");
      return VoidResult{ErrorCode::InvalidState};
    }
    auto proc_result = process_manager_->create_process(0);
    if (!proc_result) {
      log::klog::error("failed to create init process");
      return VoidResult{proc_result.error()};
    }
    Process *init_proc = proc_result.value();
    ProcessId init_pid = init_proc->pid();
    log::klog::info("init process registered: PID={}", init_pid);

    // Step 2: Create real AddressSpace with buddy-allocated PGD
    auto as_result = user_space::create_user_address_space();
    if (!as_result) {
      log::klog::error("failed to create address space for init process");
      return VoidResult{as_result.error()};
    }
    auto as = moss::move(*as_result);

    // Step 3: Register VMA regions for demand paging
    //
    // The embedded user program is raw machine code (not ELF).
    // We place it at a fixed user virtual address and register as a code VMA
    // with backing data pointing to the kernel-resident copy.
    constexpr VirtAddr USER_CODE_BASE = 0x0000000200000000ULL;  // 8GB — above kernel identity map
    const auto* raw_code = reinterpret_cast<const u8*>(_user_program_start);
    usize code_size = static_cast<usize>(
        reinterpret_cast<VirtAddr>(_user_program_end) -
        reinterpret_cast<VirtAddr>(_user_program_start));

    // Code VMA: readable + executable, backed by the embedded raw program
    VirtAddr code_end = (USER_CODE_BASE + code_size + PAGE_SIZE - 1)
                        & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1);
    as->add_vma(USER_CODE_BASE, code_end,
                VmaFlags::READ | VmaFlags::EXEC,
                VmaType::CODE,
                raw_code, 0, code_size);
    log::klog::info("  VMA code: {:#x}-{:#x} backing={} bytes",
                    USER_CODE_BASE, code_end, code_size);

    VirtAddr entry_point = USER_CODE_BASE;  // entry = start of raw code

    // Stack VMA: 8 pages (32KB) at user stack area, demand-zero
    constexpr VirtAddr USER_STACK_TOP = 0x00007FFF00000000ULL;
    constexpr usize USER_STACK_SIZE = 32 * 1024;  // 32KB
    constexpr VirtAddr USER_STACK_BOTTOM = USER_STACK_TOP - USER_STACK_SIZE;
    as->add_vma(USER_STACK_BOTTOM, USER_STACK_TOP,
                VmaFlags::READ | VmaFlags::WRITE | VmaFlags::DEMAND_ZERO,
                VmaType::STACK);
    log::klog::info("  VMA stack: {:#x}-{:#x}", USER_STACK_BOTTOM, USER_STACK_TOP);

    // Heap VMA: small initial region, demand-zero
    constexpr VirtAddr USER_HEAP_START = 0x0000000100000000ULL;
    constexpr usize USER_HEAP_INIT_SIZE = 64 * 1024;
    as->add_vma(USER_HEAP_START, USER_HEAP_START + USER_HEAP_INIT_SIZE,
                VmaFlags::READ | VmaFlags::WRITE | VmaFlags::DEMAND_ZERO,
                VmaType::HEAP);

    // Bind AddressSpace to process
    auto set_result = init_proc->set_address_space(moss::move(as));
    if (!set_result) {
      log::klog::error("failed to set address space on process");
      return VoidResult{set_result.error()};
    }

    // Step 4: Create thread with user-space entry point
    auto *init_thread = new Thread(1000, init_pid);
    if (!init_thread) {
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // User context: entry point and stack pointer are user-space VAs
    // (demand-paged on first access)
    init_thread->stack_base = USER_STACK_BOTTOM;
    init_thread->stack_size = USER_STACK_SIZE;
    init_thread->context.pc = entry_point;
    init_thread->context.sp = USER_STACK_TOP - 16;  // 16-byte aligned
    init_thread->context.pstate = 0x00000000;  // EL0t
    init_thread->needs_initial_eret = true;  // First dispatch uses switch_to_user + eret

    init_thread->sched_class = SchedClass::Normal;
    init_thread->se.nice = -5;
    init_thread->se.weight = CfsParams::nice_to_weight(-5);
    init_thread->se.vruntime = 1;
    init_thread->state = ProcessState::Ready;

    init_proc->set_state(ProcessState::Running);

    // Step 5: Enqueue into scheduler
    scheduler_->enqueue_task(init_thread, 0);

    log::klog::info("init process TID=1000: entry={:#x} stack={:#x}-{:#x} pgd={:#x} asid={}",
                    entry_point, USER_STACK_BOTTOM, USER_STACK_TOP,
                    init_proc->address_space()->pgd_phys,
                    init_proc->address_space()->asid);
    (void)code_size;
#else
    log::klog::info("user process creation not yet supported on this architecture");
#endif
    return VoidResult{};
  }

  // Enable interrupts
  void enable_interrupts() noexcept {
    arch::enable_interrupts();
  }

  // Kernel panic handler
  [[noreturn]] void kernel_panic(const char *message,
                                 ErrorCode error) noexcept {
    arch::disable_interrupts();

    log::klog::panic("KERNEL PANIC");
    log::klog::panic("Error: {}", message);
    log::klog::panic("Error code: {}", static_cast<int>(error));
    log::klog::panic("Current phase: {}", static_cast<int>(current_phase_));

    // Print call stack
    print_stack_trace();

    // Halt
    while (true) {
      arch::cpu_halt();
    }
  }

  // Print boot banner
  void print_banner() const noexcept {
    log::klog::info("");
    log::klog::info("MOSS Hybrid Kernel v1.0");
    log::klog::info("");
  }

  // Print call stack
  void print_stack_trace() const noexcept {
    log::klog::panic("Stack trace:");

    u64 fp = arch::get_frame_pointer();

    for (int i = 0; i < 10 && fp != 0; i++) {
      u64 *frame = reinterpret_cast<u64 *>(fp);
      if (frame != nullptr) {
        u64 lr = frame[1]; // Return address
        fp = frame[0];     // Next frame pointer

        log::klog::panic("  [{}] {:#x}", i, lr);
      } else {
        break;
      }
    }
  }

  // Get current time
  [[nodiscard]] static u64 get_current_time() noexcept {
    return arch::get_timestamp_counter();
  }

};

// Global kernel instance
extern Kernel *g_kernel;

} // namespace moss::kernel
