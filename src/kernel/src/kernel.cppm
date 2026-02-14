// MOSS Kernel Module - ELF Loader, Syscall Table, Syscall Arch, Kernel Main
// Merges kernel_main.hpp, syscall_table.hpp, elf_loader.hpp, syscall_arch.hpp
// into a single C++26 module.

module;

// Architecture detection
#include "arch_detect.h"

// Kernel environment va_list support (must be in global module fragment)
#ifdef __GNUC__
typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v, l) __builtin_va_arg(v, l)
#else
#error "Unsupported compiler for va_list"
#endif

// extern "C" declarations (global module fragment)
extern "C" {
void kernel_test_all_subsystems(void) noexcept;
void early_debug_print(const char *message) noexcept;

// Syscall arch assembly function declarations
void syscall_entry_point() noexcept;
long system_call_handler(long syscall_number, long arg0, long arg1,
                         long arg2, long arg3, long arg4, long arg5) noexcept;
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
import moss.containers;
import moss.mm;
import moss.interrupts;
import moss.drivers;
import moss.fdt;
import moss.ipc;
import moss.process;
import moss.timer;
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
    kernel_print("MOSS kernel boot completed (time: %llu cycles)\n", boot_time);

    return VoidResult{};
  }

  // Kernel main loop
  [[nodiscard]] VoidResult run() noexcept {
    kernel_print("MOSS kernel starting...\n");

    // Enable interrupts
    enable_interrupts();

    // Create initial user process
    auto init_result = create_init_process();
    if (!init_result) {
      kernel_panic("Failed to create init process", init_result.error());
    }

    // Enter scheduling loop (start_scheduling is [[noreturn]])
    kernel_print("Entering scheduling loop\n");
    scheduler_->start_scheduling();
  }

  // Kernel shutdown
  void shutdown() noexcept {
    kernel_print("MOSS kernel shutting down...\n");

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
    kernel_print("Shutting down unified memory management system...\n");
    mm::shutdown_kernel_memory();

    kernel_print("MOSS kernel shutdown complete\n");
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
    kernel_print("=== MOSS Kernel System Info ===\n");
    kernel_print("Boot phase: %d\n", static_cast<int>(current_phase_));
    kernel_print("SMP support: %s\n", config_.enable_smp ? "enabled" : "disabled");
    kernel_print("Preemptive scheduling: %s\n", config_.enable_preemption ? "enabled" : "disabled");

    auto stats = get_statistics();
    kernel_print("Uptime: %llu cycles\n", stats.uptime);
    kernel_print("Active processes: %u\n", stats.active_processes);
    kernel_print("Interrupts handled: %llu\n", stats.interrupts_handled);
    kernel_print("IPC messages: %llu\n", stats.ipc_messages);
    kernel_print("Registered devices: %u\n", stats.registered_devices);
    kernel_print("===============================\n");
  }

private:
  // Test kernel_print formatting
  void test_kernel_print_formatting() noexcept {
    kernel_print("=== kernel_print formatting test ===\n");

    // Test all major format specifiers
    kernel_print("%%d test: %d\n", -12345);
    kernel_print("%%u test: %u\n", 54321u);
    kernel_print("%%x test: %x\n", 0xabcd);
    kernel_print("%%X test: %X\n", 0xABCD);
    kernel_print("%%s test: %s\n", "Hello MOSS!");
    kernel_print("%%c test: %c\n", 'M');

    // Key test: %llu format (previous bug)
    u64 large_number = 0x123456789ABCDEF0ULL;
    kernel_print("%%llu test: %llu\n", large_number);
    kernel_print("%%llx test: %llx\n", large_number);
    kernel_print("%%llX test: %llX\n", large_number);

    // Pointer format test
    void* test_ptr = reinterpret_cast<void*>(0x40080000);
    kernel_print("%%p test: %p\n", test_ptr);

    // Edge cases
    kernel_print("Zero value test: %llu\n", 0ULL);
    kernel_print("Max value test: %llu\n", static_cast<u64>(-1));

    kernel_print("=== kernel_print test complete ===\n");
  }

  // Phase-by-phase initialization
  [[nodiscard]] VoidResult initialize_phase_by_phase() noexcept {
    const char *phase_names[] = {"Early init", "Memory management", "Scheduler",
                                 "IPC system", "Device management", "System services",
                                 "User-space", "Complete"};

    for (int phase = 0; phase < 7; ++phase) {
      current_phase_ = static_cast<BootPhase>(phase);
      phase_start_times_[phase] = get_current_time();

      kernel_print("Phase %d: %s\n", phase, phase_names[phase]);

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
        kernel_print("Phase %d failed: %d\n", phase,
                     static_cast<int>(result.error()));
        return result;
      }

      u64 phase_time = get_current_time() - phase_start_times_[phase];
      kernel_print("Phase %d complete (time: %llu cycles)\n", phase, phase_time);
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
    kernel_print("Initializing timer subsystem...\n");
    auto timer_result = timer::TimerSubsystem::instance().initialize();
    if (!timer_result) {
      kernel_print("Timer subsystem init failed (non-fatal)\n");
      // Non-fatal: kernel can operate without timer, just no preemption
    } else {
      auto freq = timer::TimerSubsystem::instance().clocksource().frequency_hz();
      kernel_print("Timer subsystem initialized: freq=%llu Hz\n", freq);
    }

    return VoidResult{};
  }

  // Memory management initialization
  [[nodiscard]] VoidResult initialize_memory() noexcept {
    kernel_print("Initializing unified memory management system...\n");

    // Initialize unified memory management system first
    if (!mm::initialize_kernel_memory()) {
      kernel_print("Unified memory management system initialization failed\n");
      return VoidResult{ErrorCode::InternalError};
    }
    kernel_print("Unified memory management system initialized\n");

    // Create page table manager (now uses new memory management system)
    page_table_manager_ = new mm::PageTableManager();
    if (!page_table_manager_) {
      kernel_print("PageTableManager creation failed\n");
      mm::shutdown_kernel_memory();
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Initialize page table manager (use current page table)
    auto init_result = page_table_manager_->initialize_from_current();
    if (!init_result) {
      kernel_print("PageTableManager initialization failed\n");
      delete page_table_manager_;
      page_table_manager_ = nullptr;
      mm::shutdown_kernel_memory();
      return init_result;
    }

    // Check memory system health
    if (!mm::is_memory_system_healthy()) {
      kernel_print("Memory system status abnormal\n");
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
    kernel_print("Memory pressure level: %s\n", pressure_str);

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
    if (!load_balancer_) {
      delete scheduler_;
      delete process_manager_;
      scheduler_ = nullptr;
      process_manager_ = nullptr;
      ::moss::kernel::process::g_scheduler = nullptr;
      ::moss::kernel::process::g_process_manager = nullptr;
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // Linux-style SMP delayed activation: activate secondary CPUs after scheduler is ready
    if (config_.enable_smp) {
      kernel_print("Scheduler ready, activating parked secondary CPUs...\n");

      // Activate all parked secondary CPUs
      moss::boot::activate_secondary_cpus();

      // Wait for secondary CPUs to complete activation
      u32 active_cpus = moss::boot::wait_for_all_cpus_active(5000);

      kernel_print("CPU activation complete: %u CPUs active\n", active_cpus);

      if (active_cpus > 1) {
        kernel_print("Linux-style multi-CPU scheduler started successfully!\n");
      } else {
        kernel_print("Falling back to single-core mode\n");
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
    kernel_print("Initializing multi-architecture syscall support...\n");
    if (!arch::syscall::initialize_architecture_syscalls()) {
        kernel_print("Syscall architecture initialization failed\n");
        return VoidResult{ErrorCode::NotSupported};
    }
    kernel_print("Syscall architecture initialization succeeded\n");

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

  // Create initial process
  [[nodiscard]] VoidResult create_init_process() noexcept {
    // Simplified implementation: create kernel thread as initial "process"
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

    kernel_print("\nKERNEL PANIC\n");
    kernel_print("Error: %s\n", message);
    kernel_print("Error code: %d\n", static_cast<int>(error));
    kernel_print("Current phase: %d\n", static_cast<int>(current_phase_));

    // Print call stack
    print_stack_trace();

    // Halt
    while (true) {
      arch::cpu_halt();
    }
  }

  // Print boot banner
  void print_banner() const noexcept {
    kernel_print("\n");
    kernel_print("MOSS Hybrid Kernel v1.0\n");
    kernel_print("\n");
  }

  // Print call stack
  void print_stack_trace() const noexcept {
    kernel_print("Stack trace:\n");

    u64 fp = arch::get_frame_pointer();

    for (int i = 0; i < 10 && fp != 0; i++) {
      u64 *frame = reinterpret_cast<u64 *>(fp);
      if (frame != nullptr) {
        u64 lr = frame[1]; // Return address
        fp = frame[0];     // Next frame pointer

        kernel_print("  [%d] 0x%016llx\n", i, lr);
      } else {
        break;
      }
    }
  }

  // Get current time
  [[nodiscard]] static u64 get_current_time() noexcept {
    return arch::get_timestamp_counter();
  }

  // Number formatting helper functions
  static void print_signed_number(i64 num) noexcept {
    if (num < 0) {
      uart_putc('-');
      print_unsigned_number(static_cast<u64>(-num), 10);
    } else {
      print_unsigned_number(static_cast<u64>(num), 10);
    }
  }

  static void print_unsigned_number(u64 num, u32 base, bool uppercase = false) noexcept {
    if (num == 0) {
      uart_putc('0');
      return;
    }

    char buffer[32];
    u32 idx = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    while (num > 0) {
      buffer[idx++] = digits[num % base];
      num /= base;
    }

    for (i32 i = static_cast<i32>(idx) - 1; i >= 0; i--) {
      uart_putc(buffer[i]);
    }
  }

  // Kernel print function - full implementation
  static void kernel_print(const char *format, ...) noexcept {
    if (format == nullptr) return;

    va_list args;
    va_start(args, format);

    const char *ptr = format;
    while (*ptr) {
      if (*ptr == '\n') {
        uart_putc('\r');
        uart_putc('\n');
      } else if (*ptr == '%') {
        ptr++;
        if (*ptr == '\0') break;

        // Parse length modifiers
        bool is_long = false;
        bool is_long_long = false;

        if (*ptr == 'l') {
          is_long = true;
          ptr++;
          if (*ptr == 'l') {
            is_long_long = true;
            ptr++;
          }
        }

        // Handle format specifiers
        switch (*ptr) {
        case 'd': {
          if (is_long_long) {
            signed long long value = va_arg(args, signed long long);
            print_signed_number(static_cast<i64>(value));
          } else {
            int value = va_arg(args, int);
            print_signed_number(static_cast<i64>(value));
          }
          break;
        }
        case 'u': {
          if (is_long_long) {
            unsigned long long value = va_arg(args, unsigned long long);
            print_unsigned_number(static_cast<u64>(value), 10);
          } else {
            unsigned int value = va_arg(args, unsigned int);
            print_unsigned_number(static_cast<u64>(value), 10);
          }
          break;
        }
        case 'x': {
          if (is_long_long) {
            unsigned long long value = va_arg(args, unsigned long long);
            print_unsigned_number(static_cast<u64>(value), 16);
          } else {
            unsigned int value = va_arg(args, unsigned int);
            print_unsigned_number(static_cast<u64>(value), 16);
          }
          break;
        }
        case 'X': {
          if (is_long_long) {
            unsigned long long value = va_arg(args, unsigned long long);
            print_unsigned_number(static_cast<u64>(value), 16, true);
          } else {
            unsigned int value = va_arg(args, unsigned int);
            print_unsigned_number(static_cast<u64>(value), 16, true);
          }
          break;
        }
        case 's': {
          const char *str = va_arg(args, char*);
          if (str) {
            while (*str) {
              uart_putc(*str);
              str++;
            }
          } else {
            const char *null_str = "(null)";
            while (*null_str) {
              uart_putc(*null_str);
              null_str++;
            }
          }
          break;
        }
        case 'c': {
          char ch = static_cast<char>(va_arg(args, int));
          uart_putc(ch);
          break;
        }
        case 'p': {
          void *ptr_val = va_arg(args, void*);
          uart_putc('0');
          uart_putc('x');
          print_unsigned_number(reinterpret_cast<u64>(ptr_val), 16);
          break;
        }
        case '%': {
          uart_putc('%');
          break;
        }
        default:
          // Unsupported format
          uart_putc('%');
          if (is_long_long) {
            uart_putc('l');
            uart_putc('l');
          } else if (is_long) {
            uart_putc('l');
          }
          uart_putc(*ptr);
          break;
        }
      } else {
        uart_putc(*ptr);
      }
      ptr++;
    }

    va_end(args);
  }

  // UART output — delegates to HAL for architecture-specific implementation
  static void uart_putc(char c) noexcept {
    hal::uart::putc(c);
  }
};

// Global kernel instance
extern Kernel *g_kernel;

} // namespace moss::kernel
