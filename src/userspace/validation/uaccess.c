#include "validation/internal.h"

// Control 12/13 exhausts/releases physical pages while the writable test VMA
// remains valid but absent: a copy must contain the allocation fault as EFAULT.
unsigned long uaccess_allocation_fault(void) {
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

unsigned long uaccess_read_fault(void) {
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

unsigned long uaccess_write_fault(void) {
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
unsigned long uaccess_partial_read(void) {
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

unsigned long uaccess_partial_write(void) {
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

unsigned long uaccess_partial_pipe_read(void) {
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

unsigned long uaccess_sigframe_fault(void) {
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
    errors |= (unsigned long)(waited != child || status != SIGUSR1) << 2;
  }
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, area, 4096) != 0) << 3;
  return errors;
}

long uaccess_sigreturn_probe(unsigned long user_sp);
unsigned long uaccess_sigreturn_fault(void) {
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

unsigned long uaccess_devices(void) {
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
  const int expected_status = kind == USER_FAULT ? SIGSEGV : 37 << 8;
  errors |= (unsigned long)(waited != child || status != expected_status) << 3;
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
unsigned long uaccess_cow_copy_fault(void) { return uaccess_cow_fault(0); }
unsigned long uaccess_cow_partial_read(void) { return uaccess_cow_fault(1); }
unsigned long uaccess_cow_user_fault(void) { return uaccess_cow_fault(2); }
