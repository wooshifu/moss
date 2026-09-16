#include "syscall.h"

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
  long child = fork();
  if (!child) {
    volatile unsigned long canary = 0x12345678;
    static const char *args[130], *environment[129];
    args[0] = "exec";
    static char bytes[16385];
    const char *path = "/validation_child.elf";
    long wanted = -8;
    if (test < 3) {
      const char *paths[] = {"/bad_entry.elf", "/bad_phentsize.elf", "/bad_load.elf"};
      path = paths[test];
    } else if (test == 3) {
      // A bad env vector must not destroy the caller's address space.
      long result = syscall3(SYS_EXECVE, (long)path, (long)args, 1);
      _exit(result == -14 && canary == 0x12345678 ? 37 : 98);
    } else if (test == 4) {
      environment[0] = (const char *)1;
      wanted = -14;
    } else if (test == 5) {
      for (unsigned i = 0; i < 129; ++i)
        args[i] = "";
      wanted = -7;
    } else if (test == 6) {
      for (unsigned i = 0; i < 128; ++i)
        environment[i] = ""; // One argv plus 128 environment strings exceeds the shared cap.
      wanted = -7;
    } else if (test == 7) {
      for (unsigned i = 0; i < 16384; ++i)
        bytes[i] = 'a';
      args[0] = bytes;
      wanted = -7;
    } else {
      path = "/libc_validation.elf";
      if (test == 8) {
        static char names[64][6];
        args[0] = "count";
        for (unsigned i = 1; i < 64; ++i)
          args[i] = "x";
        for (unsigned i = 0; i < 64; ++i) {
          names[i][0] = 'M';
          names[i][1] = (char)('0' + i / 10);
          names[i][2] = (char)('0' + i % 10);
          names[i][3] = '=';
          names[i][4] = '1';
          environment[i] = names[i];
        }
      } else if (test == 9) {
        args[0] = "bytes";
        args[1] = bytes;
        for (unsigned i = 0; i < 16377; ++i)
          bytes[i] = 'a'; // Including both terminators and argv[0], exactly 16 KiB.
      } else {
        args[0] = 0;
      }
    }
    long result = syscall3(SYS_EXECVE, (long)path, (long)args, (long)environment);
    _exit(test < 8 && result == wanted && canary == 0x12345678 ? 37 : 98);
  }
  int status = 0;
  long waited = child > 0 ? waitpid(child, &status, 0) : -1;
  int expected = test < 8 ? 37 : 39;
  return child > 0 && waited == child && status == (expected << 8) ? 0 : 1 | (unsigned long)status << 8;
}

static unsigned long busybox_script(const char *script, const char *expected) {
  long fds[2];
  if (pipe(fds))
    return 1;
  long child = fork();
  if (!child) {
    close(fds[0]);
    if (dup2(fds[1], 1) != 1)
      _exit(98);
    close(fds[1]);
    const char *args[] = {"busybox", "ash", "-c", script, 0};
    syscall3(SYS_EXECVE, (long)"/busybox.elf", (long)args, 0);
    _exit(99);
  }
  close(fds[1]);
  unsigned long errors = child < 0 ? 2 : 0;
  unsigned long length = strlen(expected), received = 0;
  char buffer[64];
  long count;
  while ((count = read(fds[0], buffer, sizeof(buffer))) > 0) {
    for (long i = 0; i < count; ++i, ++received)
      if (received >= length || buffer[i] != expected[received])
        errors |= 4;
    if (errors & 4) {
      const char *message = "BusyBox unexpected stdout chunk: ";
      write(2, message, strlen(message));
      write(2, buffer, (unsigned long)count);
    }
  }
  errors |= count < 0 || received != length ? 8 : 0;
  close(fds[0]);
  int status = 0;
  if (child > 0 && (waitpid(child, &status, 0) != child || status != (37 << 8)))
    errors |= 16 | (unsigned long)status << 8;
  return errors;
}

static unsigned long wait_status_rollback(void) {
  unsigned long errors = 0;
  for (unsigned i = 0; i < 16; ++i) {
    long child = fork();
    if (child == 0) {
      _exit(37);
    }
    if (child < 0) {
      return errors | 1;
    }
    int status = 0;
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
  for (unsigned long page = (unsigned long)__user_text_start; page < (unsigned long)__user_text_end; page += 4096)
    (void)*(const volatile unsigned char *)page;
  long fd = open("/fixture.bin", 0);
  if (fd < 0)
    return 1;
  unsigned long errors = syscall3(SYS_FCNTL, fd, 2, 1) != 0;
  volatile unsigned long canary = 0x12345678;
  for (unsigned cycle = 0; cycle < cycles && !errors; ++cycle) {
    if (!control(arm, (long)cycle, 0)) {
      errors |= 2;
      break;
    }
    long child = fork();
    if (!child)
      _exit(99);
    int status = 0;
    errors |= child != -12 ? 4 : 0;
    if (child > 0)
      waitpid(child, &status, 0);
    errors |= !control(release, 0, 0) ? 8 : 0;
    canary += 1; // The parent can still write its COW stack after rollback.
    unsigned char byte = 255;
    errors |= waitpid(-1, &status, 1) != -10 || canary != 0x12345679 + cycle ? 16 : 0;
    errors |= syscall3(SYS_FCNTL, fd, 1, 0) != 1 || read(fd, &byte, 1) != 1 || byte != cycle ? 32 : 0;
    child = fork();
    if (!child)
      _exit(syscall3(SYS_FCNTL, fd, 1, 0) == 1 ? 37 : 99);
    errors |= child <= 0 || waitpid(child, &status, 0) != child || status != (37 << 8) ? 64 : 0;
  }
  close(fd);
  return errors;
}

static unsigned long mmap_heap_rollback(void) {
  for (unsigned long page = (unsigned long)__user_text_start; page < (unsigned long)__user_text_end; page += 4096)
    (void)*(const volatile unsigned char *)page;
  unsigned long errors = 0;
  for (unsigned cycle = 0; cycle < 4 && !errors; ++cycle) {
    long original = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
    if (original < 0)
      return errors | 1;
    volatile unsigned char *byte = (volatile unsigned char *)original;
    *byte = 37;
    if (!control(42, 0, 0)) {
      syscall2(SYS_MUNMAP, original, 4096);
      return errors | 2;
    }
    long failed = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
    errors |= failed != -12 ? 4 : 0;
    errors |= !control(43, 0, 0) ? 8 : 0;
    if (failed >= 0)
      syscall2(SYS_MUNMAP, failed, 4096);
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
  volatile unsigned long canary = 0x12345678;
  const char *args[] = {"exec", 0};
  unsigned long errors = syscall3(SYS_EXECVE, (long)"/bad_entry.elf", (long)args, 0) != -8;
  long fd = open("/fixture.bin", 0);
  unsigned char byte = 255;
  if (fd < 0 || read((int)fd, &byte, 1) != 1 || byte != 0)
    return errors | 2;
  // Warm the instruction stream and copy source before exhausting real pages.
  volatile unsigned char warm = 0;
  for (const unsigned char *p = __user_text_start; p < __user_text_end; p += 4096)
    warm ^= *p;
  const char *path = "/validation_child.elf";
  for (const char *p = path; *p; ++p)
    warm ^= (unsigned char)*p;
  (void)warm;
  long stages = control(35, 0, 0);
  for (long budget = 0; budget < stages; ++budget) {
    if (!control(36, budget, 0)) {
      errors |= 4;
      break;
    }
    long result = syscall3(SYS_EXECVE, (long)path, (long)args, 0);
    int recovered = control(37, budget, 0) != 0;
    if (result != -12 || canary != 0x12345678 || !recovered) {
      errors |= 8;
      break;
    }
    if (read((int)fd, &byte, 1) != 1 || byte != budget + 1)
      errors |= 16;
  }
  close((int)fd);
  long child = fork();
  if (!child) {
    syscall3(SYS_EXECVE, (long)path, (long)args, 0);
    _exit(98);
  }
  int status = 0;
  if (child <= 0 || waitpid(child, &status, 0) != child || status != (37 << 8))
    errors |= 32;
  return errors;
}

static unsigned long uaccess_allocation_fault(void) {
  unsigned long now = 0;
  unsigned long errors = syscall2(SYS_CLOCK_GETTIME, 0, (long)&now) != 0;
  long area = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
  if (area <= 0)
    return errors | 2;
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
  if (fd < 0)
    return 1;
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
  if (pipe(fds) != 0)
    return 1;
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

static unsigned long uaccess_partial_read(void) {
  long area = syscall6(SYS_MMAP, 0, 8192, 3, 0x22, -1, 0);
  if (area <= 0)
    return 1;
  volatile unsigned char *data = (volatile unsigned char *)(area + 4092);
  for (unsigned i = 0; i < 4; ++i)
    data[i] = 0xff;
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
  for (unsigned i = 0; i < 4; ++i)
    errors |= (unsigned long)(data[i] != i) << 5;
  errors |= (unsigned long)(read((int)fd, (void *)(area + 4096), 4) != 4) << 7;
  for (unsigned i = 0; i < 4; ++i)
    errors |= (unsigned long)(data[4 + i] != 4 + i) << 8;
  errors |= (unsigned long)(close((int)fd) != 0) << 9;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 8192) != 0) << 9;
  return errors;
}

static unsigned long uaccess_partial_write(void) {
  long area = syscall6(SYS_MMAP, 0, 8192, 3, 0x22, -1, 0);
  if (area <= 0)
    return 1;
  volatile unsigned char *data = (volatile unsigned char *)(area + 4092);
  for (unsigned i = 0; i < 4; ++i)
    data[i] = (unsigned char)i;
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
  for (unsigned i = 0; i < 4; ++i)
    errors |= (unsigned long)(bytes[i] != i) << 5;
  errors |= (unsigned long)(close(fds[1]) != 0) << 7;
  errors |= (unsigned long)(read(fds[0], bytes, 1) != 0) << 6;
  errors |= (unsigned long)(close(fds[0]) != 0) << 7;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 8192) != 0) << 7;
  return errors;
}

static unsigned long uaccess_partial_pipe_read(void) {
  long area = syscall6(SYS_MMAP, 0, 8192, 3, 0x22, -1, 0);
  if (area <= 0)
    return 1;
  volatile unsigned char *data = (volatile unsigned char *)(area + 4092);
  for (unsigned i = 0; i < 4; ++i)
    data[i] = 0xff;
  long fds[2];
  if (pipe(fds) != 0) {
    syscall2(SYS_MUNMAP, area, 8192);
    return 2;
  }
  unsigned char bytes[8];
  for (unsigned i = 0; i < 8; ++i)
    bytes[i] = (unsigned char)(80 + i);
  unsigned long errors = (unsigned long)(write(fds[1], bytes, 8) != 8) << 2;
  if (control(12, area + 4096, 0)) {
    long result = read(fds[0], (void *)data, 8);
    control(13, area + 4096, 0);
    errors |= (unsigned long)(result != 4) << 3;
  } else {
    errors |= 16;
  }
  errors |= (unsigned long)(read(fds[0], bytes, 8) != 4) << 5;
  for (unsigned i = 0; i < 4; ++i)
    errors |= (unsigned long)(data[i] != 80 + i || bytes[i] != 84 + i) << 6;
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
  if (area <= 0)
    return 1;
  long gate[2];
  if (pipe(gate) != 0) {
    syscall2(SYS_MUNMAP, area, 4096);
    return 2;
  }
  int status = 0;
  long child = fork();
  if (child == 0) {
    unsigned char ready = 0;
    while (read(gate[0], &ready, 1) == 0)
      sched_yield();
    close(gate[0]);
    close(gate[1]);
    struct sigaction_t action = {(unsigned long)unexpected_sigframe_handler, 0, SA_ONSTACK};
    struct stack_t stack = {(unsigned long)area, 4096, 0};
    if (ready != 1 || moss_sigaction(SIGUSR1, &action, 0) != 0 || sigaltstack(&stack, 0) != 0)
      _exit(92);
    if (!control(12, area, 0))
      _exit(93);
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
  if (area <= 0)
    return 1;
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
    if (zero >= 0)
      close((int)zero);
    if (null >= 0)
      close((int)null);
    if (console >= 0)
      close((int)console);
    if (area > 0)
      syscall2(SYS_MUNMAP, area, 8192);
    return 1;
  }
  unsigned char bytes[301];
  for (unsigned i = 0; i < sizeof(bytes); ++i)
    bytes[i] = 0xa5;
  unsigned long errors = (unsigned long)(read((int)zero, bytes, sizeof(bytes)) != sizeof(bytes)) << 1;
  for (unsigned i = 0; i < sizeof(bytes); ++i)
    errors |= (unsigned long)(bytes[i] != 0) << 2;
  bytes[0] = 0xa5;
  errors |= (unsigned long)(read((int)null, bytes, 1) != 0 || bytes[0] != 0xa5) << 3;
  errors |= (unsigned long)(write((int)console, "\n", 1) != 1) << 4;
  volatile unsigned char *data = (volatile unsigned char *)(area + 4092);
  for (unsigned i = 0; i < 4; ++i)
    data[i] = 0xa5;
  if (control(12, area + 4096, 0)) {
    errors |= (unsigned long)(read((int)zero, (void *)(area + 4096), 1) != -14) << 5;
    errors |= (unsigned long)(write((int)console, (const void *)(area + 4096), 1) != -14) << 6;
    // /dev/null deliberately does not touch valid, nonresident user storage.
    errors |= (unsigned long)(write((int)null, (const void *)(area + 4096), 8) != 8) << 7;
    errors |= (unsigned long)(read((int)null, (void *)(area + 4096), 8) != 0) << 8;
    errors |= (unsigned long)(read((int)zero, (void *)data, 8) != 4) << 9;
    control(13, area + 4096, 0);
    for (unsigned i = 0; i < 4; ++i)
      errors |= (unsigned long)(data[i] != 0) << 10;
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

static unsigned long pipe_waits_for_writer(void) {
  long ends[2] = {-1, -1};
  if (pipe(ends) != 0)
    return 1;
  long child = fork();
  if (child == 0) {
    if (close((int)ends[0]) != 0)
      _exit(91);
    // Let the reader reach an empty pipe while this endpoint is still open.
    unsigned long delay = 10000000;
    const unsigned char sent[] = {17, 44, 23, 99};
    if (nanosleep_ns(&delay) != 0 || write((int)ends[1], sent, sizeof(sent)) != sizeof(sent))
      _exit(92);
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
  if (pipe(request) != 0)
    return 1;
  if (pipe(reply) != 0) {
    close((int)request[0]);
    close((int)request[1]);
    return 2;
  }
  long child = fork();
  if (child == 0) {
    unsigned mask = 2;
    if (syscall3(20, 0, sizeof(mask), (long)&mask) != 0 || close((int)request[1]) != 0 || close((int)reply[0]) != 0)
      _exit(91);
    unsigned long delay = 1000000;
    if (nanosleep_ns(&delay) != 0 || current_cpu() != 1)
      _exit(93);
    for (unsigned i = 0; i < rounds; ++i) {
      unsigned char bytes[4];
      if (read((int)request[0], bytes, sizeof(bytes)) != sizeof(bytes) || bytes[0] != 17 || bytes[1] != i ||
          bytes[2] != 23 || bytes[3] != 99 || write((int)reply[1], bytes, sizeof(bytes)) != sizeof(bytes))
        _exit(92);
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
  if (pipe(ends) != 0)
    return 1;
  unsigned char bytes[4096];
  for (unsigned i = 0; i < sizeof(bytes); ++i)
    bytes[i] = (unsigned char)(i % 251);
  if (write((int)ends[1], bytes, sizeof(bytes)) != sizeof(bytes)) {
    close((int)ends[0]);
    close((int)ends[1]);
    return 2;
  }
  long child = fork();
  if (child == 0) {
    unsigned long delay = 10000000;
    if (close((int)ends[1]) != 0 || nanosleep_ns(&delay) != 0 ||
        read((int)ends[0], bytes, sizeof(bytes)) != sizeof(bytes))
      _exit(91);
    for (unsigned i = 0; i < sizeof(bytes); ++i)
      if (bytes[i] != i % 251)
        _exit(92);
    if (read((int)ends[0], bytes, 4) != 4 || bytes[0] != 9 || bytes[1] != 8 || bytes[2] != 7 || bytes[3] != 6)
      _exit(93);
    _exit(37);
  }
  unsigned long errors = child < 0;
  errors |= (unsigned long)(close((int)ends[0]) != 0) << 2;
  if (child > 0) {
    const unsigned char extra[] = {9, 8, 7, 6};
    errors |= (unsigned long)(write((int)ends[1], extra, sizeof(extra)) != sizeof(extra)) << 3;
  }
  errors |= (unsigned long)(close((int)ends[1]) != 0) << 4;
  if (child > 0)
    errors |= (unsigned long)!wait_exit(child, 37) << 5;
  return errors;
}

static unsigned long lifecycle_cycle(void) {
  long fd = syscall3(SYS_OPEN, (long)"/fixture.bin", 0, 0);
  if (fd < 0)
    return 1;
  long area = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
  long ends[2] = {-1, -1};
  if (area <= 0 || pipe(ends) != 0) {
    if (area > 0)
      syscall2(SYS_MUNMAP, area, 4096);
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
    if (private_page[0] != 17 || private_page[4095] != 23)
      _exit(90);
    private_page[0] = 44;
    struct sigaction_t action = {(unsigned long)frame_signal_handler, 0, 0};
    handled_signo = 0;
    handler_pid = 0;
    if (moss_sigaction(SIGUSR1, &action, 0) != 0 || kill(getpid(), SIGUSR1) != 0 || handled_signo != SIGUSR1 ||
        handler_pid != getpid())
      _exit(91);
    unsigned long before = 0, after = 0, duration = 1000000;
    long before_result = clock_gettime_ns(&before);
    long sleep_result = nanosleep_ns(&duration);
    long after_result = clock_gettime_ns(&after);
    if (before_result || sleep_result || after_result || after < before || after - before < duration) {
      _exit(92);
    }
    const unsigned char payload[4] = {17, 44, 23, 99};
    if (close((int)ends[0]) != 0 || write((int)ends[1], payload, 4) != 4)
      _exit(93);
    unsigned char byte = 255;
    if (syscall3(SYS_READ, fd, (long)&byte, 1) != 1 || byte != 0)
      _exit(97);
    // Keep the inherited descriptor open across exec and exit. The kernel must
    // release its ownership even when the application never calls close.
    const char *args[] = {"exec", 0};
    syscall3(SYS_EXECVE, (long)"/validation_child.elf", (long)args, 0);
    _exit(99);
  }
  unsigned long errors = (unsigned long)(close((int)ends[1]) != 0) << 2;
  int status = 0;
  long waited = child > 0 ? syscall3(SYS_WAITPID, child, (long)&status, 0) : -1;
  if (child <= 0 || waited != child || status != (37 << 8))
    errors |= 8 | ((unsigned long)(unsigned short)status << 16);
  unsigned char payload[4] = {0};
  errors |= (unsigned long)(read((int)ends[0], payload, 4) != 4 || payload[0] != 17 || payload[1] != 44 ||
                            payload[2] != 23 || payload[3] != 99)
            << 4;
  errors |= (unsigned long)(read((int)ends[0], payload, 1) != 0) << 5; // Exit releases the last writer.
  errors |= (unsigned long)(close((int)ends[0]) != 0) << 6;
  errors |= (unsigned long)(private_page[0] != 17 || private_page[4095] != 23) << 7;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 4096) != 0) << 8;
  errors |= (unsigned long)(syscall3(SYS_LSEEK, fd, 0, 1) != 1) << 9; // fork shares the file position.
  errors |= (unsigned long)(syscall1(SYS_CLOSE, fd) != 0) << 10;
  // Every cycle execs and reaps a fresh ash, covering all nine selected applets.
  // Count only after exact stdout, exit status and script cleanup succeeded.
  if (!errors && application_workload) {
    errors = busybox_script(application_script, "application ok\n") << 32;
    if (!errors)
      ++application_cycles;
  }
  return errors;
}

static unsigned long pipe_output_rollback(void) {
  for (unsigned i = 0; i < 1000; ++i) {
    if (syscall1(SYS_PIPE, (long)vm_rodata) != -14)
      return 1;
  }
  long ends[2] = {-1, -1};
  if (pipe(ends) != 0)
    return 2;
  unsigned long errors = ends[0] != 3 || ends[1] != 4;
  errors |= (unsigned long)(syscall1(SYS_CLOSE, ends[0]) != 0) << 1;
  errors |= (unsigned long)(syscall1(SYS_CLOSE, ends[1]) != 0) << 2;
  return errors;
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
  unsigned long errors = 0;
  for (unsigned i = 0; i < 8192; ++i)
    errors |= (unsigned long)(data[i] != 0) << 4;
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
  char too_long[1024]; // Native MAX_PATH_LEN bytes, with no terminating NUL.
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

static volatile unsigned long long benchmark_handler_tick;
static volatile unsigned long benchmark_handler_calls;

static void benchmark_signal_handler(int signo) {
  benchmark_handler_tick = counter();
  if (signo == SIGUSR1)
    ++benchmark_handler_calls;
}

static void user_benchmark(long mode) {
  // Avoid unrelated instruction-page faults in the measured regions.
  for (unsigned long page = (unsigned long)__user_text_start; page < (unsigned long)__user_text_end; page += 4096)
    (void)*(const volatile unsigned char *)page;
  long pid = getpid();
  struct sigaction_t action = {(unsigned long)benchmark_signal_handler, 0, 0};
  if (mode == 15 && moss_sigaction(SIGUSR1, &action, 0) != 0)
    control(5, 0, 0);
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
      for (long i = 0; i < count; ++i)
        *(volatile unsigned char *)(area + i * 4096) = 31;
      valid &= pipe(fds) == 0;
      if (valid)
        child = fork();
      if (child == 0) {
        unsigned char done;
        long result;
        while ((result = read(fds[0], &done, 1)) == 0)
          sched_yield();
        int isolated = result == 1 && done == 1;
        for (long i = 0; i < count; ++i)
          isolated &= *(volatile unsigned char *)(area + i * 4096) == 31;
        _exit(isolated ? 37 : 99);
      }
      valid &= child > 0;
    }
    if ((mode == 11 || mode == 12) && valid)
      valid &= control(31, area, count) == 1;
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
        if (delivered > start)
          ticks += delivered - start;
      } else if (mode == 16) {
        valid &= write(fds[1], sent, sizeof(sent)) == sizeof(sent);
        valid &= read(fds[0], received, sizeof(received)) == sizeof(received);
      }
    }
    const unsigned long long end = counter();
    if (mode != 15)
      ticks = end - begin;
    if (mode == 11 || mode == 12) {
      if (area > 0) {
        if (valid) {
          for (long i = 0; i < count; ++i)
            valid &= *(volatile unsigned char *)(area + i * 4096) == 73;
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
      for (unsigned i = 0; i < sizeof(sent); ++i)
        valid &= sent[i] == received[i];
    }
    if (fds[0] >= 0)
      valid &= close(fds[0]) == 0;
    if (fds[1] >= 0)
      valid &= close(fds[1]) == 0;
    valid &= control(30, 1, 0) == 1;
    const unsigned long long overhead_start = counter();
    if (mode == 15) {
      for (long i = 0; i < count; ++i) {
        (void)counter();
        (void)counter();
      }
    } else {
      for (long i = 0; i < count; ++i)
        asm volatile("" : "+r"(i) : : "memory");
    }
    const unsigned long long overhead = counter() - overhead_start;
    control(6, (long)overhead, 0);
    control(5, (long)ticks, valid);
  }
  control(3, 0, 0);
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
  unsigned long errors = syscall2(SYS_NANOSLEEP, 0, 0) != -14;
  errors |= (unsigned long)(syscall6(SYS_CLOCK_NANOSLEEP, 1, 0, 0, 0, 0, 0) != -14) << 1;
  errors |= (unsigned long)(syscall6(SYS_CLOCK_NANOSLEEP, 2, 0, (long)&duration, 0, 0, 0) != -22) << 2;
  errors |= (unsigned long)(syscall6(SYS_CLOCK_NANOSLEEP, 1, 2, (long)&duration, 0, 0, 0) != -22) << 3;
  errors |= (unsigned long)(syscall6(SYS_CLOCK_NANOSLEEP, 1, -1, (long)&duration, 0, 0, 0) != -22) << 4;
  duration = ~0UL;
  errors |= (unsigned long)(syscall2(SYS_NANOSLEEP, (long)&duration, 0) != -22) << 5;
  errors |= (unsigned long)(syscall6(SYS_CLOCK_NANOSLEEP, 1, 0, (long)&duration, 0, 0, 0) != -22) << 6;
  return errors;
}

static unsigned long timer_short_reuse(void) {
  unsigned long duration = 100000;
  for (unsigned i = 0; i < 1000; ++i) {
    unsigned long before = 0, after = 0;
    if (syscall2(SYS_CLOCK_GETTIME, 1, (long)&before) != 0 || syscall2(SYS_NANOSLEEP, (long)&duration, 0) != 0 ||
        syscall2(SYS_CLOCK_GETTIME, 1, (long)&after) != 0 || after < before || after - before < duration)
      return 1;
  }
  return 0;
}

static unsigned long timer_early_wakeup(void) {
  // CPU0 dispatches the global timer queue. Hold the actual sleeping child on
  // CPU1 so expiry can happen before its IRQ-masked context handoff.
  unsigned mask = 2;
  if (syscall3(20, 0, sizeof(mask), (long)&mask) != 0)
    return 1;
  long child = fork();
  if (child == 0) {
    if (!control(24, 0, 0))
      _exit(99);
    unsigned long errors = timer_relative_sleep();
    errors |= timer_absolute_sleep();
    errors |= !control(25, 0, 0);
    _exit(errors ? 99 : 37);
  }
  mask = 1;
  unsigned long errors = syscall3(20, 0, sizeof(mask), (long)&mask) != 0 || child < 0;
  if (child > 0)
    errors |= !wait_exit(child, 37);
  return errors;
}

static unsigned long timer_arm_failure_recovery(void) {
  if (!control(28, 0, 0))
    return 1;
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
  if (!control(20, 0, 0))
    return 1;
  long children[2] = {-1, -1};
  for (long actor = 1; actor <= 2; ++actor) {
    unsigned mask = 1U << actor;
    if (syscall3(20, 0, sizeof(mask), (long)&mask) != 0)
      return 2;
    children[actor - 1] = fork();
    if (children[actor - 1] == 0)
      _exit(control(21, actor, 0) ? 37 : 99);
    if (children[actor - 1] < 0)
      return 4;
  }
  unsigned mask = 1;
  if (syscall3(20, 0, sizeof(mask), (long)&mask) != 0)
    return 8;
  unsigned long errors = !control(22, 0, 0);
  for (unsigned i = 0; i < 2; ++i) {
    int status = 0;
    errors |= syscall3(SYS_WAITPID, children[i], (long)&status, 0) != children[i] || ((status >> 8) & 255) != 37;
  }
  errors |= !control(23, 0, 0);
  return errors;
}

void _start(void) {
  unsigned mask = 1;
  long affinity = syscall3(20, 0, sizeof(mask), (long)&mask);
  long mode = control(0, affinity, 0);
  if (mode >= 11 && mode <= 16) {
    user_benchmark(mode);
  } else if (mode == 7) {
    // Only the chosen data page may fault under allocation pressure. Keep every
    // test/helper code page resident before draining the physical allocator.
    for (unsigned long page = (unsigned long)__user_text_start; page < (unsigned long)__user_text_end; page += 4096)
      (void)*(const volatile unsigned char *)page;
    unsigned long (*const tests[])(void) = {
        uaccess_allocation_fault, uaccess_write_fault,     uaccess_read_fault,
        uaccess_partial_read,     uaccess_partial_write,   uaccess_partial_pipe_read,
        uaccess_sigframe_fault,   uaccess_sigreturn_fault, uaccess_devices};
    for (long test = 0; test < (long)(sizeof(tests) / sizeof(tests[0])); ++test) {
      control(1, test, 0);
      unsigned long errors = tests[test]();
      if (!control(2, errors == 0, (long)errors))
        break;
    }
    control(3, 0, 0);
  } else if (mode == 6) {
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
  } else if (mode == 21) {
    const char *scripts[] = {
        "exit 37",
        "value=$(printf 'moss\\n'); [ \"$value\" = moss ] && printf '%s\\n' \"$value\" && exit 37; exit 98",
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
        application_script};
    for (long test = 0; test < 9; ++test) {
      control(1, test, 0);
      unsigned long errors = busybox_script(scripts[test], test == 1   ? "moss\n"
                                                           : test == 3 ? "pipeline ok\n"
                                                           // The pinned minimal ls profile has sorting disabled.
                                                           : test == 4 ? "beta\nalpha\n"
                                                           : test == 5 ? "moss data\nappended\nnew\n"
                                                           : test == 6 ? "copy payload\n"
                                                           : test == 7 ? "move payload\nmove payload\n"
                                                           : test == 8 ? "application ok\n"
                                                                       : "");
      if (!control(2, errors == 0, (long)errors))
        break;
    }
    control(3, 0, 0);
  } else if (mode == 22) {
    for (long test = 0; test < 12; ++test) {
      control(1, test, 0);
      unsigned long errors = test < 11 ? exec_probe(test) : exec_allocation_rollback();
      if (!control(2, errors == 0, (long)errors))
        break;
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
      if (!control(2, ok, ok ? 0 : status))
        break;
    }
    control(3, 0, 0);
  } else if (mode == 10) {
    unsigned long (*const tests[])(void) = {timer_relative_sleep,      timer_absolute_sleep,   timer_invalid_arguments,
                                            timer_short_reuse,         timer_cancel_in_flight, timer_early_wakeup,
                                            timer_arm_failure_recovery};
    for (long test = 0; test < (long)(sizeof(tests) / sizeof(tests[0])); ++test) {
      control(1, test, 0);
      unsigned long errors = tests[test]();
      if (!control(2, errors == 0, (long)errors))
        break;
    }
    control(3, 0, 0);
  } else if (mode == 9 || mode == 23) {
    application_workload = mode == 23;
    const long interval = application_workload ? 10 : 100;
    control(1, 0, 0);
    unsigned long cycle_errors = lifecycle_cycle(); // Warm mappings before the resource baseline.
    int ok = cycle_errors == 0;
    if (ok)
      ok = control(10, 0, application_workload ? (long)application_cycles - 1 : 0) != 0;
    // A next-checkpoint counter avoids RV64's lazy-loaded 64-bit modulo
    // constants becoming resident only after the resource baseline.
    long next_checkpoint = interval;
    long progress = 1;
    for (long cycle = 1; ok && progress == 1; ++cycle) {
      cycle_errors = lifecycle_cycle();
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
                           "sigprocmask",
                           "sigaltstack",
                           "sig_ign",
                           "invalid_arguments",
                           "frame_validation",
                           "inheritance",
                           "pid_lifecycle",
                           "pipe_sigpipe",
                           "pipe_interrupted",
                           "pipe_noninterrupting_signals",
                           "pipe_partial_interrupt",
                           "signal_wakeup_affinity"};
    for (unsigned test = 0; test < sizeof(cases) / sizeof(cases[0]); ++test) {
      control(1, test, 0);
      long child = fork();
      if (child == 0) {
        const char *args[] = {"signal_test", cases[test], 0};
        syscall3(SYS_EXECVE, (long)"/signal_test.elf", (long)args, 0);
        _exit(99);
      }
      if (!control(2, wait_exit(child, 0), 0))
        break;
    }
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
                    for (unsigned i = 0; i < 128 && !errors; ++i)
                      errors = pipe_cross_cpu_roundtrip(1);
                    if (control(2, errors == 0, (long)errors)) {
                      control(1, 10, 0);
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
      if (!parent_fp_state())
        _exit(97);
#endif
      long peer = control(7, affinity, 0);
      if (peer == 2)
        peer = control(7, 1, syscall1(SYS_PIPE, (long)vm_rodata));
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
