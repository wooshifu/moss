// MOSS userspace syscall wrappers — shared header for all user programs
//
// Provides inline ARM64, x64 and RV64 stubs for the Moss-native syscall ABI.
// All user programs should #include "syscall.h" instead of defining their own.

#pragma once

// ============================================================================
// Syscall numbers (must match kernel-syscall_table.cppm SyscallNumber enum)
// ============================================================================

enum {
  SYS_DEBUG_PRINT = 0,
  SYS_EXIT = 1,
  SYS_GETPID = 2,
  SYS_GETPPID = 3,
  SYS_GETUID = 4,
  SYS_GETGID = 5,
  SYS_GETEUID = 6,
  SYS_GETEGID = 7,
  SYS_FORK = 10,
  SYS_EXECVE = 11,
  SYS_WAIT4 = 12,
  SYS_WAITPID = 13,
  SYS_KILL = 14,
  SYS_SIGACTION = 15,
  SYS_SIGPROCMASK = 16,
  SYS_SIGRETURN = 17,
  SYS_SCHED_YIELD = 18,
  SYS_SIGALTSTACK = 21,
  SYS_OPEN = 30,
  SYS_CLOSE = 31,
  SYS_READ = 32,
  SYS_WRITE = 33,
  SYS_LSEEK = 34,
  SYS_STAT = 35,
  SYS_FSTAT = 36,
  SYS_LSTAT = 37,
  SYS_ACCESS = 38,
  SYS_DUP = 42,
  SYS_DUP2 = 43,
  SYS_PIPE = 44,
  SYS_MKDIR = 45,
  SYS_RMDIR = 46,
  SYS_UNLINK = 48,
  SYS_CHDIR = 51,
  SYS_GETCWD = 52,
  SYS_RENAME = 53,
  SYS_MMAP = 60,
  SYS_MUNMAP = 61,
  SYS_BRK = 69,
  SYS_CLOCK_GETTIME = 83,
  SYS_NANOSLEEP = 86,
  SYS_CLOCK_NANOSLEEP = 87,
  SYS_UNAME = 110,
  SYS_TOPINFO = 111,
  SYS_ARCH_PRCTL = 126,
  SYS_FCNTL = 130,
  SYS_GETDENTS = 131,
  SYS_IOCTL = 132
};

// No payload; succeeds only on a terminal. Not a Linux termios command.
// 0x4d01 is a Moss-specific command ID shared with vfs:types; its exact
// allocation rationale is unrecorded. Do not substitute a Linux termios opcode.
enum { MOSS_IOCTL_ISATTY = 0x4d01 };

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
  SIGSTOP = 19
};

// Special handler values
enum { SIG_DFL = 0, SIG_IGN = 1 };

// sigprocmask 'how' values
enum { SIG_BLOCK = 0, SIG_UNBLOCK = 1, SIG_SETMASK = 2 };

// Moss-native flags, translated by mlibc: SA_ONSTACK here is bit 0, unlike
// libc's public encoding. Handler/mask/stack layouts below must match the kernel.
// sigaction flags
enum { SA_ONSTACK = 0x1 };

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
static inline long clock_gettime_ns(unsigned long *ns) { return syscall2(SYS_CLOCK_GETTIME, 1, (long)ns); }

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
