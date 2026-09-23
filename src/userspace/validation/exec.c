#include "validation/internal.h"

unsigned long exec_startup_capability(void) {
  enum { BAD_HANDLE = 9, ACCESS_DENIED = 13, CHILD_SUCCESS = 37 };
  struct moss_ipc_endpoints pair = {0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0)
    return 1;

  unsigned long errors = (unsigned long)(syscall2(SYS_CAP_SET_INHERIT, (long)pair.receive, 1) != 0) << 1;
  if (!errors) {
    long child = fork();
    if (child == 0) {
      const char *path = "/validation_child.elf";
      char handle_text[MOSS_DECIMAL_BUFFER_SIZE];
      (void)ultoa(pair.receive, handle_text, sizeof(handle_text));
      const char *args[] = {"startup-cap", handle_text, 0};
      // The selected handle starts closed-on-exec; execve_cap must retain it.
      if (syscall2(SYS_CAP_SET_EXEC, (long)pair.receive, 0) != 0 ||
          syscall6(SYS_EXECVE_CAP, (long)path, (long)args, 0, 0, 0, 0) != -BAD_HANDLE)
        _exit(96);
      long limited = syscall2(SYS_CAP_DUPLICATE, (long)pair.receive, MOSS_CAP_RECEIVE);
      if (limited <= 0 || syscall6(SYS_EXECVE_CAP, (long)path, (long)args, 0, limited, 0, 0) != -ACCESS_DENIED ||
          syscall1(SYS_CAP_CLOSE, limited) != 0)
        _exit(97);
      syscall6(SYS_EXECVE_CAP, (long)path, (long)args, 0, (long)pair.receive, 0, 0);
      _exit(98);
    }
    errors |= (unsigned long)(child <= 1 || !wait_exit(child, CHILD_SUCCESS)) << 2;
  }
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 3;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0) << 4;
  return errors;
}

unsigned long exec_probe(long test) {
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

unsigned long busybox_script(const char *script, const char *expected) {
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

unsigned long wait_status_rollback(void) {
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
    // 16 includes an unsupported wait option; adding 2^32 must not alias the
    // live child PID through narrowing. Status uses its 8-bit code at bit 8.
    errors |= (unsigned long)(waitpid(child, &status, 16) != -22) << 1;
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


unsigned long fork_allocation_rollback(long arm, long release, unsigned cycles) {
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

unsigned long mmap_heap_rollback(void) {
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

unsigned long exec_allocation_rollback(void) {
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

unsigned long exec_mutable_snapshot_rollback(void) {
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

unsigned long exec_boundary_load_plan(void) {
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

unsigned long exec_source_version(void) {
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

unsigned long exec_shared_thread_gate(void) {
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

unsigned long exec_registration_gate(void) {
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
