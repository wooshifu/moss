#include "validation/internal.h"

// Two milliseconds (2,000,000 ns) is a short nonzero timer workload, not a
// latency budget; assertions reject early return but allow scheduling overshoot.
unsigned long timer_relative_sleep(void) {
  unsigned long before = 0, after = 0, duration = 2000000;
  unsigned long errors = clock_gettime_ns(&before) != 0;
  errors |= (unsigned long)(nanosleep_ns(&duration) != 0) << 1;
  errors |= (unsigned long)(clock_gettime_ns(&after) != 0) << 2;
  errors |= (unsigned long)(after < before || after - before < duration) << 3;
  return errors;
}

unsigned long timer_absolute_sleep(void) {
  unsigned long now = 0, after = 0;
  unsigned long errors = syscall2(SYS_CLOCK_GETTIME, 1, (long)&now) != 0;
  // Clock ID 1 is monotonic and sleep flag 1 is TIMER_ABSTIME in the native ABI.
  unsigned long deadline = now + 2000000;
  errors |= (unsigned long)(syscall6(SYS_CLOCK_NANOSLEEP, 1, 1, (long)&deadline, 0, 0, 0) != 0) << 1;
  errors |= (unsigned long)(syscall2(SYS_CLOCK_GETTIME, 1, (long)&after) != 0) << 2;
  errors |= (unsigned long)(after < deadline) << 3;
  // Both a past absolute deadline and zero relative duration return successfully.
  unsigned long zero = 0;
  errors |= (unsigned long)(syscall6(SYS_CLOCK_NANOSLEEP, 1, 1, (long)&now, 0, 0, 0) != 0) << 4;
  errors |= (unsigned long)(syscall2(SYS_NANOSLEEP, (long)&zero, 0) != 0) << 5;
  return errors;
}

unsigned long timer_invalid_arguments(void) {
  unsigned long duration = 0;
  unsigned long resolution = 0;
  unsigned long errors = syscall2(SYS_NANOSLEEP, 0, 0) != -14;
  errors |= (unsigned long)(syscall6(SYS_CLOCK_NANOSLEEP, 1, 0, 0, 0, 0, 0) != -14) << 1;
  errors |= (unsigned long)(syscall6(SYS_CLOCK_NANOSLEEP, 2, 0, (long)&duration, 0, 0, 0) != -22) << 2;
  errors |= (unsigned long)(syscall6(SYS_CLOCK_NANOSLEEP, 1, 2, (long)&duration, 0, 0, 0) != -22) << 3;
  errors |= (unsigned long)(syscall6(SYS_CLOCK_NANOSLEEP, 1, -1, (long)&duration, 0, 0, 0) != -22) << 4;
  duration = ~0UL;
  errors |= (unsigned long)(syscall2(SYS_NANOSLEEP, (long)&duration, 0) != -22) << 5;
  errors |= (unsigned long)(syscall6(SYS_CLOCK_NANOSLEEP, 1, 0, (long)&duration, 0, 0, 0) != -22) << 6;
  // clock_getres must neither claim sub-tick precision nor accept inaccessible
  // output memory or a clock domain that the native ABI does not implement.
  errors |= clock_getres_ns(&resolution) != 0 || resolution == 0;
  errors |= syscall2(SYS_CLOCK_GETRES, 1, 0) != -14;
  errors |= syscall2(SYS_CLOCK_GETRES, 2, (long)&resolution) != -22;
  errors |= syscall2(SYS_CLOCK_GETRES, -1, (long)&resolution) != -22;
  return errors;
}

static volatile int timer_handler_called;
static void timer_signal_handler(int signo) { timer_handler_called = signo == SIGUSR1; }

static unsigned long timer_signal_interrupted(unsigned mode) {
  // POSIX sleep calls report EINTR even when the handler requests SA_RESTART.
  struct sigaction_t action = {(unsigned long)timer_signal_handler, 0, SA_RESTART};
  timer_handler_called = 0;
  if (moss_sigaction(SIGUSR1, &action, 0) != 0) {
    return 1;
  }
  long release[2];
  if (pipe(release) != 0) {
    return 1;
  }
  const long parent = getpid();
  const long child = fork();
  if (child == 0) {
    close((int)release[1]);
    unsigned cpu_mask = 2;
    if (syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0) {
      _exit(98);
    }
    long ready;
    while ((ready = control(56, parent, mode)) == 0) {
      sched_yield(); // The host case deadline bounds a missing sleep.
    }
    int errors = ready != 1 || kill(parent, SIGUSR1) != 0;
    unsigned char byte = 0;
    errors |= read((int)release[0], &byte, 1) != 1 || byte != 37;
    close((int)release[0]);
    _exit(errors ? 98 : 42);
  }
  close((int)release[0]);
  if (child < 0) {
    close((int)release[1]);
    return 1;
  }
  // The child sends only after observing this syscall asleep on CPU 0, and
  // cannot exit early to create a second wake source.
  const unsigned long duration = 2000000000UL; // Two seconds leave time for the cross-CPU signal.
  unsigned long remaining = ~0UL;
  unsigned long target = duration;
  int clock_failed = 0;
  if (mode == 2) {
    clock_failed = clock_gettime_ns(&target) != 0;
    if (clock_failed) {
      target = 0;
    }
    target += duration;
  }
  long result = mode == 0 ? syscall2(SYS_NANOSLEEP, (long)&duration, (long)&remaining)
                          : syscall6(SYS_CLOCK_NANOSLEEP, 1, mode == 2, (long)&target, (long)&remaining, 0, 0);
  int errors = clock_failed || result != -4 || timer_handler_called != 1;
  errors |= mode == 2 ? remaining != ~0UL : remaining == 0 || remaining > duration;
  const unsigned char byte = 37;
  errors |= write((int)release[1], &byte, 1) != 1;
  close((int)release[1]);
  int status = 0;
  errors |= waitpid(child, &status, 0) != child || status != (42 << 8);
  return errors;
}

unsigned long timer_relative_interrupted(void) { return timer_signal_interrupted(0); }
unsigned long timer_clock_relative_interrupted(void) { return timer_signal_interrupted(1); }
unsigned long timer_clock_absolute_interrupted(void) { return timer_signal_interrupted(2); }

unsigned long timer_short_reuse(void) {
  // Reuse a 100 us request 1000 times to exercise timer retirement/rearming.
  // The exact duration/count are fixture choices; no maximum-latency claim follows.
  unsigned long duration = 100000;
  for (unsigned i = 0; i < 1000; ++i) {
    unsigned long before = 0, after = 0;
    if (syscall2(SYS_CLOCK_GETTIME, 1, (long)&before) != 0 || syscall2(SYS_NANOSLEEP, (long)&duration, 0) != 0 ||
        syscall2(SYS_CLOCK_GETTIME, 1, (long)&after) != 0 || after < before || after - before < duration) {
      return 1;
    }
  }
  return 0;
}

unsigned long timer_early_wakeup(void) {
  // CPU0 dispatches the global timer queue. Hold the actual sleeping child on
  // CPU1 so expiry can happen before its IRQ-masked context handoff.
  unsigned mask = 2;
  if (syscall3(20, 0, sizeof(mask), (long)&mask) != 0) {
    return 1;
  }
  long child = fork();
  if (child == 0) {
    if (!control(24, 0, 0)) {
      _exit(99);
    }
    unsigned long errors = timer_relative_sleep();
    errors |= timer_absolute_sleep();
    errors |= !control(25, 0, 0);
    _exit(errors ? 99 : 37);
  }
  mask = 1;
  unsigned long errors = syscall3(20, 0, sizeof(mask), (long)&mask) != 0 || child < 0;
  if (child > 0) {
    errors |= !wait_exit(child, 37);
  }
  return errors;
}

unsigned long timer_arm_failure_recovery(void) {
  if (!control(28, 0, 0)) {
    return 1;
  }
  // A 1 s future deadline keeps the exhausted queue from being bypassed as
  // an already-expired request; controls 28/29 hold/release real timer capacity.
  unsigned long duration = 1000000000, deadline = 0;
  unsigned long errors = syscall2(SYS_NANOSLEEP, (long)&duration, 0) != -12;
  errors |= syscall2(SYS_CLOCK_GETTIME, 1, (long)&deadline) != 0;
  deadline += duration;
  errors |= syscall6(SYS_CLOCK_NANOSLEEP, 1, 1, (long)&deadline, 0, 0, 0) != -12;
  errors |= !control(29, 0, 0);
  errors |= timer_relative_sleep();
  errors |= timer_absolute_sleep();
  return errors;
}

unsigned long timer_cancel_in_flight(void) {
  if (!control(20, 0, 0)) {
    return 1;
  }
  // Two peer roles run on CPU1/CPU2 while CPU0 owns control 22; the four
  // 20..23 opcodes coordinate real in-flight cancellation and fixture retirement.
  long children[2] = {-1, -1};
  for (long actor = 1; actor <= 2; ++actor) {
    unsigned mask = 1U << actor;
    if (syscall3(20, 0, sizeof(mask), (long)&mask) != 0) {
      return 2;
    }
    children[actor - 1] = fork();
    if (children[actor - 1] == 0) {
      _exit(control(21, actor, 0) ? 37 : 99);
    }
    if (children[actor - 1] < 0) {
      return 4;
    }
  }
  unsigned mask = 1;
  if (syscall3(20, 0, sizeof(mask), (long)&mask) != 0) {
    return 8;
  }
  unsigned long errors = !control(22, 0, 0);
  for (unsigned i = 0; i < 2; ++i) {
    int status = 0;
    errors |= syscall3(SYS_WAITPID, children[i], (long)&status, 0) != children[i] || ((status >> 8) & 255) != 37;
  }
  errors |= !control(23, 0, 0);
  return errors;
}
