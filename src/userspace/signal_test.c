// MOSS signal mechanism test program
// Tests: basic handler, nested signals, sigprocmask, sigaltstack, SIG_IGN
// Nonzero child exit codes identify fixture failures; 42 is a successful child
// sentinel. Wait status uses the exit byte at bits 8..15 (shift 8, mask 255).
#include "syscall.h"

static volatile int handler_called = 0;
static volatile int handler2_called = 0;
static volatile unsigned long handler_sp = 0;

static void sigusr1_handler(int sig) {
  handler_called = sig == SIGUSR1;
  print("[signal_test] SIGUSR1 handler called\n");
}

static void sigusr2_handler(int sig) {
  handler2_called = sig == SIGUSR2;
  print("[signal_test] SIGUSR2 handler called\n");
}

static void nested_handler(int sig) {
  handler_called = sig == SIGUSR1;
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
  handler_called = sig == SIGCHLD;
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

  if (kill(getpid(), SIGUSR1) != 0)
    return 1;

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
  if (moss_sigaction(SIGUSR1, &sa1, 0) != 0)
    return 1;

  struct sigaction_t sa2;
  sa2.handler = (unsigned long)sigusr2_handler;
  sa2.mask = 0;
  sa2.flags = 0;
  if (moss_sigaction(SIGUSR2, &sa2, 0) != 0)
    return 1;

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
  if (moss_sigaction(SIGCHLD, &sa, 0) != 0)
    return 1;

  long pid = fork();
  if (pid == 0) {
    // Child: exit immediately
    _exit(42);
  }
  // Parent: wait for child
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid || ((status >> 8) & 255) != 42)
    return 1;

  if (handler_called) {
    print("  PASS: SIGCHLD received\n");
    return 0;
  }
  print("  FAIL: SIGCHLD not delivered\n");
  return 1;
}

// Test 4: sigprocmask — block and unblock
static int test_sigprocmask(void) {
  print("\n=== Test 4: sigprocmask block/unblock ===\n");
  handler_called = 0;

  struct sigaction_t sa;
  sa.handler = (unsigned long)sigusr1_handler;
  sa.mask = 0;
  sa.flags = 0;
  if (moss_sigaction(SIGUSR1, &sa, 0) != 0)
    return 1;

  // Block SIGUSR1
  unsigned long mask = (1UL << SIGUSR1);
  if (sigprocmask(SIG_BLOCK, &mask, 0) != 0)
    return 1;

  // Send — should be pended, not delivered
  kill(getpid(), SIGUSR1);
  if (handler_called) {
    print("  FAIL: handler called while signal blocked\n");
    return 1;
  }

  // Unblock — should deliver on next syscall return
  if (sigprocmask(SIG_UNBLOCK, &mask, 0) != 0)
    return 1;
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
  if (moss_sigaction(SIGUSR1, &sa, 0) != 0)
    return 1;

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
  if (moss_sigaction(SIGUSR1, &sa, 0) != 0)
    return 1;

  // Send SIGUSR1 — should be silently ignored
  if (kill(getpid(), SIGUSR1) != 0)
    return 1;
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
  sa.flags = 8; // Unsupported native action bit; only SA_ONSTACK is accepted.
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

static void frame_handler(int signo) {
  unsigned long red_zone = 0;
#if defined(__x86_64__)
  red_zone = 128; // x86-64 SysV reserves 128 bytes below the interrupted SP.
#endif
  struct signal_frame_t *sf = (void *)(frame_stack + sizeof(frame_stack) - red_zone - sizeof(struct signal_frame_t));
  frame_errors |= signo != SIGUSR1;
  // Modes 0/1/2 corrupt magic/PC/SP; mode 3 checks privileged flag sanitizing.
  // x86 mode 4 additionally validates the architecture FP control payload.
  if (frame_mode < 3) {
    unsigned long *field = frame_mode == 0 ? &sf->magic : frame_mode == 1 ? &sf->pc : &sf->sp;
    unsigned long saved = *field;
    *field = frame_mode == 0 ? 0 : 0xfffffffffffff000UL;
    frame_errors |= try_sigreturn((unsigned long)sf) != -14;
    *field = saved;
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
  struct sigaction_t sa = {(unsigned long)frame_handler, 0, SA_ONSTACK};
  if (sigaltstack(&ss, 0) != 0 || moss_sigaction(SIGUSR1, &sa, 0) != 0)
    return 1;
  // Four common frame cases plus the fifth, x86-only MXCSR case above.
  const int count =
#if defined(__x86_64__)
      5;
#else
      4;
#endif
  for (frame_mode = 0; frame_mode < count; ++frame_mode) {
    handler_called = 0;
    if (kill(getpid(), SIGUSR1) != 0 || !handler_called || frame_errors)
      return 1;
    unsigned long mask = ~0UL;
    if (sigprocmask(SIG_SETMASK, 0, &mask) != 0 || (mask & ((1UL << SIGKILL) | (1UL << SIGSTOP))))
      return 1;
    struct stack_t current;
    if (sigaltstack(0, &current) != 0 || (current.ss_flags & SS_ONSTACK))
      return 1;
  }
  return 0;
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
      sigprocmask(SIG_BLOCK, &mask, 0) != 0 || kill(getpid(), SIGUSR1) != 0)
    return 1;
  long child = fork();
  if (child == 0) {
    struct sigaction_t inherited;
    struct stack_t ss;
    unsigned long bits = 0;
    if (moss_sigaction(SIGUSR1, 0, &inherited) != 0 || inherited.handler != sa.handler ||
        sigprocmask(SIG_SETMASK, 0, &bits) != 0 || bits != mask || sigaltstack(0, &ss) != 0 ||
        ss.ss_sp != stack.ss_sp || ss.ss_size != stack.ss_size || ss.ss_flags != 0)
      _exit(71);
    // Pending signals are not inherited; masks/actions/altstack are.
    handler_called = 0;
    if (sigprocmask(SIG_UNBLOCK, &mask, 0) != 0 || handler_called || kill(getpid(), SIGUSR1) != 0 || !handler_called ||
        sigprocmask(SIG_BLOCK, &mask, 0) != 0)
      _exit(72);
    const char *args[] = {"signal_test", "exec_reset", 0};
    syscall3(SYS_EXECVE, (long)"/signal_test.elf", (long)args, 0);
    _exit(73);
  }
  int status = 0;
  if (child < 0 || waitpid(child, &status, 0) != child || status != 0)
    return 1;
  handler_called = 0;
  return sigprocmask(SIG_UNBLOCK, &mask, 0) != 0 || !handler_called;
}

static void quiet_handler(int signo) { handler_called = signo == SIGUSR1; }

static void sigpipe_handler(int signo) { handler_called += signo == SIGPIPE; }

static int test_pipe_sigpipe(void) {
  long ends[2];
  if (pipe(ends) != 0)
    return 1;
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
    if (moss_sigaction(SIGPIPE, &action, 0) != 0)
      _exit(98);
    write((int)ends[1], &byte, 1);
    _exit(99); // The default action must terminate at the syscall return.
  }
  int status = 0;
  // Default signal termination uses the shell convention 128 + SIGPIPE(13).
  errors |= child < 0 || waitpid(child, &status, 0) != child || ((status >> 8) & 255) != 141;
  errors |= close((int)ends[1]) != 0;
  return errors;
}

enum pipe_disposition { PIPE_CAUGHT, PIPE_IGNORED, PIPE_BLOCKED };

static int pipe_signal_wait(int writing, enum pipe_disposition disposition) {
  // Validation syscall 511, operation 39 reports a queued pipe waiter. A
  // return of 1 enables coordination; -38 (ENOSYS) keeps standalone fallback.
  // Keep this protocol synchronized with moss_validation_call.
  const long fixture = syscall3(511, 39, 0, 0);
  if (fixture != 1 && fixture != -38)
    return 1;
  const int coordinated = fixture == 1;
  long ends[2];
  if (pipe(ends) != 0)
    return 1;
  long acknowledgement[2];
  const int acknowledge = coordinated && disposition == PIPE_CAUGHT;
  if (acknowledge && pipe(acknowledgement) != 0) {
    close((int)ends[0]);
    close((int)ends[1]);
    return 1;
  }
  // One 4096-byte pipe capacity makes the next single-byte write block.
  // The byte-index pattern detects changed or misplaced bytes across the wait.
  unsigned char data[4096];
  for (unsigned i = 0; i < sizeof(data); ++i)
    data[i] = (unsigned char)i;
  struct sigaction_t action = {(unsigned long)quiet_handler, 0, 0};
  if (disposition == PIPE_IGNORED)
    action.handler = SIG_IGN;
  handler_called = 0;
  int errors = moss_sigaction(SIGUSR1, &action, 0) != 0;
  unsigned long mask = 1UL << SIGUSR1;
  if (disposition == PIPE_BLOCKED)
    errors |= sigprocmask(SIG_BLOCK, &mask, 0) != 0;
  // SYS_SCHED_SETAFFINITY=20 uses bit 0 for CPU 0 and bit 1 (value 2) for
  // CPU 1, placing the sender and waiter on different CPUs.
  unsigned cpu_mask = 1;
  errors |= syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0;
  if (writing)
    errors |= write((int)ends[1], data, sizeof(data)) != sizeof(data);
  long parent = getpid();
  long child = fork();
  if (child == 0) {
    close((int)ends[writing ? 1 : 0]);
    if (acknowledge)
      close((int)acknowledgement[1]);
    cpu_mask = 2;
    if (syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0)
      _exit(98);
    // Affinity takes effect on the existing sleep/wakeup path. This sleep is
    // not evidence of parent readiness; the validation hook checks that below.
    unsigned long delay = 10000000; // 10 ms in ns; the exact delay has no timing calibration.
    int failed = nanosleep_ns(&delay) != 0 || current_cpu() != 1;
    if (coordinated) {
      long ready;
      while ((ready = syscall3(511, 39, parent, ends[writing ? 1 : 0])) == 0)
        sched_yield(); // The unchanged host watchdog bounds a missing waiter.
      if (ready != 1)
        _exit(97);
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
      for (unsigned i = 0; i < sizeof(data); ++i)
        failed |= data[i] != (unsigned char)i;
      failed |= read((int)ends[0], data, 1) != (disposition == PIPE_CAUGHT ? 0 : 1);
      if (disposition != PIPE_CAUGHT)
        failed |= data[0] != 0 || read((int)ends[0], data, 1) != 0;
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
  if (acknowledge)
    errors |= close((int)acknowledgement[0]) != 0;
  errors |= close((int)ends[writing ? 0 : 1]) != 0;
  // Exercise preparation that outlasts the old sender's guessed 10 ms delay.
  unsigned long prepare_delay = 30000000; // 30 ms in ns, deliberately longer than the sender's 10 ms.
  if (coordinated)
    errors |= nanosleep_ns(&prepare_delay) != 0;
  long result = writing ? write((int)ends[1], data, 1) : read((int)ends[0], data, 1);
  errors |= result != (disposition == PIPE_CAUGHT ? -4 : 1) || handler_called != (disposition == PIPE_CAUGHT);
  errors |= current_cpu() != 0;
  if (acknowledge) {
    const unsigned char observed = 37;
    errors |= write((int)acknowledgement[1], &observed, 1) != 1;
    errors |= close((int)acknowledgement[1]) != 0;
  }
  if (!writing && result == -4)
    errors |= read((int)ends[0], data, 1) != 1 || data[0] != 37;
  else if (!writing)
    errors |= data[0] != 37;
  if (disposition == PIPE_BLOCKED)
    errors |= sigprocmask(SIG_UNBLOCK, &mask, 0) != 0 || handler_called != 1;
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

static int test_pipe_noninterrupting_signals(void) {
  int errors = pipe_signal_wait(0, PIPE_IGNORED);
  errors |= pipe_signal_wait(1, PIPE_IGNORED);
  errors |= pipe_signal_wait(0, PIPE_BLOCKED);
  errors |= pipe_signal_wait(1, PIPE_BLOCKED);
  return errors;
}

static int test_pipe_partial_interrupt(void) {
  long ends[2];
  if (pipe(ends) != 0)
    return 1;
  // Two 4096-byte pipe capacities force a partial write: one capacity commits
  // before the signal interrupts the blocked suffix, so return 4096, not EINTR.
  unsigned char data[8192];
  for (unsigned i = 0; i < sizeof(data); ++i)
    data[i] = (unsigned char)i;
  struct sigaction_t action = {(unsigned long)quiet_handler, 0, 0};
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
    for (unsigned i = 0; i < 4096; ++i)
      failed |= data[i] != (unsigned char)i;
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
  if (pipe(ready) != 0)
    return 1;
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
    if (syscall3(20, 0, sizeof(cpu_mask), (long)&cpu_mask) != 0 || write((int)ready[1], &byte, 1) != 1)
      _exit(98);
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
  if (child > 0)
    errors |= kill(child, SIGUSR1) != 0 || waitpid(child, &status, 0) != child || status != 0;
  if (errors) {
    print("  FAIL: signal wake child status=");
    print_long(status);
    print("\n");
  }
  return errors;
}

static int test_pid_lifecycle(void) {
  // 300 sequential children exceed the 256-slot process/signal bookkeeping
  // capacity while keeping concurrent population low, exercising slot reuse.
  for (unsigned cycle = 0; cycle < 300; ++cycle) {
    long child = fork();
    if (child == 0) {
      struct sigaction_t sa = {(unsigned long)quiet_handler, 0, 0};
      unsigned long mask = 0;
      handler_called = 0;
      int errors = sigprocmask(SIG_SETMASK, &mask, 0) != 0;
      errors |= (moss_sigaction(SIGUSR1, &sa, 0) != 0) << 1;
      errors |= (kill(getpid(), SIGUSR1) != 0) << 2;
      errors |= (!handler_called) << 3;
      _exit(errors);
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child || status != 0) {
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

void _start(long argc, const char **argv) {
  const struct {
    const char *name;
    int (*run)(void);
  } cases[] = {{"basic_handler", test_basic_handler},
               {"nested_signals", test_nested_signals},
               {"sigchld", test_sigchld},
               {"sigprocmask", test_sigprocmask},
               {"sigaltstack", test_sigaltstack},
               {"sig_ign", test_sig_ign},
               {"invalid_arguments", test_invalid_arguments},
               {"frame_validation", test_frame_validation},
               {"inheritance", test_inheritance},
               {"pid_lifecycle", test_pid_lifecycle},
               {"pipe_sigpipe", test_pipe_sigpipe},
               {"pipe_interrupted", test_pipe_interrupted},
               {"pipe_noninterrupting_signals", test_pipe_noninterrupting_signals},
               {"pipe_partial_interrupt", test_pipe_partial_interrupt},
               {"signal_wakeup_affinity", test_signal_wakeup_affinity}};
  // The automated suite executes each case in a fresh process. The standalone
  // program still runs all cases when no selector is supplied.
  if (argc == 2 && argv && argv[1]) {
    if (streq(argv[1], "exec_reset"))
      _exit(test_exec_reset());
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
      if (streq(argv[1], cases[i].name))
        _exit(cases[i].run());
    _exit(99);
  }
  print("[signal_test] Starting signal mechanism tests\n");

  int failures = 0;
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    failures += cases[i].run();

  print("\n[signal_test] ");
  if (failures == 0) {
    print("ALL TESTS PASSED\n");
  } else {
    print("SOME TESTS FAILED\n");
  }

  _exit(failures);
}
