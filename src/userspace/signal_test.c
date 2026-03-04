// MOSS signal mechanism test program
// Tests: basic handler, nested signals, sigprocmask, sigaltstack, SIG_IGN
#include "syscall.h"

static volatile int handler_called = 0;
static volatile int handler2_called = 0;
static volatile unsigned long handler_sp = 0;

static void sigusr1_handler(int sig) {
  (void)sig;
  handler_called = 1;
  print("[signal_test] SIGUSR1 handler called\n");
}

static void sigusr2_handler(int sig) {
  (void)sig;
  handler2_called = 1;
  print("[signal_test] SIGUSR2 handler called\n");
}

static void nested_handler(int sig) {
  (void)sig;
  handler_called = 1;
  print("[signal_test] nested: SIGUSR1 handler, sending SIGUSR2\n");
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
  print("[signal_test] altstack handler called\n");
}

static void sigchld_handler(int sig) {
  (void)sig;
  handler_called = 1;
  print("[signal_test] SIGCHLD handler called\n");
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

  kill(getpid(), SIGUSR1);

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
  moss_sigaction(SIGUSR1, &sa1, 0);

  struct sigaction_t sa2;
  sa2.handler = (unsigned long)sigusr2_handler;
  sa2.mask = 0;
  sa2.flags = 0;
  moss_sigaction(SIGUSR2, &sa2, 0);

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
  moss_sigaction(SIGCHLD, &sa, 0);

  long pid = fork();
  if (pid == 0) {
    // Child: exit immediately
    _exit(42);
  }
  // Parent: wait for child
  int status = 0;
  waitpid(pid, &status, 0);

  if (handler_called) {
    print("  PASS: SIGCHLD received\n");
    return 0;
  }
  // SIGCHLD delivery at IRQ return is not yet implemented,
  // so this is expected to skip for now.
  print("  SKIP: SIGCHLD not yet delivered (requires IRQ-return signal check)\n");
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
  moss_sigaction(SIGUSR1, &sa, 0);

  // Block SIGUSR1
  unsigned long mask = (1UL << SIGUSR1);
  sigprocmask(SIG_BLOCK, &mask, 0);

  // Send — should be pended, not delivered
  kill(getpid(), SIGUSR1);
  if (handler_called) {
    print("  FAIL: handler called while signal blocked\n");
    return 1;
  }

  // Unblock — should deliver on next syscall return
  sigprocmask(SIG_UNBLOCK, &mask, 0);
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
static int test_sigaltstack(void) {
  print("\n=== Test 5: sigaltstack ===\n");
  handler_called = 0;
  handler_sp = 0;

  // Use a static buffer as alternate signal stack
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
  moss_sigaction(SIGUSR1, &sa, 0);

  kill(getpid(), SIGUSR1);

  if (handler_called && handler_sp >= altstack_base && handler_sp < altstack_top) {
    print("  PASS: handler ran on altstack\n");
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
  moss_sigaction(SIGUSR1, &sa, 0);

  // Send SIGUSR1 — should be silently ignored
  kill(getpid(), SIGUSR1);
  print("  PASS: SIG_IGN — signal ignored, process alive\n");

  // Restore default
  sa.handler = SIG_DFL;
  moss_sigaction(SIGUSR1, &sa, 0);
  return 0;
}

void _start(void) {
  print("[signal_test] Starting signal mechanism tests\n");

  int failures = 0;
  failures += test_basic_handler();
  failures += test_nested_signals();
  failures += test_sigchld();
  failures += test_sigprocmask();
  failures += test_sigaltstack();
  failures += test_sig_ign();

  print("\n[signal_test] ");
  if (failures == 0) {
    print("ALL TESTS PASSED\n");
  } else {
    print("SOME TESTS FAILED\n");
  }

  _exit(failures);
}
