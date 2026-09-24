#include "validation/internal.h"

// Signal regression fixtures use nonzero child exit codes for failures; 42 is
// a successful child sentinel. Wait status stores the exit byte at bits 8..15
// (shift 8, mask 255). Each case runs in a freshly executed validation image.
static volatile int handler_called = 0;
static volatile int handler2_called = 0;
static volatile unsigned long handler_sp = 0;
// Every fork maps this symbol at the same user VA. Child-specific writes make
// stale ASID translations observable without changing the parent's value.
static volatile unsigned long asid_pattern = 0;

static void sigusr1_handler(int sig) {
  handler_called = sig == SIGUSR1;
  print("[users.signals] SIGUSR1 handler called\n");
}

static void sigusr2_handler(int sig) {
  handler2_called = sig == SIGUSR2;
  print("[users.signals] SIGUSR2 handler called\n");
}

static void nested_handler(int sig) {
  handler_called = sig == SIGUSR1;
  print("[users.signals] nested: SIGUSR1 handler, sending SIGUSR2\n");
  kill(getpid(), SIGUSR2);
}

static void altstack_handler(int sig) {
  (void)sig;
  // Read SP to verify we're on the altstack
  unsigned long sp;
#if defined(__aarch64__)
  __asm__ volatile("mov %0, sp" : "=r"(sp));
#elif defined(__x86_64__)
  __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
#elif defined(__riscv)
  __asm__ volatile("mv %0, sp" : "=r"(sp));
#endif
  handler_sp = sp;
  handler_called = 1;
  print("[users.signals] altstack handler called\n");
}

static void sigchld_handler(int sig) {
  handler_called = sig == SIGCHLD;
  print("[users.signals] SIGCHLD handler called\n");
}

// Test 1: Basic signal handler
static int test_basic_handler(void) {
  print("\n=== Test 1: Basic SIGUSR1 handler ===\n");
  handler_called = 0;

  struct sigaction_t sa;
  sa.handler = (unsigned long)sigusr1_handler;
  sa.mask = 0;
  sa.flags = 0;
  long ret = moss_sigaction(SIGUSR1, &sa, 0);
  if (ret < 0) {
    print("  FAIL: sigaction returned error\n");
    return 1;
  }

  if (kill(getpid(), SIGUSR1) != 0) {
    return 1;
  }

  if (handler_called) {
    print("  PASS: handler was called and returned\n");
    return 0;
  }
  print("  FAIL: handler was not called\n");
  return 1;
}

// Test 2: Nested signals (SIGUSR1 handler sends SIGUSR2)
static int test_nested_signals(void) {
  print("\n=== Test 2: Nested signals ===\n");
  handler_called = 0;
  handler2_called = 0;

  struct sigaction_t sa1;
  sa1.handler = (unsigned long)nested_handler;
  sa1.mask = 0; // Don't block SIGUSR2 during handler
  sa1.flags = 0;
  if (moss_sigaction(SIGUSR1, &sa1, 0) != 0) {
    return 1;
  }

  struct sigaction_t sa2;
  sa2.handler = (unsigned long)sigusr2_handler;
  sa2.mask = 0;
  sa2.flags = 0;
  if (moss_sigaction(SIGUSR2, &sa2, 0) != 0) {
    return 1;
  }

  kill(getpid(), SIGUSR1);

  if (handler_called && handler2_called) {
    print("  PASS: both handlers called\n");
    return 0;
  }
  print("  FAIL: missing handler calls\n");
  return 1;
}

// Test 3: SIGCHLD on child exit
static int test_sigchld(void) {
  print("\n=== Test 3: SIGCHLD on child exit ===\n");
  handler_called = 0;

  struct sigaction_t sa;
  sa.handler = (unsigned long)sigchld_handler;
  sa.mask = 0;
  sa.flags = 0;
  if (moss_sigaction(SIGCHLD, &sa, 0) != 0) {
    return 1;
  }

  long pid = fork();
  if (pid == 0) {
    // Child: exit immediately
    _exit(42);
  }
  // Parent: wait for child
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid || ((status >> 8) & 255) != 42) {
    return 1;
  }

  if (handler_called) {
    print("  PASS: SIGCHLD received\n");
    return 0;
  }
  print("  FAIL: SIGCHLD not delivered\n");
  return 1;
}

static int test_wait_registration(void) {
  // The ready byte proves the child reached CPU1 before CPU0 pauses in wait4;
  // otherwise CPU0 could starve a child still waiting to migrate.
  long ready[2];
  if (pipe(ready) != 0) {
    return 1;
  }
  if (control(52, 0, 0) != 1) {
    close((int)ready[0]);
    close((int)ready[1]);
    return 1;
  }
  long child = fork();
  if (child == 0) {
    close((int)ready[0]);
    unsigned cpu_mask = 2;
    unsigned char byte = 37; // A nonzero fixture marker, not a syscall result.
    if (syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0 || write((int)ready[1], &byte, 1) != 1) {
      _exit(98);
    }
    close((int)ready[1]);
    if (control(53, 0, 0) != 1) {
      _exit(98);
    }
    _exit(42);
  }
  close((int)ready[1]);
  unsigned char byte = 0;
  int prepared = child > 0 && read((int)ready[0], &byte, 1) == 1 && byte == 37;
  close((int)ready[0]);
  int status = 0;
  long waited = child > 0 ? waitpid(child, &status, 0) : -1;
  long observed = control(54, child, 0);
  return !prepared || waited != child || status != (42 << 8) || observed != 1;
}

// A restarted wait cannot release the child after waitpid returns.
static volatile long wait_restart_release_fd = -1;
static void wait_restart_handler(int signo) {
  const unsigned char byte = 37;
  handler_called = signo == SIGUSR1 && write((int)wait_restart_release_fd, &byte, 1) == 1;
}

static int wait_signal_test(int restart) {
  struct sigaction_t action = {(unsigned long)(restart ? wait_restart_handler : sigusr1_handler), 0,
                               restart ? SA_RESTART : 0};
  handler_called = 0;
  if (moss_sigaction(SIGUSR1, &action, 0) != 0) {
    return 1;
  }
  long release[2];
  if (pipe(release) != 0) {
    return 1;
  }
  wait_restart_release_fd = release[1];
  long parent = getpid();
  long child = fork();
  if (child == 0) {
    close((int)release[1]);
    unsigned cpu_mask = 2;
    if (syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0) {
      _exit(98);
    }
    long ready;
    while ((ready = control(55, parent, getpid())) == 0) {
      sched_yield(); // The host case deadline bounds a missing wait.
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
  // EINTR must leave the still-running child's status untouched. With
  // SA_RESTART, the handler releases the child and waitpid reaps it itself.
  const int status_canary = 0x5a5a5a5a;
  int status = status_canary;
  long result = waitpid(child, &status, 0);
  int errors = handler_called != 1;
  if (restart) {
    errors |= result != child || status != (42 << 8);
  } else {
    errors |= result != -4 || status != status_canary;
    const unsigned char byte = 37;
    errors |= write((int)release[1], &byte, 1) != 1;
  }
  close((int)release[1]);
  if (!restart) {
    status = 0;
    errors |= waitpid(child, &status, 0) != child || status != (42 << 8);
  }
  return errors;
}

static int test_wait_interrupted(void) { return wait_signal_test(0); }
static int test_wait_restarted(void) { return wait_signal_test(1); }

enum { CPU_BOUND_ARM_PROBE = 60, CPU_BOUND_CHECK_PROBE = 61 };
// Keep the two labels around the actual user loop in every build mode; the
// validation kernel admits the signal only after an IRQ has returned there.
__attribute__((optnone)) static int test_cpu_bound_irq(void) {
  long ready[2];
  if (pipe(ready) != 0) {
    return 1;
  }
  const long child = fork();
  if (child == 0) {
    close((int)ready[0]);
    unsigned cpu_mask = 2;
    struct sigaction_t action = {(unsigned long)sigusr1_handler, 0, 0};
    handler_called = 0;
    if (syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0 || moss_sigaction(SIGUSR1, &action, 0) != 0 ||
        control(CPU_BOUND_ARM_PROBE, (long)&&spin_begin, (long)&&spin_end) != 1) {
      _exit(98);
    }
    const unsigned char byte = 37;
    if (write((int)ready[1], &byte, 1) != 1 || close((int)ready[1]) != 0) {
      _exit(98);
    }
  spin_begin:
    while (!handler_called) {
      __asm__ volatile("" ::: "memory");
    }
  spin_end:
    _exit(handler_called == 1 && current_cpu() == 1 ? 42 : 98);
  }
  close((int)ready[1]);
  if (child < 0) {
    close((int)ready[0]);
    return 1;
  }
  unsigned char byte = 0;
  int errors = read((int)ready[0], &byte, 1) != 1 || byte != 37;
  close((int)ready[0]);
  long observed;
  while (!errors && (observed = control(CPU_BOUND_CHECK_PROBE, child, 0)) == 0) {
    sched_yield(); // Require repeated IRQ returns, not a single page fault.
  }
  if (errors || observed != 1 || kill(child, SIGUSR1) != 0) {
    kill(child, SIGKILL);
    errors = 1;
  }
  int status = 0;
  errors |= waitpid(child, &status, 0) != child || (!errors && status != (42 << 8));
  return errors;
}

enum { STOP_STATE_PROBE = 62, STOP_PENDING_PROBE = 63 };
enum {
  STOP_DEFAULT_CONT,
  STOP_MASKED_CONT,
  STOP_IGNORED_CONT,
  STOP_KILL,
  STOP_JOB_CONT,
  STOP_PENDING_ORDER,
  STOP_CASE_COUNT
};
static void sigcont_handler(int signo) { handler2_called = signo == SIGCONT; }

static int test_stop_continue(void) {
  const unsigned long cont_mask = 1UL << SIGCONT;
  const unsigned long blocked_stop_cont = cont_mask | (1UL << SIGTSTP);
  for (int mode = STOP_DEFAULT_CONT; mode < STOP_CASE_COUNT; ++mode) {
    long ready[2];
    if (pipe(ready) != 0) {
      return 1;
    }
    const long child = fork();
    if (child == 0) {
      close((int)ready[0]);
      unsigned cpu_mask = 1U << 1;
      struct sigaction_t usr1 = {(unsigned long)sigusr1_handler, 0, 0};
      unsigned long cont_handler = SIG_DFL;
      if (mode == STOP_MASKED_CONT) {
        cont_handler = (unsigned long)sigcont_handler;
      } else if (mode == STOP_IGNORED_CONT) {
        cont_handler = SIG_IGN;
      }
      struct sigaction_t cont = {cont_handler, 0, 0};
      handler_called = 0;
      handler2_called = 0;
      if (syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0 || moss_sigaction(SIGUSR1, &usr1, 0) != 0 ||
          moss_sigaction(SIGCONT, &cont, 0) != 0 ||
          (mode == STOP_MASKED_CONT && sigprocmask(SIG_BLOCK, &cont_mask, 0) != 0) ||
          (mode == STOP_PENDING_ORDER && sigprocmask(SIG_BLOCK, &blocked_stop_cont, 0) != 0)) {
        _exit(98);
      }
      const unsigned char byte = 37;
      if (write((int)ready[1], &byte, 1) != 1 || close((int)ready[1]) != 0) {
        _exit(98);
      }
      while (!handler_called) {
        __asm__ volatile("" ::: "memory");
      }
      int good = current_cpu() == 1 && handler2_called == 0;
      if (mode == STOP_MASKED_CONT) {
        good &= sigprocmask(SIG_UNBLOCK, &cont_mask, 0) == 0 && handler2_called == 1;
      } else if (mode == STOP_PENDING_ORDER) {
        good &= sigprocmask(SIG_UNBLOCK, &blocked_stop_cont, 0) == 0;
      }
      _exit(good ? 42 : 98);
    }
    close((int)ready[1]);
    if (child < 0) {
      close((int)ready[0]);
      return 1;
    }
    unsigned char byte = 0;
    int errors = read((int)ready[0], &byte, 1) != 1 || byte != 37;
    close((int)ready[0]);
    if (mode == STOP_PENDING_ORDER) {
      // The signals are blocked, so generation order alone determines which
      // member of the stop/continue pair remains pending.
      errors |= kill(child, SIGTSTP) != 0 || control(STOP_PENDING_PROBE, child, 0) != 1;
      errors |= kill(child, SIGCONT) != 0 || control(STOP_PENDING_PROBE, child, 1) != 1;
      errors |= kill(child, SIGTSTP) != 0 || control(STOP_PENDING_PROBE, child, 0) != 1;
      errors |= kill(child, SIGCONT) != 0 || control(STOP_PENDING_PROBE, child, 1) != 1;
      if (errors || kill(child, SIGUSR1) != 0) {
        kill(child, SIGKILL);
        errors = 1;
      }
      int status = 0;
      errors |= waitpid(child, &status, 0) != child || status != (42 << 8);
      if (errors) {
        return 1;
      }
      continue;
    }
    errors |= kill(child, mode == STOP_JOB_CONT ? SIGTSTP : SIGSTOP) != 0;
    long stopped = 0;
    while (!errors && (stopped = control(STOP_STATE_PROBE, child, 0)) == 0) {
      sched_yield(); // The host case deadline bounds a missing stop handoff.
    }
    errors |= stopped != 1;
    if (!errors && mode != STOP_KILL) {
      errors |= kill(child, SIGUSR1) != 0 || control(STOP_STATE_PROBE, child, 1) != 1;
    }
    if (errors || kill(child, mode == STOP_KILL ? SIGKILL : SIGCONT) != 0) {
      kill(child, SIGKILL);
      errors = 1;
    }
    int status = 0;
    errors |= waitpid(child, &status, 0) != child || status != (mode == STOP_KILL ? SIGKILL : 42 << 8);
    if (errors) {
      return 1;
    }
  }
  return 0;
}

static int run_wait_job_status(int no_cldstop, int group_wait) {
  enum { WAIT_NOHANG = 1, WAIT_UNTRACED = 2, WAIT_CONTINUED = 8 };
  long ready[2];
  if (pipe(ready) != 0) {
    return 1;
  }
  struct sigaction_t previous_chld = {0};
  if (no_cldstop) {
    struct sigaction_t action = {(unsigned long)sigchld_handler, 0, SA_NOCLDSTOP};
    if (moss_sigaction(SIGCHLD, &action, &previous_chld) != 0) {
      close((int)ready[0]);
      close((int)ready[1]);
      return 1;
    }
    handler_called = 0;
  }
  const long child = fork();
  if (child == 0) {
    close((int)ready[0]);
    unsigned cpu_mask = 1U << 1;
    struct sigaction_t usr1 = {(unsigned long)sigusr1_handler, 0, 0};
    handler_called = 0;
    if (syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0 || moss_sigaction(SIGUSR1, &usr1, 0) != 0 ||
        (group_wait && syscall0(SYS_SETPGRP) != 0)) {
      _exit(98);
    }
    const unsigned char byte = 37;
    if (write((int)ready[1], &byte, 1) != 1 || close((int)ready[1]) != 0) {
      _exit(98);
    }
    while (control(55, getppid(), getpid()) != 1) {
      sched_yield();
    }
    if (kill(getpid(), SIGSTOP) != 0) {
      _exit(98);
    }
    while (!handler_called) {
      __asm__ volatile("" ::: "memory");
    }
    _exit(current_cpu() == 1 ? 42 : 98);
  }
  close((int)ready[1]);
  if (child < 0) {
    close((int)ready[0]);
    if (no_cldstop) {
      moss_sigaction(SIGCHLD, &previous_chld, 0);
    }
    return 1;
  }
  unsigned char byte = 0;
  int errors = read((int)ready[0], &byte, 1) != 1 || byte != 37;
  close((int)ready[0]);
  const long wait_target = group_wait ? -child : child;
  int status = 0x12345678;
  if (group_wait) {
    errors |= waitpid(0, &status, WAIT_NOHANG) != -10 || status != 0x12345678;
    errors |= waitpid(-child - 1, &status, WAIT_NOHANG) != -10 || status != 0x12345678;
  }
  errors |= waitpid(wait_target, &status, WAIT_NOHANG | WAIT_UNTRACED | WAIT_CONTINUED) != 0 || status != 0x12345678;
  if (!errors) {
    errors |= waitpid(wait_target, (int *)1, WAIT_UNTRACED) != -14;
    errors |= waitpid(wait_target, &status, WAIT_UNTRACED) != child || status != ((SIGSTOP << 8) | 0x7f);
    errors |= no_cldstop && handler_called != 0;
    status = 0x12345678;
    errors |= waitpid(wait_target, &status, WAIT_NOHANG | WAIT_UNTRACED) != 0 || status != 0x12345678;
    errors |= waitpid(wait_target, &status, WAIT_NOHANG) != 0;
  }
  if (!errors) {
    errors |= kill(child, SIGCONT) != 0;
  }
  if (!errors) {
    errors |= waitpid(wait_target, &status, WAIT_CONTINUED) != child || status != 0xffff;
    errors |= no_cldstop && handler_called != 0;
    status = 0x12345678;
    errors |= waitpid(wait_target, &status, WAIT_NOHANG | WAIT_CONTINUED) != 0 || status != 0x12345678;
    errors |= waitpid(wait_target, &status, WAIT_NOHANG) != 0;
  }
  if (errors || kill(child, SIGUSR1) != 0) {
    kill(child, SIGKILL);
    errors = 1;
  }
  status = 0;
  errors |= waitpid(wait_target, &status, 0) != child || (!errors && status != (42 << 8));
  if (no_cldstop) {
    errors |= handler_called != 1;
    errors |= moss_sigaction(SIGCHLD, &previous_chld, 0) != 0;
  }
  return errors != 0;
}

static int test_wait_job_status(void) { return run_wait_job_status(0, 0); }

static int test_no_cldstop(void) { return run_wait_job_status(1, 0); }

static int test_wait_process_group(void) {
  if (run_wait_job_status(0, 1)) {
    return 1;
  }
  const long child = fork();
  if (child == 0) {
    _exit(37);
  }
  int status = 0;
  return child < 0 || waitpid(0, &status, 0) != child || status != (37 << 8);
}

static int test_wait_group_change(void) {
  for (int mode = 0; mode < 2; ++mode) {
    const long child = fork();
    if (child == 0) {
      unsigned cpu_mask = 1U << 1;
      if (syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0) {
        _exit(98);
      }
      while (control(55, getppid(), getpid()) != 1) {
        sched_yield();
      }
      const long moved = mode == 0 ? syscall0(SYS_SETPGRP) : syscall0(SYS_SETSID);
      if (moved != (mode == 0 ? 0 : getpid())) {
        _exit(98);
      }
      long waiting;
      do {
        waiting = control(55, getppid(), getpid());
        sched_yield();
      } while (waiting == 1);
      _exit(waiting == 0 ? 42 : 98);
    }
    int status = 0x12345678;
    const long result = child > 0 ? waitpid(0, &status, 0) : -1;
    int errors = child <= 0 || result != -10 || status != 0x12345678;
    if (child > 0) {
      status = 0;
      errors |= waitpid(child, &status, 0) != child || status != (42 << 8);
    }
    if (errors) {
      return 1;
    }
  }
  return 0;
}

static int test_no_cldwait(void) {
  enum { NOCLDWAIT_HANDLER, NOCLDWAIT_DEFAULT, SIGCHLD_IGNORED };
  for (int mode = NOCLDWAIT_HANDLER; mode <= SIGCHLD_IGNORED; ++mode) {
    struct sigaction_t action = {SIG_DFL, 0, SA_NOCLDWAIT};
    if (mode == NOCLDWAIT_HANDLER) {
      action.handler = (unsigned long)sigchld_handler;
    } else if (mode == SIGCHLD_IGNORED) {
      action.handler = SIG_IGN;
      action.flags = 0;
    }
    struct sigaction_t previous = {0};
    if (moss_sigaction(SIGCHLD, &action, &previous) != 0) {
      return 1;
    }
    handler_called = 0;
    const long child = fork();
    if (child == 0) {
      unsigned cpu_mask = 1U << 1;
      if (syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0) {
        _exit(98);
      }
      while (control(55, getppid(), getpid()) != 1) {
        sched_yield();
      }
      _exit(42);
    }
    int status = 0x12345678;
    const long result = child > 0 ? waitpid(child, &status, 0) : -1;
    const int errors = child <= 0 || result != -10 || status != 0x12345678 || kill(child, 0) != -3 ||
                       handler_called != (mode == NOCLDWAIT_HANDLER);
    if (moss_sigaction(SIGCHLD, &previous, 0) != 0 || errors) {
      return 1;
    }
  }
  return 0;
}

static int test_sigaction_race(void) {
  enum { SIGACTION_RACE_CONTROL = 59 }; // Private validation opcode; keep in sync with the kernel fixture.
  struct sigaction_t base = {SIG_DFL, 0, 0}, previous = {0};
  if (moss_sigaction(SIGUSR1, &base, &previous) != 0) {
    return 1;
  }
  struct sigaction_t desired = {(unsigned long)sigusr1_handler, 1UL << SIGUSR1, SA_RESTART};
  struct sigaction_t observed = {0}, current = {0};
  int errors = control(SIGACTION_RACE_CONTROL, 0, 0) != 1;
  if (!errors) {
    errors |= moss_sigaction(SIGUSR1, &desired, &observed) != 0;
    errors |= control(SIGACTION_RACE_CONTROL, 1, 0) != 1;
    errors |= observed.handler != SIG_IGN || observed.mask != (1UL << SIGUSR2) || observed.flags != SA_RESTART;
    errors |= moss_sigaction(SIGUSR1, 0, &current) != 0;
    errors |= current.handler != desired.handler || current.mask != desired.mask || current.flags != desired.flags;
  }
  errors |= moss_sigaction(SIGUSR1, &previous, 0) != 0;
  return errors;
}

static int test_sigaction_discard(void) {
  const unsigned long mask = 1UL << SIGUSR1;
  unsigned long previous_mask = 0;
  if (sigprocmask(SIG_BLOCK, &mask, &previous_mask) != 0) {
    return 1;
  }

  struct sigaction_t handler = {(unsigned long)sigusr1_handler, 0, 0};
  struct sigaction_t ignored = {SIG_IGN, 0, 0}, previous = {0};
  int errors = moss_sigaction(SIGUSR1, &handler, &previous) != 0;
  handler_called = 0;
  if (!errors) {
    errors |= kill(getpid(), SIGUSR1) != 0;
    errors |= moss_sigaction(SIGUSR1, &ignored, 0) != 0;
    errors |= kill(getpid(), SIGUSR1) != 0;
    errors |= moss_sigaction(SIGUSR1, &handler, 0) != 0;
    errors |= sigprocmask(SIG_UNBLOCK, &mask, 0) != 0;
    sched_yield();
    errors |= handler_called != 0;
    errors |= kill(getpid(), SIGUSR1) != 0;
    errors |= handler_called != 1;
  }
  errors |= sigprocmask(SIG_SETMASK, &previous_mask, 0) != 0;
  errors |= moss_sigaction(SIGUSR1, &previous, 0) != 0;
  return errors;
}

static int test_signal_exit_status(void) {
  for (int mode = 0; mode < 3; ++mode) {
    const long child = fork();
    if (child == 0) {
      if (mode == 0) {
        kill(getpid(), SIGKILL);
      } else if (mode == 1) {
        _exit(-SIGSEGV); // A negative user exit code must remain a normal exit.
      } else {
        *(volatile long *)1 = 1;
      }
      _exit(98);
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child) {
      return 1;
    }
    int expected = SIGSEGV;
    if (mode == 0) {
      expected = SIGKILL;
    } else if (mode == 1) {
      expected = (unsigned char)-SIGSEGV << 8;
    }
    if (status != expected) {
      return 1;
    }
  }
  return 0;
}

// Test 4: sigprocmask — block and unblock
static int test_sigprocmask(void) {
  print("\n=== Test 4: sigprocmask block/unblock ===\n");
  handler_called = 0;

  struct sigaction_t sa;
  sa.handler = (unsigned long)sigusr1_handler;
  sa.mask = 0;
  sa.flags = 0;
  if (moss_sigaction(SIGUSR1, &sa, 0) != 0) {
    return 1;
  }

  // Block SIGUSR1
  unsigned long mask = (1UL << SIGUSR1);
  if (sigprocmask(SIG_BLOCK, &mask, 0) != 0) {
    return 1;
  }

  // Send — should be pended, not delivered
  kill(getpid(), SIGUSR1);
  if (handler_called) {
    print("  FAIL: handler called while signal blocked\n");
    return 1;
  }

  // Unblock — should deliver on next syscall return
  if (sigprocmask(SIG_UNBLOCK, &mask, 0) != 0) {
    return 1;
  }
  // Force a syscall to trigger signal checkpoint
  sched_yield();

  if (handler_called) {
    print("  PASS: signal delivered after unblock\n");
    return 0;
  }
  print("  FAIL: signal not delivered after unblock\n");
  return 1;
}

// Test 5: sigaltstack
static volatile int alt_nested_errors, alt_nested_phase;
static unsigned long alt_nested_base, alt_nested_top;

static void alt_nested_handler(int signo) {
  struct stack_t current, disabled = {0, 0, SS_DISABLE};
  unsigned long mask = 0;
  // An address in this C frame proves both handlers use the registered stack.
  volatile unsigned long canary = 0x71b59a63UL; // Mixed bytes expose nested-stack clobbering.
  alt_nested_errors |= (unsigned long)&canary < alt_nested_base || (unsigned long)&canary >= alt_nested_top;
  alt_nested_errors |= sigaltstack(0, &current) != 0 || current.ss_flags != SS_ONSTACK;
  alt_nested_errors |= sigaltstack(&disabled, 0) != -1; // EPERM while an alternate-stack handler is active.
  alt_nested_errors |= sigprocmask(SIG_SETMASK, 0, &mask) != 0;
  unsigned long expected = (1UL << SIGHUP) | (1UL << SIGUSR1);
  if (signo == SIGUSR2) {
    expected |= 1UL << SIGUSR2;
    alt_nested_errors |= alt_nested_phase != 1 || mask != expected;
    alt_nested_phase = 2;
  } else {
    alt_nested_errors |= signo != SIGUSR1 || alt_nested_phase != 0 || mask != expected;
    alt_nested_phase = 1;
    alt_nested_errors |= kill(getpid(), SIGUSR2) != 0 || alt_nested_phase != 2;
    alt_nested_errors |= canary != 0x71b59a63UL;
    alt_nested_errors |= sigprocmask(SIG_SETMASK, 0, &mask) != 0 || mask != expected;
    alt_nested_errors |= sigaltstack(0, &current) != 0 || current.ss_flags != SS_ONSTACK;
    alt_nested_phase = 3;
  }
}

static int test_sigaltstack(void) {
  print("\n=== Test 5: sigaltstack ===\n");
  handler_called = 0;
  handler_sp = 0;

  // Keep the buffer alive through signal delivery. 8192 bytes exceeds the native
  // 2048-byte admission floor and leaves handler workspace; exact sizing evidence
  // is not recorded. Alignment 16 matches the signal-frame and call-stack ABI.
  static char altstack_buf[8192] __attribute__((aligned(16)));
  unsigned long altstack_base = (unsigned long)altstack_buf;
  unsigned long altstack_top = altstack_base + sizeof(altstack_buf);

  struct stack_t ss;
  ss.ss_sp = altstack_base;
  ss.ss_size = sizeof(altstack_buf);
  ss.ss_flags = 0;
  long ret = sigaltstack(&ss, 0);
  if (ret < 0) {
    print("  FAIL: sigaltstack returned error\n");
    return 1;
  }

  struct sigaction_t sa;
  sa.handler = (unsigned long)altstack_handler;
  sa.mask = 0;
  sa.flags = SA_ONSTACK;
  if (moss_sigaction(SIGUSR1, &sa, 0) != 0) {
    return 1;
  }

  kill(getpid(), SIGUSR1);

  if (handler_called && handler_sp >= altstack_base && handler_sp < altstack_top) {
    print("  PASS: handler ran on altstack\n");
    alt_nested_base = altstack_base;
    alt_nested_top = altstack_top;
    sa.handler = (unsigned long)alt_nested_handler;
    if (moss_sigaction(SIGUSR1, &sa, 0) != 0) {
      return 1;
    }
    // A nested handler uses the current alternate stack regardless of its own
    // SA_ONSTACK bit. Repeat after a complete return to verify state reset.
    const unsigned long initial_mask = 1UL << SIGHUP;
    for (int onstack = 0; onstack <= 1; ++onstack) {
      sa.flags = onstack ? SA_ONSTACK : 0;
      alt_nested_phase = 0;
      if (moss_sigaction(SIGUSR2, &sa, 0) != 0 || sigprocmask(SIG_SETMASK, &initial_mask, 0) != 0 ||
          frame_register_probe(SYS_KILL, getpid()) != 0 || alt_nested_phase != 3 || alt_nested_errors) {
        return 1;
      }
      unsigned long restored = 0;
      struct stack_t current;
      if (sigprocmask(SIG_SETMASK, 0, &restored) != 0 || restored != initial_mask || sigaltstack(0, &current) != 0 ||
          current.ss_flags != 0) {
        return 1;
      }
    }
    return 0;
  }
  if (handler_called) {
    print("  WARN: handler called but SP not on altstack\n");
    return 1;
  }
  print("  FAIL: handler not called\n");
  return 1;
}

// Test 6: SIG_IGN
static int test_sig_ign(void) {
  print("\n=== Test 6: SIG_IGN ===\n");

  struct sigaction_t sa;
  sa.handler = SIG_IGN;
  sa.mask = 0;
  sa.flags = 0;
  if (moss_sigaction(SIGUSR1, &sa, 0) != 0) {
    return 1;
  }

  // Send SIGUSR1 — should be silently ignored
  if (kill(getpid(), SIGUSR1) != 0) {
    return 1;
  }
  print("  PASS: SIG_IGN — signal ignored, process alive\n");

  // Restore default
  sa.handler = SIG_DFL;
  return moss_sigaction(SIGUSR1, &sa, 0) != 0;
}

static int test_invalid_arguments(void) {
  unsigned check = 0;
  unsigned failures = 0;
#define CHECK(expr)                                                                                                    \
  do {                                                                                                                 \
    ++check;                                                                                                           \
    if (!(expr)) {                                                                                                     \
      print("  FAIL: signal argument check ");                                                                         \
      print_ulong(check);                                                                                              \
      print("\n");                                                                                                     \
      ++failures;                                                                                                      \
    }                                                                                                                  \
  } while (0)
  // Native errno results: EINVAL=-22, ESRCH=-3, EFAULT=-14, ENOMEM=-12.
  // Signals 0 and 32 fall outside the handler range 1..31; adding 2^32 tests
  // rejection before narrowing a raw 64-bit PID or signal to its smaller type.
  // 0x1000 belongs to the reserved low identity region, outside process VMAs.
  struct sigaction_t sa = {(unsigned long)sigusr1_handler, 0, 0};
  CHECK(moss_sigaction(0, &sa, 0) == -22);
  CHECK(moss_sigaction(32, &sa, 0) == -22);
  CHECK(moss_sigaction(SIGKILL, &sa, 0) == -22);
  CHECK(moss_sigaction(SIGSTOP, &sa, 0) == -22);
  CHECK(syscall3(SYS_SIGACTION, (1L << 32) + SIGUSR1, (long)&sa, 0) == -22);
  CHECK(kill(0x7fffffff, 0) == -3);
  CHECK(kill((1L << 32) + getpid(), 0) == -3);
  CHECK(kill(-0x7fffffffffffffffL - 1, 0) == -3);
  CHECK(syscall2(SYS_KILL, getpid(), (1L << 32) + SIGUSR1) == -22);
  CHECK(moss_sigaction(SIGUSR1, (void *)0x1000, 0) == -14);
  CHECK(moss_sigaction(SIGUSR1, &sa, (void *)0x1000) == -14);
  sa.flags = 0x20; // Bit 5 remains outside the supported native action flags.
  CHECK(moss_sigaction(SIGUSR1, &sa, 0) == -22);
  unsigned long bits = (1UL << SIGKILL) | (1UL << SIGSTOP), old = 0;
  CHECK(sigprocmask(SIG_SETMASK, &bits, 0) == 0);
  CHECK(sigprocmask(SIG_SETMASK, 0, &old) == 0 && (old & bits) == 0);
  CHECK(sigprocmask(3, &bits, 0) == -22);
  CHECK(sigprocmask(SIG_SETMASK, (void *)0x1000, 0) == -14);
  // 2^47 is the exclusive user-address ceiling; unsigned -4096 also checks
  // wrapped/out-of-range bases. 8192 supplies an otherwise admissible size.
  struct stack_t ss = {0x800000000000UL, 8192, 0};
  CHECK(sigaltstack(&ss, 0) == -14);
  ss.ss_sp = (unsigned long)-4096;
  CHECK(sigaltstack(&ss, 0) == -14);
  ss.ss_sp = (unsigned long)&ss;
  ss.ss_flags = 4; // Unsupported bit, beyond SS_ONSTACK=1 and SS_DISABLE=2.
  CHECK(sigaltstack(&ss, 0) == -22);
  ss.ss_flags = 0;
  ss.ss_size = 2047; // MINSIGSTKSZ (2048) minus one must fail admission.
  CHECK(sigaltstack(&ss, 0) == -12);
  CHECK(syscall0(SYS_SIGRETURN) == -14); // No active signal frame.
#undef CHECK
  return failures != 0;
}

// Exercise the real user/kernel ABI without relying on a handler's compiler-
// chosen stack frame. On rejection, restore the caller's SP and report errno.
// The immediate 17 is SYS_SIGRETURN in the native Moss ABI on all three CPUs.
__attribute__((naked)) static long try_sigreturn(unsigned long frame) {
#if defined(__aarch64__)
  asm volatile("mov x9, sp; mov sp, x0; mov x8, #17; svc #0; mov sp, x9; ret");
#elif defined(__x86_64__)
  asm volatile("mov %rsp, %r10; mov %rdi, %rsp; mov $17, %eax; syscall; mov %r10, %rsp; ret");
#else
  asm volatile("mv t1, sp; mv sp, a0; li a7, 17; ecall; mv sp, t1; ret");
#endif
}

// Moss signal ABI v2: native registers plus architecture FP state. Deliberate
// independent layout assertion catches accidental kernel/userspace ABI drift.
// 31 GP slots reserve the architecture-neutral register snapshot. The first
// 37 u64 fields occupy 296 bytes, followed by 8 alignment bytes: FP starts at
// 304. Its 64 u64 slots hold 512 bytes; four trailing u64 fields make 848 total.
// Change these assertions together with the kernel signal-frame ABI.
struct __attribute__((aligned(16))) signal_frame_t {
  unsigned long magic, gp_regs[31], pc, flags, sp, fpsr, fpcr;
  unsigned long fp[64] __attribute__((aligned(16)));
  unsigned long signo, mask, on_alt_stack, previous;
};
_Static_assert(sizeof(struct signal_frame_t) == 848, "signal frame ABI");
_Static_assert(__builtin_offsetof(struct signal_frame_t, fp) == 304, "signal FP ABI");
// 16 KiB keeps the forged frame and handler workspace in persistent storage;
// the exact workspace budget has no recorded measurement.
static unsigned char frame_stack[16384] __attribute__((aligned(16)));
static volatile int frame_errors;
static volatile int frame_mode;
static unsigned long frame_kernel_address;

static void reject_frame_field(struct signal_frame_t *sf, unsigned long *field, unsigned long value) {
  const unsigned long saved = *field;
  *field = value;
  frame_errors |= try_sigreturn((unsigned long)sf) != -14;
  *field = saved;
}

// Issue kill from a caller-selected, aligned user SP without a compiler frame.
// Native syscall IDs 14/1 are kill/exit; exit marker 93 means the delivery that
// should have killed this child unexpectedly returned to the interrupted PC.
__attribute__((naked, noreturn)) static void signal_from_stack(unsigned long stack, long pid, long signo) {
#if defined(__aarch64__)
  asm volatile("mov sp, x0; mov x0, x1; mov x1, x2; mov x8, #14; svc #0; "
               "mov x0, #93; mov x8, #1; svc #0; brk #0");
#elif defined(__x86_64__)
  asm volatile("mov %rdi, %rsp; mov %rsi, %rdi; mov %rdx, %rsi; mov $14, %eax; syscall; "
               "mov $93, %edi; mov $1, %eax; syscall; ud2");
#else
  asm volatile("mv sp, a0; mv a0, a1; mv a1, a2; li a7, 14; ecall; "
               "li a0, 93; li a7, 1; ecall; unimp");
#endif
}

static void overflow_inner_handler(int signo) {
  (void)signo;
  _exit(94); // Distinguish an illegally delivered nested frame from rejection.
}

static void overflow_outer_handler(int signo) {
  struct stack_t current;
  if (signo != SIGUSR1 || sigaltstack(0, &current) != 0 || !(current.ss_flags & SS_ONSTACK)) {
    _exit(91); // The intended on-altstack precondition was not established.
  }
  // One ABI alignment unit remains above the registered base. The complete
  // 848-byte frame cannot fit, even though memory below that base is writable.
  signal_from_stack(current.ss_sp + 16, getpid(), SIGUSR2);
}

static int test_altstack_overflow(void) {
  // One writable base page below the registered stack ensures a missing range
  // check cannot accidentally pass because an unmapped guard page faults. Two
  // further pages provide the outer frame and C-handler workspace; the nested
  // probe explicitly shrinks the remaining capacity to one alignment unit.
  enum { PREFIX_BYTES = 4096, STACK_BYTES = 2 * PREFIX_BYTES };
  const unsigned char canary = 0xa7; // Distinct from zero and serialized frame bytes.
  for (unsigned i = 0; i < PREFIX_BYTES; ++i) {
    frame_stack[i] = canary;
  }
  long child = fork();
  if (child == 0) {
    struct stack_t ss = {(unsigned long)frame_stack + PREFIX_BYTES, STACK_BYTES, 0};
    struct sigaction_t outer = {(unsigned long)overflow_outer_handler, 0, SA_ONSTACK};
    struct sigaction_t inner = {(unsigned long)overflow_inner_handler, 0, SA_ONSTACK};
    if (sigaltstack(&ss, 0) != 0 || moss_sigaction(SIGUSR1, &outer, 0) != 0 ||
        moss_sigaction(SIGUSR2, &inner, 0) != 0) {
      _exit(92); // Setup failure must not match the fatal-delivery exit code.
    }
    kill(getpid(), SIGUSR1);
    _exit(93);
  }
  int status = 0;
  // Failed signal-frame setup must identify the signal that caused termination.
  const int rejected = child > 1 && waitpid(child, &status, 0) == child && status == SIGUSR2;
  if (!rejected) {
    print("altstack overflow child status: ");
    print_long(status);
    print("\n");
  }
  for (unsigned i = 0; i < PREFIX_BYTES; ++i) {
    if (((volatile unsigned char *)frame_stack)[i] != canary) {
      return 1; // Child failure must not corrupt the parent's COW backing.
    }
  }
  return !rejected;
}

static int test_altstack_boundaries(void) {
  // Native pages are 4 KiB; two pages leave ordinary handler workspace, while
  // the adjacent read-only page tests whole-interval admission across VMAs.
  enum { PAGE_BYTES = 4096, STACK_BYTES = 2 * PAGE_BYTES };
  // Native mmap protection 3=R|W, 1=R; 0x22 is PRIVATE|ANONYMOUS, not MAP_FIXED.
  long area = syscall6(SYS_MMAP, 0, STACK_BYTES, 3, 0x22, -1, 0);
  if (area <= 0) {
    return 1;
  }
  long adjacent = syscall6(SYS_MMAP, area + STACK_BYTES, PAGE_BYTES, 1, 0x22, -1, 0);
  if (adjacent != area + STACK_BYTES) {
    return 1; // Prove the intended adjacent VMA layout, not just two mappings.
  }
  struct stack_t ss = {(unsigned long)area, STACK_BYTES + PAGE_BYTES, 0};
  int errors = sigaltstack(&ss, 0) != -14;
  ss.ss_sp = (unsigned long)vm_rodata;
  ss.ss_size = sizeof(vm_rodata);
  errors |= sigaltstack(&ss, 0) != -14;
  ss.ss_sp = (unsigned long)area;
  ss.ss_size = ~(unsigned long)area + 1; // Exact end-address wrap to zero.
  errors |= sigaltstack(&ss, 0) != -14;
  errors |= syscall2(SYS_MUNMAP, adjacent, PAGE_BYTES) != 0;
  ss.ss_sp = (unsigned long)adjacent;
  ss.ss_size = PAGE_BYTES;
  errors |= sigaltstack(&ss, 0) != -14;
  // Both aliases refer to a real mapped kernel-data sentinel. Controls 50/51
  // share the VM-isolation fixture; target 2 identifies kernel data.
  for (long alias = 0; alias <= 1; ++alias) {
    long address = control(50, 2, alias);
    if (!address) {
      return 1;
    }
    ss.ss_sp = (unsigned long)address;
    errors |= sigaltstack(&ss, 0) != -14;
    errors |= !control(51, 2, alias);
  }
  struct stack_t current;
  errors |= sigaltstack(0, &current) != 0 || current.ss_flags != SS_DISABLE;
  // Registration is not a lease: delivery must revalidate a later unmap or
  // read-only replacement. Each attack has its own child and exact exit oracle.
  const unsigned long canary = 0x6d71b3a5UL; // Nonzero mixed bytes detect parent-page corruption.
  *(volatile unsigned long *)area = canary;
  if (!control(50, 2, 0)) {
    return 1;
  }
  for (int readonly = 0; readonly <= 1; ++readonly) {
    long child = fork();
    if (child == 0) {
      struct stack_t child_stack = {(unsigned long)area, STACK_BYTES, 0};
      struct sigaction_t action = {(unsigned long)overflow_inner_handler, 0, SA_ONSTACK};
      if (sigaltstack(&child_stack, 0) != 0 || moss_sigaction(SIGUSR1, &action, 0) != 0 ||
          syscall2(SYS_MUNMAP, area, STACK_BYTES) != 0) {
        _exit(92); // Setup failures are distinct from failed signal delivery.
      }
      if (readonly && syscall6(SYS_MMAP, area, STACK_BYTES, 1, 0x22, -1, 0) != area) {
        _exit(92);
      }
      kill(getpid(), SIGUSR1);
      _exit(93); // Delivery unexpectedly returned; handler execution uses 94.
    }
    errors |= !wait_signal(child, SIGUSR1);
    errors |= *(volatile unsigned long *)area != canary;
  }
  errors |= !control(51, 2, 0);
  errors |= syscall2(SYS_MUNMAP, area, STACK_BYTES) != 0;
  return errors;
}

static void frame_handler(int signo) {
  unsigned long red_zone = 0;
#if defined(__x86_64__)
  red_zone = 128; // x86-64 SysV reserves 128 bytes below the interrupted SP.
#endif
  struct signal_frame_t *sf = (void *)(frame_stack + sizeof(frame_stack) - red_zone - sizeof(struct signal_frame_t));
  frame_errors |= signo != SIGUSR1;
  // Modes 0/1/2 corrupt magic/PC/SP; mode 3 requests privileged flags, mode 4
  // observes the next native trap after that return; x86 mode 5 checks MXCSR.
  if (frame_mode < 3) {
    unsigned long *field = &sf->sp;
    if (frame_mode == 0) {
      field = &sf->magic;
    } else if (frame_mode == 1) {
      field = &sf->pc;
    }
    unsigned long saved = *field;
    *field = frame_mode == 0 ? 0 : 0xfffffffffffff000UL;
    frame_errors |= try_sigreturn((unsigned long)sf) != -14;
    *field = saved;
    if (frame_mode == 0) {
      reject_frame_field(sf, &sf->pc, frame_kernel_address);
      reject_frame_field(sf, &sf->sp, frame_kernel_address);
      reject_frame_field(sf, &sf->pc, (unsigned long)frame_stack);                   // Writable, but not executable.
      reject_frame_field(sf, &sf->sp, (unsigned long)vm_rodata + sizeof(vm_rodata)); // Read-only stack.
      reject_frame_field(sf, &sf->sp, 0);
      reject_frame_field(sf, &sf->previous, frame_kernel_address);
#if !defined(__x86_64__)
      // Unlike x86, native ARM/RV instruction and stack alignment is mandatory.
      reject_frame_field(sf, &sf->pc, sf->pc | 1UL);
      reject_frame_field(sf, &sf->sp, sf->sp - 1);
#endif
      // A half-aligned frame and a valid copied frame at an inactive address
      // must fail without consuming the real active frame or changing masks.
      frame_errors |= try_sigreturn((unsigned long)sf + sizeof(unsigned long)) != -14;
      struct signal_frame_t *copy = (void *)frame_stack;
      // This freestanding fixture has no libc memcpy; volatile byte accesses
      // also force the compiler to materialize the entire forged user frame.
      for (unsigned i = 0; i < sizeof(*sf); ++i) {
        ((volatile unsigned char *)copy)[i] = ((volatile unsigned char *)sf)[i];
      }
      frame_errors |= try_sigreturn((unsigned long)copy) != -14;
    }
  } else if (frame_mode == 3) {
    // The kernel may restore arithmetic flags, never privileged return modes,
    // interrupt masks, IOPL, SUM or a blocked SIGKILL/SIGSTOP.
#if defined(__aarch64__)
    sf->flags |= 0x3cfUL; // EL mode and DAIF
#elif defined(__x86_64__)
    sf->flags |= 0x1a3000UL; // IOPL, VM, VIF, VIP (not the user's DF)
#else
    sf->flags |= (1UL << 8) | (1UL << 18) | 2; // SPP, SUM, SIE
#endif
    sf->mask |= (1UL << SIGKILL) | (1UL << SIGSTOP);
  } else if (frame_mode == 4) {
    // Inspect a fresh kernel-written frame after the forged return, not the
    // userspace buffer we modified. Ordinary GP/arithmetic flags are checked
    // independently by frame_register_probe around each signal delivery.
#if defined(__aarch64__)
    frame_errors |= (sf->flags & 0x3cfUL) != 0;
#elif defined(__x86_64__)
    frame_errors |= (sf->flags & 0x1a3000UL) != 0;
#else
    frame_errors |= (sf->flags & ((1UL << 8) | (1UL << 18) | 2)) != 0;
#endif
  }
#if defined(__x86_64__)
  else {
    // FXSAVE byte 24 is u64 slot 3: low 32 bits are MXCSR and high 32 are
    // MXCSR_MASK. Bit 31 is reserved, while 0x40 is the optional DAZ bit 6.
    const unsigned long saved = sf->fp[3]; // MXCSR + hardware capability mask
    sf->fp[3] |= 1UL << 31;
    frame_errors |= try_sigreturn((unsigned long)sf) != -14;
    // DAZ is valid only when the CPU's FXSAVE mask advertises it. A conservative
    // hard-coded mask must not reject a legitimate saved user context.
    sf->fp[3] = saved | ((saved >> 32) & 0x40);
  }
#endif
  handler_called = 1;
}

static int test_frame_validation(void) {
  struct stack_t ss = {(unsigned long)frame_stack, sizeof(frame_stack), 0};
  struct sigaction_t sa = {(unsigned long)sigusr1_handler, 0, SA_ONSTACK};
  if (sigaltstack(&ss, 0) != 0 || moss_sigaction(SIGUSR1, &sa, 0) != 0) {
    return 1;
  }
  // Complete one benign return before freezing the root-table snapshot. The
  // lazy sigreturn trampoline at 6 GiB occupies a separate Sv39 root entry;
  // its first instruction fault legitimately allocates that user subtree.
  handler_called = 0;
  if (kill(getpid(), SIGUSR1) != 0 || !handler_called) {
    return 1;
  }
  sa.handler = (unsigned long)frame_handler;
  if (moss_sigaction(SIGUSR1, &sa, 0) != 0) {
    return 1;
  }
  // Existing isolation controls 50/51 snapshot/verify real kernel data and
  // its root mapping. Target 2 is kernel data; alias 0 is the identity address.
  frame_kernel_address = (unsigned long)control(50, 2, 0);
  if (!frame_kernel_address) {
    return 1;
  }
  // Five common frame cases plus the sixth, x86-only MXCSR case above.
  const int count =
#if defined(__x86_64__)
      6;
#else
      5;
#endif
  for (frame_mode = 0; frame_mode < count; ++frame_mode) {
    handler_called = 0;
    if (frame_register_probe(SYS_KILL, getpid()) != 0 || !handler_called || frame_errors) {
      return 1;
    }
    unsigned long mask = ~0UL;
    if (sigprocmask(SIG_SETMASK, 0, &mask) != 0 || (mask & ((1UL << SIGKILL) | (1UL << SIGSTOP)))) {
      return 1;
    }
    struct stack_t current;
    if (sigaltstack(0, &current) != 0 || (current.ss_flags & SS_ONSTACK)) {
      return 1;
    }
  }
  return !control(51, 2, 0);
}

static int test_exec_reset(void) {
  struct sigaction_t first, second;
  struct stack_t stack;
  unsigned long mask = 0;
  return moss_sigaction(SIGUSR1, 0, &first) != 0 || first.handler != SIG_DFL ||
         moss_sigaction(SIGUSR2, 0, &second) != 0 || second.handler != SIG_IGN ||
         sigprocmask(SIG_SETMASK, 0, &mask) != 0 || mask != (1UL << SIGUSR1) || sigaltstack(0, &stack) != 0 ||
         stack.ss_flags != SS_DISABLE;
}

static int test_inheritance(void) {
  struct sigaction_t sa = {(unsigned long)sigusr1_handler, 0, 0};
  struct sigaction_t ignore = {SIG_IGN, 0, 0};
  struct stack_t stack = {(unsigned long)frame_stack, sizeof(frame_stack), 0};
  unsigned long mask = 1UL << SIGUSR1;
  if (moss_sigaction(SIGUSR1, &sa, 0) != 0 || moss_sigaction(SIGUSR2, &ignore, 0) != 0 || sigaltstack(&stack, 0) != 0 ||
      sigprocmask(SIG_BLOCK, &mask, 0) != 0 || kill(getpid(), SIGUSR1) != 0) {
    return 1;
  }
  long child = fork();
  if (child == 0) {
    struct sigaction_t inherited;
    struct stack_t ss;
    unsigned long bits = 0;
    if (moss_sigaction(SIGUSR1, 0, &inherited) != 0 || inherited.handler != sa.handler ||
        sigprocmask(SIG_SETMASK, 0, &bits) != 0 || bits != mask || sigaltstack(0, &ss) != 0 ||
        ss.ss_sp != stack.ss_sp || ss.ss_size != stack.ss_size || ss.ss_flags != 0) {
      _exit(71);
    }
    // Pending signals are not inherited; masks/actions/altstack are.
    handler_called = 0;
    if (sigprocmask(SIG_UNBLOCK, &mask, 0) != 0 || handler_called || kill(getpid(), SIGUSR1) != 0 || !handler_called ||
        sigprocmask(SIG_BLOCK, &mask, 0) != 0) {
      _exit(72);
    }
    const char *args[] = {"validation", "signals", "exec_reset", 0};
    syscall3(SYS_EXECVE, (long)"/validation.elf", (long)args, 0);
    _exit(73);
  }
  int status = 0;
  if (child < 0 || waitpid(child, &status, 0) != child || status != 0) {
    return 1;
  }
  handler_called = 0;
  return sigprocmask(SIG_UNBLOCK, &mask, 0) != 0 || !handler_called;
}

static void quiet_handler(int signo) { handler_called = signo == SIGUSR1; }

static void sigpipe_handler(int signo) { handler_called += signo == SIGPIPE; }

static int test_pipe_sigpipe(void) {
  long ends[2];
  if (pipe(ends) != 0) {
    return 1;
  }
  const unsigned char byte = 37; // Nonzero payload distinguishes delivery from an untouched buffer.
  struct sigaction_t action = {(unsigned long)sigpipe_handler, 0, 0};
  handler_called = 0;
  int errors = close((int)ends[0]) != 0 || moss_sigaction(SIGPIPE, &action, 0) != 0;
  errors |= write((int)ends[1], &byte, 1) != -32 || handler_called != 1;
  unsigned long mask = 1UL << SIGPIPE;
  errors |= sigprocmask(SIG_BLOCK, &mask, 0) != 0;
  errors |= write((int)ends[1], &byte, 1) != -32 || handler_called != 1;
  errors |= sigprocmask(SIG_UNBLOCK, &mask, 0) != 0 || handler_called != 2;
  action.handler = SIG_IGN;
  errors |= moss_sigaction(SIGPIPE, &action, 0) != 0;
  errors |= write((int)ends[1], &byte, 1) != -32 || handler_called != 2;
  long child = fork();
  if (child == 0) {
    action.handler = SIG_DFL;
    if (moss_sigaction(SIGPIPE, &action, 0) != 0) {
      _exit(98);
    }
    write((int)ends[1], &byte, 1);
    _exit(99); // The default action must terminate at the syscall return.
  }
  int status = 0;
  errors |= child < 0 || waitpid(child, &status, 0) != child || status != SIGPIPE;
  errors |= close((int)ends[1]) != 0;
  return errors;
}

enum pipe_disposition { PIPE_CAUGHT, PIPE_IGNORED, PIPE_BLOCKED, PIPE_RESTART };

// A restarted read/write releases its peer from the handler, before returning.
static volatile long pipe_restart_ack_fd = -1;
static void pipe_restart_handler(int signo) {
  const unsigned char byte = 37;
  handler_called = signo == SIGUSR1 && write((int)pipe_restart_ack_fd, &byte, 1) == 1;
}

static int pipe_signal_wait(int writing, enum pipe_disposition disposition) {
  // Validation syscall 511, operation 39 reports a queued pipe waiter. A
  // return of 1 enables coordination; -38 (ENOSYS) keeps standalone fallback.
  // Keep this protocol synchronized with moss_validation_call.
  const long fixture = syscall3(511, 39, 0, 0);
  if (fixture != 1 && fixture != -38) {
    return 1;
  }
  const int coordinated = fixture == 1;
  long ends[2];
  if (pipe(ends) != 0) {
    return 1;
  }
  long acknowledgement[2];
  const int caught = disposition == PIPE_CAUGHT || disposition == PIPE_RESTART;
  const int acknowledge = coordinated && caught;
  if (acknowledge && pipe(acknowledgement) != 0) {
    close((int)ends[0]);
    close((int)ends[1]);
    return 1;
  }
  if (disposition == PIPE_RESTART && acknowledge) {
    pipe_restart_ack_fd = acknowledgement[1];
  }
  // One 4096-byte pipe capacity makes the next single-byte write block.
  // The byte-index pattern detects changed or misplaced bytes across the wait.
  unsigned char data[4096];
  for (unsigned i = 0; i < sizeof(data); ++i) {
    data[i] = (unsigned char)i;
  }
  struct sigaction_t action = {(unsigned long)(disposition == PIPE_RESTART ? pipe_restart_handler : quiet_handler), 0,
                               disposition == PIPE_RESTART ? SA_RESTART : 0};
  if (disposition == PIPE_IGNORED) {
    action.handler = SIG_IGN;
  }
  handler_called = 0;
  int errors = moss_sigaction(SIGUSR1, &action, 0) != 0;
  unsigned long mask = 1UL << SIGUSR1;
  if (disposition == PIPE_BLOCKED) {
    errors |= sigprocmask(SIG_BLOCK, &mask, 0) != 0;
  }
  // SYS_SCHED_SETAFFINITY=20 uses bit 0 for CPU 0 and bit 1 (value 2) for
  // CPU 1, placing the sender and waiter on different CPUs.
  unsigned cpu_mask = 1;
  errors |= syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0;
  if (writing) {
    errors |= write((int)ends[1], data, sizeof(data)) != sizeof(data);
  }
  long parent = getpid();
  long child = fork();
  if (child == 0) {
    close((int)ends[writing ? 1 : 0]);
    if (acknowledge) {
      close((int)acknowledgement[1]);
    }
    cpu_mask = 2;
    if (syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0) {
      _exit(98);
    }
    // Affinity takes effect on the existing sleep/wakeup path. This sleep is
    // not evidence of parent readiness; the validation hook checks that below.
    unsigned long delay = 10000000; // 10 ms in ns; the exact delay has no timing calibration.
    int failed = nanosleep_ns(&delay) != 0 || current_cpu() != 1;
    if (coordinated) {
      long ready;
      while ((ready = syscall3(511, 39, parent, ends[writing ? 1 : 0])) == 0) {
        sched_yield(); // The unchanged host watchdog bounds a missing waiter.
      }
      if (ready != 1) {
        _exit(97);
      }
    }
    failed |= kill(parent, SIGUSR1) != 0;
    if (acknowledge) {
      unsigned char observed = 0;
      failed |= read((int)acknowledgement[0], &observed, 1) != 1 || observed != 37;
      failed |= close((int)acknowledgement[0]) != 0;
    } else if (!coordinated) {
      failed |= nanosleep_ns(&delay) != 0;
    }
    if (writing) {
      failed |= read((int)ends[0], data, sizeof(data)) != sizeof(data);
      for (unsigned i = 0; i < sizeof(data); ++i) {
        failed |= data[i] != (unsigned char)i;
      }
      failed |= read((int)ends[0], data, 1) != (disposition == PIPE_CAUGHT ? 0 : 1);
      if (disposition != PIPE_CAUGHT) {
        failed |= data[0] != 0 || read((int)ends[0], data, 1) != 0;
      }
    } else {
      data[0] = 37;
      failed |= write((int)ends[1], data, 1) != 1;
    }
    close((int)ends[writing ? 0 : 1]);
    _exit(failed);
  }
  if (child < 0) {
    close((int)ends[0]);
    close((int)ends[1]);
    if (acknowledge) {
      close((int)acknowledgement[0]);
      close((int)acknowledgement[1]);
    }
    return 1;
  }
  if (acknowledge) {
    errors |= close((int)acknowledgement[0]) != 0;
  }
  errors |= close((int)ends[writing ? 0 : 1]) != 0;
  // Exercise preparation that outlasts the old sender's guessed 10 ms delay.
  unsigned long prepare_delay = 30000000; // 30 ms in ns, deliberately longer than the sender's 10 ms.
  if (coordinated) {
    errors |= nanosleep_ns(&prepare_delay) != 0;
  }
  long result = writing ? write((int)ends[1], data, 1) : read((int)ends[0], data, 1);
  errors |= result != (disposition == PIPE_CAUGHT ? -4 : 1) || handler_called != caught;
  errors |= current_cpu() != 0;
  if (acknowledge) {
    if (disposition != PIPE_RESTART) {
      const unsigned char observed = 37;
      errors |= write((int)acknowledgement[1], &observed, 1) != 1;
    }
    errors |= close((int)acknowledgement[1]) != 0;
  }
  if (!writing && result == -4) {
    errors |= read((int)ends[0], data, 1) != 1 || data[0] != 37;
  } else if (!writing) {
    errors |= data[0] != 37;
  }
  if (disposition == PIPE_BLOCKED) {
    errors |= sigprocmask(SIG_UNBLOCK, &mask, 0) != 0 || handler_called != 1;
  }
  errors |= close((int)ends[writing ? 1 : 0]) != 0;
  int status = 0;
  errors |= waitpid(child, &status, 0) != child || status != 0;
  return errors;
}

static int test_pipe_interrupted(void) {
  int errors = pipe_signal_wait(0, PIPE_CAUGHT);
  errors |= pipe_signal_wait(1, PIPE_CAUGHT);
  return errors;
}

static int test_pipe_restarted(void) {
  int errors = pipe_signal_wait(0, PIPE_RESTART);
  errors |= pipe_signal_wait(1, PIPE_RESTART);
  return errors;
}

static int test_pipe_noninterrupting_signals(void) {
  int errors = pipe_signal_wait(0, PIPE_IGNORED);
  errors |= pipe_signal_wait(1, PIPE_IGNORED);
  errors |= pipe_signal_wait(0, PIPE_BLOCKED);
  errors |= pipe_signal_wait(1, PIPE_BLOCKED);
  return errors;
}

static int test_pipe_partial_interrupt(void) {
  long ends[2];
  if (pipe(ends) != 0) {
    return 1;
  }
  // Two 4096-byte pipe capacities force a partial write: one capacity commits
  // before the signal interrupts the blocked suffix, so return 4096, not EINTR.
  unsigned char data[8192];
  for (unsigned i = 0; i < sizeof(data); ++i) {
    data[i] = (unsigned char)i;
  }
  struct sigaction_t action = {(unsigned long)quiet_handler, 0, SA_RESTART};
  handler_called = 0;
  int errors = moss_sigaction(SIGUSR1, &action, 0) != 0;
  long parent = getpid();
  long child = fork();
  if (child == 0) {
    close((int)ends[1]);
    // A 10 ms guessed scheduling window lets the parent fill the pipe; this
    // fallback has no waiter handshake or calibrated readiness guarantee.
    unsigned long delay = 10000000;
    int failed = nanosleep_ns(&delay) != 0 || kill(parent, SIGUSR1) != 0;
    failed |= nanosleep_ns(&delay) != 0;
    failed |= read((int)ends[0], data, sizeof(data)) != 4096;
    for (unsigned i = 0; i < 4096; ++i) {
      failed |= data[i] != (unsigned char)i;
    }
    // Drain a wrongly written tail as well, so a regression remains bounded.
    failed |= read((int)ends[0], data, sizeof(data)) != 0;
    close((int)ends[0]);
    _exit(failed);
  }
  if (child < 0) {
    close((int)ends[0]);
    close((int)ends[1]);
    return 1;
  }
  errors |= close((int)ends[0]) != 0;
  errors |= write((int)ends[1], data, sizeof(data)) != 4096 || handler_called != 1;
  errors |= close((int)ends[1]) != 0;
  int status = 0;
  errors |= waitpid(child, &status, 0) != child || status != 0;
  return errors;
}

static int test_signal_wakeup_affinity(void) {
  long ready[2];
  if (pipe(ready) != 0) {
    return 1;
  }
  struct sigaction_t action = {(unsigned long)quiet_handler, 0, 0};
  handler_called = 0;
  int errors = moss_sigaction(SIGUSR1, &action, 0) != 0;
  long child = fork();
  if (child == 0) {
    close((int)ready[0]);
    // Affinity syscall 20 and mask 2 target CPU 1; byte 37 is a nonzero
    // handshake sentinel, not a timing or affinity value.
    unsigned cpu_mask = 2;
    unsigned char byte = 37;
    if (syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0 || write((int)ready[1], &byte, 1) != 1) {
      _exit(98);
    }
    close((int)ready[1]);
    unsigned long delay = 1000000000; // One second in ns leaves a sleep for the signal to interrupt.
    nanosleep_ns(&delay);
    long cpu = current_cpu();
    if (handler_called != 1 || cpu != 1) {
      print("  FAIL: signal wake handler=");
      print_long(handler_called);
      print(" cpu=");
      print_long(cpu);
      print("\n");
      _exit(97);
    }
    _exit(0);
  }
  close((int)ready[1]);
  unsigned char byte = 0;
  errors |= child < 0 || read((int)ready[0], &byte, 1) != 1 || byte != 37;
  close((int)ready[0]);
  // 10 ms in ns is a guessed window after the pipe handshake; the handshake
  // proves the child set affinity, but does not prove it has entered nanosleep.
  unsigned long delay = 10000000;
  errors |= nanosleep_ns(&delay) != 0;
  int status = 0;
  if (child > 0) {
    errors |= kill(child, SIGUSR1) != 0 || waitpid(child, &status, 0) != child || status != 0;
  }
  if (errors) {
    print("  FAIL: signal wake child status=");
    print_long(status);
    print("\n");
  }
  return errors;
}

static void console_restart_handler(int signo) {
  handler_called = signo == SIGUSR1;
  // The host injects the byte only after this marker, into the resumed read.
  print("MOSS_CONSOLE_RESTART_READY\n");
}

static int console_signal_wait(int partial, int restart) {
  const long fd = open("/dev/console", 0);
  if (fd < 0) {
    return 1;
  }
  if (partial) {
    char warmup;
    // The host sends two bytes together. Consume one before forking so input
    // is ready before the later partial read and signal handoff begin.
    if (read((int)fd, &warmup, 1) != 1 || warmup != 'k') {
      close((int)fd);
      return 1;
    }
  }
  struct sigaction_t action = {(unsigned long)(restart ? console_restart_handler : quiet_handler), 0,
                               restart ? SA_RESTART : 0};
  handler_called = 0;
  unsigned cpu_mask = 1;
  int errors = moss_sigaction(SIGUSR1, &action, 0) != 0;
  errors |= syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0;
  // RV64 polls in supervisor mode, so migrate the reader before occupying a CPU.
  unsigned long migration_delay = 1000000; // One millisecond in nanoseconds.
  errors |= nanosleep_ns(&migration_delay) != 0 || current_cpu() != 0;
  long ready_pipe[2];
  if (pipe(ready_pipe) != 0) {
    close((int)fd);
    return 1;
  }
  const long parent = getpid();
  const long child = fork();
  if (child == 0) {
    close((int)ready_pipe[0]);
    cpu_mask = 2;
    if (syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0) {
      _exit(1);
    }
    // Affinity moves the continuation through sleep/wakeup; readiness is observed
    // separately below, so this delay is not a guessed parent-read window.
    unsigned long delay = 1000000; // One millisecond in nanoseconds.
    int failed = nanosleep_ns(&delay) != 0;
    const unsigned char ready_byte = 37; // Nonzero handshake sentinel.
    failed |= write((int)ready_pipe[1], &ready_byte, 1) != 1;
    close((int)ready_pipe[1]);
    // Query the existing validation observation of the real read continuation,
    // so the signal originates on CPU 1 after CPU 0 has entered console I/O.
    long ready;
    while ((ready = syscall3(511, 39, parent, fd)) == 0) {
      sched_yield();
    }
    failed |= ready != 1 || current_cpu() != 1;
    failed |= kill(parent, SIGUSR1) != 0;
    _exit(failed);
  }
  if (child < 0) {
    close((int)ready_pipe[0]);
    close((int)ready_pipe[1]);
    close((int)fd);
    return 1;
  }
  close((int)ready_pipe[1]);
  unsigned char ready_byte = 0;
  // Fork initially queues the child on this CPU. Let it run and migrate
  // before RV64 enters a supervisor polling read that cannot schedule it.
  errors |= read((int)ready_pipe[0], &ready_byte, 1) != 1 || ready_byte != 37;
  close((int)ready_pipe[0]);
  char bytes[2];
  const long result = read((int)fd, bytes, partial ? 2 : 1);
  int status = 0;
  // Moss read returns Linux EINTR (4) when no bytes were copied.
  errors |= result != (partial || restart ? 1 : -4) || handler_called != 1;
  errors |= partial && bytes[0] != 'k';
  errors |= restart && bytes[0] != 'r';
  // Console echo has no newline; keep the next validation event on its own line.
  if (partial || restart) {
    print("\n");
  }
  errors |= waitpid(child, &status, 0) != child || status != 0;
  errors |= close((int)fd) != 0;
  return errors;
}

static int test_console_interrupted(void) { return console_signal_wait(0, 0); }
static int test_console_partial_interrupt(void) { return console_signal_wait(1, 0); }
static int test_console_restarted(void) { return console_signal_wait(0, 1); }

static int test_console_multi_reader(void) {
  long results[2];
  if (pipe(results) != 0) {
    return 1;
  }
  long children[2] = {-1, -1};
  for (unsigned actor = 0; actor < 2; ++actor) {
    children[actor] = fork();
    if (children[actor] == 0) {
      close((int)results[0]);
      unsigned cpu_mask = 1U << (actor + 1);
      if (syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0 || current_cpu() != actor + 1) {
        _exit(98);
      }
      long fd = open("/dev/console", 0);
      char byte = 0;
      long count = fd >= 0 ? read((int)fd, &byte, 1) : -1;
      int failed = count != 1 || (byte != 'a' && byte != 'b');
      if (!failed) {
        failed = write((int)results[1], &byte, 1) != 1;
      }
      if (fd >= 0) {
        close((int)fd);
      }
      close((int)results[1]);
      _exit(failed ? 98 : 42);
    }
    if (children[actor] < 0) {
      break;
    }
  }
  close((int)results[1]);
  int errors = children[0] < 0 || children[1] < 0;
  if (!errors) {
    long ready;
    while ((ready = control(57, children[0], children[1])) == 0) {
      sched_yield(); // The host case deadline bounds a missing reader.
    }
    errors = ready != 1;
  }
  if (errors) {
    for (unsigned i = 0; i < 2; ++i) {
      if (children[i] > 0) {
        kill(children[i], SIGKILL);
      }
    }
  } else {
    // The host injects actual serial bytes only after both read continuations
    // are observed; two results prove each blocked reader made progress.
    print("MOSS_CONSOLE_MULTI_READY\n");
    char got[2] = {0, 0};
    errors |= read((int)results[0], &got[0], 1) != 1;
    errors |= read((int)results[0], &got[1], 1) != 1;
    print("\n"); // Keep validation protocol lines clear of console echo.
    errors |= !((got[0] == 'a' && got[1] == 'b') || (got[0] == 'b' && got[1] == 'a'));
  }
  close((int)results[0]);
  for (unsigned i = 0; i < 2; ++i) {
    int status = 0;
    if (children[i] > 0) {
      errors |= waitpid(children[i], &status, 0) != children[i] || status != (42 << 8);
    }
  }
  return errors;
}

#if defined(__aarch64__) || defined(__x86_64__)
static int test_console_irq_registration(void) {
  long result[2];
  if (pipe(result) != 0) {
    return 1;
  }
  if (control(58, 0, 0) != 1) {
    close((int)result[0]);
    close((int)result[1]);
    return 1;
  }
  long child = fork();
  if (child == 0) {
    close((int)result[0]);
    unsigned cpu_mask = 1U << 1;
    if (syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0 || current_cpu() != 1) {
      _exit(98);
    }
    long fd = open("/dev/console", 0);
    char byte = 0;
    long count = fd >= 0 ? read((int)fd, &byte, 1) : -1;
    int failed = count != 1 || byte != 'r' || write((int)result[1], &byte, 1) != 1;
    if (fd >= 0) {
      close((int)fd);
    }
    close((int)result[1]);
    _exit(failed ? 98 : 42);
  }
  close((int)result[1]);
  int errors = child < 0;
  if (!errors) {
    long ready;
    while ((ready = control(58, child, 0)) == 0) {
      sched_yield(); // The host case deadline bounds a missing reader.
    }
    errors = ready != 1;
  }
  if (!errors) {
    print("MOSS_CONSOLE_IRQ_READY\n");
    char byte = 0;
    errors = read((int)result[0], &byte, 1) != 1 || byte != 'r';
    print("\n"); // Keep validation protocol lines clear of console echo.
  }
  close((int)result[0]);
  errors |= control(58, -1, 0) != 1;
  if (errors && child > 0) {
    kill(child, SIGKILL);
  }
  if (child > 0) {
    int status = 0;
    errors |= waitpid(child, &status, 0) != child || status != (42 << 8);
  }
  return errors;
}
#endif

static int test_pid_lifecycle(void) {
  // 300 children cross both the 255-user-ASID lease limit and the former
  // 256-slot signal table boundary while keeping concurrent population low.
  enum { ASID_REUSE_CYCLES = 300 };
  // Eight redispatches are a bounded repeat, not a claimed minimum: they keep
  // the 300-cycle boundary test short while checking more than one TLB refill.
  enum { ASID_REDISPATCHES = 8 };
  asid_pattern = 0;
  for (unsigned cycle = 0; cycle < ASID_REUSE_CYCLES; ++cycle) {
    long child = fork();
    if (child == 0) {
      struct sigaction_t sa = {(unsigned long)quiet_handler, 0, 0};
      unsigned long mask = 0;
      const unsigned expected_cpu = cycle & 1U;
      const unsigned cpu_mask = 1U << expected_cpu; // CPU0/CPU1 affinity bits alternate on each reused lease.
      const unsigned long expected_pattern = (unsigned long)cycle + 1; // Nonzero and distinct in all 300 cycles.
      // A 1 ms delay exercises sleep/wakeup placement in ordinary runs. The
      // direct check below remains the oracle if a loaded host lets the
      // deadline expire before the guest can block.
      unsigned long migration_delay = 1000000;
      handler_called = 0;
      asid_pattern = expected_pattern;
      int errors = syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0;
      // A successful affinity change must return on an allowed CPU. The sleep
      // remains a second dispatch check, not the mechanism that causes migration.
      const long affinity_cpu = current_cpu();
      if (affinity_cpu != (long)expected_cpu) {
        print("  FAIL: affinity returned on CPU ");
        print_long(affinity_cpu);
        print(" expected ");
        print_ulong(expected_cpu);
        print("\n");
        // Bit 2 is the established pid_lifecycle migration diagnostic.
        errors |= 1 << 2;
      }
      errors |= (nanosleep_ns(&migration_delay) != 0) << 1;
      const long observed_cpu = current_cpu();
      if (observed_cpu != (long)expected_cpu) {
        print("  FAIL: wake returned on CPU ");
        print_long(observed_cpu);
        print(" expected ");
        print_ulong(expected_cpu);
        print("\n");
        // Bit 2 is the established pid_lifecycle diagnostic for migration.
        errors |= 1 << 2;
      }
      for (unsigned redispatch = 0; redispatch < ASID_REDISPATCHES; ++redispatch) {
        errors |= (sched_yield() != 0 || asid_pattern != expected_pattern) << 3;
      }
      errors |= (sigprocmask(SIG_SETMASK, &mask, 0) != 0) << 4;
      errors |= (moss_sigaction(SIGUSR1, &sa, 0) != 0) << 5;
      errors |= (kill(getpid(), SIGUSR1) != 0) << 6;
      errors |= (!handler_called) << 7;
      _exit(errors);
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child || status != 0 || asid_pattern != 0) {
      print("  FAIL: signal lifecycle cycle ");
      print_ulong(cycle);
      print(" child=");
      print_long(child);
      print(" status=");
      print_long(status);
      print("\n");
      return 1;
    }
  }
  return 0;
}

// Each signal case re-executes this validation image so dispositions, pending
// signals and writable fixture state cannot leak between cases. The inheritance
// case also re-executes it to verify the kernel's exec-time signal reset rules.
int signal_case(const char *name) {
#if defined(__aarch64__) || defined(__x86_64__)
  if (streq(name, "irq_before_registration")) {
    return test_console_irq_registration();
  }
#endif
  const struct {
    const char *name;
    int (*run)(void);
  } cases[] = {{"basic_handler", test_basic_handler},
               {"nested_signals", test_nested_signals},
               {"sigchld", test_sigchld},
               {"wait_registration", test_wait_registration},
               {"wait_interrupted", test_wait_interrupted},
               {"wait_restarted", test_wait_restarted},
               {"cpu_bound_irq", test_cpu_bound_irq},
               {"stop_continue", test_stop_continue},
               {"wait_job_status", test_wait_job_status},
               {"no_cldstop", test_no_cldstop},
               {"wait_process_group", test_wait_process_group},
               {"wait_group_change", test_wait_group_change},
               {"no_cldwait", test_no_cldwait},
               {"sigaction_race", test_sigaction_race},
               {"sigaction_discard", test_sigaction_discard},
               {"signal_exit_status", test_signal_exit_status},
               {"sigprocmask", test_sigprocmask},
               {"sigaltstack", test_sigaltstack},
               {"sig_ign", test_sig_ign},
               {"invalid_arguments", test_invalid_arguments},
               {"frame_validation", test_frame_validation},
               {"altstack_overflow", test_altstack_overflow},
               {"altstack_boundaries", test_altstack_boundaries},
               {"inheritance", test_inheritance},
               {"pid_lifecycle", test_pid_lifecycle},
               {"pipe_sigpipe", test_pipe_sigpipe},
               {"pipe_interrupted", test_pipe_interrupted},
               {"pipe_restarted", test_pipe_restarted},
               {"pipe_noninterrupting_signals", test_pipe_noninterrupting_signals},
               {"pipe_partial_interrupt", test_pipe_partial_interrupt},
               {"signal_wakeup_affinity", test_signal_wakeup_affinity},
               {"console_interrupted", test_console_interrupted},
               {"console_partial_interrupt", test_console_partial_interrupt},
               {"console_restarted", test_console_restarted},
               {"console_multi_reader", test_console_multi_reader}};
  if (streq(name, "exec_reset")) {
    return test_exec_reset();
  }
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (streq(name, cases[i].name)) {
      return cases[i].run();
    }
  }
  return 99; // Unknown fixture selector must fail the existing exit-status protocol.
}
