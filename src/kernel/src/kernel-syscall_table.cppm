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
inline constexpr long E2BIG = 7;         // Argument list too long
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
inline constexpr long EPIPE = 32;        // Peer closed
inline constexpr long ESPIPE = 29;       // Illegal seek (pipe)
inline constexpr long ENAMETOOLONG = 36; // File name too long
inline constexpr long ENOSYS = 38;       // Function not implemented
inline constexpr long ETIMEDOUT = 110;   // Deadline expired
} // namespace errc

// Standard POSIX file descriptor numbers
namespace fd_num {
inline constexpr long STDIN = 0;
inline constexpr long STDOUT = 1;
inline constexpr long STDERR = 2;
} // namespace fd_num

// These ordinals define the Moss ABI shared by userspace/syscall.h and the
// indexed dispatch table. They are not Linux syscall numbers or reorderable IDs.
enum class SyscallNumber : long {
#include "moss/syscall_numbers.def"
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
long sys_geteuid(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_getegid(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_arch_prctl(long operation, long address, long arg2, long arg3, long arg4, long arg5) noexcept;

// Process management - framework implementation
long sys_fork(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_fork_domain(long cap_out_addr, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_fork_domain_inherit(long cap_out_addr, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_fork_domain_select(long cap_out_addr, long handles_addr, long count, long arg3, long arg4, long arg5) noexcept;
long sys_fork_domain_scoped(long cap_out_addr, long handles_addr, long count, long scope_handle, long arg4,
                            long arg5) noexcept;
long sys_domain_id(long handle, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_domain_same(long left, long right, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_domain_terminate(long handle, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_domain_signal(long handle, long signo, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_domain_wait(long handle, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_domain_wait_any(long handles_addr, long count, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_domain_status(long handle, long status_addr, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_domain_self(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_domain_spawn(long factory, long image_addr, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_domain_layout(long layout_addr, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_domain_factory(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_domain_scope_create(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_domain_scope_terminate(long handle, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_domain_scope_status(long handle, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_domain_scope_contains(long scope_handle, long domain_handle, long arg2, long arg3, long arg4,
                               long arg5) noexcept;
long sys_code_snapshot(long source, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_code_read(long version, long destination, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_code_authority(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_code_approve(long authority, long version, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_code_snapshot_range(long source, long page_count, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_code_read_range(long version, long first_page, long destination, long page_count, long arg4,
                         long arg5) noexcept;
long sys_code_page_count(long version, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_execve(long pathname_addr, long argv_addr, long envp_addr, long arg3, long arg4, long arg5) noexcept;
long sys_execve_cap(long pathname_addr, long argv_addr, long envp_addr, long startup_cap, long arg4,
                    long arg5) noexcept;
long sys_wait4(long wait_pid, long wstatus_addr, long options, long arg3, long arg4, long arg5) noexcept;
long sys_waitpid(long pid, long wstatus, long options, long arg3, long arg4, long arg5) noexcept;
long sys_kill(long pid_arg, long sig_arg, long arg2, long arg3, long arg4, long arg5) noexcept;

// Filesystem - framework implementation
long sys_open(long pathname_addr, long flags, long mode, long arg3, long arg4, long arg5) noexcept;
long sys_close(long fd, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_read(long fd, long buf_addr, long count, long arg3, long arg4, long arg5) noexcept;
long sys_write(long fd, long buf_addr, long count, long arg3, long arg4, long arg5) noexcept;
long sys_lseek(long fd, long offset, long whence, long arg3, long arg4, long arg5) noexcept;
long sys_fstat(long fd, long stat_buf_addr, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_stat(long path_addr, long stat_addr, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_mkdir(long path_addr, long mode, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_rmdir(long path_addr, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_unlink(long path_addr, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_rename(long old_path, long new_path, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_getdents(long fd, long buffer, long size, long arg3, long arg4, long arg5) noexcept;
long sys_dup(long oldfd, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_dup2(long oldfd, long newfd, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_pipe(long pipefd_addr, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_fcntl(long fd, long command, long argument, long arg3, long arg4, long arg5) noexcept;

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
long sys_clock_getres(long clock_id, long resolution_ns_addr, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_nanosleep(long ns_addr, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_clock_nanosleep(long clockid, long flags, long ns_addr, long remaining, long arg4, long arg5) noexcept;

// System monitoring
long sys_topinfo(long info_addr, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

long sys_cap_close(long handle, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_cap_duplicate(long handle, long rights, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_cap_set_inherit(long handle, long inherit, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_cap_set_exec(long handle, long keep, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_ipc_create(long pair_addr, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_ipc_mint_badge(long endpoint, long badge, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_ipc_call(long endpoint, long request_addr, long response_addr, long deadline_ns, long arg4,
                  long arg5) noexcept;
long sys_ipc_receive(long endpoint, long request_addr, long reply_addr, long arg3, long arg4, long arg5) noexcept;
long sys_ipc_reply(long reply, long response_addr, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_mem_create(long size, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
long sys_mem_map(long handle, long rights, long arg2, long arg3, long arg4, long arg5) noexcept;

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
