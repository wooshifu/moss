#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "syscall.h"

// Fixture exit protocol: 37 reports a completed runtime/permission worker;
// 39 proves a selected exec continuation ran. Other nonzero returns identify
// failed checks (some add a subsystem base) and must match validation.c.
// Patterns 0xa5/0x5a and distinct TLS sentinels reveal unchanged, zeroed or
// aliased storage. Page fixtures use the kernel's 4096-byte granule.
extern char **environ;

// A nonzero .tdata initializer and a separate .tbss zero initializer exercise
// both halves of TLS startup; the marker is deliberately unlike later writes.
static _Thread_local volatile unsigned long tls_value = 0x12345678;
static _Thread_local volatile unsigned long tls_zero;

// Separate cold read-only pages: neither copying the file nor starting its
// new image faults these pages in before the backing file is destroyed.
// 8192 bytes span two base pages; distinct markers at bytes 0/8191 check both
// ends after destruction rather than merely verifying the first mapped page.
static const unsigned char snapshot_canary[8192] __attribute__((aligned(4096))) = {[0] = 0x37, [8191] = 0xa9};

static volatile sig_atomic_t delivered;

static int uname_runtime(void) {
  struct {
    unsigned char before[16];
    struct utsname identity;
    unsigned char after[16];
  } result;
  memset(&result, 0xa5, sizeof(result));
  if (uname(&result.identity)) {
    return 1;
  }
#if defined(__aarch64__)
  const char *machine = "aarch64";
#elif defined(__x86_64__)
  const char *machine = "x86_64";
#else
  const char *machine = "riscv64";
#endif
  // The native uname layout has six fixed 65-byte fields (64 chars + NUL).
  // Sixteen-byte guards and 0xa5 fill expose writes beyond or short of that ABI.
  _Static_assert(sizeof(struct utsname) == (size_t)6 * 65, "Moss uname ABI");
  const char *expected[] = {"Moss", "moss", MOSS_TEST_RELEASE, MOSS_TEST_VERSION, machine, ""};
  for (unsigned i = 0; i < 6; ++i) {
    const char *field = (const char *)&result.identity + (size_t)(i * 65);
    size_t length = strlen(expected[i]);
    if (memcmp(field, expected[i], length + 1)) {
      return 2;
    }
    for (size_t j = length + 1; j < 65; ++j) {
      if (field[j]) {
        return 3;
      }
    }
  }
  for (unsigned i = 0; i < sizeof(result.before); ++i) {
    if (result.before[i] != 0xa5 || result.after[i] != 0xa5) {
      return 4;
    }
  }
  struct utsname native;
  memset(&native, 0xa5, sizeof(native));
  if (syscall1(SYS_UNAME, (long)&native) || memcmp(&native, &result.identity, sizeof(native))) {
    return 5;
  }
  if (syscall1(SYS_UNAME, 0) != -EFAULT || syscall1(SYS_UNAME, 1) != -EFAULT || syscall1(SYS_UNAME, -1) != -EFAULT ||
      syscall1(SYS_UNAME, (long)0xffff800000000000UL) != -EFAULT) {
    return 6;
  }
  char *area = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  char *guard = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (area == MAP_FAILED || guard == MAP_FAILED || guard != area + 4096) {
    return 7;
  }
  memset(area, 0xa5, 4096);
  memset(guard, 0xa5, 4096);
  char *across = area + 4096 - sizeof(native) / 2;
  if (syscall1(SYS_UNAME, (long)across) || memcmp(across, &native, sizeof(native)) ||
      (unsigned char)across[-1] != 0xa5 || (unsigned char)across[sizeof(native)] != 0xa5) {
    return 8;
  }
  // Native munmap releases a complete VMA, as in the directory-copyout fixture.
  if (munmap(guard, 4096)) {
    return 9;
  }
  memset(across, 0xa5, sizeof(native) / 2);
  if (syscall1(SYS_UNAME, (long)across) != -EFAULT) {
    return 9;
  }
  for (unsigned i = 0; i < sizeof(native) / 2; ++i) {
    if ((unsigned char)across[i] != 0xa5) {
      return 9;
    }
  }
  if (munmap(area, 4096)) {
    return 9;
  }
  area = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (area == MAP_FAILED || syscall1(SYS_UNAME, (long)area) != -EFAULT || munmap(area, 4096)) {
    return 10;
  }
  // Rejected output pointers must not corrupt the immutable system identity.
  return uname(&native) == 0 && !memcmp(&native, &result.identity, sizeof(native)) ? 0 : 11;
}

static int filesystem_permissions_runtime(void) {
  // Directory modes 0555/0777/0700 distinguish search-only, public mutation
  // and owner-only search; file modes 0600/0000 separate owner access from
  // the initial create-open exception. Control 511/38 drops this child to UID/GID
  // 99, a non-root identity distinct from the parent-owned fixture (UID/GID 0).
  if (mkdir("/permission-lock", 0555) || mkdir("/permission-open", 0777) || mkdir("/permission-hidden", 0700) ||
      mkdir("/permission-lock/sub", 0777)) {
    return 1;
  }
  int held = open("/permission-lock/data", O_CREAT | O_EXCL | O_RDWR, 0600);
  int source = open("/permission-open/source", O_CREAT | O_EXCL | O_RDWR, 0);
  if (held < 0 || source < 0 || write(held, "safe", 4) != 4 || close(source) || lseek(held, 0, SEEK_SET) != 0) {
    return 2;
  }
  source = open("/libc_validation.elf", O_RDONLY);
  int program = open("/permission-lock/program", O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (source < 0 || program < 0) {
    return 6;
  }
  char image_buffer[4096];
  ssize_t count;
  while ((count = read(source, image_buffer, sizeof(image_buffer))) > 0) {
    if (write(program, image_buffer, count) != count) {
      return 7;
    }
  }
  if (count < 0 || close(source) || close(program)) {
    return 8;
  }
  pid_t child = fork();
  if (!child) {
    // Validation-image fixture only: the production control hook returns ENOSYS.
    if (syscall3(511, 38, 0, 0) || getuid() != 99 || geteuid() != 99 || getgid() != 99 || getegid() != 99) {
      _exit(10);
    }
    char bytes[4];
    // An inherited, already authorized descriptor remains usable after the drop.
    if (read(held, bytes, sizeof(bytes)) != 4 || memcmp(bytes, "safe", 4) || close(held)) {
      _exit(11);
    }
    errno = 0;
    if (open("/permission-lock/data", O_RDONLY) != -1 || errno != EACCES) {
      _exit(12);
    }
    errno = 0;
    if (open("/permission-lock/data", O_WRONLY | O_TRUNC) != -1 || errno != EACCES) {
      _exit(13);
    }
    errno = 0;
    if (mkdir("/permission-lock/new-dir", 0700) != -1 || errno != EACCES) {
      _exit(14);
    }
    errno = 0;
    if (open("/permission-lock/new-file", O_CREAT | O_WRONLY, 0600) != -1 || errno != EACCES) {
      _exit(15);
    }
    struct stat metadata;
    memset(&metadata, 0xa5, sizeof(metadata));
    errno = 0;
    if (stat("/permission-hidden/../permission-lock/data", &metadata) != -1 || errno != EACCES) {
      _exit(23);
    }
    for (size_t i = 0; i < sizeof(metadata); ++i) {
      if (((unsigned char *)&metadata)[i] != 0xa5) {
        _exit(23);
      }
    }
    // Stat requires path search, not permission to read the file's contents.
    if (stat("/permission-lock/data", &metadata) || metadata.st_size != 4) {
      _exit(24);
    }
    char *forbidden[] = {"libc_validation", "forbidden-exec", NULL};
    volatile unsigned long canary = 0x12345678;
    errno = 0;
    if (execve("/permission-lock/program", forbidden, NULL) != -1 || errno != EACCES || canary != 0x12345678) {
      _exit(25);
    }
    errno = 0;
    if (execve("/permission-hidden/../libc_validation.elf", forbidden, NULL) != -1 || errno != EACCES ||
        canary != 0x12345678) {
      _exit(25);
    }
    errno = 0;
    if (unlink("/permission-lock/data") != -1 || errno != EACCES) {
      _exit(16);
    }
    errno = 0;
    if (rmdir("/permission-lock/sub") != -1 || errno != EACCES) {
      _exit(17);
    }
    errno = 0;
    if (rename("/permission-lock/data", "/permission-open/moved") != -1 || errno != EACCES) {
      _exit(18);
    }
    errno = 0;
    if (rename("/permission-open/source", "/permission-lock/data") != -1 || errno != EACCES) {
      _exit(19);
    }
    // Namespace changes require parent permissions, not the file's mode bits.
    if (rename("/permission-open/source", "/permission-open/moved") || unlink("/permission-open/moved")) {
      _exit(20);
    }
    int fd = open("/permission-open/new", O_CREAT | O_EXCL | O_RDWR, 0);
    struct stat status;
    if (fd < 0 || write(fd, "user", 4) != 4 || fstat(fd, &status) || status.st_uid != 99 || status.st_gid != 99 ||
        (status.st_mode & 0777) || close(fd)) {
      _exit(21);
    }
    errno = 0;
    if (open("/permission-open/new", O_RDONLY) != -1 || errno != EACCES || unlink("/permission-open/new") ||
        mkdir("/permission-open/new-dir", 0700) || rmdir("/permission-open/new-dir")) {
      _exit(22);
    }
    pid_t peer = fork();
    if (!peer) {
      _exit(getuid() == 99 && geteuid() == 99 && getgid() == 99 && getegid() == 99 ? 37 : 26);
    }
    int peer_status = 0;
    if (peer < 0 || waitpid(peer, &peer_status, 0) != peer || peer_status != (37 << 8)) {
      _exit(26);
    }
    char *after_exec[] = {"libc_validation", "credentials-after-exec", NULL};
    execve("/libc_validation.elf", after_exec, NULL);
    _exit(27);
  }
  int status = 0;
  if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
    return 3;
  }
  if (WEXITSTATUS(status) != 37) {
    return WEXITSTATUS(status);
  }
  // Root also needs at least one execute bit on a regular executable file.
  child = fork();
  if (!child) {
    char *forbidden[] = {"libc_validation", "forbidden-exec", NULL};
    errno = 0;
    _exit(execve("/permission-lock/program", forbidden, NULL) == -1 && errno == EACCES ? 37 : 28);
  }
  if (child < 0 || waitpid(child, &status, 0) != child || status != (37 << 8)) {
    return 28;
  }
  char bytes[4];
  struct stat metadata;
  if (getuid() || geteuid() || getgid() || getegid() || stat("/permission-lock/data", &metadata) ||
      metadata.st_size != 4 || lseek(held, 0, SEEK_SET) != 0 || read(held, bytes, sizeof(bytes)) != 4 ||
      memcmp(bytes, "safe", 4) || close(held)) {
    return 4;
  }
  if (unlink("/permission-lock/data") || unlink("/permission-lock/program") || rmdir("/permission-lock/sub") ||
      rmdir("/permission-lock") || rmdir("/permission-open") || rmdir("/permission-hidden")) {
    return 5;
  }
  return 37;
}

static int terminal_runtime(void) {
  int console = open("/dev/console", O_WRONLY);
  if (console < 0) {
    return 1;
  }
  // Choose a high valid fd (200 < native MAX_FDS 256) to exercise descriptor
  // identity separately from stdio and the just-opened console slot.
  int duplicate = fcntl(console, F_DUPFD, 200);
  if (duplicate < 200 || close(console) || isatty(duplicate) != 1) {
    return 2;
  }
  // Native commands are 32-bit, descriptors remain full-width, and this
  // query takes no payload. Rejected requests must leave the stream usable.
  if (syscall3(SYS_IOCTL, (1L << 32) + duplicate, MOSS_IOCTL_ISATTY, 0) != -EBADF ||
      syscall3(SYS_IOCTL, duplicate, (1L << 32) | MOSS_IOCTL_ISATTY, 0) != -EINVAL ||
      syscall3(SYS_IOCTL, duplicate, -1, 0) != -EINVAL || syscall3(SYS_IOCTL, duplicate, 0, 0) != -ENOTTY ||
      syscall3(SYS_IOCTL, duplicate, MOSS_IOCTL_ISATTY, 1) != -EINVAL || isatty(duplicate) != 1) {
    return 11;
  }
  FILE *stream = fdopen(duplicate, "w");
  if (!stream || fputs("mlibc terminal ok\n", stream) == EOF || fflush(stream) || fclose(stream)) {
    return 3;
  }
  errno = 0;
  if (isatty(duplicate) || errno != EBADF) {
    return 4;
  }
  const char *paths[] = {"/fixture.bin", "/dev/null", "/dev/zero"};
  for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    int fd = open(paths[i], O_RDONLY);
    if (fd < 0) {
      return 5;
    }
    errno = 0;
    if (isatty(fd) || errno != ENOTTY) {
      return 6;
    }
    stream = fdopen(fd, "r");
    if (!stream || fflush(stream) || fclose(stream)) {
      return 7;
    }
  }
  int fds[2];
  if (pipe(fds)) {
    return 8;
  }
  for (int i = 0; i < 2; ++i) {
    errno = 0;
    if (isatty(fds[i]) || errno != ENOTTY || close(fds[i])) {
      return 9;
    }
  }
  errno = 0;
  return isatty(-1) == 0 && errno == EBADF ? 0 : 10;
}

static int access_runtime(void) {
  int fd = open("/access-file", O_CREAT | O_EXCL | O_RDWR, 0640);
  if (fd < 0 || close(fd) || access("/access-file", F_OK) || access("/access-file", R_OK | W_OK)) {
    return 1;
  }
  errno = 0;
  if (access("/access-file", X_OK) != -1 || errno != EACCES) {
    return 2;
  }
  errno = 0;
  if (access("/access-missing", F_OK) != -1 || errno != ENOENT) {
    return 3;
  }
  errno = 0;
  if (access("/access-file", 8) != -1 || errno != EINVAL ||
      syscall2(SYS_ACCESS, (long)"/access-file", 1L << 32) != -EINVAL || syscall2(SYS_ACCESS, 1, R_OK) != -EFAULT) {
    return 4;
  }
  errno = 0;
  if (access("/access-file/", F_OK) != -1 || errno != ENOTDIR) {
    return 5;
  }
  if (mkdir("/access-dir", 0700) || chdir("/access-dir") || access("../access-file", W_OK) ||
      access(".", R_OK | W_OK | X_OK) || chdir("/") || rmdir("/access-dir") || unlink("/access-file")) {
    return 6;
  }
  errno = 0;
  return access("/access-file", F_OK) == -1 && errno == ENOENT ? 0 : 7;
}

static int working_directory_runtime(void) {
  char cwd[256];
  if (getcwd(cwd, sizeof(cwd)) != cwd || strcmp(cwd, "/")) {
    return 1;
  }
  // Root requires exactly two bytes ("/" plus NUL); the third-byte sentinel
  // proves an exact-sized output does not overrun, while size 1 must fail.
  char exact[3] = {'x', 'y', 'z'};
  if (getcwd(exact, 2) != exact || exact[0] != '/' || exact[1] || exact[2] != 'z') {
    return 5;
  }
  errno = 0;
  if (getcwd(cwd, 1) || errno != ERANGE) {
    return 6;
  }
  errno = 0;
  if (getcwd(cwd, 0) || errno != EINVAL || syscall2(SYS_GETCWD, 1, sizeof(cwd)) != -EFAULT ||
      syscall2(SYS_GETCWD, (long)cwd, -1) != -EINVAL) {
    return 7;
  }
  char too_long[1024];
  memset(too_long, 'x', sizeof(too_long));
  if (syscall1(SYS_CHDIR, (long)too_long) != -ENAMETOOLONG) {
    return 17;
  }
  // The native path snapshot is 1024 bytes; libc's pinned adapter separately
  // caps path strings at 256, so test both boundaries rather than equating them.
  too_long[256] = 0;
  errno = 0;
  if (chdir(too_long) != -1 || errno != ENAMETOOLONG) {
    return 18;
  }
  if (chdir("/dev") || !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, "/dev") || chdir(".././dev/..") ||
      !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, "/")) {
    return 8;
  }
  if (mkdir("/cwd-sandbox", 0755) || chdir("/cwd-sandbox") || !getcwd(cwd, sizeof(cwd)) ||
      strcmp(cwd, "/cwd-sandbox")) {
    return 2;
  }
  int fd = open("payload", O_CREAT | O_WRONLY, 0644);
  struct stat relative, absolute;
  if (fd < 0 || write(fd, "cwd", 3) != 3 || close(fd) || stat("./payload", &relative) ||
      stat("/cwd-sandbox/payload", &absolute) || relative.st_ino != absolute.st_ino || relative.st_size != 3) {
    return 3;
  }
  const char *bad_paths[] = {"", "missing/..", "payload", "payload/..", "payload/"};
  const int errors[] = {ENOENT, ENOENT, ENOTDIR, ENOTDIR, ENOTDIR};
  for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    errno = 0;
    if (chdir(bad_paths[i]) != -1 || errno != errors[i] || !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, "/cwd-sandbox")) {
      return 9;
    }
  }
  if (syscall1(SYS_CHDIR, 1) != -EFAULT || !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, "/cwd-sandbox")) {
    return 10;
  }
  if (rename("payload", "renamed") || unlink("renamed") || mkdir("child", 0755) || chdir("./child/..") ||
      !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, "/cwd-sandbox") || chdir("child")) {
    return 4;
  }
  int go[2], ready[2];
  if (pipe(go) || pipe(ready) || current_cpu() != 0) {
    return 15;
  }
  pid_t child = fork();
  if (!child) {
    unsigned mask = 2;
    unsigned long pause = 1000000;
    char byte;
    if (syscall3(20, 0, sizeof(mask), (long)&mask) || syscall2(SYS_NANOSLEEP, (long)&pause, 0) || current_cpu() != 1 ||
        close(go[1]) || close(ready[0]) || !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, "/cwd-sandbox/child") ||
        write(ready[1], "R", 1) != 1 || read(go[0], &byte, 1) != 1 || byte != 'G' || !getcwd(cwd, sizeof(cwd)) ||
        strcmp(cwd, "/cwd-moved/child") || chdir("..") || close(go[0]) || close(ready[1])) {
      _exit(98);
    }
    char *args[] = {"libc_validation", "cwd-after-exec", NULL};
    execve("../libc_validation.elf", args, NULL);
    _exit(99);
  }
  char byte;
  if (child < 0 || close(go[0]) || close(ready[1]) || read(ready[0], &byte, 1) != 1 || byte != 'R' ||
      rename("/cwd-sandbox", "/cwd-moved") || write(go[1], "G", 1) != 1 || close(go[1]) || close(ready[0])) {
    return 16;
  }
  int status;
  if (child < 0 || waitpid(child, &status, 0) != child || status != (39 << 8) || !getcwd(cwd, sizeof(cwd)) ||
      strcmp(cwd, "/cwd-moved/child") || current_cpu() != 0) {
    return 11;
  }
  if (rmdir("/cwd-moved/child") || rmdir("/cwd-moved")) {
    return 12;
  }
  errno = 0;
  if (getcwd(cwd, sizeof(cwd)) || errno != ENOENT || stat(".", &relative) || relative.st_nlink != 0 || chdir("..")) {
    return 13;
  }
  errno = 0;
  if (getcwd(cwd, sizeof(cwd)) || errno != ENOENT || chdir("..") || !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, "/")) {
    return 14;
  }
  return 0;
}

static void handle_usr1(int signo) {
  sigset_t mask;
  delivered = signo == SIGUSR1 && sigprocmask(SIG_SETMASK, NULL, &mask) == 0 && sigismember(&mask, SIGUSR1) == 1 &&
              sigismember(&mask, SIGUSR2) == 1;
}

static int signal_runtime(void) {
  struct sigaction action = {0}, previous, observed;
  sigset_t original, blocked, current;
  if (sigprocmask(SIG_SETMASK, NULL, &original)) {
    return 0;
  }
  action.sa_handler = handle_usr1;
  action.sa_flags = SA_ONSTACK | SA_RESTART;
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGUSR2);
  if (sigaction(SIGUSR1, &action, &previous) || sigaction(SIGUSR1, NULL, &observed) ||
      observed.sa_handler != handle_usr1 || observed.sa_flags != (SA_ONSTACK | SA_RESTART) ||
      sigismember(&observed.sa_mask, SIGUSR2) != 1 || sigismember(&observed.sa_mask, SIGUSR1) != 0) {
    return 0;
  }
  // Unknown flags must not silently change the installed action.
  action.sa_flags = SA_SIGINFO;
  errno = 0;
  if (sigaction(SIGUSR1, &action, NULL) != -1 || errno != EINVAL || sigaction(SIGUSR1, NULL, &observed) ||
      observed.sa_flags != (SA_ONSTACK | SA_RESTART)) {
    return 0;
  }
  struct sigaction chld = {0}, previous_chld, observed_chld;
  chld.sa_handler = SIG_DFL;
  chld.sa_flags = SA_NOCLDSTOP;
  if (sigaction(SIGCHLD, &chld, &previous_chld) || sigaction(SIGCHLD, NULL, &observed_chld) ||
      observed_chld.sa_flags != SA_NOCLDSTOP || sigaction(SIGCHLD, &previous_chld, NULL)) {
    return 0;
  }
  sigfillset(&blocked);
  if (sigprocmask(SIG_SETMASK, &blocked, NULL) || sigprocmask(SIG_SETMASK, NULL, &current)) {
    return 0;
  }
  // Moss implements signals 1..31; KILL and STOP cannot be blocked.
  for (int signo = 1; signo < 32; ++signo) {
    if (sigismember(&current, signo) != (signo != SIGKILL && signo != SIGSTOP)) {
      return 0;
    }
  }
  if (raise(SIGUSR1) || delivered || sigprocmask(SIG_SETMASK, &original, NULL) || !delivered) {
    return 0;
  }
  if (sigprocmask(SIG_SETMASK, NULL, &current) || memcmp(&current, &original, sizeof(current)) ||
      sigaction(SIGUSR1, &previous, NULL)) {
    return 0;
  }
  errno = 0;
  if (sigaction(32, NULL, &observed) != -1 || errno != ENOSYS) {
    return 0; // mlibc's reserved cancellation signal is not implemented.
  }
  errno = 0;
  return sigaction(33, NULL, &observed) == -1 && errno == EINVAL;
}

static int pipe_runtime(void) {
  struct {
    int fds[2];
    unsigned long guard;
    // libc expects two 32-bit ints; native pipe copies two 64-bit longs into
    // adapter storage. The adjacent nonzero guard catches copying them here directly.
  } pipe_result = {{-1, -1}, 0x12345678};
  if (pipe(pipe_result.fds) || pipe_result.guard != 0x12345678) {
    return 0;
  }
  int input = pipe_result.fds[0], output = pipe_result.fds[1];
  int duplicate = dup(input);
  if (duplicate < 0 || dup2(duplicate, input) != input || dup2(input, input) != input) {
    return 0;
  }
  if (fcntl(input, F_GETFL) != O_RDONLY || fcntl(output, F_GETFL) != O_WRONLY ||
      fcntl(duplicate, F_GETFL) != O_RDONLY) {
    return 0;
  }
  errno = 0;
  if (fcntl(-1, F_GETFL) != -1 || errno != EBADF) {
    return 0;
  }
  errno = 0;
  if (fcntl(input, F_SETFL, O_NONBLOCK) != -1 || errno != ENOSYS || fcntl(input, F_GETFL) != O_RDONLY) {
    return 0;
  }
  if (syscall2(SYS_FCNTL, input, 3 + (1UL << 32)) != -ENOSYS) {
    return 0;
  }
  errno = 0;
  if (isatty(input) != 0 || errno != ENOTTY) {
    return 0;
  }
  errno = 0;
  if (isatty(-1) != 0 || errno != EBADF) {
    return 0;
  }
  char byte = 0;
  if (write(output, "m", 1) != 1 || read(duplicate, &byte, 1) != 1 || byte != 'm' || close(output) ||
      read(input, &byte, 1) != 0 || close(duplicate) || close(input)) {
    return 0;
  }
  errno = 0;
  return pipe2(pipe_result.fds, O_CLOEXEC) == -1 && errno == ENOSYS && pipe_result.guard == 0x12345678;
}

static int descriptor_runtime(void) {
  int boundary_fd = open("/descriptor-width", O_RDWR | O_CREAT | O_EXCL, 0600);
  if (boundary_fd < 0 || write(boundary_fd, "fd", 2) != 2 || lseek(boundary_fd, 0, SEEK_SET) != 0) {
    return 7;
  }
  // Cross the real syscall entry with values that must not alias this live FD.
  // 256 is the exclusive table bound. +/-2^32 preserve the live descriptor
  // if incorrectly narrowed to 32 bits, exposing aliasing instead of a true EBADF.
  const long invalid_fds[] = {-1, 256, boundary_fd + (1L << 32), boundary_fd - (1L << 32)};
  for (unsigned i = 0; i < sizeof(invalid_fds) / sizeof(invalid_fds[0]); ++i) {
    unsigned char byte = 0xa5;
    if (syscall3(SYS_READ, invalid_fds[i], (long)&byte, 1) != -EBADF || byte != 0xa5) {
      return 8;
    }
    if (syscall3(SYS_WRITE, invalid_fds[i], (long)"x", 1) != -EBADF) {
      return 9;
    }
    if (syscall1(SYS_CLOSE, invalid_fds[i]) != -EBADF) {
      return 10;
    }
    if (lseek(boundary_fd, 0, SEEK_CUR) != 0) {
      return 11;
    }
  }
  char original[2];
  if (read(boundary_fd, original, sizeof(original)) != sizeof(original) || memcmp(original, "fd", sizeof(original)) ||
      close(boundary_fd) || unlink("/descriptor-width")) {
    return 12;
  }
  const long ignored_modes[] = {-1, 1L << 32};
  for (unsigned i = 0; i < sizeof(ignored_modes) / sizeof(ignored_modes[0]); ++i) {
    long opened = syscall3(SYS_OPEN, (long)"/dev/null", O_RDONLY, ignored_modes[i]);
    if (opened < 0 || close(opened)) {
      return 5;
    }
  }
  struct stat absent;
  errno = 0;
  if (syscall3(SYS_OPEN, (long)"/mode-uncreated", O_WRONLY | O_CREAT, 1L << 32) != -EINVAL ||
      stat("/mode-uncreated", &absent) != -1 || errno != ENOENT ||
      syscall3(SYS_OPEN, (long)"/dev/null", 1L << 32, 0) != -EINVAL) {
    return 6;
  }
  // 200/201 are adjacent high valid slots reused by the fd-after-exec selector:
  // the CLOEXEC duplicate disappears only on successful exec; its peer survives.
  int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (fd < 0 || fcntl(fd, F_GETFD) != FD_CLOEXEC || fcntl(fd, F_DUPFD_CLOEXEC, 200) != 200 ||
      fcntl(fd, F_DUPFD, 201) != 201 || fcntl(200, F_GETFD) != FD_CLOEXEC || fcntl(201, F_GETFD) != 0 ||
      dup2(fd, fd) != fd || fcntl(fd, F_GETFD) != FD_CLOEXEC || dup2(fd, 201) != 201 || fcntl(201, F_GETFD) != 0) {
    return 1;
  }
  errno = 0;
  if (fcntl(fd, F_DUPFD, 256) != -1 || errno != EINVAL || syscall3(SYS_FCNTL, fd, F_DUPFD, 1L << 32) != -EINVAL) {
    return 2;
  }
  errno = 0;
  if (fcntl(fd, F_SETFD, 2) != -1 || errno != EINVAL || fcntl(fd, F_GETFD) != FD_CLOEXEC || fcntl(fd, F_SETFD, 0) ||
      fcntl(fd, F_GETFD) != 0 || fcntl(fd, F_SETFD, FD_CLOEXEC)) {
    return 3;
  }
  pid_t child = fork();
  if (!child) {
    char *args[] = {"libc_validation", "fd-after-exec", NULL};
    errno = 0;
    if (fcntl(200, F_GETFD) != FD_CLOEXEC || execve("/missing-exec", args, NULL) != -1 || errno != ENOENT ||
        fcntl(200, F_GETFD) != FD_CLOEXEC) {
      _exit(98);
    }
    execve("/libc_validation.elf", args, NULL);
    _exit(99);
  }
  int status;
  if (child < 0 || waitpid(child, &status, 0) != child || status != (39 << 8) || fcntl(200, F_GETFD) != FD_CLOEXEC ||
      close(200) || close(201) || close(fd)) {
    return 4;
  }
  return 0;
}

static int stream_redirection_runtime(void) {
  FILE *stream = fopen("/stream-data", "w+");
  if (!stream || fputs("abc", stream) < 0 || fflush(stream) || fseek(stream, 0, SEEK_SET) || fgetc(stream) != 'a' ||
      fflush(stream) || lseek(fileno(stream), 0, SEEK_CUR) != 1) {
    return 1;
  }
  int fds[2];
  if (pipe(fds) || dup2(fds[1], fileno(stream)) != fileno(stream) || close(fds[1])) {
    return 2;
  }
  // An existing FILE can outlive dup2 redirection. No reposition is needed
  // after writing or when flushing an empty buffer, even if it cached a file.
  char byte = 0;
  if (fputs("m", stream) < 0 || fflush(stream) || fflush(stream) || ferror(stream) || read(fds[0], &byte, 1) != 1 ||
      byte != 'm') {
    return 3;
  }
  errno = 0;
  if (lseek(fileno(stream), 0, SEEK_CUR) != -1 || errno != ESPIPE || fclose(stream) || read(fds[0], &byte, 1) != 0 ||
      close(fds[0]) || unlink("/stream-data")) {
    return 4;
  }
  return 0;
}

static int executable_snapshot_child(void) {
  int fd = open("/libc-copy.elf", O_WRONLY);
  struct stat st;
  char buffer[4096];
  memset(buffer, 0xa5, sizeof(buffer));
  if (fd < 0 || fstat(fd, &st)) {
    return 145;
  }
  // Overwrite existing storage before truncation so a stale lazy ELF backing
  // pointer cannot pass merely because freed heap memory retained its bytes.
  for (off_t left = st.st_size; left > 0;) {
    size_t count = left < (off_t)sizeof(buffer) ? (size_t)left : sizeof(buffer);
    if (write(fd, buffer, count) != (ssize_t)count) {
      return 146;
    }
    left -= count;
  }
  if (close(fd)) {
    return 147;
  }
  fd = open("/libc-copy.elf", O_WRONLY | O_TRUNC);
  if (fd < 0 || close(fd) || unlink("/libc-copy.elf")) {
    return 147;
  }
  pid_t child = fork();
  if (!child) {
    const volatile unsigned char *canary = snapshot_canary;
    _exit(canary[0] == 0x37 && canary[4095] == 0 && canary[8191] == 0xa9 && tls_value == 0x12345678 && !tls_zero ? 39
                                                                                                                 : 148);
  }
  int status;
  return child > 0 && waitpid(child, &status, 0) == child && status == (39 << 8) ? 39 : 149;
}

static int executable_snapshot_runtime(void) {
  int source = open("/libc_validation.elf", O_RDONLY);
  int target = open("/libc-copy.elf", O_WRONLY | O_CREAT | O_EXCL, 0700);
  if (source < 0 || target < 0) {
    return 1;
  }
  char buffer[4096];
  ssize_t count;
  while ((count = read(source, buffer, sizeof(buffer))) > 0) {
    if (write(target, buffer, count) != count) {
      fprintf(stderr, "snapshot fixture copy failed: errno=%d offset=%ld\n", errno, (long)lseek(target, 0, SEEK_CUR));
      return 2;
    }
  }
  if (count < 0 || close(source) || close(target)) {
    return 3;
  }
  pid_t child = fork();
  if (!child) {
    char *args[] = {"libc-copy", "snapshot", NULL};
    execve("/libc-copy.elf", args, NULL);
    _exit(150);
  }
  int status;
  struct stat st;
  errno = 0;
  return child > 0 && waitpid(child, &status, 0) == child && status == (39 << 8) && stat("/libc-copy.elf", &st) == -1 &&
                 errno == ENOENT
             ? 0
             : 4;
}

static int rename_runtime(void) {
  int source = open("/libc-rename-source", O_RDWR | O_CREAT | O_EXCL, 0600);
  int target = open("/libc-rename-target", O_RDWR | O_CREAT | O_EXCL, 0600);
  struct stat original, replaced, current;
  if (source < 0 || target < 0 || write(source, "new", 3) != 3 || write(target, "old", 3) != 3 ||
      fstat(source, &original) || fstat(target, &replaced)) {
    return 1;
  }
  char unterminated[1024];
  memset(unterminated, 'a', sizeof(unterminated));
  if (syscall2(SYS_RENAME, (long)"/libc-rename-source", 1) != -EFAULT ||
      syscall2(SYS_RENAME, 0, (long)"/libc-rename-target") != -EFAULT ||
      syscall2(SYS_RENAME, (long)"/libc-rename-source", (long)unterminated) != -ENAMETOOLONG ||
      stat("/libc-rename-source", &current) || current.st_ino != original.st_ino ||
      stat("/libc-rename-target", &current) || current.st_ino != replaced.st_ino) {
    return 2;
  }
  // Slash + 256-byte component + NUL needs 258 bytes; shortening the component
  // by one next checks the native 255-byte MAX_NAME_LEN acceptance boundary.
  char leaf[258];
  leaf[0] = '/';
  memset(leaf + 1, 'x', 256);
  leaf[257] = 0;
  if (syscall2(SYS_RENAME, (long)"/libc-rename-source", (long)leaf) != -ENAMETOOLONG) {
    return 2;
  }
  leaf[256] = 0; // Exactly MAX_NAME_LEN characters is valid.
  if (rename("/libc-rename-source", leaf) || stat(leaf, &current) || current.st_ino != original.st_ino ||
      rename(leaf, "/libc-rename-source")) {
    return 2;
  }
  int go[2], reply[2];
  if (pipe(go) || pipe(reply)) {
    return 3;
  }
  pid_t child = fork();
  if (!child) {
    unsigned mask = 2; // CPU 1; the validation parent is pinned to CPU 0.
    char byte, bytes[3];
    if (syscall3(20, 0, sizeof(mask), (long)&mask) || close(source) || close(go[1]) || close(reply[0]) ||
        write(reply[1], "R", 1) != 1 || read(go[0], &byte, 1) != 1 || byte != 'G') {
      _exit(161);
    }
    // Parent has closed its old-target descriptor and atomically replaced the
    // pathname. This child is the sole remaining owner of the removed file.
    if (fstat(target, &current) || current.st_ino != replaced.st_ino || current.st_nlink != 0 ||
        lseek(target, 0, SEEK_SET) != 0 || read(target, bytes, 3) != 3 || memcmp(bytes, "old", 3)) {
      _exit(162);
    }
    int moved = open("/libc-rename-target", O_RDONLY);
    if (moved < 0 || fstat(moved, &current) || current.st_ino != original.st_ino || read(moved, bytes, 3) != 3 ||
        memcmp(bytes, "new", 3) || close(moved) || close(target) || close(go[0]) || write(reply[1], "D", 1) != 1 ||
        close(reply[1])) {
      _exit(163);
    }
    _exit(39);
  }
  char byte;
  if (child < 0 || close(go[0]) || close(reply[1]) || read(reply[0], &byte, 1) != 1 || byte != 'R' || close(target) ||
      rename("/libc-rename-source", "/libc-rename-target") || close(source) || write(go[1], "G", 1) != 1 ||
      close(go[1]) || read(reply[0], &byte, 1) != 1 || byte != 'D' || close(reply[0])) {
    return 4;
  }
  int status;
  if (waitpid(child, &status, 0) != child || status != (39 << 8) || unlink("/libc-rename-target")) {
    return 5;
  }
  errno = 0;
  return stat("/libc-rename-source", &current) == -1 && errno == ENOENT ? 0 : 6;
}

static int directory_runtime(void) {
  struct stat root, device, created, held, current;
  if (stat("/", &root) || stat("/dev/null", &device) || !S_ISDIR(root.st_mode) || !S_ISCHR(device.st_mode) ||
      root.st_dev == device.st_dev) {
    return 1;
  }
  // Exceed both 256-entry pools: deletion must reclaim names, not only hide them.
  for (unsigned cycle = 0; cycle < 300; ++cycle) {
    if (mkdir("/libc-dir", 0700) || stat("/libc-dir", &created) || !S_ISDIR(created.st_mode) ||
        (created.st_mode & 0777) != 0700 || created.st_uid != geteuid() || created.st_gid != getegid() ||
        created.st_nlink != 2 || created.st_size || created.st_dev != root.st_dev) {
      return 2;
    }
    errno = 0;
    if (mkdir("/libc-dir", 0700) != -1 || errno != EEXIST) {
      return 3;
    }
    int fd = open("/libc-dir", O_RDONLY);
    if (fd < 0 || fstat(fd, &held) || held.st_dev != created.st_dev || held.st_ino != created.st_ino) {
      return 4;
    }
    if (mkdir("/libc-dir/child", 0750)) {
      return 5;
    }
    errno = 0;
    if (rmdir("/libc-dir") != -1 || errno != ENOTEMPTY) {
      return 6;
    }
    if (rmdir("/libc-dir/child") || rmdir("/libc-dir")) {
      return 7;
    }
    errno = 0;
    if (stat("/libc-dir", &current) != -1 || errno != ENOENT || fstat(fd, &held) || held.st_ino != created.st_ino ||
        held.st_nlink != 0) {
      return 8;
    }
    if (mkdir("/libc-dir", 0700) || stat("/libc-dir", &current) || current.st_ino == held.st_ino || close(fd) ||
        rmdir("/libc-dir")) {
      return 9;
    }
  }
  if (stat("/", &current) || current.st_nlink != root.st_nlink) {
    return 10;
  }
  return 0;
}

static int directory_entries_runtime(void) {
  struct dirent entry;
  // Native record: 19-byte dirent prefix + 256-byte name + 5 explicit tail bytes.
  // A 280-byte fixed record must match vfs::DirEntry and the pinned mlibc layout.
  _Static_assert(sizeof(entry) == 280, "Moss fixed directory record");
  if (mkdir("/libc-entries", 0700) || mkdir("/libc-entries/one", 0700) || mkdir("/libc-entries/two", 0700)) {
    return 1;
  }
  int fd = open("/libc-entries", O_RDONLY), regular = open("/busybox.elf", O_RDONLY);
  struct stat directory;
  if (fd < 0 || regular < 0 || fstat(fd, &directory)) {
    return 2;
  }
  if (syscall3(SYS_GETDENTS, -1, (long)&entry, sizeof(entry)) != -EBADF ||
      syscall3(SYS_GETDENTS, fd + (1L << 32), (long)&entry, sizeof(entry)) != -EBADF ||
      syscall3(SYS_GETDENTS, regular, (long)&entry, sizeof(entry)) != -ENOTDIR ||
      syscall3(SYS_GETDENTS, fd, (long)&entry, sizeof(entry) - 1) != -EINVAL ||
      syscall3(SYS_GETDENTS, fd, (long)&entry, -1) != -EINVAL ||
      syscall3(SYS_GETDENTS, fd, 0, sizeof(entry)) != -EFAULT || close(regular)) {
    return 3;
  }
  char *area = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  char *guard = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  // Native mmap advances a cursor; unmap the second complete VMA to leave a known hole.
  // Place the record 100 bytes before the boundary: a 280-byte copy then crosses
  // it. Exact offset 100 is a fixture choice; any value strictly inside the record
  // width forces full-range admission to fail before altering user memory.
  if (area == MAP_FAILED || guard == MAP_FAILED || guard != area + 4096 || munmap(guard, 4096) ||
      syscall3(SYS_GETDENTS, fd, (long)(area + 4096 - 100), sizeof(entry)) != -EFAULT || munmap(area, 4096)) {
    return 4;
  }
  // Failed copyout must leave the first entry available, and dup shares its cursor.
  int alias = dup(fd);
  if (alias < 0 || syscall3(SYS_GETDENTS, fd, (long)&entry, sizeof(entry)) != sizeof(entry) ||
      strcmp(entry.d_name, ".") || entry.d_ino != directory.st_ino || entry.d_type != DT_DIR ||
      entry.d_reclen != sizeof(entry) || entry.d_off != 1 ||
      syscall3(SYS_GETDENTS, alias, (long)&entry, sizeof(entry)) != sizeof(entry) || strcmp(entry.d_name, "..") ||
      entry.d_off != 2) {
    return 5;
  }
  if (syscall3(SYS_GETDENTS, fd, (long)&entry, sizeof(entry)) != sizeof(entry) || strcmp(entry.d_name, "one") ||
      entry.d_type != DT_DIR || rmdir("/libc-entries/one") ||
      syscall3(SYS_GETDENTS, alias, (long)&entry, sizeof(entry)) != sizeof(entry) || strcmp(entry.d_name, "two") ||
      rmdir("/libc-entries/two") || syscall3(SYS_GETDENTS, fd, (long)&entry, sizeof(entry)) != 0 ||
      syscall3(SYS_GETDENTS, alias, (long)&entry, sizeof(entry)) != 0 || rmdir("/libc-entries") ||
      syscall3(SYS_GETDENTS, fd, (long)&entry, sizeof(entry)) != 0 || close(alias) || close(fd)) {
    return 6;
  }
  errno = 0;
  if (opendir("/busybox.elf") != NULL || errno != ENOTDIR) {
    return 7;
  }
  DIR *stream = opendir("/dev");
  if (!stream) {
    return 8;
  }
  unsigned seen = 0;
  errno = 0;
  for (;;) {
    struct dirent *next = readdir(stream);
    if (!next) {
      break;
    }
    if (!strcmp(next->d_name, "null")) {
      seen += next->d_type == DT_CHR ? 1 : 100;
    }
  }
  return errno || seen != 1 || closedir(stream) ? 9 : 0;
}

// Resolve TLS afresh after fork; the compiler may otherwise reuse a TLS address
// cached in a preserved register before the fork syscall.
__attribute__((noinline)) static int tls_matches(unsigned long value, unsigned long zero, int error) {
  return tls_value == value && tls_zero == zero && errno == error;
}

int main(int argc, char **argv) {
  if (tls_value != 0x12345678 || tls_zero) {
    return 90;
  }
  if (!argc) {
    return argv && !argv[0] && !environ[0] ? 39 : 111;
  }
  // Match exec_probe's exact limit fixtures: 64 argv plus 64 environment strings
  // hit 128 total; "bytes" plus NUL uses 6 bytes and 16377 'a's plus NUL use
  // 16378, totaling the inclusive 16384-byte native exec budget.
  if (!strcmp(argv[0], "count")) {
    if (argc != 64 || argv[64]) {
      return 112;
    }
    for (int i = 1; i < argc; ++i) {
      if (strcmp(argv[i], "x")) {
        return 112;
      }
    }
    for (int i = 0; i < 64; ++i) {
      char name[] = "M00";
      name[1] = (char)('0' + i / 10);
      name[2] = (char)('0' + i % 10);
      const char *value = getenv(name);
      if (!environ[i] || !value || strcmp(value, "1")) {
        return 112;
      }
    }
    return !environ[64] ? 39 : 112;
  }
  if (!strcmp(argv[0], "bytes")) {
    if (argc != 2 || argv[2] || environ[0] || strlen(argv[1]) != 16377) {
      return 113;
    }
    for (int i = 0; i < 16377; ++i) {
      if (argv[1][i] != 'a') {
        return 113;
      }
    }
    return 39;
  }
  if (argc != 2) {
    return 90;
  }
  if (!strcmp(argv[1], "permissions")) {
    return filesystem_permissions_runtime();
  }
  if (!strcmp(argv[1], "credentials-after-exec")) {
    return getuid() == 99 && geteuid() == 99 && getgid() == 99 && getegid() == 99 ? 37 : 29;
  }
  if (!strcmp(argv[1], "snapshot")) {
    return executable_snapshot_child();
  }
  if (!strcmp(argv[1], "cwd-after-exec")) {
    char cwd[256];
    return getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/cwd-moved") && current_cpu() == 1 ? 39 : 199;
  }
  if (!strcmp(argv[1], "fd-after-exec")) {
    errno = 0;
    return fcntl(200, F_GETFD) == -1 && errno == EBADF && fcntl(201, F_GETFD) == 0 && fcntl(201, F_GETFL) == O_RDONLY
               ? 39
               : 140;
  }
  if (!strcmp(argv[1], "after-exec")) {
    const char *value = getenv("MOSS_TEST"), *empty = getenv("MOSS_EMPTY"), *equal = getenv("MOSS_EQ");
    return value && !strcmp(value, "value with spaces") && empty && !*empty && equal && !strcmp(equal, "a=b") ? 39
                                                                                                              : 110;
  }
  if (strcmp(argv[1], "runtime")) {
    return 90;
  }
  // The validation init process and its children start with root credentials.
  if (getuid() != 0 || geteuid() != 0 || getgid() != 0 || getegid() != 0 || getppid() <= 0) {
    return 104;
  }
  int uname_error = uname_runtime();
  if (uname_error) {
    return 230 + uname_error;
  }
  int terminal_error = terminal_runtime();
  if (terminal_error) {
    return 200 + terminal_error;
  }
  int access_error = access_runtime();
  if (access_error) {
    return 215 + access_error;
  }
  int cwd_error = working_directory_runtime();
  if (cwd_error) {
    return 170 + cwd_error;
  }
  if (!signal_runtime()) {
    return 105;
  }
  if (!pipe_runtime()) {
    return 106;
  }
  int descriptor_error = descriptor_runtime();
  if (descriptor_error) {
    return 135 + descriptor_error;
  }
  int stream_error = stream_redirection_runtime();
  if (stream_error) {
    return 140 + stream_error;
  }
  int snapshot_error = executable_snapshot_runtime();
  if (snapshot_error) {
    return 150 + snapshot_error;
  }
  int rename_error = rename_runtime();
  if (rename_error) {
    return 155 + rename_error;
  }
  int seek_fd = open("/fixture.bin", O_RDONLY);
  if (seek_fd < 0 || syscall3(SYS_LSEEK, seek_fd, 0, 1L << 32) != -EINVAL ||
      syscall3(SYS_LSEEK, seek_fd, 0, -1) != -EINVAL || lseek(seek_fd, 0, SEEK_CUR) != 0 || close(seek_fd)) {
    return 155;
  }
  int directory_error = directory_runtime();
  if (directory_error) {
    return 114 + directory_error;
  }
  directory_error = directory_entries_runtime();
  if (directory_error) {
    return 125 + directory_error;
  }
#if defined(__x86_64__)
  unsigned long base = 0, after = 0;
  if (syscall2(SYS_ARCH_PRCTL, 0x1003, (long)&base) || !base || syscall2(SYS_ARCH_PRCTL, 0, 0) != -EINVAL ||
      syscall2(SYS_ARCH_PRCTL, 0x1002, 1) != -EINVAL || syscall2(SYS_ARCH_PRCTL, 0x1002, -1) != -EINVAL ||
      syscall2(SYS_ARCH_PRCTL, 0x1003, 1) != -EFAULT || syscall2(SYS_ARCH_PRCTL, 0x1003, (long)&after) || after != base)
    return 103;
#endif
  // Two pages test allocator startup and a later byte (4096) beyond the first
  // page; independent parent/child TLS markers verify COW isolation after fork.
  char *buffer = malloc(8192);
  if (!buffer) {
    return 91;
  }
  memset(buffer, 0x5a, 8192);
  tls_value = 37;
  tls_zero = 19;
  errno = 0;
  if (close(-1) != -1 || errno != EBADF || getpid() <= 0 || buffer[4096] != 0x5a) {
    return 92;
  }
  free(buffer);
  if (sched_yield() != 0 || tls_value != 37 || tls_zero != 19) {
    return 93;
  }
  jmp_buf saved;
  int jump = setjmp(saved);
  if (!jump) {
    longjmp(saved, 37);
  }
  if (jump != 37) {
    return 95;
  }
  // Binary-exact inputs/results avoid rounding tolerance and cover compiler-rt
  // arithmetic/conversion paths across float, double, long double and integers.
  volatile float single = 1.5F;
  volatile double twice = 2.0;
  volatile long double wide = 4.0L;
  volatile long long integer = 7;
  if (single * single != 2.25F || twice * single != 3.0 || twice / 4.0 != 0.5 || wide / twice != 2.0L ||
      (long long)(wide + single) != 5 || (float)wide != 4.0F || (long double)integer != 7.0L) {
    return 96;
  }
  long initial_break = syscall1(SYS_BRK, 0);
  if (initial_break <= 0) {
    return 109;
  }
  errno = E2BIG;
  pid_t child = fork();
  if (child < 0) {
    return 97;
  }
  if (!child) {
    if (!tls_matches(37, 19, E2BIG)) {
      _exit(98);
    }
    if (syscall1(SYS_BRK, 0) != initial_break) {
      _exit(109);
    }
    // A forked libc must be able to acquire new anonymous memory, not only
    // reuse the parent's already-mapped allocator arenas.
    char *area = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area == MAP_FAILED || area[0] || area[4095]) {
      _exit(107);
    }
    area[0] = 37;
    if (munmap(area, 4096)) {
      _exit(108);
    }
    tls_value = 101;
    tls_zero = 202;
    errno = EDOM;
    // Thirty-two yields provide repeated save/restore opportunities; this
    // bounded fixture count is not proof that every CPU interleaving occurs.
    for (int i = 0; i < 32; ++i) {
      sched_yield();
      if (!tls_matches(101, 202, EDOM)) {
        _exit(99);
      }
    }
    char *args[] = {"libc_validation", "after-exec", NULL};
    char *environment[] = {"MOSS_TEST=value with spaces", "MOSS_EMPTY=", "MOSS_EQ=a=b", NULL};
    execve("/libc_validation.elf", args, environment);
    _exit(100);
  }
  for (int i = 0; i < 32; ++i) {
    sched_yield();
    if (!tls_matches(37, 19, E2BIG)) {
      return 101;
    }
  }
  int status = 0;
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 39) {
    return 102;
  }
  return write(1, "mlibc runtime ok\n", 17) == 17 ? 37 : 94;
}
