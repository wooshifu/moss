// MOSS userspace syscall wrappers — shared header for all user programs
//
// Provides inline ARM64, x64 and RV64 stubs for the Moss-native syscall ABI.
// All user programs should #include "syscall.h" instead of defining their own.

#pragma once

#include <moss/domain_spawn.h>

// ============================================================================
// Syscall numbers shared with the kernel through moss/syscall_numbers.def
// ============================================================================

enum {
#include <moss/syscall_numbers.def>
};

enum {
  MOSS_CAP_SEND = 1U << 0,
  MOSS_CAP_RECEIVE = 1U << 1,
  MOSS_CAP_TRANSFER = 1U << 2,
  MOSS_CAP_DUPLICATE = 1U << 3,
  MOSS_CAP_MAP_READ = 1U << 4,
  MOSS_CAP_MAP_WRITE = 1U << 5,
  MOSS_CAP_MINT = 1U << 6,
  MOSS_CAP_DOMAIN_TERMINATE = 1U << 7,
  MOSS_CAP_DOMAIN_INSPECT = 1U << 8,
  MOSS_CAP_DOMAIN_OBSERVE = 1U << 9,
  MOSS_CAP_DOMAIN_SPAWN = 1U << 10,
  MOSS_CAP_CODE_APPROVE = 1U << 11,
  MOSS_CAP_CODE_EXEC = 1U << 12,
  MOSS_CAP_CODE_IDENTIFY = 1U << 13,
  MOSS_CAP_CODE_REVOKE = 1U << 14,
  MOSS_IPC_MAX_MESSAGE = 256
};

// CODE_IDENTIFY can name an approval without granting execution. CODE_REVOKE
// is scoped to the authority that issued it; SYS_CODE_REVOKE requires both.
// Revocation blocks new native-domain admission, not existing mappings.

// The initial Memory Object is one 4 KiB page. MAP_WRITE mappings are also
// readable on all supported architectures; closing a handle keeps mappings.
#define MOSS_MEM_OBJECT_BYTES 4096UL

struct moss_ipc_endpoints {
  unsigned long send;
  unsigned long receive;
};

enum { MOSS_FORK_CAP_INHERIT = 1U << 0 };

// SYS_CAP_SET_INHERIT changes fork inheritance and exec retention together.
// SYS_CAP_SET_EXEC overrides only exec retention: 1 keeps the handle, 0 closes
// it after a successful exec. Opting in requires DUPLICATE authority.
// SYS_FORK_DOMAIN_SELECT preserves each selected handle number through exec.
// INHERIT additionally permits later ordinary forks.
// SYS_FORK_DOMAIN_INHERIT creates a native child with only handles previously
// opted into inheritance; the parent receives its domain capability.
struct moss_fork_capability {
  unsigned long handle;
  unsigned long rights;
  unsigned long flags;
};

// SYS_DOMAIN_SAME requires INSPECT on both handles and returns 1 for the same
// domain incarnation, 0 for different domains; it never compares PID values.
// SYS_DOMAIN_STATUS returns this after the domain exits. A signal exit sets
// signal and leaves code zero; a normal exit sets code and clears signal.
struct moss_domain_exit {
  int code;
  unsigned int signal;
};

// One capability may accompany each bounded request or reply. Ordinary
// capabilities copy reduced rights; a Reply capability moves its one-shot
// handle and cannot be duplicated. If its message is abandoned while the
// original call is pending, that caller receives EPIPE. Zero capability
// requires zero rights.
// Request/reply senders must set badge to zero. Receive fills it from the
// sender capability, so a service can trust it as an object identifier.
struct moss_ipc_message {
  unsigned long size;
  unsigned long capability;
  unsigned long rights;
  unsigned long badge;
  unsigned char payload[MOSS_IPC_MAX_MESSAGE];
};

// SYS_IPC_CALL uses (endpoint, request*, response*, absolute deadline_ns).
// A zero deadline disables it; both messages use the fixed structure above.
// Closing the last receiver rejects new and queued calls; a delivered call
// remains owned by its Reply holder until reply, cancellation or expiry.

// No payload; succeeds only on a terminal. Not a Linux termios command.
// 0x4d01 is a Moss-specific command ID shared with vfs:types; its exact
// allocation rationale is unrecorded. Do not substitute a Linux termios opcode.
enum { MOSS_IOCTL_ISATTY = 0x4d01 };

// Native clock ID 1 selects monotonic nanoseconds, not a POSIX timespec.
enum { MOSS_CLOCK_MONOTONIC = 1 };

// ============================================================================
// Low-level syscall wrappers
// The memory clobber prevents moving user-buffer accesses across the trap.
// x64 additionally clobbers RCX/R11 because SYSCALL uses them for return PC/flags.
// ============================================================================

#ifdef __riscv
// RISC-V 64: a7=nr, a0-a5=args, ecall, return in a0

static inline long syscall0(long number) {
  register long a7 asm("a7") = number;
  register long a0 asm("a0");
  asm volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
  return a0;
}

static inline long syscall1(long number, long arg0) {
  register long a7 asm("a7") = number;
  register long a0 asm("a0") = arg0;
  asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
  return a0;
}

static inline long syscall2(long number, long arg0, long arg1) {
  register long a7 asm("a7") = number;
  register long a0 asm("a0") = arg0;
  register long a1 asm("a1") = arg1;
  asm volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1) : "memory");
  return a0;
}

static inline long syscall3(long number, long arg0, long arg1, long arg2) {
  register long a7 asm("a7") = number;
  register long a0 asm("a0") = arg0;
  register long a1 asm("a1") = arg1;
  register long a2 asm("a2") = arg2;
  asm volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
  return a0;
}

static inline long syscall6(long number, long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) {
  register long a7 asm("a7") = number;
  register long a0 asm("a0") = arg0;
  register long a1 asm("a1") = arg1;
  register long a2 asm("a2") = arg2;
  register long a3 asm("a3") = arg3;
  register long a4 asm("a4") = arg4;
  register long a5 asm("a5") = arg5;
  asm volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
  return a0;
}

#elif defined(__x86_64__)
// x64: RAX=nr, RDI/RSI/RDX/R10/R8/R9=args, syscall, return in RAX

static inline long syscall0(long number) {
  register long rax asm("rax") = number;
  asm volatile("syscall" : "+r"(rax) : : "rcx", "r11", "memory");
  return rax;
}

static inline long syscall1(long number, long arg0) {
  register long rax asm("rax") = number;
  register long rdi asm("rdi") = arg0;
  asm volatile("syscall" : "+r"(rax) : "r"(rdi) : "rcx", "r11", "memory");
  return rax;
}

static inline long syscall2(long number, long arg0, long arg1) {
  register long rax asm("rax") = number;
  register long rdi asm("rdi") = arg0;
  register long rsi asm("rsi") = arg1;
  asm volatile("syscall" : "+r"(rax) : "r"(rdi), "r"(rsi) : "rcx", "r11", "memory");
  return rax;
}

static inline long syscall3(long number, long arg0, long arg1, long arg2) {
  register long rax asm("rax") = number;
  register long rdi asm("rdi") = arg0;
  register long rsi asm("rsi") = arg1;
  register long rdx asm("rdx") = arg2;
  asm volatile("syscall" : "+r"(rax) : "r"(rdi), "r"(rsi), "r"(rdx) : "rcx", "r11", "memory");
  return rax;
}

static inline long syscall6(long number, long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) {
  register long rax asm("rax") = number;
  register long rdi asm("rdi") = arg0;
  register long rsi asm("rsi") = arg1;
  register long rdx asm("rdx") = arg2;
  register long r10 asm("r10") = arg3;
  register long r8 asm("r8") = arg4;
  register long r9 asm("r9") = arg5;
  asm volatile("syscall"
               : "+r"(rax)
               : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
               : "rcx", "r11", "memory");
  return rax;
}

#else
// ARM64: x8=nr, x0-x5=args, svc #0, return in x0

static inline long syscall0(long number) {
  register long x8 asm("x8") = number;
  register long ret asm("x0");
  asm volatile("svc #0" : "=r"(ret) : "r"(x8) : "memory");
  return ret;
}

static inline long syscall1(long number, long a0) {
  register long x8 asm("x8") = number;
  register long x0 asm("x0") = a0;
  register long ret asm("x0");
  asm volatile("svc #0" : "=r"(ret) : "r"(x8), "r"(x0) : "memory");
  return ret;
}

static inline long syscall2(long number, long a0, long a1) {
  register long x8 asm("x8") = number;
  register long x0 asm("x0") = a0;
  register long x1 asm("x1") = a1;
  register long ret asm("x0");
  asm volatile("svc #0" : "=r"(ret) : "r"(x8), "r"(x0), "r"(x1) : "memory");
  return ret;
}

static inline long syscall3(long number, long a0, long a1, long a2) {
  register long x8 asm("x8") = number;
  register long x0 asm("x0") = a0;
  register long x1 asm("x1") = a1;
  register long x2 asm("x2") = a2;
  register long ret asm("x0");
  asm volatile("svc #0" : "=r"(ret) : "r"(x8), "r"(x0), "r"(x1), "r"(x2) : "memory");
  return ret;
}

static inline long syscall6(long number, long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) {
  register long x8 asm("x8") = number;
  register long x0 asm("x0") = arg0;
  register long x1 asm("x1") = arg1;
  register long x2 asm("x2") = arg2;
  register long x3 asm("x3") = arg3;
  register long x4 asm("x4") = arg4;
  register long x5 asm("x5") = arg5;
  asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
  return x0;
}

#endif

// ============================================================================
// Signal constants
// ============================================================================

#ifndef MOSS_SYSCALL_RAW_ONLY

#define SIGHUP 1
enum {
  SIGINT = 2,
  SIGQUIT = 3,
  SIGILL = 4,
  SIGTRAP = 5,
  SIGABRT = 6,
  SIGBUS = 7,
  SIGFPE = 8,
  SIGKILL = 9,
  SIGUSR1 = 10,
  SIGSEGV = 11,
  SIGUSR2 = 12,
  SIGPIPE = 13,
  SIGALRM = 14,
  SIGTERM = 15,
  SIGCHLD = 17,
  SIGCONT = 18,
  SIGSTOP = 19,
  SIGTSTP = 20
};

// Special handler values
enum { SIG_DFL = 0, SIG_IGN = 1 };

// sigprocmask 'how' values
enum { SIG_BLOCK = 0, SIG_UNBLOCK = 1, SIG_SETMASK = 2 };

// Moss-native flags, translated by mlibc: ONSTACK/RESTART/NOCLDSTOP/NOCLDWAIT
// occupy bits 0/1/3/4; bit 2 is reserved for SIGINFO. Public libc bits differ.
// sigaction flags
enum { SA_ONSTACK = 0x1, SA_RESTART = 0x2, SA_NOCLDSTOP = 0x8, SA_NOCLDWAIT = 0x10 };

// sigaltstack flags
enum { SS_ONSTACK = 1, SS_DISABLE = 2 };

// Sigaction structure (must match kernel UserSigaction layout)
struct sigaction_t {
  unsigned long handler; // function pointer or SIG_DFL(0)/SIG_IGN(1)
  unsigned long mask;    // signals to block during handler
  unsigned long flags;   // SA_ONSTACK, etc.
};

// Sigaltstack structure (must match kernel UserStack layout)
struct stack_t {
  unsigned long ss_sp;
  unsigned long ss_size;
  unsigned long ss_flags;
};

// ============================================================================
// POSIX-like wrapper functions
// ============================================================================

static inline void _exit(int status) {
  syscall1(SYS_EXIT, status);
  __builtin_unreachable();
}

static inline long getpid(void) { return syscall0(SYS_GETPID); }

static inline long getppid(void) { return syscall0(SYS_GETPPID); }

static inline long fork(void) { return syscall0(SYS_FORK); }

static inline long execve(const char *pathname, char *const argv[], char *const envp[]) {
  return syscall3(SYS_EXECVE, (long)pathname, (long)argv, (long)envp);
}

static inline long waitpid(long pid, int *wstatus, int options) {
  return syscall3(SYS_WAITPID, pid, (long)wstatus, options);
}

static inline long open(const char *pathname, int flags) { return syscall2(SYS_OPEN, (long)pathname, flags); }

static inline long close(int fd) { return syscall1(SYS_CLOSE, fd); }

static inline long read(int fd, void *buf, long count) { return syscall3(SYS_READ, fd, (long)buf, count); }

static inline long write(int fd, const void *buf, long count) { return syscall3(SYS_WRITE, fd, (long)buf, count); }

static inline long dup(int oldfd) { return syscall1(SYS_DUP, oldfd); }

static inline long dup2(int oldfd, int newfd) { return syscall2(SYS_DUP2, oldfd, newfd); }

static inline long pipe(long pipefd[2]) { return syscall1(SYS_PIPE, (long)pipefd); }

static inline long sched_yield(void) { return syscall0(SYS_SCHED_YIELD); }

static inline long kill(long pid, int sig) { return syscall2(SYS_KILL, pid, (long)sig); }

static inline long moss_sigaction(int sig, const struct sigaction_t *act, struct sigaction_t *oldact) {
  return syscall3(SYS_SIGACTION, (long)sig, (long)act, (long)oldact);
}

static inline long sigprocmask(int how, const unsigned long *set, unsigned long *oldset) {
  return syscall3(SYS_SIGPROCMASK, (long)how, (long)set, (long)oldset);
}

static inline long sigaltstack(const struct stack_t *ss, struct stack_t *old_ss) {
  return syscall2(SYS_SIGALTSTACK, (long)ss, (long)old_ss);
}

// Clock ID 1 selects monotonic time. Moss returns a single u64 nanosecond
// count, not a POSIX timespec; mlibc performs that representation conversion.
static inline long clock_gettime_ns(unsigned long *ns) {
  return syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)ns);
}

// Like clock_gettime_ns, the native ABI writes a u64 nanosecond count instead
// of a POSIX timespec. Clock ID 1 selects the monotonic hardware clocksource.
static inline long clock_getres_ns(unsigned long *ns) {
  return syscall2(SYS_CLOCK_GETRES, MOSS_CLOCK_MONOTONIC, (long)ns);
}

static inline long nanosleep_ns(unsigned long *ns) { return syscall2(SYS_NANOSLEEP, (long)ns, 0); }

#endif // MOSS_SYSCALL_RAW_ONLY

// ============================================================================
// TopInfo — system monitoring structures (for top command)
// Layout must match kernel-side topinfo_layout exactly.
// ============================================================================

// Fixed ABI capacities and 16-byte names below match topinfo_layout; they bound
// the returned snapshot independently of the kernel's actual task/CPU limits.
// Any change requires rebuilding both producer and consumer with the same layout.
enum { TOP_MAX_PROCS = 64, TOP_MAX_CPUS = 32 };

struct TopProcessInfo {
  long pid;
  long ppid;
  unsigned long state; // ProcessState enum value
  unsigned long cpu;   // current CPU
  long nice;
  unsigned long vruntime;
  unsigned long sum_exec_runtime; // total CPU time in nanoseconds
  unsigned long load_avg;
  unsigned long util_avg;
  char name[16];
};

struct TopInfo {
  unsigned long uptime_ns;
  unsigned long total_processes;
  unsigned long total_context_switches;
  unsigned long total_preemptions;
  unsigned long total_forks;
  unsigned long total_exits;
  unsigned long nr_cpus;
  unsigned long cpu_load[TOP_MAX_CPUS];
  unsigned long cpu_nr_running[TOP_MAX_CPUS];
  unsigned long cpu_idle_time_ns[TOP_MAX_CPUS];
  unsigned long mem_total_pages;
  unsigned long mem_used_pages;
  unsigned long mem_free_pages;
  unsigned long page_size;
  unsigned long nr_processes;
  struct TopProcessInfo procs[TOP_MAX_PROCS];
};

static inline long topinfo(struct TopInfo *info) { return syscall1(SYS_TOPINFO, (long)info); }

static inline long current_cpu(void) {
  struct TopInfo info;
  if (topinfo(&info) != 0) {
    return -1;
  }
  long self = syscall0(SYS_GETPID);
  for (unsigned long i = 0; i < info.nr_processes; ++i) {
    if (info.procs[i].pid == self) {
      return (long)info.procs[i].cpu;
    }
  }
  return -1;
}

// ============================================================================
// String utilities (no libc available)
// ============================================================================

#ifndef MOSS_SYSCALL_RAW_ONLY

static inline int strlen(const char *s) {
  int n = 0;
  while (s[n]) {
    n++;
  }
  return n;
}

static inline int streq(const char *a, const char *b) {
  while (*a && *b) {
    if (*a != *b) {
      return 0;
    }
    a++;
    b++;
  }
  return *a == *b;
}

static inline int strncmp(const char *a, const char *b, int n) {
  for (int i = 0; i < n; i++) {
    if (a[i] != b[i]) {
      return a[i] - b[i];
    }
    if (a[i] == '\0') {
      return 0;
    }
  }
  return 0;
}

static inline void strcpy(char *dst, const char *src) {
  while (*src) {
    *dst++ = *src++;
  }
  *dst = '\0';
}

static inline void print(const char *msg) { write(1, msg, strlen(msg)); }

static inline void eprint(const char *msg) { write(2, msg, strlen(msg)); }

// ============================================================================
// Number formatting utilities (no printf available)
// ============================================================================

// A 64-bit unsigned long needs 20 digits; signed long needs at most 19 digits
// plus a sign. Both caller buffers need one additional byte for the NUL.
enum { MOSS_DECIMAL_BUFFER_SIZE = 21 };

// Return the number of characters stored, excluding NUL. Positive buffer sizes
// always produce a terminated string; short buffers retain the decimal prefix.
static inline int ultoa(unsigned long val, char *buf, int bufsize) {
  if (bufsize <= 0) {
    return 0;
  }
  // Reverse only the digits; the caller's terminator is handled separately.
  char tmp[MOSS_DECIMAL_BUFFER_SIZE - 1];
  int len = 0;
  do {
    tmp[len++] = '0' + (int)(val % 10);
    val /= 10;
  } while (val > 0);
  // Keep the full digit count for the reversal. Reducing len itself would
  // discard high digits and print the numeric suffix when capacity is short.
  int written = len < bufsize ? len : bufsize - 1;
  for (int i = 0; i < written; i++) {
    buf[i] = tmp[len - 1 - i];
  }
  buf[written] = '\0';
  return written;
}

// Same NUL/prefix contract as ultoa, with the sign included in the stored length.
static inline int ltoa(long val, char *buf, int bufsize) {
  if (bufsize <= 0) {
    return 0;
  }
  if (bufsize == 1) {
    buf[0] = '\0';
    return 0;
  }
  if (val < 0) {
    buf[0] = '-';
    // LONG_MIN has no positive signed counterpart. Unsigned subtraction wraps
    // by definition, so casting before negation obtains its magnitude safely.
    return 1 + ultoa(0UL - (unsigned long)val, buf + 1, bufsize - 1);
  }
  return ultoa((unsigned long)val, buf, bufsize);
}

// Print an unsigned long as decimal
static inline void print_ulong(unsigned long val) {
  char buf[MOSS_DECIMAL_BUFFER_SIZE];
  ultoa(val, buf, sizeof(buf));
  print(buf);
}

// Print a signed long as decimal
static inline void print_long(long val) {
  char buf[MOSS_DECIMAL_BUFFER_SIZE];
  ltoa(val, buf, sizeof(buf));
  print(buf);
}

// Print unsigned long right-aligned in a field of given width
static inline void print_num_padded(unsigned long val, int width) {
  char buf[MOSS_DECIMAL_BUFFER_SIZE];
  int len = ultoa(val, buf, sizeof(buf));
  for (int i = len; i < width; i++) {
    print(" ");
  }
  print(buf);
}

// Print signed long right-aligned in a field of given width
static inline void print_snum_padded(long val, int width) {
  char buf[MOSS_DECIMAL_BUFFER_SIZE];
  int len = ltoa(val, buf, sizeof(buf));
  for (int i = len; i < width; i++) {
    print(" ");
  }
  print(buf);
}

// Print a string left-aligned, padded to given width
static inline void print_str_padded(const char *s, int width) {
  int len = strlen(s);
  print(s);
  for (int i = len; i < width; i++) {
    print(" ");
  }
}

#endif // MOSS_SYSCALL_RAW_ONLY
