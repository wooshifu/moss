// MOSS Kernel Module - Syscall Table Partition
// Syscall number enumeration, handler types, dispatcher, and statistics.

export module moss.kernel:syscall_table;

import moss.std;
import moss.types;

export namespace moss::kernel::syscall {

// Linux-compatible errno constants.
// Syscall handlers return the negated value (e.g. -EINVAL).
namespace errc {
inline constexpr long EPERM = 1;         // Operation not permitted
inline constexpr long ENOENT = 2;        // No such file or directory
inline constexpr long ESRCH = 3;         // No such process
inline constexpr long EINTR = 4;         // Interrupted system call
inline constexpr long EIO = 5;           // I/O error
inline constexpr long ENOEXEC = 8;       // Exec format error
inline constexpr long EBADF = 9;         // Bad file descriptor
inline constexpr long ECHILD = 10;       // No child processes
inline constexpr long EAGAIN = 11;       // Try again / resource temporarily unavailable
inline constexpr long ENOMEM = 12;       // Out of memory
inline constexpr long EACCES = 13;       // Permission denied
inline constexpr long EFAULT = 14;       // Bad address
inline constexpr long EEXIST = 17;       // File exists
inline constexpr long ENOTDIR = 20;      // Not a directory
inline constexpr long EISDIR = 21;       // Is a directory
inline constexpr long EINVAL = 22;       // Invalid argument
inline constexpr long EMFILE = 24;       // Too many open files
inline constexpr long ESPIPE = 29;       // Illegal seek (pipe)
inline constexpr long ENAMETOOLONG = 36; // File name too long
inline constexpr long ENOSYS = 38;       // Function not implemented
} // namespace errc

// Standard POSIX file descriptor numbers
namespace fd_num {
inline constexpr long STDIN = 0;
inline constexpr long STDOUT = 1;
inline constexpr long STDERR = 2;
} // namespace fd_num

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
  SYS_SCHED_YIELD = 18,
  SYS_SCHED_GETAFFINITY = 19,
  SYS_SCHED_SETAFFINITY = 20,
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
using SyscallHandler = long (*)(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

// Syscall descriptor
struct SyscallDescriptor {
  const char *name;        // Syscall name
  SyscallHandler handler;  // Handler function pointer
  u8 arg_count;            // Argument count
  bool implemented;        // Whether implemented
  const char *description; // Functionality description

  constexpr SyscallDescriptor() noexcept
      : name(nullptr), handler(nullptr), arg_count(0), implemented(false), description(nullptr) {}

  constexpr SyscallDescriptor(const char *n, SyscallHandler h, u8 argc, bool impl, const char *desc) noexcept
      : name(n), handler(h), arg_count(argc), implemented(impl), description(desc) {}
};

// Syscall handler function declarations
namespace handlers {
// Basic syscalls
long sys_debug_print(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_exit(long exit_code, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_getpid(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_getppid(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_getuid(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_getgid(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

// Process management - framework implementation
long sys_fork(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_execve(long pathname_addr, long argv_addr, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_wait4(long wait_pid, long wstatus_addr, long options, long arg3, long arg4, long arg5) noexcept;
long sys_waitpid(long pid, long wstatus, long options, long arg3, long arg4, long arg5) noexcept;
long sys_kill(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

// Filesystem - framework implementation
long sys_open(long pathname_addr, long flags, long mode, long arg3, long arg4, long arg5) noexcept;
long sys_close(long fd, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_read(long fd, long buf_addr, long count, long arg3, long arg4, long arg5) noexcept;
long sys_write(long fd, long buf_addr, long count, long arg3, long arg4, long arg5) noexcept;
long sys_lseek(long fd, long offset, long whence, long arg3, long arg4, long arg5) noexcept;
long sys_fstat(long fd, long stat_buf_addr, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_dup(long oldfd, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_dup2(long oldfd, long newfd, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_pipe(long pipefd_addr, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

// Memory management
long sys_mmap(long addr, long length, long prot, long flags, long fd, long offset) noexcept;
long sys_munmap(long addr, long length, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_mprotect(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_brk(long addr, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

// Network communication - framework implementation
long sys_socket(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_bind(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_listen(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_accept(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

// Scheduling syscalls
long sys_nice(long increment, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_getpriority(long which, long who, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_sched_yield(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_sched_getaffinity(long pid_arg, long arg1, long mask_addr, long arg3, long arg4, long arg5) noexcept;
long sys_sched_setaffinity(long pid_arg, long arg1, long mask_addr, long arg3, long arg4, long arg5) noexcept;

// Time syscalls
long sys_clock_gettime(long arg0, long time_ns_addr, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_nanosleep(long ns_addr, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

// System monitoring
long sys_topinfo(long info_addr, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

// Default handler for unimplemented syscalls
long sys_not_implemented(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
} // namespace handlers

// Global syscall table
extern const SyscallDescriptor SYSCALL_TABLE[static_cast<int>(SyscallNumber::MAX_SYSCALL)];

// Syscall dispatcher
class SyscallDispatcher {
public:
  // Dispatch a syscall
  static long dispatch(long syscall_number, long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

  // Get syscall information
  static const SyscallDescriptor *get_syscall_info(long syscall_number) noexcept;

  // Check whether a syscall is implemented
  static bool is_implemented(long syscall_number) noexcept;

  // Validate syscall number
  static bool is_valid_syscall(long syscall_number) noexcept;

  // Get syscall statistics
  static void get_syscall_stats(u64 *total_calls, u64 *implemented_calls) noexcept;

  // Debug: print all implemented syscalls
  static void print_implemented_syscalls() noexcept;
};

// Syscall statistics
struct SyscallStats {
  u64 total_syscalls;         // Total invocation count
  u64 successful_syscalls;    // Successful invocation count
  u64 failed_syscalls;        // Failed invocation count
  u64 unimplemented_syscalls; // Unimplemented invocation count
  u64 invalid_syscalls;       // Invalid invocation count
};

// Global syscall statistics
extern SyscallStats g_syscall_stats;

} // namespace moss::kernel::syscall
