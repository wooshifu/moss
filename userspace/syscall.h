// MOSS userspace syscall wrappers — shared header for all user programs
//
// Provides inline ARM64 syscall stubs and POSIX-like wrapper functions.
// All user programs should #include "syscall.h" instead of defining their own.

#pragma once

// ============================================================================
// Syscall numbers (must match kernel-syscall_table.cppm SyscallNumber enum)
// ============================================================================

#define SYS_EXIT 1
#define SYS_GETPID 2
#define SYS_GETPPID 3
#define SYS_FORK 10
#define SYS_EXECVE 11
#define SYS_WAIT4 12
#define SYS_WAITPID 13
#define SYS_SCHED_YIELD 18
#define SYS_OPEN 30
#define SYS_CLOSE 31
#define SYS_READ 32
#define SYS_WRITE 33
#define SYS_LSEEK 34
#define SYS_FSTAT 36
#define SYS_DUP 42
#define SYS_DUP2 43
#define SYS_PIPE 44
#define SYS_CLOCK_GETTIME 83
#define SYS_NANOSLEEP 86
#define SYS_TOPINFO 111

// ============================================================================
// Low-level syscall wrappers
// ============================================================================

#ifdef __riscv
// RISC-V: a7=nr, a0-a5=args, ecall, return in a0

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

#elif defined(__x86_64__)
// x86_64: RAX=nr, RDI/RSI/RDX/R10/R8/R9=args, syscall, return in RAX

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

#endif

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

static inline long clock_gettime_ns(unsigned long *ns) { return syscall1(SYS_CLOCK_GETTIME, (long)ns); }

static inline long nanosleep_ns(unsigned long *ns) { return syscall1(SYS_NANOSLEEP, (long)ns); }

// ============================================================================
// TopInfo — system monitoring structures (for top command)
// Layout must match kernel-side topinfo_layout exactly.
// ============================================================================

#define TOP_MAX_PROCS 64
#define TOP_MAX_CPUS 32

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

// ============================================================================
// String utilities (no libc available)
// ============================================================================

static inline int strlen(const char *s) {
  int n = 0;
  while (s[n])
    n++;
  return n;
}

static inline int streq(const char *a, const char *b) {
  while (*a && *b) {
    if (*a != *b)
      return 0;
    a++;
    b++;
  }
  return *a == *b;
}

static inline int strncmp(const char *a, const char *b, int n) {
  for (int i = 0; i < n; i++) {
    if (a[i] != b[i])
      return a[i] - b[i];
    if (a[i] == '\0')
      return 0;
  }
  return 0;
}

static inline void strcpy(char *dst, const char *src) {
  while (*src)
    *dst++ = *src++;
  *dst = '\0';
}

static inline void print(const char *msg) { write(1, msg, strlen(msg)); }

static inline void eprint(const char *msg) { write(2, msg, strlen(msg)); }

// ============================================================================
// Number formatting utilities (no printf available)
// ============================================================================

// Convert unsigned long to decimal string, return length written
static inline int ultoa(unsigned long val, char *buf, int bufsize) {
  if (bufsize <= 0)
    return 0;
  if (val == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return 1;
  }
  char tmp[20];
  int len = 0;
  while (val > 0 && len < 20) {
    tmp[len++] = '0' + (int)(val % 10);
    val /= 10;
  }
  if (len >= bufsize)
    len = bufsize - 1;
  for (int i = 0; i < len; i++)
    buf[i] = tmp[len - 1 - i];
  buf[len] = '\0';
  return len;
}

// Convert signed long to decimal string, return length written
static inline int ltoa(long val, char *buf, int bufsize) {
  if (bufsize <= 1)
    return 0;
  if (val < 0) {
    buf[0] = '-';
    return 1 + ultoa((unsigned long)(-val), buf + 1, bufsize - 1);
  }
  return ultoa((unsigned long)val, buf, bufsize);
}

// Print an unsigned long as decimal
static inline void print_ulong(unsigned long val) {
  char buf[20];
  ultoa(val, buf, 20);
  print(buf);
}

// Print a signed long as decimal
static inline void print_long(long val) {
  char buf[21];
  ltoa(val, buf, 21);
  print(buf);
}

// Print unsigned long right-aligned in a field of given width
static inline void print_num_padded(unsigned long val, int width) {
  char buf[20];
  int len = ultoa(val, buf, 20);
  for (int i = len; i < width; i++)
    print(" ");
  print(buf);
}

// Print signed long right-aligned in a field of given width
static inline void print_snum_padded(long val, int width) {
  char buf[21];
  int len = ltoa(val, buf, 21);
  for (int i = len; i < width; i++)
    print(" ");
  print(buf);
}

// Print a string left-aligned, padded to given width
static inline void print_str_padded(const char *s, int width) {
  int len = strlen(s);
  print(s);
  for (int i = len; i < width; i++)
    print(" ");
}
