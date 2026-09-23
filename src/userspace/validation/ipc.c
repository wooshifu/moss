#include "validation/internal.h"

enum { IPC_EINTR = 4, IPC_EBADF = 9, IPC_EACCES = 13, IPC_EINVAL = 22, IPC_EPIPE = 32, IPC_ETIMEDOUT = 110 };
static volatile int ipc_signal_seen;

static void ipc_signal_handler(int signo) { ipc_signal_seen = signo; }

static long deadline_after(unsigned long offset_ns) {
  unsigned long now = 0;
  return clock_gettime_ns(&now) == 0 ? (long)(now + offset_ns) : -1;
}

unsigned long ipc_roundtrip(void) {
  struct moss_ipc_endpoints pair = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0)
    return 1;
  long limited = syscall2(SYS_CAP_DUPLICATE, (long)pair.send, MOSS_CAP_SEND);
  unsigned long errors = limited <= 0;
  if (limited > 0) {
    errors |= (unsigned long)(syscall2(SYS_CAP_DUPLICATE, limited, MOSS_CAP_SEND) != -IPC_EACCES) << 1;
    errors |= (unsigned long)(syscall2(SYS_CAP_SET_INHERIT, limited, 1) != -IPC_EACCES) << 2;
    errors |= (unsigned long)(syscall6(SYS_IPC_RECEIVE, limited, 0, 0, 0, 0, 0) != -IPC_EINVAL) << 3;
  }
  if (syscall2(SYS_CAP_SET_INHERIT, (long)pair.receive, 1) != 0)
    errors |= 1UL << 4;
  long child = fork();
  if (child == 0) {
    unsigned cpu1 = 2; // Affinity mask bit 1 selects CPU1, away from the parent on CPU0.
    unsigned long migration_wait = 1000000UL;
    if (syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(cpu1), (long)&cpu1) != 0 || nanosleep_ns(&migration_wait) != 0 ||
        current_cpu() != 1)
      _exit(96);
    unsigned char request[2] = {0, 0};
    unsigned long reply = 0;
    if (syscall1(SYS_CAP_CLOSE, (long)pair.send) != -IPC_EBADF)
      _exit(91);
    if (syscall6(SYS_IPC_RECEIVE, (long)pair.receive, (long)request, sizeof(request), (long)&reply, 0, 0) != 2 ||
        request[0] != 'h' || request[1] != 'i')
      _exit(92);
    if (syscall2(SYS_CAP_DUPLICATE, (long)reply, MOSS_CAP_SEND) != -IPC_EACCES)
      _exit(93);
    const unsigned char response[2] = {'o', 'k'};
    if (syscall3(SYS_IPC_REPLY, (long)reply, (long)response, sizeof(response)) != 0 ||
        syscall1(SYS_CAP_CLOSE, (long)reply) != -IPC_EBADF)
      _exit(94);
    _exit(syscall1(SYS_CAP_CLOSE, (long)pair.receive) == 0 ? 37 : 95);
  }
  if (child < 0) {
    syscall1(SYS_CAP_CLOSE, (long)pair.receive);
    syscall1(SYS_CAP_CLOSE, (long)pair.send);
    return errors | (1UL << 5);
  }
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0) << 6;
  const unsigned char request[2] = {'h', 'i'};
  unsigned char response[2] = {0, 0};
  // This bound only prevents a broken service from hanging validation; it is
  // not a latency acceptance threshold for a real control call.
  enum { CALL_TIMEOUT_NS = 5000000000UL };
  long deadline = deadline_after(CALL_TIMEOUT_NS);
  long result = deadline > 0 ? syscall6(SYS_IPC_CALL, (long)pair.send, (long)request, sizeof(request), (long)response,
                                        sizeof(response), deadline)
                             : -1;
  errors |= (unsigned long)(result != 2 || response[0] != 'o' || response[1] != 'k') << 7;
  errors |= (unsigned long)!wait_exit(child, 37) << 8;
  errors |= (unsigned long)(syscall6(SYS_IPC_CALL, (long)pair.send, (long)request, sizeof(request), (long)response,
                                     sizeof(response), 0) != -IPC_EPIPE)
            << 9;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 10;
  if (limited > 0)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, limited) != 0) << 11;
  return errors;
}

unsigned long ipc_deadline(void) {
  struct moss_ipc_endpoints pair = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0)
    return 1;
  unsigned long errors = 0;
  const unsigned char request = 7;
  unsigned char response = 0;
  errors |= (unsigned long)(syscall6(SYS_IPC_CALL, (long)pair.send, (long)&request, MOSS_IPC_MAX_MESSAGE + 1,
                                     (long)&response, 1, 0) != -IPC_EINVAL);
  // Three expired calls reuse the same bounded queue slots and timer state.
  for (unsigned i = 0; i < 3; ++i) {
    long deadline = deadline_after(1000000UL); // 1 ms is a fixture timeout, not a throughput target.
    if (deadline <= 0 ||
        syscall6(SYS_IPC_CALL, (long)pair.send, (long)&request, 1, (long)&response, 1, deadline) != -IPC_ETIMEDOUT) {
      errors |= 2;
      break;
    }
  }
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0) << 2;
  errors |=
      (unsigned long)(syscall6(SYS_IPC_CALL, (long)pair.send, (long)&request, 1, (long)&response, 1, 0) != -IPC_EPIPE)
      << 3;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 4;
  return errors;
}

unsigned long ipc_peer_death(void) {
  struct moss_ipc_endpoints pair = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0 || syscall2(SYS_CAP_SET_INHERIT, (long)pair.receive, 1) != 0)
    return 1;
  long child = fork();
  if (child == 0) {
    unsigned char request = 0;
    unsigned long reply = 0;
    if (syscall6(SYS_IPC_RECEIVE, (long)pair.receive, (long)&request, 1, (long)&reply, 0, 0) != 1 || request != 19)
      _exit(91);
    // Exit without replying: the one-shot reply capability must wake the caller.
    _exit(37);
  }
  if (child < 0)
    return 2;
  unsigned long errors = syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0;
  const unsigned char request = 19;
  unsigned char response = 0;
  long deadline = deadline_after(5000000000UL);
  errors |= (unsigned long)(deadline <= 0 || syscall6(SYS_IPC_CALL, (long)pair.send, (long)&request, 1, (long)&response,
                                                      1, deadline) != -IPC_EPIPE)
            << 1;
  errors |= (unsigned long)!wait_exit(child, 37) << 2;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 3;
  return errors;
}

unsigned long ipc_signal_cancel(void) {
  struct moss_ipc_endpoints pair = {0, 0};
  struct sigaction_t action = {(unsigned long)ipc_signal_handler, 0, 0};
  struct sigaction_t old_action = {0, 0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0)
    return 1;
  if (moss_sigaction(SIGUSR1, &action, &old_action) != 0)
    return 2;
  ipc_signal_seen = 0;
  long parent = getpid();
  long child = fork();
  if (child == 0) {
    // Give the parent time to enter the blocking call before sending SIGUSR1.
    unsigned long delay = 10000000UL;
    _exit(nanosleep_ns(&delay) == 0 && kill(parent, SIGUSR1) == 0 ? 37 : 91);
  }
  unsigned long errors = child < 0;
  if (child > 0) {
    unsigned char response = 0;
    long deadline = deadline_after(5000000000UL);
    errors |=
        (unsigned long)(deadline <= 0 ||
                        syscall6(SYS_IPC_CALL, (long)pair.send, 0, 0, (long)&response, 1, deadline) != -IPC_EINTR ||
                        ipc_signal_seen != SIGUSR1)
        << 1;
    errors |= (unsigned long)!wait_exit(child, 37) << 2;
  }
  errors |= (unsigned long)(moss_sigaction(SIGUSR1, &old_action, 0) != 0) << 3;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0) << 4;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 5;
  return errors;
}
