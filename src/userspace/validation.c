#include "syscall.h"

static long control(long op, long a, long b) { return syscall3(511, op, a, b); }
long frame_register_probe(long number);
static volatile int handled_signo;
static volatile long handler_pid;
#if defined(__x86_64__)
static void set_fp_state(int child);
static int parent_fp_state(void);
#endif
static void frame_signal_handler(int signo) {
  handled_signo = signo;
  handler_pid = getpid(); // nested syscall must borrow its own frame
#if defined(__x86_64__)
  set_fp_state(1);
#endif
}

static unsigned long frame_signal_return(void) {
  struct sigaction_t action = {(unsigned long)frame_signal_handler, 0, 0};
  unsigned long errors = moss_sigaction(SIGUSR1, &action, 0) != 0;
  long result = frame_register_probe(SYS_KILL);
  errors |= (unsigned long)(result != 0 || handled_signo != SIGUSR1 || handler_pid != getpid()) << 1;
  errors |= (unsigned long)(frame_register_probe(SYS_GETPID) != getpid()) << 2;
  action.handler = SIG_DFL;
  errors |= (unsigned long)(moss_sigaction(SIGUSR1, &action, 0) != 0) << 3;
  return errors;
}

static const unsigned char vm_rodata[4096] __attribute__((aligned(4096))) = {0x5a};
__attribute__((noinline)) static void vm_text(void) { asm volatile("" ::: "memory"); }

static int wait_exit(long child, int code) {
  int status = 0;
  return child > 1 && syscall3(SYS_WAITPID, child, (long)&status, 0) == child && ((status >> 8) & 255) == code;
}

static int vm_fault(long address, int access) {
  long child = syscall0(SYS_FORK);
  if (child == 0) {
    if (access == 2) {
      ((void (*)(void))address)();
    } else if (access == 1) {
      // An architectural write attempt, including to text/const data: do not
      // rely on undefined C writes to const objects surviving optimization.
#if defined(__aarch64__)
      asm volatile("strb wzr, [%0]" : : "r"(address) : "memory");
#elif defined(__x86_64__)
      asm volatile("movb $0, (%0)" : : "r"(address) : "memory");
#else
      asm volatile("sb zero, 0(%0)" : : "r"(address) : "memory");
#endif
    } else {
      unsigned char value = *(volatile unsigned char *)address;
      asm volatile("" : : "r"(value) : "memory");
    }
    _exit(94); // A forbidden access must fault, not reach this exit.
  }
  return wait_exit(child, 245); // Moss page faults exit -11, not POSIX wait status.
}

static unsigned long vm_private_cow(void) {
  long area = syscall6(SYS_MMAP, 0, 8192, 3, 0x22, -1, 0);
  if (area <= 0)
    return 1;
  volatile unsigned char *data = (volatile unsigned char *)area;
  data[0] = 11;
  data[4096] = 22;
  data[8191] = 33;
  long child = syscall0(SYS_FORK);
  if (child == 0) {
    if (data[0] != 11 || data[4096] != 22 || data[8191] != 33)
      _exit(91);
    data[0] = 44;
    long grandchild = syscall0(SYS_FORK);
    if (grandchild == 0) {
      data[4096] = 55;
      _exit(data[0] == 44 && data[4096] == 55 && data[8191] == 33 ? 31 : 92);
    }
    _exit(wait_exit(grandchild, 31) && data[0] == 44 && data[4096] == 22 && data[8191] == 33 ? 33 : 93);
  }
  unsigned long errors = !wait_exit(child, 33);
  errors |= (unsigned long)!(data[0] == 11 && data[4096] == 22 && data[8191] == 33) << 1;
  data[0] = 77; // Last-reference COW after descendants have exited.
  errors |= (unsigned long)(data[0] != 77) << 2;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 8192) != 0) << 3;
  return errors;
}

static unsigned long vm_readonly_cow(void) {
  unsigned long errors = *(const volatile unsigned char *)vm_rodata != 0x5a;
  vm_text(); // Both mappings are resident before fork.
  errors |= (unsigned long)!vm_fault((long)vm_rodata, 1) << 1;
  errors |= (unsigned long)!vm_fault((long)vm_text, 1) << 2;
  errors |= (unsigned long)(*(const volatile unsigned char *)vm_rodata != 0x5a) << 3;
  return errors;
}

static unsigned long vm_access_permissions(void) {
  long none = syscall6(SYS_MMAP, 0, 4096, 0, 0x22, -1, 0);
  long nx = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
  unsigned long errors = none <= 0 || nx <= 0;
  if (none > 0) {
    errors |= (unsigned long)!vm_fault(none, 0) << 1;
    errors |= (unsigned long)!vm_fault(none, 1) << 2;
    errors |= (unsigned long)(syscall2(SYS_MUNMAP, none, 4096) != 0) << 3;
  }
  if (nx > 0) {
    // Keep this page absent: an instruction miss must check EXEC before any
    // demand allocation. Otherwise it can allocate then fault indefinitely.
    errors |= (unsigned long)!vm_fault(nx, 2) << 4;
    errors |= (unsigned long)(syscall2(SYS_MUNMAP, nx, 4096) != 0) << 5;
  }
  return errors;
}

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
  if (mode == 6) {
    control(1, 0, 0);
    long probe = frame_register_probe(SYS_GETPID);
    if (!control(2, syscall6(511, 10, 11, 22, 33, 44, 55) == 12345 && probe == getpid(), probe == getpid() ? 0 : probe))
      control(3, 0, 0);
    control(1, 1, 0);
    long child = frame_register_probe(SYS_FORK);
    if (getpid() != 1)
      _exit(child == 0 ? 37 : (int)(90 - child - 1000));
    int child_status = 0;
    long waited = child > 0 ? syscall3(SYS_WAITPID, child, (long)&child_status, 0) : -1;
    if (!control(2, waited == child && child > 0 && ((child_status >> 8) & 255) == 37,
                 child < 0 ? child : child_status))
      control(3, 0, 0);
    control(1, 2, 0);
#if defined(__x86_64__)
    set_fp_state(0);
#endif
    unsigned long errors = frame_signal_return();
#if defined(__x86_64__)
    errors |= (unsigned long)!parent_fp_state() << 4;
#endif
    control(2, errors == 0, (long)errors);
    control(3, 0, 0);
  } else if (mode == 5) {
    for (long test = 0; test < 3; ++test) {
      control(1, test, 0);
      unsigned long errors = test == 0 ? vm_private_cow() : test == 1 ? vm_readonly_cow() : vm_access_permissions();
      if (!control(2, errors == 0, (long)errors))
        break;
    }
    control(3, 0, 0);
  } else if (mode == 1) {
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
