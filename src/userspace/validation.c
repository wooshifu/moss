#include "syscall.h"

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
static long control(long op, long a, long b) { return syscall3(511, op, a, b); }
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

static unsigned long exec_probe(long test) {
  // The first 17 catalog entries are the malformed ELF fixtures listed below;
  // subsequent selectors exercise argument admission using a valid image.
  enum { EXEC_REJECTION_CASES = 17 };
  enum {
    EXEC_BAD_ENV_VECTOR = EXEC_REJECTION_CASES,
    EXEC_BAD_ENV_STRING,
    EXEC_ARGUMENT_COUNT,
    EXEC_COMBINED_COUNT,
    EXEC_STRING_BYTES,
    EXEC_EXACT_COUNT,
    EXEC_EXACT_BYTES,
    EXEC_EMPTY_VECTORS,
  };
  static const char *invalid_images[EXEC_REJECTION_CASES] = {
      "/bad_entry.elf",       "/bad_phentsize.elf",  "/bad_load.elf",       "/truncated_header.elf",
      "/truncated_phdr.elf",  "/bad_file_range.elf", "/bad_user_range.elf", "/bad_address_overflow.elf",
      "/bad_page_offset.elf", "/bad_alignment.elf",  "/bad_reserved.elf",   "/bad_overlap.elf",
      "/too_many_phdrs.elf",  "/bad_rwx.elf",        "/bad_dynamic.elf",    "/bad_interp.elf",
      "/bad_tls_file.elf",
  };
  long child = fork();
  if (!child) {
    volatile unsigned long canary = 0x12345678;
    // Test the shared 128-string cap at/beyond its boundary while retaining
    // terminator slots: 129 argv strings need 130 pointers, 128 env need 129.
    static const char *args[130], *environment[129];
    args[0] = "exec";
    static char bytes[16385]; // 16 KiB + NUL exceeds the inclusive exec byte budget by one.
    const char *path = "/validation_child.elf";
    long wanted = -8;
    if (test < EXEC_REJECTION_CASES) {
      path = invalid_images[test];
      // If a malformed image is accidentally accepted, validation_child must
      // exit with its startup-error marker instead of sharing this probe's 37.
      // A correct ENOEXEC returns here and still observes the live stack canary.
      args[0] = "must-reject";
    } else if (test == EXEC_BAD_ENV_VECTOR) {
      // A bad env vector must not destroy the caller's address space.
      long result = syscall3(SYS_EXECVE, (long)path, (long)args, 1);
      _exit(result == -14 && canary == 0x12345678 ? 37 : 98);
    } else if (test == EXEC_BAD_ENV_STRING) {
      environment[0] = (const char *)1;
      wanted = -14;
    } else if (test == EXEC_ARGUMENT_COUNT) {
      for (unsigned i = 0; i < 129; ++i) {
        args[i] = "";
      }
      wanted = -7;
    } else if (test == EXEC_COMBINED_COUNT) {
      for (unsigned i = 0; i < 128; ++i) {
        environment[i] = ""; // One argv plus 128 environment strings exceeds the shared cap.
      }
      wanted = -7;
    } else if (test == EXEC_STRING_BYTES) {
      for (unsigned i = 0; i < 16384; ++i) {
        bytes[i] = 'a';
      }
      args[0] = bytes;
      wanted = -7;
    } else {
      path = "/libc_validation.elf";
      if (test == EXEC_EXACT_COUNT) {
        // 64 argv + 64 environment strings exactly hit the 128-string limit;
        // six bytes hold each generated "M00=1" string including its NUL.
        static char names[64][6];
        args[0] = "count";
        for (unsigned i = 1; i < 64; ++i) {
          args[i] = "x";
        }
        for (unsigned i = 0; i < 64; ++i) {
          names[i][0] = 'M';
          names[i][1] = (char)('0' + i / 10);
          names[i][2] = (char)('0' + i % 10);
          names[i][3] = '=';
          names[i][4] = '1';
          environment[i] = names[i];
        }
      } else if (test == EXEC_EXACT_BYTES) {
        args[0] = "bytes";
        args[1] = bytes;
        for (unsigned i = 0; i < 16377; ++i) {
          bytes[i] = 'a'; // Including both terminators and argv[0], exactly 16 KiB.
        }
      } else {
        args[0] = 0;
      }
    }
    long result = syscall3(SYS_EXECVE, (long)path, (long)args, (long)environment);
    _exit(test < EXEC_EXACT_COUNT && result == wanted && canary == 0x12345678 ? 37 : 98);
  }
  int status = 0;
  long waited = child > 0 ? waitpid(child, &status, 0) : -1;
  int expected = test < EXEC_EXACT_COUNT ? 37 : 39;
  return child > 0 && waited == child && status == (expected << 8) ? 0 : 1 | (unsigned long)status << 8;
}

static unsigned long busybox_script(const char *script, const char *expected) {
  long fds[2];
  if (pipe(fds)) {
    return 1;
  }
  long child = fork();
  if (!child) {
    close(fds[0]);
    if (dup2(fds[1], 1) != 1) {
      _exit(98);
    }
    close(fds[1]);
    const char *args[] = {"busybox", "ash", "-c", script, 0};
    syscall3(SYS_EXECVE, (long)"/busybox.elf", (long)args, 0);
    _exit(99);
  }
  close(fds[1]);
  unsigned long errors = child < 0 ? 2 : 0;
  unsigned long length = strlen(expected), received = 0;
  // Fixed 64-byte drain chunks bound stack use independent of captured stdout;
  // the precise chunk size is unrecorded. Continue draining after a mismatch
  // so a child blocked on its stdout pipe can still exit and be reaped.
  char buffer[64];
  long count;
  while ((count = read(fds[0], buffer, sizeof(buffer))) > 0) {
    for (long i = 0; i < count; ++i, ++received) {
      if (received >= length || buffer[i] != expected[received]) {
        errors |= 4;
      }
    }
    if (errors & 4) {
      const char *message = "BusyBox unexpected stdout chunk: ";
      write(2, message, strlen(message));
      write(2, buffer, (unsigned long)count);
    }
  }
  errors |= count < 0 || received != length ? 8 : 0;
  close(fds[0]);
  int status = 0;
  if (child > 0 && (waitpid(child, &status, 0) != child || status != (37 << 8))) {
    errors |= 16 | (unsigned long)status << 8;
  }
  return errors;
}

static unsigned long wait_status_rollback(void) {
  unsigned long errors = 0;
  // Repeat sixteen failed/retried waits to expose retained zombie ownership.
  // This bounded repetition count is a fixture choice, not a measured threshold.
  for (unsigned i = 0; i < 16; ++i) {
    long child = fork();
    if (child == 0) {
      _exit(37);
    }
    if (child < 0) {
      return errors | 1;
    }
    int status = 0;
    // 3 includes an unsupported wait option; adding 2^32 must not alias the
    // live child PID through narrowing. Status uses its 8-bit code at bit 8.
    errors |= (unsigned long)(waitpid(child, &status, 3) != -22) << 1;
    errors |= (unsigned long)(waitpid(child + (1UL << 32), &status, 1) != -10) << 2;
    // A failed copyout must leave the zombie collectable by a later wait.
    errors |= (unsigned long)(waitpid(child, (int *)1, 0) != -14) << 3;
    long result = waitpid(child, &status, 0);
    errors |= (unsigned long)(result != child || ((status >> 8) & 255) != 37) << 4;
    errors |= (unsigned long)(waitpid(child, &status, 1) != -10) << 5;
    if (errors) {
      break;
    }
  }
  return errors;
}

extern const unsigned char __user_text_start[], __user_text_end[];

static unsigned long fork_allocation_rollback(long arm, long release, unsigned cycles) {
  for (unsigned long page = (unsigned long)__user_text_start; page < (unsigned long)__user_text_end; page += 4096) {
    (void)*(const volatile unsigned char *)page;
  }
  long fd = open("/fixture.bin", 0);
  if (fd < 0) {
    return 1;
  }
  unsigned long errors = syscall3(SYS_FCNTL, fd, 2, 1) != 0;
  volatile unsigned long canary = 0x12345678;
  for (unsigned cycle = 0; cycle < cycles && !errors; ++cycle) {
    if (!control(arm, (long)cycle, 0)) {
      errors |= 2;
      break;
    }
    long child = fork();
    if (!child) {
      _exit(99);
    }
    int status = 0;
    errors |= child != -12 ? 4 : 0;
    if (child > 0) {
      waitpid(child, &status, 0);
    }
    errors |= !control(release, 0, 0) ? 8 : 0;
    canary += 1; // The parent can still write its COW stack after rollback.
    unsigned char byte = 255;
    errors |= waitpid(-1, &status, 1) != -10 || canary != 0x12345679 + cycle ? 16 : 0;
    errors |= syscall3(SYS_FCNTL, fd, 1, 0) != 1 || read(fd, &byte, 1) != 1 || byte != cycle ? 32 : 0;
    child = fork();
    if (!child) {
      _exit(syscall3(SYS_FCNTL, fd, 1, 0) == 1 ? 37 : 99);
    }
    errors |= child <= 0 || waitpid(child, &status, 0) != child || status != (37 << 8) ? 64 : 0;
  }
  close(fd);
  return errors;
}

static unsigned long mmap_heap_rollback(void) {
  // Control 42/43 exhausts/restores the VMA-allocation heap; four independent
  // retries check resource reuse. The repetition count is a bounded fixture choice.
  for (unsigned long page = (unsigned long)__user_text_start; page < (unsigned long)__user_text_end; page += 4096) {
    (void)*(const volatile unsigned char *)page;
  }
  unsigned long errors = 0;
  for (unsigned cycle = 0; cycle < 4 && !errors; ++cycle) {
    long original = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
    if (original < 0) {
      return errors | 1;
    }
    volatile unsigned char *byte = (volatile unsigned char *)original;
    *byte = 37;
    if (!control(42, 0, 0)) {
      syscall2(SYS_MUNMAP, original, 4096);
      return errors | 2;
    }
    long failed = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
    errors |= failed != -12 ? 4 : 0;
    errors |= !control(43, 0, 0) ? 8 : 0;
    if (failed >= 0) {
      syscall2(SYS_MUNMAP, failed, 4096);
    }
    errors |= *byte != 37 ? 16 : 0;
    *byte = 53;
    errors |= *byte != 53 ? 16 : 0;
    long retry = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
    errors |= retry != original + 4096 ? 32 : 0;
    if (retry >= 0) {
      volatile unsigned char *mapped = (volatile unsigned char *)retry;
      *mapped = 71;
      errors |= *mapped != 71 ? 64 : 0;
      errors |= syscall2(SYS_MUNMAP, retry, 4096) != 0 ? 128 : 0;
    }
    errors |= syscall2(SYS_MUNMAP, original, 4096) != 0 ? 128 : 0;
  }
  return errors;
}

static unsigned long exec_allocation_rollback(void) {
  enum {
    EXEC_ENOENT = 2,
    EXEC_ENOEXEC = 8,
    EXEC_ENOMEM = 12,
  };
  enum {
    EXEC_ERR_MALFORMED = 1UL << 0,
    EXEC_ERR_FIXTURE = 1UL << 1,
    EXEC_ERR_ARM = 1UL << 2,
    EXEC_ERR_ROLLBACK = 1UL << 3,
    EXEC_ERR_FD_OFFSET = 1UL << 4,
    EXEC_ERR_RETRY = 1UL << 5,
    EXEC_ERR_MISSING = 1UL << 6,
  };
  // The nonzero asymmetric value makes stack replacement or clobbering visible.
  const unsigned long canary_value = 0x12345678;
  volatile unsigned long canary = canary_value;
  const char *args[] = {"exec", 0};
  unsigned long errors =
      syscall3(SYS_EXECVE, (long)"/bad_entry.elf", (long)args, 0) != -EXEC_ENOEXEC ? EXEC_ERR_MALFORMED : 0;
  errors |= syscall3(SYS_EXECVE, (long)"/exec-missing.elf", (long)args, 0) != -EXEC_ENOENT ? EXEC_ERR_MISSING : 0;
  long fd = open("/fixture.bin", 0);
  unsigned char byte = 255;
  if (fd < 0 || read((int)fd, &byte, 1) != 1 || byte != 0) {
    return errors | EXEC_ERR_FIXTURE;
  }
  // Warm the instruction stream and copy source before exhausting real pages.
  volatile unsigned char warm = 0;
  for (const unsigned char *p = __user_text_start; p < __user_text_end; p += 4096) {
    warm ^= *p;
  }
  const char *path = "/validation_child.elf";
  for (const volatile char *p = path; *p; ++p) {
    warm ^= (unsigned char)*p;
  }
  // The merged signal fixtures enlarge rodata: argv[0] can now occupy a
  // different demand-loaded page from path. Warm every exec string so injected
  // allocation pressure tests page-table rollback, not a user-copy page fault.
  // Volatile reads prevent short literals from being folded into constants.
  for (const volatile char *p = args[0]; *p; ++p) {
    warm ^= (unsigned char)*p;
  }
  (void)warm;
  // Control 35 returns native page-table allocation stages; 36 reserves a
  // given page budget, and 37 verifies rollback and releases that pressure.
  long stages = control(35, 0, 0);
  for (long budget = 0; budget < stages; ++budget) {
    if (!control(36, budget, 0)) {
      errors |= EXEC_ERR_ARM;
      break;
    }
    long result = syscall3(SYS_EXECVE, (long)path, (long)args, 0);
    int recovered = control(37, budget, 0) != 0;
    if (result != -EXEC_ENOMEM || canary != canary_value || !recovered) {
      print("  FAIL: exec rollback budget=");
      print_long(budget);
      print(" result=");
      print_long(result);
      print(" recovered=");
      print_long(recovered);
      print("\n");
      errors |= EXEC_ERR_ROLLBACK;
      break;
    }
    if (read((int)fd, &byte, 1) != 1 || byte != budget + 1) {
      errors |= EXEC_ERR_FD_OFFSET;
    }
  }
  close((int)fd);
  long child = fork();
  if (!child) {
    syscall3(SYS_EXECVE, (long)path, (long)args, 0);
    _exit(98);
  }
  int status = 0;
  if (child <= 0 || waitpid(child, &status, 0) != child || status != (37 << 8)) {
    errors |= EXEC_ERR_RETRY;
  }
  return errors;
}

static int copy_mutable_exec_fixture(const char *source_path, const char *target_path) {
  enum {
    OPEN_READ_ONLY = 0,
    OPEN_WRITE_ONLY = 1U << 0,
    OPEN_CREATE = 1U << 6,
    OPEN_EXCLUSIVE = 1U << 7,
    // One Moss user page bounds stack use and makes each transfer naturally page-sized.
    COPY_BUFFER_BYTES = 4096,
    // The private fixture must be readable, writable and executable by its owner.
    OWNER_RWX_MODE = 0700,
  };
  (void)syscall1(SYS_UNLINK, (long)target_path);
  long source = syscall3(SYS_OPEN, (long)source_path, OPEN_READ_ONLY, 0);
  if (source < 0) {
    return 0;
  }
  long target = syscall3(SYS_OPEN, (long)target_path, OPEN_WRITE_ONLY | OPEN_CREATE | OPEN_EXCLUSIVE, OWNER_RWX_MODE);
  if (target < 0) {
    close((int)source);
    return 0;
  }

  unsigned char buffer[COPY_BUFFER_BYTES];
  int copied = 1;
  long count = 0;
  while ((count = read((int)source, buffer, sizeof(buffer))) > 0) {
    long offset = 0;
    while (offset < count) {
      long written = write((int)target, buffer + offset, count - offset);
      if (written <= 0) {
        copied = 0;
        break;
      }
      offset += written;
    }
    if (!copied) {
      break;
    }
  }
  copied = copied && count >= 0;
  copied = close((int)source) == 0 && copied;
  copied = close((int)target) == 0 && copied;
  if (!copied) {
    (void)syscall1(SYS_UNLINK, (long)target_path);
  }
  return copied;
}

static unsigned long exec_mutable_snapshot_rollback(void) {
  enum {
    // Validation-only controls; these are unrelated to native syscall numbers.
    EXEC_HEAP_PRESSURE_ARM = 48,
    EXEC_HEAP_PRESSURE_RELEASE = 49,
    // Arguments, SharedPtr object/control, bytes, address space and VMA node.
    EXEC_HEAP_ALLOCATION_STAGES = 6,
    EXEC_ENOMEM = 12,
    // Moss user mappings use 4 KiB pages on every supported architecture.
    USER_PAGE_BYTES = 4096,
  };
  enum {
    EXEC_HEAP_ERR_COPY = 1UL << 0,
    EXEC_HEAP_ERR_FIXTURE = 1UL << 1,
    EXEC_HEAP_ERR_ARM = 1UL << 2,
    EXEC_HEAP_ERR_ROLLBACK = 1UL << 3,
    EXEC_HEAP_ERR_FD_OFFSET = 1UL << 4,
    EXEC_HEAP_ERR_CLEANUP = 1UL << 5,
  };
  const char *path = "/exec-copy.elf";
  if (!copy_mutable_exec_fixture("/validation_child.elf", path)) {
    return EXEC_HEAP_ERR_COPY;
  }

  long fd = open("/fixture.bin", 0);
  // Outside the fixture's expected 0..6 sequence, so an unread sentinel cannot pass.
  unsigned char byte = 255;
  unsigned long errors = 0;
  if (fd < 0 || read((int)fd, &byte, 1) != 1 || byte != 0) {
    if (fd >= 0) {
      close((int)fd);
    }
    (void)syscall1(SYS_UNLINK, (long)path);
    return EXEC_HEAP_ERR_FIXTURE;
  }

  // Make all caller-side instruction and string pages resident before heap
  // exhaustion so the injected failure belongs to exec preparation itself.
  volatile unsigned char warm = 0;
  for (const unsigned char *p = __user_text_start; p < __user_text_end; p += USER_PAGE_BYTES) {
    warm ^= *p;
  }
  for (const volatile char *p = path; *p; ++p) {
    warm ^= (unsigned char)*p;
  }
  const char *args[] = {"exec", 0};
  for (const volatile char *p = args[0]; *p; ++p) {
    warm ^= (unsigned char)*p;
  }
  (void)warm;

  // The asymmetric nonzero value detects accidental replacement or stack clobbering.
  const unsigned long canary_value = 0x12345678;
  volatile unsigned long canary = canary_value;
  for (long stage = 0; stage < EXEC_HEAP_ALLOCATION_STAGES; ++stage) {
    if (!control(EXEC_HEAP_PRESSURE_ARM, stage, 0)) {
      errors |= EXEC_HEAP_ERR_ARM;
      break;
    }
    long result = syscall3(SYS_EXECVE, (long)path, (long)args, 0);
    int recovered = control(EXEC_HEAP_PRESSURE_RELEASE, stage, 0) != 0;
    if (result != -EXEC_ENOMEM || canary != canary_value || !recovered) {
      print("  FAIL: mutable exec rollback stage=");
      print_long(stage);
      print(" result=");
      print_long(result);
      print(" recovered=");
      print_long(recovered);
      print("\n");
      errors |= EXEC_HEAP_ERR_ROLLBACK;
      break;
    }
    if (read((int)fd, &byte, 1) != 1 || byte != (unsigned char)(stage + 1)) {
      errors |= EXEC_HEAP_ERR_FD_OFFSET;
      break;
    }
  }
  errors |= close((int)fd) != 0 ? EXEC_HEAP_ERR_CLEANUP : 0;
  errors |= syscall1(SYS_UNLINK, (long)path) != 0 ? EXEC_HEAP_ERR_CLEANUP : 0;
  return errors;
}

static unsigned long exec_boundary_load_plan(void) {
  long child = fork();
  if (child == 0) {
    const char *args[] = {"boundary", 0};
    syscall3(SYS_EXECVE, (long)"/boundary_load.elf", (long)args, 0);
    _exit(98);
  }
  int status = 0;
  long waited = child > 0 ? waitpid(child, &status, 0) : -1;
  return child > 0 && waited == child && status == (37 << 8) ? 0 : 1 | (unsigned long)status << 8;
}

static unsigned long exec_source_version(void) {
  long child = fork();
  if (child == 0) {
    // Keep the vector and its string on one page so the validation hook can
    // publish a distinct version after exec has read its pathname.
    static struct {
      const char *argv[2];
      char name[8];
    } source __attribute__((aligned(4096)));
    source.name[0] = 'e';
    source.name[1] = 'x';
    source.name[2] = 'e';
    source.name[3] = 'c';
    source.name[4] = 0;
    source.argv[0] = source.name;
    source.argv[1] = 0;
    syscall3(SYS_EXECVE, (long)"/validation_child.elf", (long)source.argv, 0);
    _exit(98);
  }
  int status = 0;
  long waited = child > 0 ? waitpid(child, &status, 0) : -1;
  return child > 0 && waited == child && status == (37 << 8) ? 0 : 1 | (unsigned long)status << 8;
}

static unsigned long exec_shared_thread_gate(void) {
  enum { EXEC_REGISTER_DORMANT_PEER = 52 };
  long child = fork();
  if (child == 0) {
    if (control(EXEC_REGISTER_DORMANT_PEER, 0, 0) != 1) {
      _exit(98);
    }
    const char *args[] = {"must-reject", 0};
    long result = syscall3(SYS_EXECVE, (long)"/validation_child.elf", (long)args, 0);
    _exit(result == -11 ? 39 : 98);
  }
  int status = 0;
  long waited = child > 0 ? waitpid(child, &status, 0) : -1;
  return child > 0 && waited == child && status == (39 << 8) ? 0 : 1 | (unsigned long)status << 8;
}

static unsigned long exec_registration_gate(void) {
  long child = fork();
  if (child == 0) {
    const char *args[] = {"exec", 0};
    syscall3(SYS_EXECVE, (long)"/validation_child.elf", (long)args, 0);
    _exit(98);
  }
  int status = 0;
  long waited = child > 0 ? waitpid(child, &status, 0) : -1;
  return child > 0 && waited == child && status == (37 << 8) ? 0 : 1 | (unsigned long)status << 8;
}

// Control 12/13 exhausts/releases physical pages while the writable test VMA
// remains valid but absent: a copy must contain the allocation fault as EFAULT.
static unsigned long uaccess_allocation_fault(void) {
  unsigned long now = 0;
  unsigned long errors = syscall2(SYS_CLOCK_GETTIME, 0, (long)&now) != 0;
  long area = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
  if (area <= 0) {
    return errors | 2;
  }
  // Keep this valid, writable VMA absent while the real allocator is exhausted.
  if (control(12, area, 0)) {
    long input = sigprocmask(SIG_BLOCK, (const unsigned long *)area, 0);
    long result = syscall2(SYS_CLOCK_GETTIME, 0, area);
    control(13, area, 0);
    errors |= (unsigned long)(input != -14) << 7;
    errors |= (unsigned long)(result != -14) << 2;
    // Failure must preserve the process and mapping: retry after recovery.
    errors |= (unsigned long)(syscall2(SYS_CLOCK_GETTIME, 0, area) != 0) << 3;
    errors |= (unsigned long)(*(volatile unsigned long *)area == 0) << 4;
  } else {
    errors |= 32;
  }
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 4096) != 0) << 6;
  return errors;
}

static unsigned long uaccess_read_fault(void) {
  long fd = open("/fixture.bin", 0);
  if (fd < 0) {
    return 1;
  }
  unsigned char warm = 0xff;
  unsigned long errors = read((int)fd, &warm, 1) != 1 || warm != 0;
  long area = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
  if (area <= 0) {
    close((int)fd);
    return errors | 2;
  }
  if (control(12, area, 0)) {
    long result = read((int)fd, (void *)area, 1);
    control(13, area, 0);
    errors |= (unsigned long)(result != -14) << 2;
  } else {
    errors |= 8;
  }
  // No byte reached userspace, so ramfs must not have advanced its position.
  errors |= (unsigned long)(syscall3(SYS_LSEEK, fd, 0, 1) != 1) << 9;
  errors |= (unsigned long)(syscall3(SYS_LSEEK, fd, 7, 0) != 7) << 4;
  errors |= (unsigned long)(read((int)fd, (void *)area, 1) != 1) << 5;
  errors |= (unsigned long)(*(volatile unsigned char *)area != 7) << 6;
  errors |= (unsigned long)(close((int)fd) != 0) << 7;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 4096) != 0) << 8;
  return errors;
}

static unsigned long uaccess_write_fault(void) {
  long fds[2] = {-1, -1};
  if (pipe(fds) != 0) {
    return 1;
  }
  unsigned char byte = 23;
  unsigned long errors = write(fds[1], &byte, 1) != 1 || read(fds[0], &byte, 1) != 1 || byte != 23;
  long area = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
  if (area <= 0) {
    close(fds[0]);
    close(fds[1]);
    return errors | 2;
  }
  if (control(12, area, 0)) {
    long result = write(fds[1], (const void *)area, 1);
    control(13, area, 0);
    errors |= (unsigned long)(result != -14) << 2;
  } else {
    errors |= 8;
  }
  *(volatile unsigned char *)area = 37;
  errors |= (unsigned long)(write(fds[1], (const void *)area, 1) != 1) << 4;
  errors |= (unsigned long)(read(fds[0], &byte, 1) != 1 || byte != 37) << 5;
  errors |= (unsigned long)(close(fds[0]) != 0) << 6;
  errors |= (unsigned long)(close(fds[1]) != 0) << 6;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 4096) != 0) << 7;
  return errors;
}

// The partial-copy fixtures start at page byte 4092 (=4096-4), request eight
// bytes, and deny the second page. Exactly four bytes may commit; retries must
// preserve the file/ring cursor and the unread suffix rather than lose data.
static unsigned long uaccess_partial_read(void) {
  long area = syscall6(SYS_MMAP, 0, 8192, 3, 0x22, -1, 0);
  if (area <= 0) {
    return 1;
  }
  volatile unsigned char *data = (volatile unsigned char *)(area + 4092);
  for (unsigned i = 0; i < 4; ++i) {
    data[i] = 0xff;
  }
  long fd = open("/fixture.bin", 0);
  if (fd < 0) {
    syscall2(SYS_MUNMAP, area, 8192);
    return 2;
  }
  unsigned long errors = 0;
  if (control(12, area + 4096, 0)) {
    long result = read((int)fd, (void *)data, 8);
    errors |= (unsigned long)!control(14, (long)data, 0) << 6;
    control(13, area + 4096, 0);
    errors |= (unsigned long)(result != 4) << 2;
  } else {
    errors |= 8;
  }
  errors |= (unsigned long)(syscall3(SYS_LSEEK, fd, 0, 1) != 4) << 4;
  for (unsigned i = 0; i < 4; ++i) {
    errors |= (unsigned long)(data[i] != i) << 5;
  }
  errors |= (unsigned long)(read((int)fd, (void *)(area + 4096), 4) != 4) << 7;
  for (unsigned i = 0; i < 4; ++i) {
    errors |= (unsigned long)(data[4 + i] != 4 + i) << 8;
  }
  errors |= (unsigned long)(close((int)fd) != 0) << 9;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 8192) != 0) << 9;
  return errors;
}

static unsigned long uaccess_partial_write(void) {
  long area = syscall6(SYS_MMAP, 0, 8192, 3, 0x22, -1, 0);
  if (area <= 0) {
    return 1;
  }
  volatile unsigned char *data = (volatile unsigned char *)(area + 4092);
  for (unsigned i = 0; i < 4; ++i) {
    data[i] = (unsigned char)i;
  }
  long fds[2];
  if (pipe(fds) != 0) {
    syscall2(SYS_MUNMAP, area, 8192);
    return 2;
  }
  unsigned char bytes[8] = {0xff};
  unsigned long errors = 0;
  if (control(12, area + 4096, 0)) {
    long result = write(fds[1], (const void *)data, 8);
    control(13, area + 4096, 0);
    errors |= (unsigned long)(result != 4) << 2;
  } else {
    errors |= 8;
  }
  errors |= (unsigned long)(read(fds[0], bytes, 8) != 4) << 4;
  for (unsigned i = 0; i < 4; ++i) {
    errors |= (unsigned long)(bytes[i] != i) << 5;
  }
  errors |= (unsigned long)(close(fds[1]) != 0) << 7;
  errors |= (unsigned long)(read(fds[0], bytes, 1) != 0) << 6;
  errors |= (unsigned long)(close(fds[0]) != 0) << 7;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 8192) != 0) << 7;
  return errors;
}

static unsigned long uaccess_partial_pipe_read(void) {
  long area = syscall6(SYS_MMAP, 0, 8192, 3, 0x22, -1, 0);
  if (area <= 0) {
    return 1;
  }
  volatile unsigned char *data = (volatile unsigned char *)(area + 4092);
  for (unsigned i = 0; i < 4; ++i) {
    data[i] = 0xff;
  }
  long fds[2];
  if (pipe(fds) != 0) {
    syscall2(SYS_MUNMAP, area, 8192);
    return 2;
  }
  unsigned char bytes[8];
  for (unsigned i = 0; i < 8; ++i) {
    bytes[i] = (unsigned char)(80 + i);
  }
  unsigned long errors = (unsigned long)(write(fds[1], bytes, 8) != 8) << 2;
  if (control(12, area + 4096, 0)) {
    long result = read(fds[0], (void *)data, 8);
    control(13, area + 4096, 0);
    errors |= (unsigned long)(result != 4) << 3;
  } else {
    errors |= 16;
  }
  errors |= (unsigned long)(read(fds[0], bytes, 8) != 4) << 5;
  for (unsigned i = 0; i < 4; ++i) {
    errors |= (unsigned long)(data[i] != 80 + i || bytes[i] != 84 + i) << 6;
  }
  errors |= (unsigned long)(close(fds[1]) != 0) << 8;
  errors |= (unsigned long)(read(fds[0], bytes, 1) != 0) << 7;
  errors |= (unsigned long)(close(fds[0]) != 0) << 8;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 8192) != 0) << 8;
  return errors;
}

static void unexpected_sigframe_handler(int signo) {
  (void)signo;
  _exit(91); // A handler cannot run on an absent stack with no free pages.
}

static unsigned long uaccess_sigframe_fault(void) {
  long area = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
  if (area <= 0) {
    return 1;
  }
  long gate[2];
  if (pipe(gate) != 0) {
    syscall2(SYS_MUNMAP, area, 4096);
    return 2;
  }
  int status = 0;
  long child = fork();
  if (child == 0) {
    unsigned char ready = 0;
    while (read(gate[0], &ready, 1) == 0) {
      sched_yield();
    }
    close(gate[0]);
    close(gate[1]);
    struct sigaction_t action = {(unsigned long)unexpected_sigframe_handler, 0, SA_ONSTACK};
    struct stack_t stack = {(unsigned long)area, 4096, 0};
    if (ready != 1 || moss_sigaction(SIGUSR1, &action, 0) != 0 || sigaltstack(&stack, 0) != 0) {
      _exit(92);
    }
    if (!control(12, area, 0)) {
      _exit(93);
    }
    kill(getpid(), SIGUSR1);
    _exit(94);
  }
  // Make the parent's wait destination private/resident before the child drains
  // memory. The pipe gate avoids depending on which fork continuation runs first.
  status = (int)getpid();
  unsigned char ready = 1;
  unsigned long errors = child <= 0 || write(gate[1], &ready, 1) != 1;
  errors |= (unsigned long)(close(gate[0]) != 0) << 1;
  errors |= (unsigned long)(close(gate[1]) != 0) << 1;
  if (child > 0) {
    long waited = waitpid(child, &status, 0);
    control(13, area, 1);
    // The signal checkpoint currently uses an exit code of 128 + signo.
    errors |= (unsigned long)(waited != child || ((status >> 8) & 255) != 128 + SIGUSR1) << 2;
  }
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 4096) != 0) << 3;
  return errors;
}

long uaccess_sigreturn_probe(unsigned long user_sp);
static unsigned long uaccess_sigreturn_fault(void) {
  long area = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
  if (area <= 0) {
    return 1;
  }
  unsigned long errors = 0;
  if (control(12, area, 0)) {
    long result = uaccess_sigreturn_probe((unsigned long)area);
    control(13, area, 0);
    errors |= (unsigned long)(result != -14) << 1;
  } else {
    errors |= 4;
  }
  // Demand paging works again, but an all-zero frame is still invalid.
  errors |= (unsigned long)(uaccess_sigreturn_probe((unsigned long)area) != -14) << 3;
  errors |= (unsigned long)(getpid() != 1) << 4;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 4096) != 0) << 5;
  return errors;
}

static unsigned long uaccess_devices(void) {
  long zero = open("/dev/zero", 0);
  long null = open("/dev/null", 2);
  long console = open("/dev/console", 1);
  long area = syscall6(SYS_MMAP, 0, 8192, 3, 0x22, -1, 0);
  if (zero < 0 || null < 0 || console < 0 || area <= 0) {
    if (zero >= 0) {
      close((int)zero);
    }
    if (null >= 0) {
      close((int)null);
    }
    if (console >= 0) {
      close((int)console);
    }
    if (area > 0) {
      syscall2(SYS_MUNMAP, area, 8192);
    }
    return 1;
  }
  // 301 crosses two complete 128-byte /dev/zero chunks and a partial third;
  // a nonzero 0xa5 fill makes incomplete zeroing observable.
  unsigned char bytes[301];
  for (unsigned i = 0; i < sizeof(bytes); ++i) {
    bytes[i] = 0xa5;
  }
  unsigned long errors = (unsigned long)(read((int)zero, bytes, sizeof(bytes)) != sizeof(bytes)) << 1;
  for (unsigned i = 0; i < sizeof(bytes); ++i) {
    errors |= (unsigned long)(bytes[i] != 0) << 2;
  }
  bytes[0] = 0xa5;
  errors |= (unsigned long)(read((int)null, bytes, 1) != 0 || bytes[0] != 0xa5) << 3;
  errors |= (unsigned long)(write((int)console, "\n", 1) != 1) << 4;
  volatile unsigned char *data = (volatile unsigned char *)(area + 4092);
  for (unsigned i = 0; i < 4; ++i) {
    data[i] = 0xa5;
  }
  if (control(12, area + 4096, 0)) {
    errors |= (unsigned long)(read((int)zero, (void *)(area + 4096), 1) != -14) << 5;
    errors |= (unsigned long)(write((int)console, (const void *)(area + 4096), 1) != -14) << 6;
    // /dev/null deliberately does not touch valid, nonresident user storage.
    errors |= (unsigned long)(write((int)null, (const void *)(area + 4096), 8) != 8) << 7;
    errors |= (unsigned long)(read((int)null, (void *)(area + 4096), 8) != 0) << 8;
    errors |= (unsigned long)(read((int)zero, (void *)data, 8) != 4) << 9;
    control(13, area + 4096, 0);
    for (unsigned i = 0; i < 4; ++i) {
      errors |= (unsigned long)(data[i] != 0) << 10;
    }
  } else {
    errors |= 2048;
  }
  errors |= (unsigned long)(read((int)zero, (void *)(area + 4096), 8) != 8) << 12;
  errors |= (unsigned long)(close((int)zero) != 0) << 13;
  errors |= (unsigned long)(close((int)null) != 0) << 13;
  errors |= (unsigned long)(close((int)console) != 0) << 13;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 8192) != 0) << 13;
  return errors;
}

static unsigned long uaccess_cow_fault(int kind) {
  // Controls 52..57 follow isolation's 50/51 and match CowAllocationPressure.
  enum { COW_BEGIN = 52, COW_ARM, COW_CHECK, COW_RELEASE, COW_SPLIT, COW_FINISH };
  enum { COPY_FAULT, PARTIAL_READ, USER_FAULT };
  // Two resident pages give a private writable prefix followed by shared COW
  // storage. A nonzero pattern distinguishes preserved bytes from zero fill.
  const long page_bytes = 4096;
  const unsigned char pattern = 0xa5;
  long area = syscall6(SYS_MMAP, 0, page_bytes * 2, 3, 0x22, -1, 0);
  if (area <= 0) {
    return 1;
  }
  volatile unsigned char *bytes = (volatile unsigned char *)area;
  for (long i = 0; i < page_bytes * 2; ++i) {
    bytes[i] = pattern;
  }
  long gate[2];
  if (pipe(gate) != 0) {
    syscall2(SYS_MUNMAP, area, page_bytes * 2);
    return 2;
  }
  long fd = open("/fixture.bin", 0);
  unsigned char warm = 0;
  unsigned long errors = fd < 0 || read((int)fd, &warm, 1) != 1 || syscall3(SYS_LSEEK, fd, 0, 0) != 0;
  const long target = area + page_bytes;
  if (errors || !control(COW_BEGIN, target, 0)) {
    close((int)fd);
    close((int)gate[0]);
    close((int)gate[1]);
    syscall2(SYS_MUNMAP, area, page_bytes * 2);
    return 4;
  }
  int status = 0;
  long child = fork();
  if (child == 0) {
    unsigned char ready = 0;
    while (read((int)gate[0], &ready, 1) == 0) {
      sched_yield();
    }
    close((int)gate[0]);
    close((int)gate[1]);
    bytes[0] = pattern; // Resolve only the prefix page's COW before pressure.
    // Warm the child's output stack, including syscall-call frames, before
    // draining memory. The target remains shared and is only read here.
    unsigned long now = 0;
    errors = ready != 1 || syscall2(SYS_CLOCK_GETTIME, 0, (long)&now) != 0 || bytes[page_bytes] != pattern;
    if (errors || !control(COW_ARM, target, 0)) {
      _exit(93); // Fixture preparation failed, not the expected fatal store.
    }
    if (kind == USER_FAULT) {
      *(volatile unsigned char *)target = 0;
      _exit(94); // A successful store must never pass as an isolated fault.
    }
    // The eight-byte read crosses four bytes on either side of the boundary.
    const long prefix = kind == PARTIAL_READ ? 4 : 0;
    if (kind == COPY_FAULT) {
      errors |= (unsigned long)(syscall2(SYS_CLOCK_GETTIME, 0, target) != -14) << 1;
    }
    long result = read((int)fd, (void *)(target - prefix), 8);
    errors |= (unsigned long)(result != (prefix ? prefix : -14)) << 2;
    errors |= (unsigned long)(syscall3(SYS_LSEEK, fd, 0, 1) != prefix) << 3;
    errors |= (unsigned long)!control(COW_CHECK, target, 0) << 4;
    errors |= (unsigned long)!control(COW_RELEASE, target, 0) << 5;
    const long remaining = 8 - prefix;
    errors |= (unsigned long)(read((int)fd, (void *)target, remaining) != remaining) << 6;
    for (long i = 0; i < prefix; ++i) {
      errors |= (unsigned long)(bytes[page_bytes - prefix + i] != (unsigned char)i) << 7;
    }
    for (long i = 0; i < page_bytes; ++i) {
      const unsigned char expected = i < remaining ? (unsigned char)(prefix + i) : pattern;
      errors |= (unsigned long)(bytes[page_bytes + i] != expected) << 8;
    }
    errors |= (unsigned long)!control(COW_SPLIT, target, 0) << 9;
    _exit(errors ? 98 : 37);
  }
  // A pipe gate makes the parent's wait destination private before the child
  // can exhaust PFA. It does not rely on which fork continuation runs first.
  status = (int)getpid();
  unsigned char ready = 1;
  errors |= (unsigned long)(child <= 0 || write((int)gate[1], &ready, 1) != 1) << 1;
  long waited = child > 0 ? waitpid(child, &status, 0) : -1;
  errors |= (unsigned long)!control(COW_FINISH, target, 0) << 2;
  // The native fault path exits with -SIGSEGV; wait encodes its low byte.
  const int expected_exit = kind == USER_FAULT ? ((-SIGSEGV) & 255) : 37;
  errors |= (unsigned long)(waited != child || ((status >> 8) & 255) != expected_exit) << 3;
  for (long i = 0; i < page_bytes * 2; ++i) {
    errors |= (unsigned long)(bytes[i] != pattern) << 4;
  }
  // The survivor must retain ordinary access after its peer failed or split.
  bytes[page_bytes] = 0;
  errors |= (unsigned long)(bytes[page_bytes] != 0) << 5;
  errors |= (unsigned long)(close((int)fd) != 0) << 6;
  errors |= (unsigned long)(close((int)gate[0]) != 0) << 7;
  errors |= (unsigned long)(close((int)gate[1]) != 0) << 7;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, page_bytes * 2) != 0) << 8;
  return errors;
}

// Ordinals select the three cases in uaccess_cow_fault's local protocol enum.
static unsigned long uaccess_cow_copy_fault(void) { return uaccess_cow_fault(0); }
static unsigned long uaccess_cow_partial_read(void) { return uaccess_cow_fault(1); }
static unsigned long uaccess_cow_user_fault(void) { return uaccess_cow_fault(2); }

long frame_register_probe(long number, long signal_pid);
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
static const unsigned char vm_rodata[4096] __attribute__((aligned(4096))) = {0x5a};
__attribute__((noinline)) static void vm_text(void) { asm volatile("" ::: "memory"); }

static int wait_exit(long child, int code) {
  int status = 0;
  return child > 1 && syscall3(SYS_WAITPID, child, (long)&status, 0) == child && ((status >> 8) & 255) == code;
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

enum vm_fault_access {
  VM_FAULT_READ,
  VM_FAULT_WRITE,
  VM_FAULT_EXECUTE,
};

static int vm_fault(long address, enum vm_fault_access access) {
  long child = syscall0(SYS_FORK);
  if (child == 0) {
    if (access == VM_FAULT_EXECUTE) {
      ((void (*)(void))address)();
    } else if (access == VM_FAULT_WRITE) {
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
  return wait_exit(child, 245); // (-SIGSEGV = -11) & 0xff = 245 in Moss's exit-code encoding.
}

static unsigned long vm_private_cow(void) {
  long area = syscall6(SYS_MMAP, 0, 8192, 3, 0x22, -1, 0);
  if (area <= 0) {
    return 1;
  }
  volatile unsigned char *data = (volatile unsigned char *)area;
  unsigned long errors = 0;
  for (unsigned i = 0; i < 8192; ++i) {
    errors |= (unsigned long)(data[i] != 0) << 4;
  }
  data[0] = 11;
  data[4096] = 22;
  data[8191] = 33;
  long child = syscall0(SYS_FORK);
  if (child == 0) {
    if (data[0] != 11 || data[4096] != 22 || data[8191] != 33) {
      _exit(91);
    }
    data[0] = 44;
    long grandchild = syscall0(SYS_FORK);
    if (grandchild == 0) {
      data[4096] = 55;
      _exit(data[0] == 44 && data[4096] == 55 && data[8191] == 33 ? 31 : 92);
    }
    _exit(wait_exit(grandchild, 31) && data[0] == 44 && data[4096] == 22 && data[8191] == 33 ? 33 : 93);
  }
  errors |= !wait_exit(child, 33);
  errors |= (unsigned long)!(data[0] == 11 && data[4096] == 22 && data[8191] == 33) << 1;
  data[0] = 77; // Last-reference COW after descendants have exited.
  errors |= (unsigned long)(data[0] != 77) << 2;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 8192) != 0) << 3;
  return errors;
}

static unsigned long vm_readonly_cow(void) {
  unsigned long errors = *(const volatile unsigned char *)vm_rodata != 0x5a;
  vm_text(); // Both mappings are resident before fork.
  errors |= (unsigned long)!vm_fault((long)vm_rodata, VM_FAULT_WRITE) << 1;
  errors |= (unsigned long)!vm_fault((long)vm_text, VM_FAULT_WRITE) << 2;
  errors |= (unsigned long)(*(const volatile unsigned char *)vm_rodata != 0x5a) << 3;
  return errors;
}

static unsigned long vm_access_permissions(void) {
  long none = syscall6(SYS_MMAP, 0, 4096, 0, 0x22, -1, 0);
  long nx = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
  unsigned long errors = none <= 0 || nx <= 0;
  if (none > 0) {
    errors |= (unsigned long)!vm_fault(none, VM_FAULT_READ) << 1;
    errors |= (unsigned long)!vm_fault(none, VM_FAULT_WRITE) << 2;
    errors |= (unsigned long)(syscall2(SYS_MUNMAP, none, 4096) != 0) << 3;
  }
  if (nx > 0) {
    // Keep this page absent: an instruction miss must check EXEC before any
    // demand allocation. Otherwise it can allocate then fault indefinitely.
    errors |= (unsigned long)!vm_fault(nx, VM_FAULT_EXECUTE) << 4;
    errors |= (unsigned long)(syscall2(SYS_MUNMAP, nx, 4096) != 0) << 5;
  }
  return errors;
}

static unsigned long vm_kernel_isolation(long target) {
  // Private validation controls 50/51 prepare an actual kernel mapping and
  // check its contents after the child is reaped; keep validation.cpp in sync.
  enum { ISOLATION_PREPARE = 50, ISOLATION_VERIFY = 51 };
  // One native base page proves legal user loads/stores survive every attack.
  const long page_bytes = 4096;
  long area = syscall6(SYS_MMAP, 0, page_bytes, 3, 0x22, -1, 0);
  if (area <= 0) {
    return 1;
  }
  volatile unsigned *user = (volatile unsigned *)area;
  const unsigned canary = 0x5a39c681; // Nonzero mixed bytes expose clobbering and demand-zero replacement.
  *user = canary;
  const long parent = getpid();
  unsigned long errors = 0;
  // Exercise both the identity mapping and its high direct-map alias. Each
  // operation gets a separate child so a rejected read cannot hide an allowed write.
  for (long alias = 0; alias < 2; ++alias) {
    for (long write = 0; write < 2; ++write) {
      long address = control(ISOLATION_PREPARE, target, alias);
      if (!address) {
        errors |= 1;
        continue;
      }
      long child = fork();
      if (child == 0) {
        // Use aligned 32-bit accesses for the interrupt-controller registers
        // as well as RAM. Inline assembly keeps forbidden writes to text/RO
        // objects from being optimized away as undefined C behavior.
        if (write) {
#if defined(__aarch64__)
          asm volatile("str wzr, [%0]" : : "r"(address) : "memory");
#elif defined(__x86_64__)
          asm volatile("movl $0, (%0)" : : "r"(address) : "memory");
#else
          asm volatile("sw zero, 0(%0)" : : "r"(address) : "memory");
#endif
        } else {
          unsigned value = *(volatile unsigned *)address;
          asm volatile("" : : "r"(value) : "memory");
        }
        _exit(94); // A completed forbidden access must never share the fault exit marker.
      }
      // Moss currently encodes fatal page faults as (-SIGSEGV)&255, not the
      // POSIX wait signal encoding. The exact code rejects unrelated exits.
      errors |= (unsigned long)!wait_exit(child, 245) << 1;
      errors |= (unsigned long)!control(ISOLATION_VERIFY, target, alias) << 2;
      errors |= (unsigned long)(getpid() != parent || *user != canary) << 3;
      *user = canary ^ 1U;
      errors |= (unsigned long)(*user != (canary ^ 1U)) << 4;
      *user = canary;
    }
  }
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, page_bytes) != 0) << 5;
  return errors;
}

static unsigned long vm_brk_lifecycle(void) {
  // Moss exposes 4 KiB user pages; three pages make one fully released page
  // remain after shrinking to a deliberately non-page-aligned break.
  enum {
    PAGE_BYTES = 4096,
    GROW_PAGES = 3,
    // 37 is an arbitrary non-power-of-two offset; any value in this page would
    // exercise the same partial-page ABI without resembling an alignment.
    PARTIAL_BYTES = 37,
  };
  // Page four leaves one unmapped guard page after the three-page heap. Growing
  // through page five must therefore collide with, rather than skip, the mmap.
  enum { COLLISION_PAGE = 4, COLLISION_GROW_PAGES = 5 };
  enum {
    MMAP_PROT_READ = 1U << 0,
    MMAP_PROT_WRITE = 1U << 1,
    MAP_PRIVATE_ANONYMOUS = 0x22, // Moss's supported MAP_PRIVATE | MAP_ANONYMOUS pair.
  };
  // Complementary nonzero bytes distinguish retained heap data, mmap data and
  // the zero-fill required after a fully released page is grown again.
  enum { STALE_PATTERN = 0x5a, COLLISION_PATTERN = 0xa5 };
  // One bit per observation keeps the serial failure mask independently decodable.
  enum {
    ERR_BASE = 1UL << 0,
    ERR_INITIAL_VISIBLE = 1UL << 1,
    ERR_GROW = 1UL << 2,
    ERR_SHRINK = 1UL << 3,
    ERR_RELEASED_VISIBLE = 1UL << 4,
    ERR_REGROW = 1UL << 5,
    ERR_STALE_CONTENT = 1UL << 6,
    ERR_COLLISION_MAP = 1UL << 7,
    ERR_COLLISION_GROW = 1UL << 8,
    ERR_PARTIAL_COMMIT = 1UL << 9,
    ERR_COLLISION_CONTENT = 1UL << 10,
    ERR_GAP_VISIBLE = 1UL << 11,
    ERR_COLLISION_UNMAP = 1UL << 12,
    ERR_RESET = 1UL << 13,
    ERR_RESET_VISIBLE = 1UL << 14,
    ERR_FINAL_REGROW = 1UL << 15,
    ERR_FINAL_CONTENT = 1UL << 16,
    ERR_FINAL_RESET = 1UL << 17,
  };

  const long base = syscall1(SYS_BRK, 0);
  if (base <= 0 || (base & (PAGE_BYTES - 1)) != 0) {
    return ERR_BASE;
  }

  unsigned long errors = (unsigned long)!vm_fault(base, VM_FAULT_READ) * ERR_INITIAL_VISIBLE;
  const long grown = base + (long)GROW_PAGES * PAGE_BYTES;
  if (syscall1(SYS_BRK, grown) != grown) {
    return errors | ERR_GROW;
  }
  volatile unsigned char *const first = (volatile unsigned char *)base;
  // The third page is wholly beyond the partially retained second page.
  volatile unsigned char *const released = (volatile unsigned char *)(base + 2L * PAGE_BYTES);
  *first = STALE_PATTERN;
  *released = STALE_PATTERN;

  const long partial = base + PAGE_BYTES + PARTIAL_BYTES;
  errors |= (unsigned long)(syscall1(SYS_BRK, partial) != partial) * ERR_SHRINK;
  errors |= (unsigned long)!vm_fault((long)released, VM_FAULT_READ) * ERR_RELEASED_VISIBLE;
  errors |= (unsigned long)(syscall1(SYS_BRK, grown) != grown) * ERR_REGROW;
  errors |= (unsigned long)(*released != 0) * ERR_STALE_CONTENT;

  const long collision_address = base + (long)COLLISION_PAGE * PAGE_BYTES;
  long collision =
      syscall6(SYS_MMAP, collision_address, PAGE_BYTES, MMAP_PROT_READ | MMAP_PROT_WRITE, MAP_PRIVATE_ANONYMOUS, -1, 0);
  errors |= (unsigned long)(collision != collision_address) * ERR_COLLISION_MAP;
  if (collision == collision_address) {
    volatile unsigned char *const collision_byte = (volatile unsigned char *)collision;
    *collision_byte = COLLISION_PATTERN;
    const long rejected = base + (long)COLLISION_GROW_PAGES * PAGE_BYTES;
    errors |= (unsigned long)(syscall1(SYS_BRK, rejected) != grown) * ERR_COLLISION_GROW;
    errors |= (unsigned long)(syscall1(SYS_BRK, 0) != grown) * ERR_PARTIAL_COMMIT;
    errors |= (unsigned long)(*collision_byte != COLLISION_PATTERN) * ERR_COLLISION_CONTENT;
    errors |= (unsigned long)!vm_fault(base + (long)GROW_PAGES * PAGE_BYTES, VM_FAULT_READ) * ERR_GAP_VISIBLE;
    errors |= (unsigned long)(syscall2(SYS_MUNMAP, collision, PAGE_BYTES) != 0) * ERR_COLLISION_UNMAP;
  } else if (collision > 0) {
    (void)syscall2(SYS_MUNMAP, collision, PAGE_BYTES);
  }

  errors |= (unsigned long)(syscall1(SYS_BRK, base) != base) * ERR_RESET;
  errors |= (unsigned long)!vm_fault(base, VM_FAULT_READ) * ERR_RESET_VISIBLE;
  const long one_page = base + PAGE_BYTES;
  errors |= (unsigned long)(syscall1(SYS_BRK, one_page) != one_page) * ERR_FINAL_REGROW;
  errors |= (unsigned long)(*first != 0) * ERR_FINAL_CONTENT;
  errors |= (unsigned long)(syscall1(SYS_BRK, base) != base) * ERR_FINAL_RESET;
  return errors;
}

static int decimal_equal(const char *actual, const char *expected) {
  while (*actual && *actual == *expected) {
    ++actual;
    ++expected;
  }
  return *actual == *expected;
}

static unsigned long numbers_regression(void) {
  // These are the 64-bit unsigned/signed boundaries, rather than arbitrary
  // canaries. Signed subtraction stays representable while constructing MIN.
  const unsigned long unsigned_max = ~0UL;
  const long signed_min = -0x7fffffffffffffffL - 1;
  const long signed_max = 0x7fffffffffffffffL;
  char buffer[MOSS_DECIMAL_BUFFER_SIZE];
  unsigned long errors =
      ultoa(unsigned_max, buffer, sizeof(buffer)) != 20 || !decimal_equal(buffer, "18446744073709551615");
  errors |=
      (unsigned long)(ltoa(signed_min, buffer, sizeof(buffer)) != 20 || !decimal_equal(buffer, "-9223372036854775808"))
      << 1;
  errors |=
      (unsigned long)(ltoa(signed_max, buffer, sizeof(buffer)) != 19 || !decimal_equal(buffer, "9223372036854775807"))
      << 2;

  // A one-byte destination admits only NUL. Nonzero neighbours catch the old
  // zero-value fast path writing two bytes and signed formatting skipping NUL.
  unsigned char one[3] = {0xa5, 0xa5, 0xa5};
  errors |= (unsigned long)(ultoa(0, (char *)one + 1, 1) != 0 || one[0] != 0xa5 || one[1] != 0 || one[2] != 0xa5) << 3;
  one[1] = 0xa5;
  errors |=
      (unsigned long)(ltoa(signed_min, (char *)one + 1, 1) != 0 || one[0] != 0xa5 || one[1] != 0 || one[2] != 0xa5)
      << 4;
  one[1] = 0xa5;
  errors |= (unsigned long)(ultoa(unsigned_max, (char *)one + 1, 0) != 0 || ltoa(signed_min, (char *)one + 1, 0) != 0 ||
                            one[1] != 0xa5)
            << 5;
  // Four bytes admit a three-character prefix and NUL, never the low digits.
  errors |= (unsigned long)(ultoa(12345, buffer, 4) != 3 || !decimal_equal(buffer, "123") ||
                            ltoa(signed_min, buffer, 4) != 3 || !decimal_equal(buffer, "-92"))
            << 6;

  long ends[2];
  if (pipe(ends) != 0) {
    return errors | (1UL << 7);
  }
  long child = fork();
  if (child == 0) {
    if (close((int)ends[0]) != 0 || dup2((int)ends[1], 1) != 1 || close((int)ends[1]) != 0) {
      _exit(99);
    }
    // Capture the real output helpers through Moss write syscalls. Width 22
    // adds two spaces to each 20-character boundary value; small width 3 also
    // covers zero/sign padding. The complete output fits one 4096-byte pipe.
    print_ulong(unsigned_max);
    print("\n");
    print_num_padded(unsigned_max, 22);
    print("\n");
    print_long(signed_min);
    print("\n");
    print_snum_padded(signed_min, 22);
    print("\n");
    print_num_padded(0, 3);
    print("\n");
    print_snum_padded(-1, 3);
    print("\n");
    _exit(37);
  }
  int failed = close((int)ends[1]) != 0 || child < 0;
  const char expected[] = "18446744073709551615\n"
                          "  18446744073709551615\n"
                          "-9223372036854775808\n"
                          "  -9223372036854775808\n"
                          "  0\n"
                          " -1\n";
  unsigned long received = 0;
  char chunk[32];
  long count;
  // Drain even mismatched output before reaping, so regressions cannot strand
  // the writer behind pipe backpressure. The chunk size is a bounded stack choice.
  while ((count = read((int)ends[0], chunk, sizeof(chunk))) > 0) {
    for (long i = 0; i < count; ++i, ++received) {
      failed |= received >= sizeof(expected) - 1 || chunk[i] != expected[received];
    }
  }
  failed |= count < 0 || received != sizeof(expected) - 1;
  failed |= close((int)ends[0]) != 0;
  if (child > 0) {
    failed |= !wait_exit(child, 37);
  }
  return errors | ((unsigned long)(failed != 0) << 7);
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
  // 2^47 is the exclusive user limit; 0x1000 lies in inherited kernel mappings.
  // Near-end and negative ranges test wrap/overflow. 0x180000000 (6 GiB) is
  // the kernel-installed SIGRETURN_PAGE, which mmap/munmap must preserve.
  // 0x32 adds unsupported MAP_FIXED, and prot bit 8 is outside R/W/X.
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
  char too_long[1024]; // Native MAX_PATH_LEN bytes, with no terminating NUL.
  for (unsigned i = 0; i < sizeof(too_long); ++i) {
    too_long[i] = 'a';
  }
  CHECK_RANGE(syscall3(SYS_OPEN, (long)too_long, 0, 0) == -36);
  long first = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
  CHECK_RANGE(first > 0);
  if (first > 0) {
    long second = syscall6(SYS_MMAP, first + 4096, 4096, 3, 0x22, -1, 0);
    CHECK_RANGE(second == first + 4096);
    if (second == first + 4096) {
      // Page offsets 4094/4092/4093 leave 2/4/3 bytes respectively in the
      // first VMA, forcing the u32 mask, u64 clock and path to cross into the next.
      unsigned char *mask = (unsigned char *)(first + 4094);
      mask[0] = 1;
      mask[1] = 0;
      mask[2] = 0;
      mask[3] = 0;
      CHECK_RANGE(syscall3(20, 0, 4, (long)mask) == 0);                  // copy_from_user across VMAs
      CHECK_RANGE(syscall3(SYS_CLOCK_GETTIME, 0, first + 4092, 0) == 0); // Moss writes one u64, crossing VMAs
      const char name[] = "/missing-range-test";
      char *path = (char *)(first + 4093);
      for (unsigned i = 0; i < sizeof(name); ++i) {
        path[i] = name[i];
      }
      CHECK_RANGE(syscall3(SYS_OPEN, (long)path, 0, 0) == -2); // string spans two VMAs
    }
    if (second > 0) {
      CHECK_RANGE(syscall2(SYS_MUNMAP, second, 4096) == 0);
    }
    CHECK_RANGE(syscall3(SYS_CLOCK_GETTIME, 0, first + 4092, 0) == -14); // second VMA now absent
    long collision = syscall6(SYS_MMAP, first, 4096, 3, 0x22, -1, 0);
    CHECK_RANGE(collision > 0 && collision != first);
    if (collision > 0 && collision != first) {
      CHECK_RANGE(syscall2(SYS_MUNMAP, collision, 4096) == 0);
    }
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

static volatile unsigned long long benchmark_handler_tick;
static volatile unsigned long benchmark_handler_calls;

static void benchmark_signal_handler(int signo) {
  benchmark_handler_tick = counter();
  if (signo == SIGUSR1) {
    ++benchmark_handler_calls;
  }
}

static void user_benchmark(long mode) {
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
  // Moss currently encodes fatal exceptions as negative exit codes, not POSIX signals.
  return child > 1 && waited == child && ((status >> 8) & 255) == 248; // (-SIGFPE = -8) & 255.
}
#endif

// Two milliseconds (2,000,000 ns) is a short nonzero timer workload, not a
// latency budget; assertions reject early return but allow scheduling overshoot.
static unsigned long timer_relative_sleep(void) {
  unsigned long before = 0, after = 0, duration = 2000000;
  unsigned long errors = clock_gettime_ns(&before) != 0;
  errors |= (unsigned long)(nanosleep_ns(&duration) != 0) << 1;
  errors |= (unsigned long)(clock_gettime_ns(&after) != 0) << 2;
  errors |= (unsigned long)(after < before || after - before < duration) << 3;
  return errors;
}

static unsigned long timer_absolute_sleep(void) {
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

static unsigned long timer_invalid_arguments(void) {
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
  struct sigaction_t action = {(unsigned long)timer_signal_handler, 0, 0};
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

static unsigned long timer_relative_interrupted(void) { return timer_signal_interrupted(0); }
static unsigned long timer_clock_relative_interrupted(void) { return timer_signal_interrupted(1); }
static unsigned long timer_clock_absolute_interrupted(void) { return timer_signal_interrupted(2); }

static unsigned long timer_short_reuse(void) {
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

static unsigned long timer_early_wakeup(void) {
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

static unsigned long timer_arm_failure_recovery(void) {
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

static unsigned long timer_cancel_in_flight(void) {
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

static int test_wait_interrupted(void) {
  struct sigaction_t action = {(unsigned long)sigusr1_handler, 0, 0};
  handler_called = 0;
  if (moss_sigaction(SIGUSR1, &action, 0) != 0) {
    return 1;
  }
  long release[2];
  if (pipe(release) != 0) {
    return 1;
  }
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
  // EINTR must leave the still-running child's status untouched and reap it later.
  const int status_canary = 0x5a5a5a5a;
  int status = status_canary;
  long result = waitpid(child, &status, 0);
  int errors = result != -4 || status != status_canary || handler_called != 1;
  const unsigned char byte = 37;
  errors |= write((int)release[1], &byte, 1) != 1;
  close((int)release[1]);
  status = 0;
  errors |= waitpid(child, &status, 0) != child || status != (42 << 8);
  return errors;
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
  // Failed signal-frame setup currently terminates with 128 + signo. Preserve
  // that native contract, rather than accepting any unrelated child death.
  const int rejected = child > 1 && waitpid(child, &status, 0) == child && ((status >> 8) & 255) == 128 + SIGUSR2;
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
    errors |= !wait_exit(child, 128 + SIGUSR1);
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
  if (fixture != 1 && fixture != -38) {
    return 1;
  }
  const int coordinated = fixture == 1;
  long ends[2];
  if (pipe(ends) != 0) {
    return 1;
  }
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
  for (unsigned i = 0; i < sizeof(data); ++i) {
    data[i] = (unsigned char)i;
  }
  struct sigaction_t action = {(unsigned long)quiet_handler, 0, 0};
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
  errors |= result != (disposition == PIPE_CAUGHT ? -4 : 1) || handler_called != (disposition == PIPE_CAUGHT);
  errors |= current_cpu() != 0;
  if (acknowledge) {
    const unsigned char observed = 37;
    errors |= write((int)acknowledgement[1], &observed, 1) != 1;
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

static int console_signal_wait(int partial) {
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
  struct sigaction_t action = {(unsigned long)quiet_handler, 0, 0};
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
  errors |= result != (partial ? 1 : -4) || handler_called != 1;
  errors |= partial && bytes[0] != 'k';
  // Console echo has no newline; keep the next validation event on its own line.
  if (partial) {
    print("\n");
  }
  errors |= waitpid(child, &status, 0) != child || status != 0;
  errors |= close((int)fd) != 0;
  return errors;
}

static int test_console_interrupted(void) { return console_signal_wait(0); }
static int test_console_partial_interrupt(void) { return console_signal_wait(1); }

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
static int signal_case(const char *name) {
  const struct {
    const char *name;
    int (*run)(void);
  } cases[] = {{"basic_handler", test_basic_handler},
               {"nested_signals", test_nested_signals},
               {"sigchld", test_sigchld},
               {"wait_registration", test_wait_registration},
               {"wait_interrupted", test_wait_interrupted},
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
               {"pipe_noninterrupting_signals", test_pipe_noninterrupting_signals},
               {"pipe_partial_interrupt", test_pipe_partial_interrupt},
               {"signal_wakeup_affinity", test_signal_wakeup_affinity},
               {"console_interrupted", test_console_interrupted},
               {"console_partial_interrupt", test_console_partial_interrupt},
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
  } else if (mode == 8) {
    const char *cases[] = {"basic_handler",
                           "nested_signals",
                           "sigchld",
                           "wait_registration",
                           "wait_interrupted",
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
                           "pipe_noninterrupting_signals",
                           "pipe_partial_interrupt",
                           "signal_wakeup_affinity",
                           "console_interrupted",
                           "console_partial_interrupt",
                           "console_multi_reader"};
    for (unsigned test = 0; test < sizeof(cases) / sizeof(cases[0]); ++test) {
      control(1, test, 0);
      long child = fork();
      if (child == 0) {
        const char *args[] = {"validation", "signals", cases[test], 0};
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
