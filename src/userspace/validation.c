#include "syscall.h"

static long control(long op, long a, long b) { return syscall3(511, op, a, b); }

static unsigned long user_ranges(void) {
  unsigned check = 0;
  unsigned long failures = 0;
#define CHECK_RANGE(expr)                                                                                              \
  do {                                                                                                                 \
    if (!(expr))                                                                                                       \
      failures |= 1UL << check;                                                                                        \
    ++check;                                                                                                           \
  } while (0)
  CHECK_RANGE(syscall1(SYS_DEBUG_PRINT, 0) == -22);
  CHECK_RANGE(syscall1(SYS_DEBUG_PRINT, 0x800000000000L) == -14);
  CHECK_RANGE(syscall3(SYS_WRITE, 1, 0x800000000000L, 0) == -22); // current Moss zero-count ABI
  CHECK_RANGE(syscall3(SYS_WRITE, 1, 0x1000, 1) == -14);
  CHECK_RANGE(syscall3(SYS_WRITE, 1, -2L, 4) == -14);
  CHECK_RANGE(syscall3(SYS_CLOCK_GETTIME, 0, (long)"read-only", 0) == -14);
  CHECK_RANGE(syscall6(SYS_MMAP, 0x1000, 4096, 3, 0x22, -1, 0) == -22);
  CHECK_RANGE(syscall6(SYS_MMAP, 0x800000000000L - 4096, 8192, 3, 0x22, -1, 0) == -22);
  CHECK_RANGE(syscall6(SYS_MMAP, 0x180000000L, 4096, 3, 0x22, -1, 0) == -22);
  CHECK_RANGE(syscall6(SYS_MMAP, 0, 4096, 3, 0x32, -1, 0) == -22); // MAP_FIXED unsupported
  CHECK_RANGE(syscall6(SYS_MMAP, 0, 4096, 8, 0x22, -1, 0) == -22);
  CHECK_RANGE(syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 4096) == -22);
  CHECK_RANGE(syscall6(SYS_MMAP, 0, 0x7fffffffffffffffL, 3, 0x22, -1, 0) == -12);
  CHECK_RANGE(syscall2(SYS_MUNMAP, 0x180000000L, 4096) == -22);
  CHECK_RANGE(syscall2(SYS_MUNMAP, -4096L, 8192) == -22);
  char too_long[256];
  for (unsigned i = 0; i < sizeof(too_long); ++i)
    too_long[i] = 'a';
  CHECK_RANGE(syscall3(SYS_OPEN, (long)too_long, 0, 0) == -36);
  long first = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
  CHECK_RANGE(first > 0);
  if (first > 0) {
    long second = syscall6(SYS_MMAP, first + 4096, 4096, 3, 0x22, -1, 0);
    CHECK_RANGE(second == first + 4096);
    if (second == first + 4096) {
      unsigned char *mask = (unsigned char *)(first + 4094);
      mask[0] = 1;
      mask[1] = 0;
      mask[2] = 0;
      mask[3] = 0;
      CHECK_RANGE(syscall3(20, 0, 4, (long)mask) == 0);                  // copy_from_user across VMAs
      CHECK_RANGE(syscall3(SYS_CLOCK_GETTIME, 0, first + 4092, 0) == 0); // Moss writes one u64, crossing VMAs
      const char name[] = "/missing-range-test";
      char *path = (char *)(first + 4093);
      for (unsigned i = 0; i < sizeof(name); ++i)
        path[i] = name[i];
      CHECK_RANGE(syscall3(SYS_OPEN, (long)path, 0, 0) == -2); // string spans two VMAs
    }
    if (second > 0)
      CHECK_RANGE(syscall2(SYS_MUNMAP, second, 4096) == 0);
    CHECK_RANGE(syscall3(SYS_CLOCK_GETTIME, 0, first + 4092, 0) == -14); // second VMA now absent
    long collision = syscall6(SYS_MMAP, first, 4096, 3, 0x22, -1, 0);
    CHECK_RANGE(collision > 0 && collision != first);
    if (collision > 0 && collision != first)
      CHECK_RANGE(syscall2(SYS_MUNMAP, collision, 4096) == 0);
    CHECK_RANGE(syscall2(SYS_MUNMAP, first, 4096) == 0);
  }
  long none = syscall6(SYS_MMAP, 0, 4096, 0, 0x22, -1, 0);
  CHECK_RANGE(none > 0);
  if (none > 0) {
    CHECK_RANGE(syscall3(SYS_WRITE, 1, none, 1) == -14);
    CHECK_RANGE(syscall3(SYS_CLOCK_GETTIME, 0, none, 0) == -14);
    CHECK_RANGE(syscall2(SYS_MUNMAP, none, 4096) == 0);
  }
#undef CHECK_RANGE
  return failures;
}

static unsigned long long counter(void) {
#if defined(__aarch64__)
  unsigned long long value;
  asm volatile("isb; mrs %0, cntvct_el0; isb" : "=r"(value) : : "memory");
  return value;
#elif defined(__x86_64__)
  unsigned lo, hi;
  asm volatile("mfence; lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) : : "memory");
  return ((unsigned long long)hi << 32) | lo;
#else
  unsigned long long value;
  asm volatile("fence iorw,iorw; rdtime %0; fence iorw,iorw" : "=r"(value) : : "memory");
  return value;
#endif
}

#if defined(__x86_64__)
static void set_fp_state(int child) {
  unsigned short cw = child ? 0xb7f : 0x77f;
  unsigned mxcsr = child ? 0x5f80 : 0x3f80;
  unsigned long vector[2] = {child ? 0x12345678UL : 0x1122334455667788UL, 0x8877665544332211UL};
  asm volatile("fninit; fldcw %0; ldmxcsr %1; movdqu %2, %%xmm15; fld1"
               :
               : "m"(cw), "m"(mxcsr), "m"(vector)
               : "xmm15", "memory");
}

static int parent_fp_state(void) {
  unsigned short cw;
  unsigned mxcsr;
  unsigned long vector[2];
  unsigned char value[10];
  asm volatile("fnstcw %0; stmxcsr %1; movdqu %%xmm15, %2; fstpt %3; fldt %3"
               : "=m"(cw), "=m"(mxcsr), "=m"(vector), "=m"(value)
               :
               : "memory");
  return cw == 0x77f && mxcsr == 0x3f80 && vector[0] == 0x1122334455667788UL && vector[1] == 0x8877665544332211UL &&
         value[7] == 0x80 && value[8] == 0xff && value[9] == 0x3f;
}

static int fp_fault_isolated(int simd) {
  long child = syscall0(SYS_FORK);
  if (child == 0) {
    if (simd) {
      unsigned mxcsr = 0x1f00; // unmask invalid-operation: 0 / 0 raises #XM
      asm volatile("ldmxcsr %0; xorps %%xmm0, %%xmm0; divss %%xmm0, %%xmm0" : : "m"(mxcsr) : "xmm0", "memory");
    } else {
      unsigned short cw = 0x37e; // unmask invalid-operation; FWAIT delivers #MF
      asm volatile("fninit; fldcw %0; fldz; fldz; fdivp; fwait" : : "m"(cw) : "st", "st(1)", "memory");
    }
    _exit(94); // the arithmetic must fault, not silently continue
  }
  int status = 0;
  long waited = child > 0 ? syscall3(SYS_WAITPID, child, (long)&status, 0) : -1;
  // Moss currently encodes fatal exceptions as negative exit codes, not POSIX signals.
  return child > 1 && waited == child && ((status >> 8) & 255) == 248;
}
#endif

void _start(void) {
  unsigned mask = 1;
  long affinity = syscall3(20, 0, sizeof(mask), (long)&mask);
  long mode = control(0, affinity, 0);
  if (mode == 1) {
    long pid = getpid();
    control(1, 0, 0);
    if (control(2, pid == 1 && syscall0(SYS_GETPPID) == 0 && syscall0(510) < 0, 0)) {
      control(1, 1, 0);
      unsigned long errors = user_ranges();
      if (!control(2, errors == 0, (long)errors)) {
        control(3, 0, 0);
      }
      control(1, 2, 0);
      unsigned long context_errors = 0;
#if defined(__x86_64__)
      set_fp_state(0);
      syscall0(SYS_SCHED_YIELD);
      context_errors |= !parent_fp_state();
#endif
      long child = syscall0(SYS_FORK);
#if defined(__x86_64__)
      context_errors |= (unsigned long)!parent_fp_state() << 1; // live fork state
#endif
      if (child == 0) {
        if (context_errors)
          _exit(97);
#if defined(__x86_64__)
        set_fp_state(1); // must not leak to the parent or survive exec
#endif
        const char *args[] = {"exec", 0};
        syscall3(SYS_EXECVE, (long)"/validation_child.elf", (long)args, 0);
        _exit(99);
      }
      int status = 0;
      long waited = child > 0 ? syscall3(SYS_WAITPID, child, (long)&status, 0) : -1;
      int child_status = status;
      long reaped = child > 0 ? syscall3(SYS_WAITPID, child, (long)&status, 1) : 0;
#if defined(__x86_64__)
      context_errors |= (unsigned long)!fp_fault_isolated(0) << 2;
      context_errors |= (unsigned long)!parent_fp_state() << 4;
#endif
      context_errors |=
          (unsigned long)!(child > 1 && waited == child && ((child_status >> 8) & 255) == 37 && reaped < 0) << 5;
      control(2, context_errors == 0, (long)context_errors);
    }
    control(3, 0, 0);
  }
#if defined(__x86_64__)
  else if (mode == 4) {
    // Explicit acceptance workload: still fails if the execution environment
    // does not deliver #XM. Do not substitute a software interrupt or a pass.
    control(2, fp_fault_isolated(1), 0);
    control(3, 0, 0);
  }
#endif
  else if (mode == 3) {
    // Fork inherits CPU1 affinity; this does not depend on migrating a running child.
    mask = 2;
    affinity = syscall3(20, 0, sizeof(mask), (long)&mask);
#if defined(__x86_64__)
    set_fp_state(0);
#endif
    long child = affinity == 0 ? syscall0(SYS_FORK) : -1;
    if (child == 0) {
#if defined(__x86_64__)
      if (!parent_fp_state())
        _exit(97);
#endif
      _exit(control(7, affinity, 0) ? 37 : 99);
    }
    mask = 1;
    affinity = syscall3(20, 0, sizeof(mask), (long)&mask);
    control(8, child > 1 && affinity == 0, 0);
    int status = 0;
    long waited = child > 0 ? syscall3(SYS_WAITPID, child, (long)&status, 0) : -1;
    int context_ok = 1;
#if defined(__x86_64__)
    context_ok = parent_fp_state();
#endif
    control(9, context_ok && waited == child && ((status >> 8) & 255) == 37, 0);
  } else if (mode == 2) {
    long pid = getpid();
    long count;
    while ((count = control(4, 0, 0)) > 0) {
      int valid = 1;
      unsigned long long begin = counter();
      for (long i = 0; i < count; ++i) {
        long result = getpid();
        asm volatile("" : "+r"(result) : : "memory");
        valid &= result == pid;
      }
      unsigned long long end = counter();
      unsigned long long overhead_start = counter();
      for (long i = 0; i < count; ++i) {
        asm volatile("" : "+r"(i) : : "memory");
      }
      unsigned long long overhead_end = counter();
      control(6, (long)(overhead_end - overhead_start), 0);
      control(5, (long)(end - begin), valid);
    }
    control(3, 0, 0);
  }
  _exit(98);
}
