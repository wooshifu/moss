#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/tcb.hpp>
#include <stddef.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#define MOSS_SYSCALL_RAW_ONLY
#include <moss-process-protocol.h>
#include <moss-syscall.h>

namespace {
int error(long result) { return result < 0 ? static_cast<int>(-result) : 0; }

unsigned long forked_session = 0;
// The service has no asynchronous exit notification yet; 10 ms polling
// bounds wait latency without spinning while a child runs.
constexpr unsigned long process_poll_ns = 10000000UL;

unsigned long process_session() {
  if (forked_session)
    return forked_session;
  int saved_errno = errno;
  unsigned long session = getauxval(MOSS_AT_STARTUP_CAP);
  errno = saved_errno;
  return session;
}

long process_call(unsigned long session, const moss_ipc_message &request,
                  moss_ipc_message &response) {
  // Keep a lost process service from hanging a managed libc call indefinitely.
  constexpr unsigned long timeout_ns = MOSS_PROCESS_RESERVATION_TIMEOUT_NS;
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC,
               reinterpret_cast<long>(&now)) != 0 ||
      now > LONG_MAX - timeout_ns)
    return -EIO;
  return syscall6(SYS_IPC_CALL, session, reinterpret_cast<long>(&request),
                  reinterpret_cast<long>(&response), now + timeout_ns, 0, 0);
}

bool no_capability(moss_ipc_message &response) {
  if (response.capability) {
    syscall1(SYS_CAP_CLOSE, response.capability);
    return false;
  }
  return !response.rights;
}

int process_status_error(unsigned char status) {
  switch (status) {
  case MOSS_PROCESS_OK:
    return 0;
  case MOSS_PROCESS_NO_ENTRY:
    return ESRCH;
  case MOSS_PROCESS_BAD_REQUEST:
    return EINVAL;
  case MOSS_PROCESS_DENIED:
    return EPERM;
  case MOSS_PROCESS_RUNNING:
    return EAGAIN;
  case MOSS_PROCESS_BUSY:
    return EBUSY;
  default:
    return EIO;
  }
}

int process_group_ids(unsigned long session, pid_t pid, pid_t &group,
                      pid_t &sid) {
  if (pid < 0)
    return EINVAL;
  moss_ipc_message request{};
  request.size = MOSS_PROCESS_REPLY_VALUE_BYTES;
  request.payload[0] = MOSS_PROCESS_GET_GROUP;
  moss_process_put_u64(request.payload + 1, static_cast<unsigned long>(pid));
  moss_ipc_message response{};
  long result = process_call(session, request, response);
  if (!no_capability(response))
    return EIO;
  if (result < 0)
    return error(result);
  if (result == 1)
    return response.payload[0] == MOSS_PROCESS_OK
               ? EIO
               : process_status_error(response.payload[0]);
  if (result != MOSS_PROCESS_REPLY_GROUP_BYTES ||
      response.payload[0] != MOSS_PROCESS_OK)
    return EIO;
  unsigned long group_id = moss_process_get_u64(response.payload + 1);
  unsigned long session_id = moss_process_get_u64(response.payload + 9);
  if (!group_id || !session_id || group_id > INT_MAX || session_id > INT_MAX)
    return EOVERFLOW;
  group = static_cast<pid_t>(group_id);
  sid = static_cast<pid_t>(session_id);
  return 0;
}

int process_identity(unsigned long session, unsigned long &id,
                     unsigned long &parent) {
  moss_ipc_message request{};
  request.size = 1;
  request.payload[0] = MOSS_PROCESS_IDENTITY;
  moss_ipc_message response{};
  long result = process_call(session, request, response);
  if (!no_capability(response) || result != MOSS_PROCESS_REPLY_IDENTITY_BYTES ||
      response.payload[0] != MOSS_PROCESS_OK)
    return result < 0 ? error(result) : EIO;
  id = moss_process_get_u64(response.payload + 1);
  parent = moss_process_get_u64(response.payload + 9);
  if (!id)
    return EIO;
  return id <= INT_MAX && parent <= INT_MAX ? 0 : EOVERFLOW;
}

bool process_child_request(unsigned long session, unsigned char operation,
                           unsigned long child_id, unsigned long domain) {
  moss_ipc_message request{};
  request.size = MOSS_PROCESS_REPLY_VALUE_BYTES;
  request.capability = domain;
  request.rights = domain ? MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT |
                                MOSS_CAP_DOMAIN_SIGNAL
                          : 0;
  request.payload[0] = operation;
  moss_process_put_u64(request.payload + 1, child_id);
  moss_ipc_message response{};
  long result = process_call(session, request, response);
  return no_capability(response) && result == 1 &&
         response.payload[0] == MOSS_PROCESS_OK;
}

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

// Native action bits differ from mlibc's public signal.h encoding.
constexpr unsigned long moss_sa_onstack = 0x1;
constexpr unsigned long moss_sa_restart = 0x2;
constexpr unsigned long moss_sa_nocldstop = 0x8;
constexpr unsigned long moss_sa_nocldwait = 0x10;

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
int Sysdeps<Sleep>::operator()(time_t *seconds, long *nanoseconds) {
  constexpr unsigned long nanos_per_second = 1000000000UL;
  if (*seconds < 0 || *nanoseconds < 0 || *nanoseconds >= static_cast<long>(nanos_per_second))
    return EINVAL;
  if (static_cast<unsigned long>(*seconds) > (ULONG_MAX - static_cast<unsigned long>(*nanoseconds)) / nanos_per_second)
    return EOVERFLOW;
  unsigned long requested =
      static_cast<unsigned long>(*seconds) * nanos_per_second + static_cast<unsigned long>(*nanoseconds);
  unsigned long remaining = 0;
  long result = syscall2(SYS_NANOSLEEP, reinterpret_cast<long>(&requested), reinterpret_cast<long>(&remaining));
  // mlibc's sleep() ignores EINTR while nanosleep() reports it with the
  // remaining duration, so both wrappers need the remainder on interruption.
  if (result == -EINTR) {
    *seconds = remaining / nanos_per_second;
    *nanoseconds = remaining % nanos_per_second;
    return EINTR;
  }
  if (result < 0)
    return error(result);
  *seconds = 0;
  *nanoseconds = 0;
  return 0;
}
pid_t Sysdeps<GetPid>::operator()() {
  unsigned long session = process_session();
  if (!session)
    return syscall0(SYS_GETPID);
  unsigned long id = 0, parent = 0;
  int result = process_identity(session, id, parent);
  if (result) {
    errno = result;
    return -1;
  }
  return static_cast<pid_t>(id);
}
pid_t Sysdeps<GetPpid>::operator()() {
  unsigned long session = process_session();
  if (!session)
    return syscall0(SYS_GETPPID);
  unsigned long id = 0, parent = 0;
  int result = process_identity(session, id, parent);
  if (result) {
    errno = result;
    return -1;
  }
  return static_cast<pid_t>(parent);
}
int Sysdeps<GetPgid>::operator()(pid_t pid, pid_t *group) {
  unsigned long session = process_session();
  if (!session) {
    long result = syscall1(SYS_GETPGID, pid);
    if (result < 0)
      return error(result);
    *group = static_cast<pid_t>(result);
    return 0;
  }
  pid_t sid = 0;
  return process_group_ids(session, pid, *group, sid);
}
int Sysdeps<GetSid>::operator()(pid_t pid, pid_t *sid) {
  unsigned long session = process_session();
  if (!session) {
    long result = syscall1(SYS_GETSID, pid);
    if (result < 0)
      return error(result);
    *sid = static_cast<pid_t>(result);
    return 0;
  }
  pid_t group = 0;
  return process_group_ids(session, pid, group, *sid);
}
int Sysdeps<SetPgid>::operator()(pid_t pid, pid_t group) {
  unsigned long session = process_session();
  if (!session) {
    // The legacy kernel ABI only provides setpgrp(0, 0); it ignores arguments.
    return pid || group ? ENOSYS : error(syscall0(SYS_SETPGRP));
  }
  if (pid < 0 || group < 0)
    return EINVAL;
  moss_ipc_message request{};
  request.size = MOSS_PROCESS_SET_GROUP_REQUEST_BYTES;
  request.payload[0] = MOSS_PROCESS_SET_GROUP;
  moss_process_put_u64(request.payload + 1, static_cast<unsigned long>(pid));
  moss_process_put_u64(request.payload + 9, static_cast<unsigned long>(group));
  moss_ipc_message response{};
  long result = process_call(session, request, response);
  if (!no_capability(response))
    return EIO;
  if (result < 0)
    return error(result);
  return result == 1 ? process_status_error(response.payload[0]) : EIO;
}
int Sysdeps<SetSid>::operator()(pid_t *sid) {
  unsigned long session = process_session();
  if (!session) {
    long result = syscall0(SYS_SETSID);
    if (result < 0)
      return error(result);
    *sid = static_cast<pid_t>(result);
    return 0;
  }
  moss_ipc_message request{};
  request.size = 1;
  request.payload[0] = MOSS_PROCESS_NEW_SESSION;
  moss_ipc_message response{};
  long result = process_call(session, request, response);
  if (!no_capability(response))
    return EIO;
  if (result < 0)
    return error(result);
  if (result == 1)
    return response.payload[0] == MOSS_PROCESS_OK
               ? EIO
               : process_status_error(response.payload[0]);
  if (result != MOSS_PROCESS_REPLY_VALUE_BYTES ||
      response.payload[0] != MOSS_PROCESS_OK)
    return EIO;
  unsigned long id = moss_process_get_u64(response.payload + 1);
  if (!id || id > INT_MAX)
    return EOVERFLOW;
  *sid = static_cast<pid_t>(id);
  return 0;
}
uid_t Sysdeps<GetUid>::operator()() { return syscall0(SYS_GETUID); }
uid_t Sysdeps<GetEuid>::operator()() { return syscall0(SYS_GETEUID); }
gid_t Sysdeps<GetGid>::operator()() { return syscall0(SYS_GETGID); }
gid_t Sysdeps<GetEgid>::operator()() { return syscall0(SYS_GETEGID); }
int Sysdeps<Kill>::operator()(pid_t pid, int signo) {
  // A compatibility PID is never a native PID; forwarding it could signal an
  // unrelated domain. The badged session limits this route to self and
  // children.
  unsigned long session = process_session();
  if (!session)
    return error(syscall2(SYS_KILL, pid, signo));
  if (pid == -1)
    return ENOSYS; // Broadcast needs a compatibility credential policy.
  if (signo < 0 || signo >= MOSS_PROCESS_SIGNAL_LIMIT)
    return EINVAL;
  moss_ipc_message request{};
  request.size = MOSS_PROCESS_SIGNAL_REQUEST_BYTES;
  request.payload[0] =
      pid > 0 ? MOSS_PROCESS_SIGNAL : MOSS_PROCESS_SIGNAL_GROUP;
  unsigned long target =
      pid < -1  ? static_cast<unsigned long>(-static_cast<long>(pid))
      : pid > 0 ? static_cast<unsigned long>(pid)
                : 0;
  moss_process_put_u64(request.payload + 1, target);
  moss_process_put_u64(request.payload + 9, static_cast<unsigned long>(signo));
  moss_ipc_message response{};
  long result = process_call(session, request, response);
  if (!no_capability(response))
    return EIO;
  if (result < 0)
    return error(result);
  if (result != 1)
    return EIO;
  return process_status_error(response.payload[0]);
}
int Sysdeps<Sigaction>::operator()(int signo, const struct sigaction *action, struct sigaction *previous) {
  // mlibc explicitly accepts ENOSYS here to opt out of pthread cancellation.
  // Its reserved signal lies outside Moss's supported signal range.
  if (signo == SIGCANCEL)
    return ENOSYS;
  MossSigaction input{}, output{};
  if (action) {
    if (action->sa_flags & ~(SA_ONSTACK | SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT))
      return EINVAL;
    input.handler = reinterpret_cast<unsigned long>(action->sa_handler);
    input.mask = moss_signal_mask(action->sa_mask);
    input.flags = (action->sa_flags & SA_ONSTACK ? moss_sa_onstack : 0) |
                  (action->sa_flags & SA_RESTART ? moss_sa_restart : 0) |
                  (action->sa_flags & SA_NOCLDSTOP ? moss_sa_nocldstop : 0) |
                  (action->sa_flags & SA_NOCLDWAIT ? moss_sa_nocldwait : 0);
  }
  long result = syscall3(SYS_SIGACTION, signo, action ? reinterpret_cast<long>(&input) : 0,
                         previous ? reinterpret_cast<long>(&output) : 0);
  if (result >= 0 && previous) {
    memset(previous, 0, sizeof(*previous));
    previous->sa_handler = reinterpret_cast<void (*)(int)>(output.handler);
    libc_signal_mask(output.mask, previous->sa_mask);
    previous->sa_flags = (output.flags & moss_sa_onstack ? SA_ONSTACK : 0) |
                         (output.flags & moss_sa_restart ? SA_RESTART : 0) |
                         (output.flags & moss_sa_nocldstop ? SA_NOCLDSTOP : 0) |
                         (output.flags & moss_sa_nocldwait ? SA_NOCLDWAIT : 0);
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
  unsigned long session = process_session();
  if (session) {
    moss_ipc_message prepare{};
    prepare.size = 1;
    prepare.payload[0] = MOSS_PROCESS_PREPARE_CHILD;
    moss_ipc_message reservation{};
    long result = process_call(session, prepare, reservation);
    if (result != MOSS_PROCESS_REPLY_VALUE_BYTES ||
        reservation.payload[0] != MOSS_PROCESS_OK || !reservation.capability ||
        reservation.rights != (MOSS_CAP_SEND | MOSS_CAP_DUPLICATE)) {
      if (reservation.capability)
        syscall1(SYS_CAP_CLOSE, reservation.capability);
      return result < 0 ? error(result) : EIO;
    }
    unsigned long id = moss_process_get_u64(reservation.payload + 1);
    unsigned long child_session = reservation.capability;
    if (!id || id > INT_MAX ||
        syscall2(SYS_CAP_SET_INHERIT, child_session, 1) != 0) {
      process_child_request(session, MOSS_PROCESS_CANCEL_CHILD, id, 0);
      syscall1(SYS_CAP_CLOSE, child_session);
      return id > INT_MAX ? EOVERFLOW : EIO;
    }
    unsigned long domain = 0;
    result = syscall1(SYS_FORK_DOMAIN_INHERIT, reinterpret_cast<long>(&domain));
    if (result == 0) {
      // The inherited parent sender must not grant this child authority over
      // its siblings. The child keeps only its own newly badged session.
      syscall1(SYS_CAP_CLOSE, session);
      forked_session = child_session;
      long self = syscall0(SYS_DOMAIN_SELF);
      bool attached =
          self > 0 && process_child_request(
                          child_session, MOSS_PROCESS_ATTACH_CHILD, id, self);
      if (self > 0)
        syscall1(SYS_CAP_CLOSE, self);
      if (!attached)
        sysdep<Exit>(127);
      *child = 0;
      return 0;
    }
    syscall2(SYS_CAP_SET_INHERIT, child_session, 0);
    int attached =
        result > 0 && domain &&
        process_child_request(session, MOSS_PROCESS_ATTACH_CHILD, id, domain);
    if (!attached) {
      if (result > 0 && domain) {
        syscall1(SYS_DOMAIN_TERMINATE, domain);
        syscall1(SYS_DOMAIN_WAIT, domain);
      }
      process_child_request(session, MOSS_PROCESS_CANCEL_CHILD, id, 0);
    }
    if (domain)
      syscall1(SYS_CAP_CLOSE, domain);
    syscall1(SYS_CAP_CLOSE, child_session);
    if (!attached)
      return result < 0 ? error(result) : EIO;
    *child = static_cast<pid_t>(id);
    return 0;
  }
  long result = syscall0(SYS_FORK);
  if (result >= 0)
    *child = result;
  return error(result);
}
int Sysdeps<Waitpid>::operator()(pid_t pid, int *status, int flags, rusage *usage, pid_t *waited) {
  if (usage)
    return ENOSYS;
  unsigned long session = process_session();
  if (session) {
    if (flags & ~WNOHANG)
      return EINVAL;
    moss_ipc_message request{};
    request.size = pid == -1 ? 1 : MOSS_PROCESS_REPLY_VALUE_BYTES;
    request.payload[0] = pid == -1 ? MOSS_PROCESS_WAIT_ANY
                         : pid > 0 ? MOSS_PROCESS_WAIT_CHILD
                                   : MOSS_PROCESS_WAIT_GROUP;
    if (pid > 0)
      moss_process_put_u64(request.payload + 1,
                           static_cast<unsigned long>(pid));
    else if (pid < -1)
      moss_process_put_u64(request.payload + 1,
                           static_cast<unsigned long>(-static_cast<long>(pid)));
    for (;;) {
      moss_ipc_message response{};
      long result = process_call(session, request, response);
      if (!no_capability(response))
        return EIO;
      if (result < 0)
        return error(result);
      if (result == MOSS_PROCESS_REPLY_WAIT_BYTES &&
          response.payload[0] == MOSS_PROCESS_EXITED) {
        unsigned long id = moss_process_get_u64(response.payload + 1);
        unsigned long state = moss_process_get_u64(response.payload + 9);
        if (!id || (pid > 0 && id != static_cast<unsigned long>(pid)))
          return EIO;
        if (id > INT_MAX)
          return EOVERFLOW;
        if (status)
          *status = state >> 32 ? static_cast<int>((state >> 32) & 0x7f)
                                : static_cast<int>((state & 0xff) << 8);
        *waited = static_cast<pid_t>(id);
        return 0;
      }
      if (result != 1)
        return EIO;
      if (response.payload[0] == MOSS_PROCESS_NO_ENTRY)
        return ECHILD;
      if (response.payload[0] != MOSS_PROCESS_RUNNING)
        return EIO;
      if (flags & WNOHANG) {
        *waited = 0;
        return 0;
      }
      unsigned long delay = process_poll_ns;
      result = syscall1(SYS_NANOSLEEP, reinterpret_cast<long>(&delay));
      if (result < 0)
        return error(result);
    }
  }
  long result = syscall3(SYS_WAITPID, pid, reinterpret_cast<long>(status), flags);
  if (result >= 0)
    *waited = result;
  return error(result);
}
int Sysdeps<Execve>::operator()(const char *path, char *const argv[], char *const envp[]) {
  unsigned long session = process_session();
  return error(session ? syscall6(SYS_EXECVE_CAP, reinterpret_cast<long>(path),
                                  reinterpret_cast<long>(argv),
                                  reinterpret_cast<long>(envp), session, 0, 0)
                       : syscall3(SYS_EXECVE, reinterpret_cast<long>(path),
                                  reinterpret_cast<long>(argv),
                                  reinterpret_cast<long>(envp)));
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
