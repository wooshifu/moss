#include "validation/internal.h"

unsigned long long counter(void) {
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

static volatile unsigned long long benchmark_handler_tick;
static volatile unsigned long benchmark_handler_calls;

static void benchmark_signal_handler(int signo) {
  benchmark_handler_tick = counter();
  if (signo == SIGUSR1) {
    ++benchmark_handler_calls;
  }
}

void user_benchmark(long mode) {
  // Protocol modes 11..16 measure faults, COW, switches, exec, signals and pipe
  // transfers. Op 30 checks resource baselines; 31 verifies absent/COW mappings.
  // The 1024-byte pipe record is one quarter of capacity, so a serial write/read
  // sample completes without needing a concurrent reader; exact tuning is unrecorded.
  // Avoid unrelated instruction-page faults in the measured regions.
  for (unsigned long page = (unsigned long)__user_text_start; page < (unsigned long)__user_text_end; page += 4096) {
    (void)*(const volatile unsigned char *)page;
  }
  long pid = getpid();
  struct sigaction_t action = {(unsigned long)benchmark_signal_handler, 0, 0};
  if (mode == 15 && moss_sigaction(SIGUSR1, &action, 0) != 0) {
    control(5, 0, 0);
  }
  long count;
  while ((count = control(4, 0, 0)) > 0) {
    int valid = control(30, 0, 0) == 1;
    long area = 0, child = -1, fds[2] = {-1, -1};
    unsigned char sent[1024], received[1024];
    if (mode == 11 || mode == 12) {
      area = syscall6(SYS_MMAP, 0, count * 4096, 3, 0x22, -1, 0);
      valid &= area > 0;
    }
    if (mode == 12 && valid) {
      for (long i = 0; i < count; ++i) {
        *(volatile unsigned char *)(area + i * 4096) = 31;
      }
      valid &= pipe(fds) == 0;
      if (valid) {
        child = fork();
      }
      if (child == 0) {
        unsigned char done;
        long result;
        while ((result = read(fds[0], &done, 1)) == 0) {
          sched_yield();
        }
        int isolated = result == 1 && done == 1;
        for (long i = 0; i < count; ++i) {
          isolated &= *(volatile unsigned char *)(area + i * 4096) == 31;
        }
        _exit(isolated ? 37 : 99);
      }
      valid &= child > 0;
    }
    if ((mode == 11 || mode == 12) && valid) {
      valid &= control(31, area, count) == 1;
    }
    if (mode == 16) {
      for (unsigned i = 0; i < sizeof(sent); ++i) {
        sent[i] = (unsigned char)i;
        received[i] = 0;
      }
      valid &= pipe(fds) == 0;
    }
    unsigned long long ticks = 0;
    const unsigned long long begin = counter();
    for (long i = 0; valid && i < count; ++i) {
      if (mode == 11 || mode == 12) {
        *(volatile unsigned char *)(area + i * 4096) = 73;
      } else if (mode == 13) {
        valid &= sched_yield() == 0;
      } else if (mode == 14) {
        long spawned = fork();
        if (spawned == 0) {
          const char *args[] = {"exec", 0};
          syscall3(SYS_EXECVE, (long)"/validation_child.elf", (long)args, 0);
          _exit(99);
        }
        int status = 0;
        valid &= spawned > 0 && waitpid(spawned, &status, 0) == spawned && status == (37 << 8);
      } else if (mode == 15) {
        const unsigned long calls = benchmark_handler_calls;
        benchmark_handler_tick = 0;
        const unsigned long long start = counter();
        valid &= kill(pid, SIGUSR1) == 0;
        const unsigned long long delivered = benchmark_handler_tick;
        valid &= benchmark_handler_calls == calls + 1 && delivered > start;
        if (delivered > start) {
          ticks += delivered - start;
        }
      } else if (mode == 16) {
        valid &= write(fds[1], sent, sizeof(sent)) == sizeof(sent);
        valid &= read(fds[0], received, sizeof(received)) == sizeof(received);
      }
    }
    const unsigned long long end = counter();
    if (mode != 15) {
      ticks = end - begin;
    }
    if (mode == 11 || mode == 12) {
      if (area > 0) {
        if (valid) {
          for (long i = 0; i < count; ++i) {
            valid &= *(volatile unsigned char *)(area + i * 4096) == 73;
          }
        }
        if (child > 0) {
          const unsigned char done = 1;
          int status = 0;
          valid &= write(fds[1], &done, 1) == 1;
          valid &= waitpid(child, &status, 0) == child && status == (37 << 8);
        }
        valid &= syscall2(SYS_MUNMAP, area, count * 4096) == 0;
      }
    }
    if (mode == 16) {
      for (unsigned i = 0; i < sizeof(sent); ++i) {
        valid &= sent[i] == received[i];
      }
    }
    if (fds[0] >= 0) {
      valid &= close(fds[0]) == 0;
    }
    if (fds[1] >= 0) {
      valid &= close(fds[1]) == 0;
    }
    valid &= control(30, 1, 0) == 1;
    const unsigned long long overhead_start = counter();
    if (mode == 15) {
      for (long i = 0; i < count; ++i) {
        (void)counter();
        (void)counter();
      }
    } else {
      for (long i = 0; i < count; ++i) {
        asm volatile("" : "+r"(i) : : "memory");
      }
    }
    const unsigned long long overhead = counter() - overhead_start;
    control(6, (long)overhead, 0);
    control(5, (long)ticks, valid);
  }
  control(3, 0, 0);
}
