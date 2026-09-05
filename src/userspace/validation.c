#include "syscall.h"

static long control(long op, long a, long b) { return syscall3(511, op, a, b); }

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

void _start(void) {
  unsigned mask = 1;
  long affinity = syscall3(20, 0, sizeof(mask), (long)&mask);
  long mode = control(0, affinity, 0);
  if (mode == 1) {
    long pid = getpid();
    control(1, 0, 0);
    if (control(2, pid == 1 && syscall0(SYS_GETPPID) == 0 && syscall0(510) < 0, 0)) {
      control(1, 1, 0);
      long child = syscall0(SYS_FORK);
      if (child == 0) {
        syscall3(SYS_EXECVE, (long)"/validation_child.elf", 0, 0);
        _exit(99);
      }
      int status = 0;
      long waited = child > 0 ? syscall3(SYS_WAITPID, child, (long)&status, 0) : -1;
      int child_status = status;
      long reaped = child > 0 ? syscall3(SYS_WAITPID, child, (long)&status, 1) : 0;
      control(2, child > 1 && waited == child && ((child_status >> 8) & 255) == 37 && reaped < 0, 0);
    }
    control(3, 0, 0);
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
