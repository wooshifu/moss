#include <dirent.h>
#include <errno.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/tcb.hpp>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#define MOSS_SYSCALL_RAW_ONLY
#include <moss-syscall.h>

namespace {
int error(long result) { return result < 0 ? static_cast<int>(-result) : 0; }

// The pinned mlibc ABI uses bit signo-1 in a 128-byte set. Moss uses bit
// signo in one word and only implements signals 1..31 (including sigfillset).
unsigned long moss_signal_mask(const sigset_t &set) { return (set.__sig[0] & 0x7fffffffUL) << 1; }
void libc_signal_mask(unsigned long mask, sigset_t &set) {
  memset(&set, 0, sizeof(set));
  set.__sig[0] = (mask & 0xfffffffeUL) >> 1;
}

struct MossSigaction {
  unsigned long handler, mask, flags;
};

// Native vfs::Stat layout, not the mlibc public struct stat.
struct MossStat {
  uint64_t ino;
  uint32_t mode, nlink;
  uint64_t size;
  uint32_t rdev, uid, gid, reserved;
  uint64_t dev;
};
static_assert(sizeof(MossStat) == 48);
} // namespace

namespace mlibc {
void Sysdeps<Exit>::operator()(int status) {
  syscall1(SYS_EXIT, status);
  __builtin_trap();
}
void Sysdeps<LibcLog>::operator()(const char *message) {
  syscall3(SYS_WRITE, 2, reinterpret_cast<long>(message), strlen(message));
  syscall3(SYS_WRITE, 2, reinterpret_cast<long>("\n"), 1);
}
void Sysdeps<LibcPanic>::operator()() {
  sysdep<LibcLog>("mlibc: fatal runtime error");
  sysdep<Exit>(127);
}
int Sysdeps<Write>::operator()(int fd, const void *buffer, size_t size, ssize_t *written) {
  long result = syscall3(SYS_WRITE, fd, reinterpret_cast<long>(buffer), size);
  if (result >= 0)
    *written = result;
  return error(result);
}
int Sysdeps<Read>::operator()(int fd, void *buffer, size_t size, ssize_t *read) {
  long result = syscall3(SYS_READ, fd, reinterpret_cast<long>(buffer), size);
  if (result >= 0)
    *read = result;
  return error(result);
}
int Sysdeps<Open>::operator()(const char *path, int flags, mode_t mode, int *fd) {
  static_assert(O_CREAT == 0x40 && O_EXCL == 0x80 && O_TRUNC == 0x200 && O_APPEND == 0x400 && O_CLOEXEC == 0x80000);
  if (flags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_CLOEXEC))
    return ENOSYS;
  long result = syscall3(SYS_OPEN, reinterpret_cast<long>(path), flags, mode);
  if (result >= 0)
    *fd = result;
  return error(result);
}
int Sysdeps<Close>::operator()(int fd) { return error(syscall1(SYS_CLOSE, fd)); }
int Sysdeps<Access>::operator()(const char *path, int mode) {
  return error(syscall2(SYS_ACCESS, reinterpret_cast<long>(path), mode));
}
int Sysdeps<Uname>::operator()(struct utsname *out) {
  // The pinned public layout matches Moss's six fixed-width native fields.
  static_assert(sizeof(*out) == 6 * 65 && offsetof(utsname, sysname) == 0 && offsetof(utsname, nodename) == 65 &&
                offsetof(utsname, release) == 130 && offsetof(utsname, version) == 195 &&
                offsetof(utsname, machine) == 260);
  return error(syscall1(SYS_UNAME, reinterpret_cast<long>(out)));
}
int Sysdeps<GetCwd>::operator()(char *buffer, size_t size) {
  return error(syscall2(SYS_GETCWD, reinterpret_cast<long>(buffer), size));
}
int Sysdeps<Chdir>::operator()(const char *path) { return error(syscall1(SYS_CHDIR, reinterpret_cast<long>(path))); }
int Sysdeps<Mkdir>::operator()(const char *path, mode_t mode) {
  return error(syscall2(SYS_MKDIR, reinterpret_cast<long>(path), mode));
}
int Sysdeps<Rmdir>::operator()(const char *path) { return error(syscall1(SYS_RMDIR, reinterpret_cast<long>(path))); }
int Sysdeps<Rename>::operator()(const char *old_path, const char *new_path) {
  return error(syscall2(SYS_RENAME, reinterpret_cast<long>(old_path), reinterpret_cast<long>(new_path)));
}
int Sysdeps<Unlinkat>::operator()(int dirfd, const char *path, int flags) {
  if (flags & ~AT_REMOVEDIR)
    return EINVAL;
  if (dirfd != AT_FDCWD && (!path || *path != '/'))
    return ENOSYS;
  return error(syscall1(flags & AT_REMOVEDIR ? SYS_RMDIR : SYS_UNLINK, reinterpret_cast<long>(path)));
}
int Sysdeps<OpenDir>::operator()(const char *path, int *handle) {
  int fd;
  if (int result = sysdep<Open>(path, O_RDONLY, 0, &fd))
    return result;
  struct stat status;
  int result = sysdep<Stat>(fsfd_target::fd, fd, "", 0, &status);
  if (result || !S_ISDIR(status.st_mode)) {
    (void)sysdep<Close>(fd);
    return result ? result : ENOTDIR;
  }
  *handle = fd;
  return 0;
}
int Sysdeps<ReadEntries>::operator()(int fd, void *buffer, size_t size, size_t *bytes_read) {
  static_assert(offsetof(struct dirent, d_ino) == 0 && offsetof(struct dirent, d_off) == 8 &&
                offsetof(struct dirent, d_reclen) == 16 && offsetof(struct dirent, d_type) == 18 &&
                offsetof(struct dirent, d_name) == 19 && sizeof(dirent::d_name) >= 256);
  static_assert(DT_REG == 8 && DT_DIR == 4 && DT_CHR == 2 && DT_BLK == 6 && DT_FIFO == 1 && DT_SOCK == 12 &&
                DT_LNK == 10);
  long result = syscall3(SYS_GETDENTS, fd, reinterpret_cast<long>(buffer), size);
  if (result >= 0)
    *bytes_read = result;
  return error(result);
}
int Sysdeps<Stat>::operator()(fsfd_target target, int fd, const char *path, int flags, struct stat *out) {
  if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
    return EINVAL;
  MossStat native{};
  long result;
  if (target == fsfd_target::fd || (target == fsfd_target::fd_path && (flags & AT_EMPTY_PATH) && path && !*path)) {
    result = syscall2(SYS_FSTAT, fd, reinterpret_cast<long>(&native));
  } else if (target == fsfd_target::path ||
             (target == fsfd_target::fd_path && (fd == AT_FDCWD || (path && *path == '/')))) {
    result = syscall2(flags & AT_SYMLINK_NOFOLLOW ? SYS_LSTAT : SYS_STAT, reinterpret_cast<long>(path),
                      reinterpret_cast<long>(&native));
  } else {
    return ENOSYS;
  }
  if (result < 0)
    return error(result);
  memset(out, 0, sizeof(*out));
  out->st_ino = native.ino;
  out->st_mode = native.mode;
  out->st_nlink = native.nlink;
  out->st_size = native.size;
  out->st_rdev = native.rdev;
  out->st_dev = native.dev;
  out->st_uid = native.uid;
  out->st_gid = native.gid;
  out->st_blksize = 4096;
  // The current in-memory VFS tracks neither timestamps nor allocated disk blocks.
  return 0;
}
int Sysdeps<Fcntl>::operator()(int fd, int command, va_list args, int *out) {
  if (command != F_GETFL && command != F_GETFD && command != F_SETFD && command != F_DUPFD &&
      command != F_DUPFD_CLOEXEC)
    return ENOSYS;
  static_assert(F_DUPFD == 0 && F_GETFD == 1 && F_SETFD == 2 && F_GETFL == 3 && F_DUPFD_CLOEXEC == 1030 &&
                FD_CLOEXEC == 1);
  int argument = command == F_SETFD || command == F_DUPFD || command == F_DUPFD_CLOEXEC ? va_arg(args, int) : 0;
  long result = syscall3(SYS_FCNTL, fd, command, argument);
  if (result >= 0)
    *out = result;
  return error(result);
}
int Sysdeps<Pipe>::operator()(int *fds, int flags) {
  if (flags)
    return ENOSYS;
  // Moss returns two longs; POSIX pipe() exposes two ints.
  long native_fds[2];
  long result = syscall1(SYS_PIPE, reinterpret_cast<long>(native_fds));
  if (result >= 0) {
    fds[0] = native_fds[0];
    fds[1] = native_fds[1];
  }
  return error(result);
}
int Sysdeps<Dup>::operator()(int fd, int flags, int *newfd) {
  if (flags)
    return ENOSYS;
  long result = syscall1(SYS_DUP, fd);
  if (result >= 0)
    *newfd = result;
  return error(result);
}
int Sysdeps<Dup2>::operator()(int fd, int flags, int newfd) {
  if (flags)
    return ENOSYS;
  return error(syscall2(SYS_DUP2, fd, newfd));
}
int Sysdeps<Seek>::operator()(int fd, off_t offset, int whence, off_t *position) {
  long result = syscall3(SYS_LSEEK, fd, offset, whence);
  if (result >= 0)
    *position = result;
  return error(result);
}
int Sysdeps<VmMap>::operator()(void *hint, size_t size, int prot, int flags, int fd, off_t offset, void **out) {
  if (flags != (MAP_PRIVATE | MAP_ANONYMOUS) || fd != -1 || offset)
    return ENOSYS;
  long result = syscall6(SYS_MMAP, reinterpret_cast<long>(hint), size, prot, 0x22, -1, 0);
  if (result >= 0)
    *out = reinterpret_cast<void *>(result);
  return error(result);
}
int Sysdeps<VmUnmap>::operator()(void *address, size_t size) {
  return error(syscall2(SYS_MUNMAP, reinterpret_cast<long>(address), size));
}
int Sysdeps<AnonAllocate>::operator()(size_t size, void **out) {
  return sysdep<VmMap>(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, out);
}
int Sysdeps<AnonFree>::operator()(void *address, size_t size) { return sysdep<VmUnmap>(address, size); }
int Sysdeps<ClockGet>::operator()(int clock, time_t *seconds, long *nanoseconds) {
  if (clock != CLOCK_MONOTONIC)
    return ENOSYS;
  unsigned long value = 0;
  long result = syscall2(SYS_CLOCK_GETTIME, 1, reinterpret_cast<long>(&value));
  if (result < 0)
    return error(result);
  *seconds = value / 1000000000;
  *nanoseconds = value % 1000000000;
  return 0;
}
pid_t Sysdeps<GetPid>::operator()() { return syscall0(SYS_GETPID); }
pid_t Sysdeps<GetPpid>::operator()() { return syscall0(SYS_GETPPID); }
uid_t Sysdeps<GetUid>::operator()() { return syscall0(SYS_GETUID); }
uid_t Sysdeps<GetEuid>::operator()() { return syscall0(SYS_GETEUID); }
gid_t Sysdeps<GetGid>::operator()() { return syscall0(SYS_GETGID); }
gid_t Sysdeps<GetEgid>::operator()() { return syscall0(SYS_GETEGID); }
int Sysdeps<Kill>::operator()(pid_t pid, int signo) { return error(syscall2(SYS_KILL, pid, signo)); }
int Sysdeps<Sigaction>::operator()(int signo, const struct sigaction *action, struct sigaction *previous) {
  // mlibc explicitly accepts ENOSYS here to opt out of pthread cancellation.
  // Its reserved signal lies outside Moss's supported signal range.
  if (signo == SIGCANCEL)
    return ENOSYS;
  MossSigaction input{}, output{};
  if (action) {
    if (action->sa_flags & ~SA_ONSTACK)
      return EINVAL;
    input.handler = reinterpret_cast<unsigned long>(action->sa_handler);
    input.mask = moss_signal_mask(action->sa_mask);
    input.flags = action->sa_flags & SA_ONSTACK ? 1 : 0;
  }
  long result = syscall3(SYS_SIGACTION, signo, action ? reinterpret_cast<long>(&input) : 0,
                         previous ? reinterpret_cast<long>(&output) : 0);
  if (result >= 0 && previous) {
    memset(previous, 0, sizeof(*previous));
    previous->sa_handler = reinterpret_cast<void (*)(int)>(output.handler);
    libc_signal_mask(output.mask, previous->sa_mask);
    previous->sa_flags = output.flags & 1 ? SA_ONSTACK : 0;
  }
  return error(result);
}
int Sysdeps<Sigprocmask>::operator()(int how, const sigset_t *set, sigset_t *previous) {
  unsigned long input = set ? moss_signal_mask(*set) : 0, output = 0;
  long result = syscall3(SYS_SIGPROCMASK, how, set ? reinterpret_cast<long>(&input) : 0,
                         previous ? reinterpret_cast<long>(&output) : 0);
  if (result >= 0 && previous)
    libc_signal_mask(output, *previous);
  return error(result);
}
void Sysdeps<Yield>::operator()() { syscall0(SYS_SCHED_YIELD); }
int Sysdeps<Fork>::operator()(pid_t *child) {
  long result = syscall0(SYS_FORK);
  if (result >= 0)
    *child = result;
  return error(result);
}
int Sysdeps<Waitpid>::operator()(pid_t pid, int *status, int flags, rusage *usage, pid_t *waited) {
  if (usage)
    return ENOSYS;
  long result = syscall3(SYS_WAITPID, pid, reinterpret_cast<long>(status), flags);
  if (result >= 0)
    *waited = result;
  return error(result);
}
int Sysdeps<Execve>::operator()(const char *path, char *const argv[], char *const envp[]) {
  return error(
      syscall3(SYS_EXECVE, reinterpret_cast<long>(path), reinterpret_cast<long>(argv), reinterpret_cast<long>(envp)));
}

// ARM64 and RV64 let userspace write its own thread pointer directly.
int Sysdeps<TcbSet>::operator()(void *pointer) {
#if defined(__aarch64__)
  uintptr_t tp = reinterpret_cast<uintptr_t>(pointer) + sizeof(Tcb) - 16;
  asm volatile("msr tpidr_el0, %0" : : "r"(tp) : "memory");
  return 0;
#elif defined(__riscv)
  uintptr_t tp = reinterpret_cast<uintptr_t>(pointer) + sizeof(Tcb);
  asm volatile("mv tp, %0" : : "r"(tp) : "memory");
  return 0;
#elif defined(__x86_64__)
  return error(syscall2(SYS_ARCH_PRCTL, 0x1002, reinterpret_cast<long>(pointer)));
#else
  (void)pointer;
  return ENOSYS;
#endif
}
// Explicit failures, not pretend success, until these interfaces are adapted.
int Sysdeps<FutexWait>::operator()(int *, int, const timespec *) { return ENOSYS; }
int Sysdeps<FutexWake>::operator()(int *, bool) { return ENOSYS; }
int Sysdeps<Isatty>::operator()(int fd) { return error(syscall3(SYS_IOCTL, fd, MOSS_IOCTL_ISATTY, 0)); }
} // namespace mlibc
