#include "validation/internal.h"

// Validation-image protocol, implemented by moss_validation_call in test/validation.cpp:
// syscall 511 is outside the production table; op 0 selects a workload, 1 begins
// a case, 2 reports success plus an error mask, 3 finishes, and 4/5/6 exchange
// benchmark count/ticks/overhead. Other opcodes coordinate the named fixtures.
// Keep both ends synchronized; production images return ENOSYS for this slot.
//
// Raw ABI conventions used below: mmap prot 3=R|W, flags 0x22=PRIVATE|ANONYMOUS,
// fd -1 and offset 0; 4096-byte pages and 8192-byte pairs test page boundaries.
// Native errors are -errno (2=ENOENT, 4=EINTR, 7=E2BIG, 8=ENOEXEC, 10=ECHILD, 11=EAGAIN, 12=ENOMEM,
// 14=EFAULT, 22=EINVAL, 36=ENAMETOOLONG, 38=ENOSYS). Error masks assign
// each check a bit. Nonzero byte/canary patterns detect untouched or aliased data;
// 37/39 are child/exec success markers, while other exit codes identify failures.
static unsigned long application_cycles;
static int application_workload;

static const char application_script[] =
    "[ \"$HOSTNAME\" = moss ] && set -o pipefail && /busybox.elf mkdir /app-input /app-output && "
    "cd /app-input && [ \"$(pwd)\" = /app-input ] && printf 'moss\\nskip\\nmoss again\\n' > input && "
    "/busybox.elf cp ./input ../app-output/copy && cd ../app-output && /busybox.elf mv copy result && "
    "count=$(/busybox.elf cat ./result | /busybox.elf grep moss | /busybox.elf wc -l) && "
    "[ \"$count\" -eq 2 ] && [ \"$(/busybox.elf ls -1 .)\" = result ] && cd .. && "
    "/busybox.elf rm -rf /app-input /app-output && [ ! -e /app-input ] && [ ! -e /app-output ] && "
    "printf 'application ok\\n' && exit 37; exit 98";

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
  long result = frame_register_probe(SYS_KILL, getpid());
  errors |= (unsigned long)(result != 0 || handled_signo != SIGUSR1 || handler_pid != getpid()) << 1;
  errors |= (unsigned long)(frame_register_probe(SYS_GETPID, 0) != getpid()) << 2;
  action.handler = SIG_DFL;
  errors |= (unsigned long)(moss_sigaction(SIGUSR1, &action, 0) != 0) << 3;
  return errors;
}

// A whole aligned base page isolates the read-only permission under test;
// 0x5a distinguishes preserved data from a demand-zero or clobbered page.
const unsigned char vm_rodata[4096] __attribute__((aligned(4096))) = {0x5a};

int wait_exit(long child, int code) {
  int status = 0;
  return child > 1 && syscall3(SYS_WAITPID, child, (long)&status, 0) == child && ((status >> 8) & 255) == code;
}

int wait_signal(long child, int signo) {
  int status = 0;
  return child > 1 && waitpid(child, &status, 0) == child && status == signo;
}

static unsigned long pipe_waits_for_writer(void) {
  long ends[2] = {-1, -1};
  if (pipe(ends) != 0) {
    return 1;
  }
  long child = fork();
  if (child == 0) {
    if (close((int)ends[0]) != 0) {
      _exit(91);
    }
    // 10,000,000 ns (10 ms) gives the reader a scheduling opportunity while
    // the endpoint stays open; this guessed delay does not prove it is waiting.
    unsigned long delay = 10000000;
    const unsigned char sent[] = {17, 44, 23, 99};
    if (nanosleep_ns(&delay) != 0 || write((int)ends[1], sent, sizeof(sent)) != sizeof(sent)) {
      _exit(92);
    }
    _exit(close((int)ends[1]) == 0 ? 37 : 93);
  }
  unsigned long errors = child < 0;
  errors |= (unsigned long)(close((int)ends[1]) != 0) << 1;
  if (child > 0) {
    unsigned char received[4] = {0};
    // Do not wait/reap first: the read itself must wait for the live writer.
    errors |= (unsigned long)(read((int)ends[0], received, sizeof(received)) != sizeof(received)) << 2;
    errors |= (unsigned long)(received[0] != 17 || received[1] != 44 || received[2] != 23 || received[3] != 99) << 3;
    errors |= (unsigned long)!wait_exit(child, 37) << 4;
    errors |= (unsigned long)(read((int)ends[0], received, 1) != 0) << 5;
  }
  errors |= (unsigned long)(close((int)ends[0]) != 0) << 6;
  return errors;
}

static unsigned long pipe_cross_cpu_roundtrip(unsigned rounds) {
  long request[2], reply[2];
  if (pipe(request) != 0) {
    return 1;
  }
  if (pipe(reply) != 0) {
    close((int)request[0]);
    close((int)request[1]);
    return 2;
  }
  long child = fork();
  if (child == 0) {
    // Native syscall 20 sets affinity; mask 2 selects CPU1 (bit 1), while
    // the parent runs on CPU0. A 1 ms sleep below permits wakeup-based migration.
    unsigned mask = 2;
    if (syscall3(20, 0, sizeof(mask), (long)&mask) != 0 || close((int)request[1]) != 0 || close((int)reply[0]) != 0) {
      _exit(91);
    }
    unsigned long delay = 1000000;
    if (nanosleep_ns(&delay) != 0 || current_cpu() != 1) {
      _exit(93);
    }
    for (unsigned i = 0; i < rounds; ++i) {
      unsigned char bytes[4];
      if (read((int)request[0], bytes, sizeof(bytes)) != sizeof(bytes) || bytes[0] != 17 || bytes[1] != i ||
          bytes[2] != 23 || bytes[3] != 99 || write((int)reply[1], bytes, sizeof(bytes)) != sizeof(bytes)) {
        _exit(92);
      }
    }
    _exit(37);
  }
  unsigned long errors = child < 0;
  errors |= (unsigned long)(close((int)request[0]) != 0 || close((int)reply[1]) != 0) << 1;
  if (child > 0) {
    for (unsigned i = 0; i < rounds; ++i) {
      const unsigned char sent[4] = {17, (unsigned char)i, 23, 99};
      unsigned char received[4] = {0};
      if (write((int)request[1], sent, sizeof(sent)) != sizeof(sent) ||
          read((int)reply[0], received, sizeof(received)) != sizeof(received) || received[0] != 17 ||
          received[1] != i || received[2] != 23 || received[3] != 99) {
        errors |= 4;
        break;
      }
    }
  }
  errors |= (unsigned long)(close((int)request[1]) != 0) << 3;
  if (child > 0) {
    errors |= (unsigned long)!wait_exit(child, 37) << 4;
    unsigned char byte;
    errors |= (unsigned long)(read((int)reply[0], &byte, 1) != 0) << 5;
  }
  errors |= (unsigned long)(close((int)reply[0]) != 0) << 6;
  return errors;
}

static unsigned long pipe_waits_for_reader(void) {
  long ends[2];
  if (pipe(ends) != 0) {
    return 1;
  }
  // Fill the entire 4096-byte pipe ring to force the extra record to wait.
  // Modulo 251 stays within a byte and shifts the pattern across a 4096-byte
  // boundary, making page-aligned duplication visible (4096 is not a multiple).
  unsigned char bytes[4096];
  for (unsigned i = 0; i < sizeof(bytes); ++i) {
    bytes[i] = (unsigned char)(i % 251);
  }
  if (write((int)ends[1], bytes, sizeof(bytes)) != sizeof(bytes)) {
    close((int)ends[0]);
    close((int)ends[1]);
    return 2;
  }
  long child = fork();
  if (child == 0) {
    // This 10 ms scheduling delay is a fixture guess, not a readiness handshake.
    unsigned long delay = 10000000;
    if (close((int)ends[1]) != 0 || nanosleep_ns(&delay) != 0 ||
        read((int)ends[0], bytes, sizeof(bytes)) != sizeof(bytes)) {
      _exit(91);
    }
    for (unsigned i = 0; i < sizeof(bytes); ++i) {
      if (bytes[i] != i % 251) {
        _exit(92);
      }
    }
    if (read((int)ends[0], bytes, 4) != 4 || bytes[0] != 9 || bytes[1] != 8 || bytes[2] != 7 || bytes[3] != 6) {
      _exit(93);
    }
    _exit(37);
  }
  unsigned long errors = child < 0;
  errors |= (unsigned long)(close((int)ends[0]) != 0) << 2;
  if (child > 0) {
    const unsigned char extra[] = {9, 8, 7, 6};
    errors |= (unsigned long)(write((int)ends[1], extra, sizeof(extra)) != sizeof(extra)) << 3;
  }
  errors |= (unsigned long)(close((int)ends[1]) != 0) << 4;
  if (child > 0) {
    errors |= (unsigned long)!wait_exit(child, 37) << 5;
  }
  return errors;
}

enum lifecycle_exit_order {
  LIFECYCLE_WAIT_BEFORE_EOF,
  LIFECYCLE_EOF_BEFORE_WAIT,
};

static unsigned long lifecycle_cycle(enum lifecycle_exit_order exit_order) {
  long fd = syscall3(SYS_OPEN, (long)"/fixture.bin", 0, 0);
  if (fd < 0) {
    return 1;
  }
  long area = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
  long ends[2] = {-1, -1};
  if (area <= 0 || pipe(ends) != 0) {
    if (area > 0) {
      syscall2(SYS_MUNMAP, area, 4096);
    }
    close((int)fd);
    return 2;
  }
  volatile unsigned char *private_page = (volatile unsigned char *)area;
  private_page[0] = 17;
  private_page[4095] = 23;
  long child = fork();
  if (child == 0) {
    // One cycle owns a real COW mapping, signal frame, timer, descriptors and
    // child process, then releases them through exec/exit/wait and munmap.
    if (private_page[0] != 17 || private_page[4095] != 23) {
      _exit(90);
    }
    private_page[0] = 44;
    struct sigaction_t action = {(unsigned long)frame_signal_handler, 0, 0};
    handled_signo = 0;
    handler_pid = 0;
    if (moss_sigaction(SIGUSR1, &action, 0) != 0 || kill(getpid(), SIGUSR1) != 0 || handled_signo != SIGUSR1 ||
        handler_pid != getpid()) {
      _exit(91);
    }
    unsigned long before = 0, after = 0, duration = 1000000;
    long before_result = clock_gettime_ns(&before);
    long sleep_result = nanosleep_ns(&duration);
    long after_result = clock_gettime_ns(&after);
    if (before_result || sleep_result || after_result || after < before || after - before < duration) {
      _exit(92);
    }
    const unsigned char payload[4] = {17, 44, 23, 99};
    if (close((int)ends[0]) != 0 || write((int)ends[1], payload, 4) != 4) {
      _exit(93);
    }
    unsigned char byte = 255;
    if (syscall3(SYS_READ, fd, (long)&byte, 1) != 1 || byte != 0) {
      _exit(97);
    }
    // Keep the inherited descriptor open across exec and exit. The kernel must
    // release its ownership even when the application never calls close.
    const char *args[] = {"exec", 0};
    syscall3(SYS_EXECVE, (long)"/validation_child.elf", (long)args, 0);
    _exit(99);
  }
  unsigned long errors = (unsigned long)(close((int)ends[1]) != 0) << 2;
  int status = 0;
  long waited = -1;
  if (exit_order == LIFECYCLE_WAIT_BEFORE_EOF) {
    waited = child > 0 ? syscall3(SYS_WAITPID, child, (long)&status, 0) : -1;
  }
  unsigned char payload[4] = {0};
  errors |= (unsigned long)(read((int)ends[0], payload, 4) != 4 || payload[0] != 17 || payload[1] != 44 ||
                            payload[2] != 23 || payload[3] != 99)
            << 4;
  errors |= (unsigned long)(read((int)ends[0], payload, 1) != 0) << 5;
  if (exit_order == LIFECYCLE_EOF_BEFORE_WAIT) {
    // Observe final EOF before waitpid can reap the child. The exit path must
    // release its inherited writer before publishing the process as a Zombie.
    waited = child > 0 ? syscall3(SYS_WAITPID, child, (long)&status, 0) : -1;
  }
  if (child <= 0 || waited != child || status != (37 << 8)) {
    errors |= 8 | ((unsigned long)(unsigned short)status << 16);
  }
  errors |= (unsigned long)(close((int)ends[0]) != 0) << 6;
  errors |= (unsigned long)(private_page[0] != 17 || private_page[4095] != 23) << 7;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 4096) != 0) << 8;
  errors |= (unsigned long)(syscall3(SYS_LSEEK, fd, 0, 1) != 1) << 9; // fork shares the file position.
  errors |= (unsigned long)(syscall1(SYS_CLOSE, fd) != 0) << 10;
  // Every cycle execs and reaps a fresh ash, covering all nine selected applets.
  // Count only after exact stdout, exit status and script cleanup succeeded.
  if (!errors && application_workload) {
    errors = busybox_script(application_script, "application ok\n") << 32;
    if (!errors) {
      ++application_cycles;
    }
  }
  return errors;
}

static unsigned long pipe_output_rollback(void) {
  // 1000 rejected creations exceed the 64-pipe and 256-metadata budgets: a
  // leaked reservation/state cannot hide in spare capacity. After stdio 0/1/2,
  // the first successful pipe must still receive the lowest fds 3/4.
  for (unsigned i = 0; i < 1000; ++i) {
    if (syscall1(SYS_PIPE, (long)vm_rodata) != -14) {
      return 1;
    }
  }
  long ends[2] = {-1, -1};
  if (pipe(ends) != 0) {
    return 2;
  }
  unsigned long errors = ends[0] != 3 || ends[1] != 4;
  errors |= (unsigned long)(syscall1(SYS_CLOSE, ends[0]) != 0) << 1;
  errors |= (unsigned long)(syscall1(SYS_CLOSE, ends[1]) != 0) << 2;
  return errors;
}

#if defined(__x86_64__)
static void set_fp_state(int child) {
  // x87 0x77f/0xb7f and MXCSR 0x3f80/0x5f80 select distinct down/up rounding
  // while keeping exceptions masked. XMM15 uses different nonzero parent/child
  // markers so a save/restore that only preserves the default state cannot pass.
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
  // x87's stored extended format is 80 bits (10 bytes); the checks below
  // recognize 1.0: integer-bit byte 0x80 and exponent bytes 0xff/0x3f.
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
  return child > 1 && waited == child && status == SIGFPE;
}
#endif

void _start(long argc, const char **argv) {
  // Native ELF entry passes argc/argv in registers on every supported ISA.
  // Dispatch before workload control: these children report through waitpid.
  // The selector invocation is exactly: validation signals <case>.
  if (argc == 3 && argv && streq(argv[1], "signals")) {
    _exit(signal_case(argv[2]));
  }

  // Native affinity syscall 20 uses a u32 mask; bit 0 pins validation to CPU0.
  // Workload selectors below are returned by control 0, not syscall numbers.
  unsigned mask = 1;
  long affinity = syscall3(20, 0, sizeof(mask), (long)&mask);
  long mode = control(0, affinity, 0);
  if (mode >= 11 && mode <= 16) {
    user_benchmark(mode);
  } else if (mode == 7) {
    // Only the chosen data page may fault under allocation pressure. Keep every
    // test/helper code page resident before draining the physical allocator.
    for (unsigned long page = (unsigned long)__user_text_start; page < (unsigned long)__user_text_end; page += 4096) {
      (void)*(const volatile unsigned char *)page;
    }
    unsigned long (*const tests[])(void) = {
        uaccess_allocation_fault, uaccess_write_fault,       uaccess_read_fault,       uaccess_partial_read,
        uaccess_partial_write,    uaccess_partial_pipe_read, uaccess_sigframe_fault,   uaccess_sigreturn_fault,
        uaccess_devices,          uaccess_cow_copy_fault,    uaccess_cow_partial_read, uaccess_cow_user_fault};
    for (long test = 0; test < (long)(sizeof(tests) / sizeof(tests[0])); ++test) {
      control(1, test, 0);
      unsigned long errors = tests[test]();
      if (!control(2, errors == 0, (long)errors)) {
        break;
      }
    }
    control(3, 0, 0);
  } else if (mode == 6) {
    control(1, 0, 0);
    long probe = frame_register_probe(SYS_GETPID, 0);
    if (!control(2, syscall6(511, 10, 11, 22, 33, 44, 55) == 12345 && probe == getpid(),
                 probe == getpid() ? 0 : probe)) {
      control(3, 0, 0);
    }
    control(1, 1, 0);
    long child = frame_register_probe(SYS_FORK, 0);
    if (getpid() != 1) {
      _exit(child == 0 ? 37 : (int)(90 - child - 1000));
    }
    int child_status = 0;
    long waited = child > 0 ? syscall3(SYS_WAITPID, child, (long)&child_status, 0) : -1;
    if (!control(2, waited == child && child > 0 && ((child_status >> 8) & 255) == 37,
                 child < 0 ? child : child_status)) {
      control(3, 0, 0);
    }
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
  } else if (mode == 25) {
    unsigned long (*const tests[])(void) = {
        ipc_roundtrip,           ipc_deadline,          ipc_peer_death,   ipc_signal_cancel,
        ipc_capability_transfer, ipc_delivery_rollback, ipc_memory_object, ipc_badged_sender,
        ipc_nested_roundtrip};
    for (long test = 0; test < (long)(sizeof(tests) / sizeof(tests[0])); ++test) {
      control(1, test, 0);
      unsigned long errors = tests[test]();
      if (!control(2, errors == 0, (long)errors))
        break;
    }
    control(3, 0, 0);
  } else if (mode == 21) {
    const char *scripts[] = {
        "exit 37", "value=$(printf 'moss\\n'); [ \"$value\" = moss ] && printf '%s\\n' \"$value\" && exit 37; exit 98",
        "[ \"$HOSTNAME\" = moss ] || exit 97; export MOSS_APP='value with spaces'; "
        "exec /busybox.elf ash -c '[ \"$HOSTNAME\" = moss ] && [ \"$MOSS_APP\" = \"value with spaces\" ] && "
        "exit 37; exit 98'",
        "set -o pipefail || exit 91; (exit 7) | /busybox.elf cat; [ \"$?\" -eq 7 ] || exit 92; "
        "count=$(printf 'moss\\nother\\nmoss again\\n' | /busybox.elf cat | "
        "/busybox.elf grep moss | /busybox.elf wc -l); [ \"$?\" -eq 0 ] && [ \"$count\" -eq 2 ] && "
        "printf 'pipeline ok\\n' && exit 37; exit 98",
        "/busybox.elf mkdir /moss-work && /busybox.elf mkdir /moss-work/alpha /moss-work/beta && "
        "[ -d /moss-work/alpha ] && [ -d /moss-work/beta ] && /busybox.elf ls -1 /moss-work && "
        "/busybox.elf rm -rf /moss-work && "
        "[ ! -e /moss-work ] && exit 37; exit 98",
        "printf 'moss data\\n' > /moss-data && printf 'appended\\n' >> /moss-data && "
        "/busybox.elf cat /moss-data && printf 'new\\n' > /moss-data && /busybox.elf cat /moss-data && "
        "/busybox.elf rm /moss-data && [ ! -e /moss-data ] && exit 37; exit 98",
        "printf 'copy payload\\n' > /copy-source || exit 91; /busybox.elf cp /copy-source /copy-target || exit 92; "
        "/busybox.elf cat /copy-target || exit 93; /busybox.elf rm /copy-source /copy-target || exit 94; "
        "[ ! -e /copy-source ] || exit 95; [ ! -e /copy-target ] || exit 96; exit 37",
        "/busybox.elf mkdir /move-dir && printf 'move payload\\n' > /move-source && "
        "/busybox.elf mv /move-source /move-dir/target && [ ! -e /move-source ] && "
        "/busybox.elf cat /move-dir/target && /busybox.elf mkdir /move-dir/nested && "
        "printf 'obsolete\\n' > /move-dir/nested/target && "
        "/busybox.elf mv -f /move-dir/target /move-dir/nested/target && "
        "/busybox.elf mv /move-dir/nested /move-tree && [ ! -e /move-dir/nested ] && "
        "/busybox.elf cat /move-tree/target && /busybox.elf rm -rf /move-dir /move-tree && "
        "[ ! -e /move-dir ] && [ ! -e /move-tree ] && exit 37; exit 98",
        application_script,
        "set -o pipefail; printf 'first\\nsecond\\nthird\\n' | /busybox.elf head -n 2 && exit 37; exit 98",
        "set -o pipefail; printf 'alpha:one\\nbeta:two\\n' | /busybox.elf cut -d : -f 2 && exit 37; exit 98",
        "set -o pipefail; printf 'pear\\napple\\npear\\nbanana\\n' | /busybox.elf sort && "
        "printf '10\\n2\\n1\\n' | /busybox.elf sort -n && exit 37; exit 98",
        "set -o pipefail; printf 'apple\\napple\\npear\\npear\\nbanana\\n' | /busybox.elf uniq && exit 37; exit 98",
        "set -o pipefail; printf 'moss 123\\n' | /busybox.elf tr a-z A-Z && "
        "printf 'moss123\\n' | /busybox.elf tr -d 0-9 && exit 37; exit 98",
        "set -o pipefail; printf 'tee payload\\n' | /busybox.elf tee /tee-copy && "
        "[ \"$(/busybox.elf cat /tee-copy)\" = 'tee payload' ] && /busybox.elf rm /tee-copy && "
        "exit 37; exit 98",
        // cmp's POSIX exit statuses distinguish different contents (1) from an I/O error (2).
        "printf 'same\\n' > /cmp-left && /busybox.elf cp /cmp-left /cmp-right && "
        "/busybox.elf cmp /cmp-left /cmp-right || exit 98; printf 'different\\n' > /cmp-right || exit 98; "
        "/busybox.elf cmp -s /cmp-left /cmp-right; [ \"$?\" -eq 1 ] || exit 98; "
        "/busybox.elf cmp -s /cmp-left /cmp-missing 2>/dev/null; [ \"$?\" -eq 2 ] || exit 98; "
        "/busybox.elf rm /cmp-left /cmp-right && printf 'cmp ok\\n' && exit 37; exit 98",
        "/busybox.elf basename /usr/local/moss.txt .txt && exit 37; exit 98",
        "/busybox.elf dirname /usr/local/moss.txt && exit 37; exit 98",
        "/busybox.elf mkdir /rmdir-work /rmdir-work/child && printf 'keep\\n' > /rmdir-work/file && "
        "! /busybox.elf rmdir /rmdir-work 2>/dev/null && [ -d /rmdir-work/child ] && "
        "[ \"$(/busybox.elf cat /rmdir-work/file)\" = keep ] && /busybox.elf rm /rmdir-work/file && "
        "/busybox.elf rmdir /rmdir-work/child /rmdir-work && [ ! -e /rmdir-work ] && "
        "printf 'rmdir ok\\n' && exit 37; exit 98",
        "[ -n \"$(/busybox.elf uname -m)\" ] && /busybox.elf uname -s && /busybox.elf uname -n && "
        "/busybox.elf uname -o && exit 37; exit 98",
        // Verify real SIGTERM delivery (POSIX signal 15) through a foreground trap.
        // Waiting for a background job would require mlibc's unavailable Sigsuspend.
        "trap 'printf \"kill ok\\n\"; exit 37' TERM; "
        "/busybox.elf kill -0 $$ && [ \"$(/busybox.elf kill -l 15)\" = TERM ] || exit 98; "
        "/busybox.elf kill -TERM $$; exit 98",
        // The size filter matches "deep" plus its newline, exactly five bytes.
        "set -o pipefail; /busybox.elf mkdir /find-work /find-work/nested && "
        "printf 'keep\\n' > /find-work/keep.txt && printf 'drop\\n' > /find-work/drop.log && "
        "printf 'deep\\n' > /find-work/nested/deep.txt && "
        "/busybox.elf find /find-work -maxdepth 1 -type f -name '*.txt' && "
        "[ \"$(/busybox.elf find /find-work -type f -path '*/nested/*' -size 5c)\" = "
        "/find-work/nested/deep.txt ] && "
        "/busybox.elf find /find-work -maxdepth 1 -type f \\( -name '*.txt' -o -name '*.log' \\) | "
        "/busybox.elf sort && "
        "[ \"$(/busybox.elf find /find-work -mindepth 1 -maxdepth 1 ! -type f)\" = /find-work/nested ] && "
        "/busybox.elf find /find-work -maxdepth 1 -name '*.txt' -print0 | /busybox.elf tr '\\000' '\\n' && "
        "/busybox.elf rm -rf /find-work && [ ! -e /find-work ] && exit 37; exit 98",
        "/busybox.elf mkdir /find-protected && printf 'keep\\n' > /find-protected/file && "
        "! /busybox.elf find /find-protected -mtime 1 2>/dev/null && "
        "! /busybox.elf find /find-protected -exec /busybox.elf rm /find-protected/file \\; 2>/dev/null && "
        "! /busybox.elf find /find-protected -delete 2>/dev/null && "
        "[ \"$(/busybox.elf cat /find-protected/file)\" = keep ] && /busybox.elf rm -rf /find-protected && "
        "printf 'find restrictions ok\\n' && exit 37; exit 98"};
    // Keep fixture order aligned with the kernel and host users.busybox catalogs.
    // Exact stdout and the existing exit marker must both match for each applet.
    const char *expected[] = {"", "moss\n", "", "pipeline ok\n",
                              // The pinned minimal ls profile has sorting disabled.
                              "beta\nalpha\n", "moss data\nappended\nnew\n", "copy payload\n",
                              "move payload\nmove payload\n", "application ok\n", "first\nsecond\n", "one\ntwo\n",
                              "apple\nbanana\npear\npear\n1\n2\n10\n", "apple\npear\nbanana\n", "MOSS 123\nmoss\n",
                              "tee payload\n", "cmp ok\n", "moss\n", "/usr/local\n", "rmdir ok\n", "Moss\nmoss\nMoss\n",
                              "kill ok\n",
                              "/find-work/keep.txt\n/find-work/drop.log\n/find-work/keep.txt\n/find-work/keep.txt\n",
                              "find restrictions ok\n"};
    _Static_assert(sizeof(scripts) / sizeof(scripts[0]) == sizeof(expected) / sizeof(expected[0]),
                   "Every BusyBox fixture needs exact expected stdout");
    for (long test = 0; test < (long)(sizeof(scripts) / sizeof(scripts[0])); ++test) {
      control(1, test, 0);
      unsigned long errors = busybox_script(scripts[test], expected[test]);
      if (!control(2, errors == 0, (long)errors)) {
        break;
      }
    }
    control(3, 0, 0);
  } else if (mode == 22) {
    // Keep this dispatch order synchronized with both users.exec catalogs.
    enum {
      EXEC_PROBE_CASES = 25,
      EXEC_ALLOCATION_CASE = 25,
      EXEC_MUTABLE_CASE = 26,
      EXEC_BOUNDARY_CASE = 27,
      EXEC_SOURCE_VERSION_CASE = 28,
      EXEC_REGISTRATION_CASE = 29,
      EXEC_SHARED_THREAD_CASE = 30,
      EXEC_CASES = 31,
    };
    for (long test = 0; test < EXEC_CASES; ++test) {
      control(1, test, 0);
      unsigned long errors = 1;
      if (test < EXEC_PROBE_CASES) {
        errors = exec_probe(test);
      } else if (test == EXEC_ALLOCATION_CASE) {
        errors = exec_allocation_rollback();
      } else if (test == EXEC_MUTABLE_CASE) {
        errors = exec_mutable_snapshot_rollback();
      } else if (test == EXEC_BOUNDARY_CASE) {
        errors = exec_boundary_load_plan();
      } else if (test == EXEC_SOURCE_VERSION_CASE) {
        errors = exec_source_version();
      } else if (test == EXEC_SHARED_THREAD_CASE) {
        errors = exec_shared_thread_gate();
      } else if (test == EXEC_REGISTRATION_CASE) {
        errors = exec_registration_gate();
      }
      if (!control(2, errors == 0, (long)errors)) {
        break;
      }
    }
    control(3, 0, 0);
  } else if (mode == 20) {
    const char *programs[] = {"runtime", "permissions"};
    for (long test = 0; test < 2; ++test) {
      control(1, test, 0);
      long child = fork();
      if (child == 0) {
        const char *libc_args[] = {"libc_validation", programs[test], 0};
        syscall3(SYS_EXECVE, (long)"/libc_validation.elf", (long)libc_args, 0);
        _exit(99);
      }
      int status = 0;
      long waited = child > 0 ? waitpid(child, &status, 0) : -1;
      int ok = child > 0 && waited == child && status == (37 << 8);
      if (!control(2, ok, ok ? 0 : status)) {
        break;
      }
    }
    control(3, 0, 0);
  } else if (mode == 10) {
    unsigned long (*const tests[])(void) = {timer_relative_sleep,
                                            timer_absolute_sleep,
                                            timer_invalid_arguments,
                                            timer_short_reuse,
                                            timer_cancel_in_flight,
                                            timer_early_wakeup,
                                            timer_arm_failure_recovery,
                                            timer_relative_interrupted,
                                            timer_clock_relative_interrupted,
                                            timer_clock_absolute_interrupted};
    for (long test = 0; test < (long)(sizeof(tests) / sizeof(tests[0])); ++test) {
      control(1, test, 0);
      unsigned long errors = tests[test]();
      if (!control(2, errors == 0, (long)errors)) {
        break;
      }
    }
    control(3, 0, 0);
  } else if (mode == 9 || mode == 23) {
    application_workload = mode == 23;
    // Five is the midpoint of the routine 10-cycle application workload, so a
    // host-throttled but live guest reports progress before exhausting the
    // 30-second no-progress window. Core recovery stays at 100 cycles to avoid
    // making its 1,000/10,000-cycle runs protocol-bound. The host cadence must
    // change with these values.
    const long interval = application_workload ? 5 : 100;
    control(1, 0, 0);
    // The warmup observes EOF before reaping once. Repeating that ordering in
    // all 1,000 resource cycles adds a second wake/block handoff per cycle and
    // would measure scheduler traffic instead of descriptor recovery.
    unsigned long cycle_errors = lifecycle_cycle(LIFECYCLE_EOF_BEFORE_WAIT);
    int ok = cycle_errors == 0;
    if (ok) {
      ok = control(10, 0, application_workload ? (long)application_cycles - 1 : 0) != 0;
    }
    // A next-checkpoint counter avoids RV64's lazy-loaded 64-bit modulo
    // constants becoming resident only after the resource baseline.
    long next_checkpoint = interval;
    long progress = 1;
    for (long cycle = 1; ok && progress == 1; ++cycle) {
      cycle_errors = lifecycle_cycle(LIFECYCLE_WAIT_BEFORE_EOF);
      ok = cycle_errors == 0;
      if (ok && cycle == next_checkpoint) {
        progress = control(10, cycle, application_workload ? (long)application_cycles - 1 : 0);
        ok = progress == 1 || progress == 2;
        next_checkpoint += interval;
      }
    }
    control(2, ok && progress == 2, (long)cycle_errors);
    control(3, 0, 0);
  } else if (mode == 8 || mode == 24) {
    const char *cases[] = {"basic_handler",
                           "nested_signals",
                           "sigchld",
                           "wait_registration",
                           "wait_interrupted",
                           "wait_restarted",
                           "cpu_bound_irq",
                           "stop_continue",
                           "wait_job_status",
                           "no_cldstop",
                           "wait_process_group",
                           "wait_group_change",
                           "no_cldwait",
                           "sigaction_race",
                           "sigaction_discard",
                           "signal_exit_status",
                           "sigprocmask",
                           "sigaltstack",
                           "sig_ign",
                           "invalid_arguments",
                           "frame_validation",
                           "altstack_overflow",
                           "altstack_boundaries",
                           "inheritance",
                           "pid_lifecycle",
                           "pipe_sigpipe",
                           "pipe_interrupted",
                           "pipe_restarted",
                           "pipe_noninterrupting_signals",
                           "pipe_partial_interrupt",
                           "signal_wakeup_affinity",
                           "console_interrupted",
                           "console_partial_interrupt",
                           "console_restarted",
                           "console_multi_reader"};
    const unsigned count = mode == 24 ? 1 : sizeof(cases) / sizeof(cases[0]);
    for (unsigned test = 0; test < count; ++test) {
      control(1, test, 0);
      long child = fork();
      if (child == 0) {
        const char *args[] = {"validation", "signals", mode == 24 ? "irq_before_registration" : cases[test], 0};
        syscall3(SYS_EXECVE, (long)"/validation.elf", (long)args, 0);
        _exit(99);
      }
      if (!control(2, wait_exit(child, 0), 0)) {
        break;
      }
    }
    control(3, 0, 0);
  } else if (mode == 5) {
    // Preserve the users.vm driver's numbered case order.
    unsigned long (*const tests[])(void) = {vm_private_cow, vm_readonly_cow, vm_access_permissions, vm_brk_lifecycle};
    for (long test = 0; test < (long)(sizeof(tests) / sizeof(tests[0])); ++test) {
      control(1, test, 0);
      unsigned long errors = tests[test]();
      if (!control(2, errors == 0, (long)errors)) {
        control(3, 0, 0);
      }
    }
    // Five kernel target classes follow the four existing VM cases. Each
    // class tests read/write against both mappings of the same physical object.
    for (long target = 0; target < 5; ++target) {
      control(1, (long)(sizeof(tests) / sizeof(tests[0])) + target, 0);
      unsigned long errors = vm_kernel_isolation(target);
      if (!control(2, errors == 0, (long)errors)) {
        break;
      }
    }
    control(3, 0, 0);
  } else if (mode == 1) {
    long pid = getpid();
    control(1, 0, 0);
    // Slot 510 is an invalid production syscall next to reserved validation
    // slot 511; rejection must not prevent this PID1 worker from continuing.
    if (control(2, pid == 1 && syscall0(SYS_GETPPID) == 0 && syscall0(510) < 0, 0)) {
      control(1, 1, 0);
      unsigned long errors = user_ranges();
      // Reuse this existing users case: bits 32..39 distinguish number failures
      // from the lower range-check bits without adding a control-protocol mode.
      errors |= numbers_regression() << 32;
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
        if (context_errors) {
          _exit(97);
        }
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
      if (control(2, context_errors == 0, (long)context_errors)) {
        control(1, 3, 0);
        unsigned long errors = pipe_output_rollback();
        if (control(2, errors == 0, (long)errors)) {
          control(1, 4, 0);
          errors = 0;
          for (unsigned i = 0; i < 64; ++i) {
            errors |= sched_yield() != 0 || getpid() != pid;
          }
          if (control(2, errors == 0, (long)errors)) {
            control(1, 5, 0);
            errors = wait_status_rollback();
            if (control(2, errors == 0, (long)errors)) {
              control(1, 6, 0);
              errors = pipe_waits_for_writer();
              if (control(2, errors == 0, (long)errors)) {
                control(1, 7, 0);
                errors = pipe_cross_cpu_roundtrip(256);
                if (control(2, errors == 0, (long)errors)) {
                  control(1, 8, 0);
                  errors = pipe_waits_for_reader();
                  if (control(2, errors == 0, (long)errors)) {
                    control(1, 9, 0);
                    // Each peer exits on CPU1 just after its reply wakes CPU0.
                    // Repeated short exchanges exercise exit racing waitpid,
                    // not just the data transfer of one long-lived child.
                    for (unsigned i = 0; i < 128 && !errors; ++i) {
                      errors = pipe_cross_cpu_roundtrip(1);
                    }
                    if (control(2, errors == 0, (long)errors)) {
                      control(1, 10, 0);
                      // 40/41 targets fd-table cloning, 44/45 process allocation,
                      // and 46/47 four metadata stages; counts 16/4 bound repetition.
                      errors = fork_allocation_rollback(40, 41, 16);
                      if (control(2, errors == 0, (long)errors)) {
                        control(1, 11, 0);
                        errors = mmap_heap_rollback();
                        if (control(2, errors == 0, (long)errors)) {
                          control(1, 12, 0);
                          errors = fork_allocation_rollback(44, 45, 4);
                          if (control(2, errors == 0, (long)errors)) {
                            control(1, 13, 0);
                            errors = fork_allocation_rollback(46, 47, 4);
                            control(2, errors == 0, (long)errors);
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
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
      if (!parent_fp_state()) {
        _exit(97);
      }
#endif
      long peer = control(7, affinity, 0);
      if (peer == 2) {
        peer = control(7, 1, syscall1(SYS_PIPE, (long)vm_rodata));
      }
      _exit(peer == 1 ? 37 : 99);
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
