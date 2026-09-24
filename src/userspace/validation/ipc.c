#include "validation/internal.h"

// The freestanding validation image has no libc; Clang lowers zeroing the
// fixed-size IPC message aggregate to this routine.
void *memset(void *destination, int value, unsigned long count) {
  volatile unsigned char *bytes = (volatile unsigned char *)destination;
  for (unsigned long i = 0; i < count; ++i)
    bytes[i] = (unsigned char)value;
  return destination;
}

enum {
  IPC_EPERM = 1,
  IPC_ESRCH = 3,
  IPC_EINTR = 4,
  IPC_EBADF = 9,
  IPC_ECHILD = 10,
  IPC_EAGAIN = 11,
  IPC_EACCES = 13,
  IPC_EFAULT = 14,
  IPC_EINVAL = 22,
  IPC_EPIPE = 32,
  IPC_ETIMEDOUT = 110
};
static volatile int ipc_signal_seen;
// Bounds broken test services so a regression fails instead of hanging validation.
static const unsigned long ipc_call_timeout_ns = 5000000000UL;

static void ipc_signal_handler(int signo) { ipc_signal_seen = signo; }

static long deadline_after(unsigned long offset_ns) {
  unsigned long now = 0;
  return clock_gettime_ns(&now) == 0 ? (long)(now + offset_ns) : -1;
}

static long ipc_call(unsigned long endpoint, const struct moss_ipc_message *request, struct moss_ipc_message *response,
                     long deadline) {
  return syscall6(SYS_IPC_CALL, (long)endpoint, (long)request, (long)response, deadline, 0, 0);
}

static long ipc_receive(unsigned long endpoint, struct moss_ipc_message *request, unsigned long *reply) {
  return syscall3(SYS_IPC_RECEIVE, (long)endpoint, (long)request, (long)reply);
}

static long ipc_reply(unsigned long reply, const struct moss_ipc_message *response) {
  return syscall2(SYS_IPC_REPLY, (long)reply, (long)response);
}

static int domain_exited(unsigned long domain, int code, unsigned int signal) {
  struct moss_domain_exit status = {0};
  return syscall1(SYS_DOMAIN_WAIT, (long)domain) == 0 &&
         syscall2(SYS_DOMAIN_STATUS, (long)domain, (long)&status) == 0 && status.code == code &&
         status.signal == signal;
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
    struct moss_ipc_message unused = {0};
    unsigned long reply = 0;
    errors |= (unsigned long)(ipc_receive((unsigned long)limited, &unused, &reply) != -IPC_EINVAL) << 3;
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
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    if (syscall1(SYS_CAP_CLOSE, (long)pair.send) != -IPC_EBADF)
      _exit(91);
    if (ipc_receive(pair.receive, &request, &reply) != 2 || request.size != 2 || request.payload[0] != 'h' ||
        request.payload[1] != 'i' || request.capability != 0)
      _exit(92);
    if (syscall2(SYS_CAP_DUPLICATE, (long)reply, MOSS_CAP_SEND) != -IPC_EACCES)
      _exit(93);
    const struct moss_ipc_message response = {.size = 2, .payload = {'o', 'k'}};
    if (ipc_reply(reply, &response) != 0 || syscall1(SYS_CAP_CLOSE, (long)reply) != -IPC_EBADF)
      _exit(94);
    _exit(syscall1(SYS_CAP_CLOSE, (long)pair.receive) == 0 ? 37 : 95);
  }
  if (child < 0) {
    syscall1(SYS_CAP_CLOSE, (long)pair.receive);
    syscall1(SYS_CAP_CLOSE, (long)pair.send);
    return errors | (1UL << 5);
  }
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0) << 6;
  const struct moss_ipc_message request = {.size = 2, .payload = {'h', 'i'}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  long result = deadline > 0 ? ipc_call(pair.send, &request, &response, deadline) : -1;
  errors |= (unsigned long)(result != 2 || response.size != 2 || response.payload[0] != 'o' ||
                            response.payload[1] != 'k' || response.capability != 0)
            << 7;
  errors |= (unsigned long)!wait_exit(child, 37) << 8;
  errors |= (unsigned long)(ipc_call(pair.send, &request, &response, 0) != -IPC_EPIPE) << 9;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 10;
  if (limited > 0)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, limited) != 0) << 11;
  return errors;
}

unsigned long ipc_badged_sender(void) {
  enum { kTestBadge = 7 }; // Nonzero object identity; unbadged endpoints deliver zero.
  struct moss_ipc_endpoints pair = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0)
    return 1;
  long minted = syscall2(SYS_IPC_MINT_BADGE, (long)pair.send, kTestBadge);
  long limited = minted > 0 ? syscall2(SYS_CAP_DUPLICATE, minted, MOSS_CAP_SEND) : -1;
  long attenuated = syscall2(SYS_CAP_DUPLICATE, (long)pair.send, MOSS_CAP_SEND | MOSS_CAP_MINT);
  unsigned long errors = (unsigned long)(minted <= 0 || limited <= 0 || attenuated <= 0);
  if (errors || syscall2(SYS_CAP_SET_INHERIT, (long)pair.receive, 1) != 0) {
    errors |= 2;
    goto done;
  }

  long child = fork();
  if (child == 0) {
    for (unsigned int i = 0; i < 2; ++i) {
      struct moss_ipc_message request = {0};
      unsigned long reply = 0;
      unsigned long expected_badge = i == 0 ? kTestBadge : 0;
      if (ipc_receive(pair.receive, &request, &reply) != 1 || request.badge != expected_badge ||
          request.payload[0] != 'b')
        _exit(91);
      const struct moss_ipc_message response = {.size = 1, .payload = {'b'}};
      if (ipc_reply(reply, &response) != 0)
        _exit(92);
    }
    _exit(37);
  }
  if (child < 0) {
    errors |= 4;
    goto done;
  }
  (void)syscall1(SYS_CAP_CLOSE, (long)pair.receive);
  pair.receive = 0;
  errors |= (unsigned long)(syscall2(SYS_IPC_MINT_BADGE, minted, kTestBadge) != -IPC_EACCES) << 3;
  errors |= (unsigned long)(syscall2(SYS_IPC_MINT_BADGE, limited, kTestBadge) != -IPC_EACCES) << 4;
  errors |= (unsigned long)(syscall2(SYS_IPC_MINT_BADGE, attenuated, kTestBadge) != -IPC_EACCES) << 8;
  struct moss_ipc_message request = {.size = 1, .badge = kTestBadge, .payload = {'b'}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  errors |=
      (unsigned long)(deadline <= 0 || ipc_call((unsigned long)limited, &request, &response, deadline) != -IPC_EINVAL)
      << 5;
  request.badge = 0;
  deadline = deadline_after(ipc_call_timeout_ns);
  long result = deadline > 0 ? ipc_call((unsigned long)limited, &request, &response, deadline) : -1;
  errors |= (unsigned long)(result != 1 || response.payload[0] != 'b' || response.badge != 0) << 6;
  if (result == 1) {
    deadline = deadline_after(ipc_call_timeout_ns);
    result = deadline > 0 ? ipc_call(pair.send, &request, &response, deadline) : -1;
    errors |= (unsigned long)(result != 1 || response.badge != 0) << 9;
  }
  if (result != 1)
    (void)kill(child, SIGKILL);
  errors |= (unsigned long)!wait_exit(child, 37) << 7;

done:
  if (attenuated > 0)
    (void)syscall1(SYS_CAP_CLOSE, attenuated);
  if (limited > 0)
    (void)syscall1(SYS_CAP_CLOSE, limited);
  if (minted > 0)
    (void)syscall1(SYS_CAP_CLOSE, minted);
  if (pair.receive)
    (void)syscall1(SYS_CAP_CLOSE, (long)pair.receive);
  (void)syscall1(SYS_CAP_CLOSE, (long)pair.send);
  return errors;
}

static unsigned long ipc_claimed_deadline(struct moss_ipc_endpoints pair) {
  if (syscall2(SYS_CAP_SET_INHERIT, (long)pair.send, 1) != 0) {
    return 1;
  }
  long done[2];
  if (pipe(done) != 0) {
    return 2;
  }
  long child = fork();
  if (child == 0) {
    close((int)done[0]);
    const struct moss_ipc_message request = {.size = 1, .payload = {'t'}};
    struct moss_ipc_message response = {0};
    // Two seconds lets the forked caller enqueue while leaving room under
    // the five-second case watchdog to observe expiry and clean up.
    long deadline = deadline_after(2000000000UL);
    const unsigned char expired = deadline > 0 && ipc_call(pair.send, &request, &response, deadline) == -IPC_ETIMEDOUT;
    const int reported = write((int)done[1], &expired, 1) == 1;
    _exit(expired && reported ? 37 : 91);
  }
  close((int)done[1]);
  if (child < 0) {
    close((int)done[0]);
    return 4;
  }
  struct moss_ipc_message request = {0};
  unsigned long reply = 0;
  if (ipc_receive(pair.receive, &request, &reply) != 1 || request.payload[0] != 't') {
    (void)kill(child, SIGKILL);
    close((int)done[0]);
    (void)wait_exit(child, 37);
    if (reply) {
      (void)syscall1(SYS_CAP_CLOSE, (long)reply);
    }
    return 8;
  }
  unsigned char expired = 0;
  // Wait until the caller has observed expiry before attempting its reply.
  unsigned long errors = read((int)done[0], &expired, 1) != 1 || expired != 1;
  close((int)done[0]);
  const struct moss_ipc_message answer = {.size = 1, .payload = {'r'}};
  errors |= (unsigned long)(ipc_reply(reply, &answer) != -IPC_ETIMEDOUT) << 1;
  errors |= (unsigned long)!wait_exit(child, 37) << 2;
  return errors;
}

unsigned long ipc_deadline(void) {
  struct moss_ipc_endpoints pair = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0)
    return 1;
  unsigned long errors = 0;
  struct moss_ipc_message request = {.size = MOSS_IPC_MAX_MESSAGE + 1, .payload = {7}};
  struct moss_ipc_message response = {0};
  errors |= (unsigned long)(ipc_call(pair.send, &request, &response, 0) != -IPC_EINVAL);
  request.size = 1;
  // Three expired calls reuse the same bounded queue slots and timer state.
  for (unsigned i = 0; i < 3; ++i) {
    long deadline = deadline_after(1000000UL); // 1 ms is a fixture timeout, not a throughput target.
    if (deadline <= 0 || ipc_call(pair.send, &request, &response, deadline) != -IPC_ETIMEDOUT) {
      errors |= 2;
      break;
    }
  }
  if (!errors) {
    errors |= ipc_claimed_deadline(pair) << 5;
  }
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0) << 2;
  errors |= (unsigned long)(ipc_call(pair.send, &request, &response, 0) != -IPC_EPIPE) << 3;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 4;
  return errors;
}

unsigned long ipc_peer_death(void) {
  struct moss_ipc_endpoints pair = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0 || syscall2(SYS_CAP_SET_INHERIT, (long)pair.receive, 1) != 0)
    return 1;
  long child = fork();
  if (child == 0) {
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    if (ipc_receive(pair.receive, &request, &reply) != 1 || request.size != 1 || request.payload[0] != 19)
      _exit(91);
    // Exit without replying: the one-shot reply capability must wake the caller.
    _exit(37);
  }
  if (child < 0) {
    (void)syscall1(SYS_CAP_CLOSE, (long)pair.receive);
    (void)syscall1(SYS_CAP_CLOSE, (long)pair.send);
    return 2;
  }
  // Keep this receiver alive: EPIPE must come from the dead reply owner,
  // not from closing the channel's last receive capability.
  unsigned long errors = 0;
  const struct moss_ipc_message request = {.size = 1, .payload = {19}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  long result = deadline > 0 ? ipc_call(pair.send, &request, &response, deadline) : -1;
  if (result != -IPC_EPIPE) {
    (void)kill(child, SIGKILL);
  }
  errors |= (unsigned long)(result != -IPC_EPIPE) << 1;
  errors |= (unsigned long)!wait_exit(child, 37) << 2;

  if (!errors) {
    long replacement = fork();
    if (replacement == 0) {
      struct moss_ipc_message next = {0};
      unsigned long reply = 0;
      if (ipc_receive(pair.receive, &next, &reply) != 1 || next.payload[0] != 20) {
        _exit(93);
      }
      const struct moss_ipc_message answer = {.size = 1, .payload = {'r'}};
      _exit(ipc_reply(reply, &answer) == 0 ? 37 : 94);
    }
    if (replacement < 0) {
      errors |= 1UL << 4;
    } else {
      const struct moss_ipc_message next = {.size = 1, .payload = {20}};
      deadline = deadline_after(ipc_call_timeout_ns);
      result = deadline > 0 ? ipc_call(pair.send, &next, &response, deadline) : -1;
      if (result != 1) {
        (void)kill(replacement, SIGKILL);
      }
      errors |= (unsigned long)(result != 1 || response.payload[0] != 'r') << 5;
      errors |= (unsigned long)!wait_exit(replacement, 37) << 6;
    }
  }
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0);
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 3;

  pair = (struct moss_ipc_endpoints){0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0)
    return errors | (1UL << 7);
  if (syscall2(SYS_CAP_SET_INHERIT, (long)pair.send, 1) != 0) {
    (void)syscall1(SYS_CAP_CLOSE, (long)pair.receive);
    (void)syscall1(SYS_CAP_CLOSE, (long)pair.send);
    return errors | (1UL << 7);
  }
  child = fork();
  if (child == 0) {
    const struct moss_ipc_message request = {.size = 1, .payload = {20}};
    struct moss_ipc_message response = {0};
    long deadline = deadline_after(ipc_call_timeout_ns);
    _exit(deadline > 0 && ipc_call(pair.send, &request, &response, deadline) == -IPC_EPIPE ? 38 : 92);
  }
  if (child < 0) {
    errors |= 1UL << 8;
  } else {
    struct moss_ipc_message received = {0};
    unsigned long reply = 0;
    long size = ipc_receive(pair.receive, &received, &reply);
    errors |= (unsigned long)(size != 1 || received.payload[0] != 20 || !reply) << 9;
    // Keep the reply handle alive: receiver closure alone must wake the call.
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0) << 10;
    pair.receive = 0;
    errors |= (unsigned long)!wait_exit(child, 38) << 11;
    if (reply)
      errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)reply) != 0) << 12;
  }
  if (pair.receive)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0) << 13;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 14;
  return errors;
}

unsigned long ipc_nested_roundtrip(void) {
  struct moss_ipc_endpoints front = {0, 0}, back = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&front) != 0 || syscall1(SYS_IPC_CREATE, (long)&back) != 0 ||
      syscall2(SYS_CAP_SET_INHERIT, (long)front.receive, 1) != 0 ||
      syscall2(SYS_CAP_SET_INHERIT, (long)back.receive, 1) != 0 ||
      syscall2(SYS_CAP_SET_INHERIT, (long)back.send, 1) != 0)
    return 1;
  long backend = fork();
  if (backend == 0) {
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    if (ipc_receive(back.receive, &request, &reply) != 1 || request.payload[0] != 'b')
      _exit(91);
    const struct moss_ipc_message response = {.size = 1, .payload = {'c'}};
    _exit(ipc_reply(reply, &response) == 0 ? 37 : 92);
  }
  if (backend < 0)
    return 2;
  long frontend = fork();
  if (frontend == 0) {
    struct moss_ipc_message request = {0}, response = {0};
    unsigned long reply = 0;
    if (ipc_receive(front.receive, &request, &reply) != 1 || request.payload[0] != 'a')
      _exit(93);
    const struct moss_ipc_message nested = {.size = 1, .payload = {'b'}};
    long deadline = deadline_after(ipc_call_timeout_ns);
    if (deadline <= 0 || ipc_call(back.send, &nested, &response, deadline) != 1 || response.payload[0] != 'c')
      _exit(94);
    const struct moss_ipc_message delivered = {.size = 1, .payload = {'d'}};
    _exit(ipc_reply(reply, &delivered) == 0 ? 37 : 95);
  }
  if (frontend < 0) {
    kill(backend, SIGKILL);
    wait_exit(backend, 37);
    return 4;
  }
  unsigned long errors = syscall1(SYS_CAP_CLOSE, (long)front.receive) != 0;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)back.receive) != 0);
  const struct moss_ipc_message request = {.size = 1, .payload = {'a'}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  long result = deadline > 0 ? ipc_call(front.send, &request, &response, deadline) : -1;
  if (result != 1)
    kill(frontend, SIGKILL);
  errors |= (unsigned long)(result != 1 || response.payload[0] != 'd') << 1;
  errors |= (unsigned long)!wait_exit(frontend, 37) << 2;
  if (errors)
    kill(backend, SIGKILL);
  errors |= (unsigned long)!wait_exit(backend, 37) << 3;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)front.send) != 0 ||
                            syscall1(SYS_CAP_CLOSE, (long)back.send) != 0)
            << 4;
  return errors;
}

static unsigned long ipc_claimed_signal_cancel(struct moss_ipc_endpoints pair) {
  if (syscall2(SYS_CAP_SET_INHERIT, (long)pair.receive, 1) != 0) {
    return 1;
  }
  long release[2];
  if (pipe(release) != 0) {
    return 2;
  }
  const long parent = getpid();
  long child = fork();
  if (child == 0) {
    close((int)release[1]);
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    if (ipc_receive(pair.receive, &request, &reply) != 1 || request.payload[0] != 'c') {
      _exit(91);
    }
    if (kill(parent, SIGUSR1) != 0) {
      _exit(92);
    }
    unsigned char ignored = 0;
    if (read((int)release[0], &ignored, 1) != 0) {
      _exit(93);
    }
    const struct moss_ipc_message answer = {.size = 1, .payload = {'r'}};
    _exit(ipc_reply(reply, &answer) == -IPC_EPIPE ? 37 : 94);
  }
  close((int)release[0]);
  if (child < 0) {
    close((int)release[1]);
    return 4;
  }
  ipc_signal_seen = 0;
  const struct moss_ipc_message request = {.size = 1, .payload = {'c'}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  long result = deadline > 0 ? ipc_call(pair.send, &request, &response, deadline) : -1;
  const int canceled = result == -IPC_EINTR && ipc_signal_seen == SIGUSR1;
  unsigned long errors = (unsigned long)!canceled;
  // EOF releases the service only after the caller has observed cancellation.
  if (!canceled) {
    (void)kill(child, SIGKILL);
  }
  close((int)release[1]);
  errors |= (unsigned long)!wait_exit(child, 37) << 2;
  return errors;
}

static unsigned long ipc_reply_signal_races(struct moss_ipc_endpoints pair) {
  if (syscall2(SYS_CAP_SET_INHERIT, (long)pair.receive, 1) != 0) {
    return 1;
  }
  unsigned long errors = 0;
  const long parent = getpid();
  for (unsigned mode = 0; mode < 2; ++mode) {
    long outcome[2];
    if (pipe(outcome) != 0) {
      return errors | (1UL << (mode * 4));
    }
    ipc_signal_seen = 0;
    long child = fork();
    if (child == 0) {
      close((int)outcome[0]);
      struct moss_ipc_message request = {0};
      unsigned long reply = 0;
      if (ipc_receive(pair.receive, &request, &reply) != 1 || request.payload[0] != 'r') {
        _exit(91);
      }
      const struct moss_ipc_message answer = {.size = 1, .payload = {'y'}};
      long result = 0;
      if (mode == 0) {
        result = ipc_reply(reply, &answer);
        if (kill(parent, SIGUSR1) != 0) {
          _exit(92);
        }
      } else {
        if (kill(parent, SIGUSR1) != 0) {
          _exit(92);
        }
        result = ipc_reply(reply, &answer);
      }
      _exit(write((int)outcome[1], &result, sizeof(result)) == sizeof(result) ? 37 : 93);
    }
    close((int)outcome[1]);
    if (child < 0) {
      close((int)outcome[0]);
      return errors | (1UL << (mode * 4));
    }
    const struct moss_ipc_message request = {.size = 1, .payload = {'r'}};
    struct moss_ipc_message response = {0};
    long deadline = deadline_after(ipc_call_timeout_ns);
    long result = deadline > 0 ? ipc_call(pair.send, &request, &response, deadline) : -1;
    if (result != 1 && result != -IPC_EINTR) {
      (void)kill(child, SIGKILL);
    }
    long replied = -1;
    // The signal under test may interrupt either blocking operation.
    long received = 0;
    do {
      received = read((int)outcome[0], &replied, sizeof(replied));
    } while (received == -IPC_EINTR);
    const int reported = received == sizeof(replied);
    close((int)outcome[0]);
    const int reply_won = result == 1 && response.size == 1 && response.payload[0] == 'y' && replied == 0;
    const int cancel_won = result == -IPC_EINTR && replied == -IPC_EPIPE;
    // The first ordering commits reply before signal; the second allows
    // either winner, but the caller and reply holder must agree on it.
    errors |= (unsigned long)!(reply_won || (mode == 1 && cancel_won)) << (mode * 4);
    errors |= (unsigned long)!reported << (mode * 4 + 2);
    int child_status = 0;
    long waited = 0;
    do {
      waited = syscall3(SYS_WAITPID, child, (long)&child_status, 0);
    } while (waited == -IPC_EINTR);
    errors |= (unsigned long)(ipc_signal_seen != SIGUSR1) << (mode * 4 + 1);
    errors |= (unsigned long)(waited != child || ((child_status >> 8) & 255) != 37) << (mode * 4 + 3);
    if (errors) {
      break;
    }
  }
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
    const struct moss_ipc_message request = {0};
    struct moss_ipc_message response = {0};
    long deadline = deadline_after(ipc_call_timeout_ns);
    errors |= (unsigned long)(deadline <= 0 || ipc_call(pair.send, &request, &response, deadline) != -IPC_EINTR ||
                              ipc_signal_seen != SIGUSR1)
              << 1;
    errors |= (unsigned long)!wait_exit(child, 37) << 2;
  }
  if (!errors) {
    errors |= ipc_claimed_signal_cancel(pair) << 6;
  }
  if (!errors) {
    errors |= ipc_reply_signal_races(pair) << 9;
  }
  errors |= (unsigned long)(moss_sigaction(SIGUSR1, &old_action, 0) != 0) << 3;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0) << 4;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 5;
  return errors;
}

unsigned long ipc_capability_transfer(void) {
  struct moss_ipc_endpoints control = {0, 0}, delegated = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&control) != 0 || syscall1(SYS_IPC_CREATE, (long)&delegated) != 0 ||
      syscall2(SYS_CAP_SET_INHERIT, (long)control.receive, 1) != 0)
    return 1;
  long child = fork();
  if (child == 0) {
    struct moss_ipc_message request = {0};
    unsigned long control_reply = 0;
    if (ipc_receive(control.receive, &request, &control_reply) != 1 || request.payload[0] != 'a' ||
        request.capability == 0 || request.rights != MOSS_CAP_SEND ||
        syscall2(SYS_CAP_DUPLICATE, (long)request.capability, MOSS_CAP_SEND) != -IPC_EACCES)
      _exit(91);
    struct moss_ipc_endpoints returned = {0, 0};
    if (syscall1(SYS_IPC_CREATE, (long)&returned) != 0)
      _exit(92);
    const struct moss_ipc_message answer = {
        .size = 1, .capability = returned.receive, .rights = MOSS_CAP_RECEIVE, .payload = {'a'}};
    if (ipc_reply(control_reply, &answer) != 0 || syscall1(SYS_CAP_CLOSE, (long)returned.receive) != 0)
      _exit(93);
    const struct moss_ipc_message delegated_request = {.size = 1, .payload = {'b'}};
    struct moss_ipc_message delegated_response = {0};
    long deadline = deadline_after(ipc_call_timeout_ns);
    if (deadline <= 0 || ipc_call(request.capability, &delegated_request, &delegated_response, deadline) != 1 ||
        delegated_response.payload[0] != 'b')
      _exit(94);
    const struct moss_ipc_message returned_request = {.size = 1, .payload = {'c'}};
    struct moss_ipc_message returned_response = {0};
    deadline = deadline_after(ipc_call_timeout_ns);
    if (deadline <= 0 || ipc_call(returned.send, &returned_request, &returned_response, deadline) != 1 ||
        returned_response.payload[0] != 'c')
      _exit(95);
    _exit(37);
  }
  if (child < 0)
    return 2;
  unsigned long errors = syscall1(SYS_CAP_CLOSE, (long)control.receive) != 0;
  const struct moss_ipc_message request = {
      .size = 1, .capability = delegated.send, .rights = MOSS_CAP_SEND, .payload = {'a'}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  long completed = deadline > 0 ? ipc_call(control.send, &request, &response, deadline) : -1;
  errors |= (unsigned long)(completed != 1 || response.payload[0] != 'a' || response.capability == 0 ||
                            response.rights != MOSS_CAP_RECEIVE)
            << 1;
  if (completed == 1 && response.capability != 0) {
    errors |= (unsigned long)(syscall2(SYS_CAP_SET_INHERIT, (long)response.capability, 1) != -IPC_EACCES) << 2;
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)delegated.send) != 0) << 3;
    struct moss_ipc_message incoming = {0};
    unsigned long reply = 0;
    errors |= (unsigned long)(ipc_receive(delegated.receive, &incoming, &reply) != 1 || incoming.payload[0] != 'b')
              << 4;
    const struct moss_ipc_message accepted = {.size = 1, .payload = {'b'}};
    errors |= (unsigned long)(ipc_reply(reply, &accepted) != 0) << 5;
    reply = 0;
    errors |= (unsigned long)(ipc_receive(response.capability, &incoming, &reply) != 1 || incoming.payload[0] != 'c')
              << 6;
    const struct moss_ipc_message returned = {.size = 1, .payload = {'c'}};
    errors |= (unsigned long)(ipc_reply(reply, &returned) != 0) << 7;
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)response.capability) != 0) << 8;
  }
  errors |= (unsigned long)!wait_exit(child, 37) << 9;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)control.send) != 0) << 10;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)delegated.receive) != 0) << 11;
  return errors;
}

unsigned long ipc_delivery_rollback(void) {
  struct moss_ipc_endpoints control = {0, 0}, delegated = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&control) != 0 || syscall1(SYS_IPC_CREATE, (long)&delegated) != 0 ||
      syscall2(SYS_CAP_SET_INHERIT, (long)control.receive, 1) != 0)
    return 1;
  long child = fork();
  if (child == 0) {
    struct moss_ipc_message first = {0};
    // The request copyout succeeds, but the reply-handle copyout fails.
    if (ipc_receive(control.receive, &first, 0) != -IPC_EFAULT || first.size != 1 || first.capability == 0 ||
        first.rights != MOSS_CAP_SEND || syscall1(SYS_CAP_CLOSE, (long)first.capability) != -IPC_EBADF ||
        syscall2(SYS_CAP_DUPLICATE, (long)first.capability, MOSS_CAP_SEND) != -IPC_EBADF)
      _exit(91);
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    if (ipc_receive(control.receive, &request, &reply) != 1 || request.payload[0] != 'r' || request.capability == 0 ||
        request.capability == first.capability || syscall1(SYS_CAP_CLOSE, (long)request.capability) != 0)
      _exit(92);
    const struct moss_ipc_message response = {.size = 1, .payload = {'r'}};
    _exit(ipc_reply(reply, &response) == 0 ? 37 : 93);
  }
  if (child < 0)
    return 2;
  unsigned long errors = syscall1(SYS_CAP_CLOSE, (long)control.receive) != 0;
  const struct moss_ipc_message request = {
      .size = 1, .capability = delegated.send, .rights = MOSS_CAP_SEND, .payload = {'r'}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  errors |= (unsigned long)(deadline <= 0 || ipc_call(control.send, &request, &response, deadline) != 1 ||
                            response.payload[0] != 'r')
            << 1;
  errors |= (unsigned long)!wait_exit(child, 37) << 2;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)control.send) != 0) << 3;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)delegated.send) != 0) << 4;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)delegated.receive) != 0) << 5;
  return errors;
}

unsigned long ipc_memory_object(void) {
  if (syscall1(SYS_MEM_CREATE, 0) != -IPC_EINVAL)
    return 1;
  long memory = syscall1(SYS_MEM_CREATE, MOSS_MEM_OBJECT_BYTES);
  if (memory <= 0)
    return 2;
  long read_only = syscall2(SYS_CAP_DUPLICATE, memory, MOSS_CAP_MAP_READ);
  if (read_only <= 0)
    return 4;
  unsigned long errors = syscall2(SYS_MEM_MAP, read_only, MOSS_CAP_MAP_WRITE) != -IPC_EACCES;
  long writable_addr = syscall2(SYS_MEM_MAP, memory, MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE);
  long readable_addr = syscall2(SYS_MEM_MAP, read_only, MOSS_CAP_MAP_READ);
  if (writable_addr <= 0 || readable_addr <= 0)
    return errors | 2;
  volatile unsigned char *writable = (volatile unsigned char *)writable_addr;
  volatile const unsigned char *readable = (volatile const unsigned char *)readable_addr;
  writable[0] = 'm';
  errors |= (unsigned long)(readable[0] != 'm') << 1;

  struct moss_ipc_endpoints control = {0, 0};
  if (syscall1(SYS_IPC_CREATE, (long)&control) != 0 || syscall2(SYS_CAP_SET_INHERIT, (long)control.receive, 1) != 0)
    return errors | 4;
  long child = fork();
  if (child == 0) {
    // This inherited writable VMA must remain shared after fork.
    if (writable[0] != 'm')
      _exit(91);
    writable[1] = 'f';
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    if (ipc_receive(control.receive, &request, &reply) != 1 || request.capability == 0 ||
        request.rights != (MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE) ||
        syscall2(SYS_CAP_DUPLICATE, (long)request.capability, MOSS_CAP_MAP_READ) != -IPC_EACCES)
      _exit(92);
    long transferred_addr = syscall2(SYS_MEM_MAP, (long)request.capability, MOSS_CAP_MAP_WRITE);
    if (transferred_addr <= 0)
      _exit(93);
    volatile unsigned char *transferred = (volatile unsigned char *)transferred_addr;
    if (transferred[0] != 'm' || transferred[1] != 'f')
      _exit(94);
    transferred[0] = 's';
    if (syscall1(SYS_CAP_CLOSE, (long)request.capability) != 0 || transferred[0] != 's')
      _exit(95);
    const struct moss_ipc_message response = {.size = 1, .payload = {'s'}};
    _exit(ipc_reply(reply, &response) == 0 ? 37 : 96);
  }
  if (child < 0)
    return errors | 8;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)control.receive) != 0) << 3;
  const struct moss_ipc_message request = {.size = 1,
                                           .capability = (unsigned long)memory,
                                           .rights = MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE,
                                           .payload = {'m'}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  long completed = deadline > 0 ? ipc_call(control.send, &request, &response, deadline) : -1;
  if (completed != 1)
    kill(child, SIGKILL); // A failed enqueue could leave the child waiting to receive forever.
  errors |= (unsigned long)(completed != 1 || response.payload[0] != 's') << 4;
  errors |= (unsigned long)!wait_exit(child, 37) << 5;
  errors |= (unsigned long)(readable[0] != 's' || readable[1] != 'f') << 6;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, memory) != 0 || syscall1(SYS_CAP_CLOSE, read_only) != 0 ||
                            writable[0] != 's')
            << 7;
  errors |= (unsigned long)(syscall2(SYS_MUNMAP, writable_addr, MOSS_MEM_OBJECT_BYTES) != 0 ||
                            syscall2(SYS_MUNMAP, readable_addr, MOSS_MEM_OBJECT_BYTES) != 0)
            << 8;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)control.send) != 0) << 9;
  return errors;
}

unsigned long ipc_domain_control(void) {
  // A failed termination must complete with exit 97 within one second,
  // rather than leave the validation runner waiting on an immortal child.
  enum { DOMAIN_CHILD_SLEEP_NS = 10000000, DOMAIN_CHILD_SLEEP_CYCLES = 100, DOMAIN_OBSERVER_DELAY_NS = 50000000 };
  unsigned long errors = (unsigned long)(syscall1(SYS_FORK_DOMAIN, 0) != -IPC_EFAULT);
  long self = syscall0(SYS_DOMAIN_SELF);
  errors |= (unsigned long)(self <= 0) << 33;
  if (self > 0) {
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_ID, self) != getpid()) << 34;
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_WAIT, self) != -IPC_EINVAL) << 35;
    long another_self = syscall0(SYS_DOMAIN_SELF);
    errors |= (unsigned long)(another_self <= 0) << 37;
    if (another_self > 0) {
      errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, self, another_self) != 1) << 38;
      (void)syscall1(SYS_CAP_CLOSE, another_self);
    }
  }
  unsigned long domain = 0;
  long child = syscall1(SYS_FORK_DOMAIN, (long)&domain);
  if (child == 0) {
    if (domain != 0 || getppid() != 0)
      _exit(96); // Native children have neither parent authority nor POSIX parentage.
    for (unsigned int attempt = 0; attempt < DOMAIN_CHILD_SLEEP_CYCLES; ++attempt) {
      unsigned long delay = DOMAIN_CHILD_SLEEP_NS;
      (void)nanosleep_ns(&delay);
    }
    _exit(97); // Bound a failed termination test instead of hanging the suite.
  }
  if (child <= 1 || !domain) {
    if (domain) {
      (void)syscall1(SYS_DOMAIN_TERMINATE, (long)domain);
      (void)syscall1(SYS_DOMAIN_WAIT, (long)domain);
      (void)syscall1(SYS_CAP_CLOSE, (long)domain);
    }
    if (self > 0)
      errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, self) != 0) << 36;
    return errors | 2;
  }

  long inspect = syscall2(SYS_CAP_DUPLICATE, (long)domain, MOSS_CAP_DOMAIN_INSPECT);
  long observe = syscall2(SYS_CAP_DUPLICATE, (long)domain, MOSS_CAP_DOMAIN_OBSERVE);
  struct moss_domain_exit status = {0};
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_ID, (long)domain) != child) << 2;
  errors |= (unsigned long)(syscall3(SYS_WAITPID, child, 0, 1) != -IPC_ECHILD) << 24;
  errors |= (unsigned long)(syscall2(SYS_KILL, child, 0) != -IPC_EPERM) << 30;
  errors |= (unsigned long)(syscall2(SYS_KILL, child, SIGKILL) != -IPC_EPERM) << 31;
  unsigned int affinity = 1;
  errors |= (unsigned long)(syscall3(SYS_SCHED_SETAFFINITY, child, sizeof(affinity), (long)&affinity) != -IPC_EPERM)
            << 32;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_STATUS, (long)domain, (long)&status) != -IPC_EAGAIN) << 27;
  errors |= (unsigned long)(inspect <= 0) << 3;
  if (inspect > 0) {
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_ID, inspect) != child) << 4;
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, inspect) != -IPC_EACCES) << 5;
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_WAIT, inspect) != -IPC_EACCES) << 20;
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_STATUS, inspect, (long)&status) != -IPC_EACCES) << 28;
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, (long)domain, inspect) != 1) << 40;
  }
  errors |= (unsigned long)(observe <= 0) << 13;
  if (observe > 0) {
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_ID, observe) != -IPC_EACCES) << 14;
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, observe) != -IPC_EACCES) << 15;
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, (long)domain, observe) != -IPC_EACCES) << 41;
  }
  if (self > 0)
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, self, (long)domain) != 0) << 39;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, (long)domain, 0) != -IPC_EBADF) << 42;
  struct moss_ipc_endpoints pair = {0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0) {
    errors |= 1UL << 43;
  } else {
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, (long)domain, (long)pair.send) != -IPC_EINVAL) << 44;
    (void)syscall1(SYS_CAP_CLOSE, (long)pair.send);
    (void)syscall1(SYS_CAP_CLOSE, (long)pair.receive);
  }
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, (long)domain) != 0) << 6;
  if (observe > 0)
    errors |= (unsigned long)!domain_exited((unsigned long)observe, 0, SIGKILL) << 16;
  errors |= (unsigned long)!domain_exited(domain, 0, SIGKILL) << 7;
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_WAIT, (long)domain) != 0) << 17;
  errors |= (unsigned long)(syscall3(SYS_WAITPID, child, 0, 1) != -IPC_ECHILD) << 25;
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, (long)domain) != -IPC_ESRCH) << 8;
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_ID, (long)domain) != child) << 9;
  if (inspect > 0) {
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, (long)domain, inspect) != 1) << 45;
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, inspect) != 0) << 10;
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_SAME, (long)domain, inspect) != -IPC_EBADF) << 46;
  }
  if (observe > 0)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, observe) != 0) << 18;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)domain) != 0) << 11;
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, (long)domain) != -IPC_EBADF) << 12;
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_WAIT, (long)domain) != -IPC_EBADF) << 19;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_STATUS, (long)domain, (long)&status) != -IPC_EBADF) << 29;

  unsigned long natural_domain = 0;
  long natural_child = syscall1(SYS_FORK_DOMAIN, (long)&natural_domain);
  if (natural_child == 0) {
    // PID 1 denies child signals; a non-supervisor native domain must allow
    // that POSIX relationship without granting scheduling control.
    long ordinary_child = fork();
    if (ordinary_child == 0) {
      unsigned int affinity = 1;
      long parent = getppid();
      _exit(parent > 1 && syscall2(SYS_KILL, parent, 0) == 0 &&
                    syscall3(SYS_SCHED_SETAFFINITY, parent, sizeof(affinity), (long)&affinity) == -IPC_EPERM
                ? 37
                : 96);
    }
    if (!wait_exit(ordinary_child, 37))
      _exit(96);
    // This fixture delay exercises the blocking path, not a timing contract.
    unsigned long delay = DOMAIN_OBSERVER_DELAY_NS;
    if (nanosleep_ns(&delay) != 0)
      _exit(96);
    _exit(37);
  }
  if (natural_child <= 1 || !natural_domain) {
    errors |= 1UL << 21;
    if (natural_domain) {
      (void)syscall1(SYS_DOMAIN_TERMINATE, (long)natural_domain);
      (void)syscall1(SYS_DOMAIN_WAIT, (long)natural_domain);
      (void)syscall1(SYS_CAP_CLOSE, (long)natural_domain);
    }
  } else {
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_WAIT, (long)natural_domain) != 0 ||
                              syscall2(SYS_DOMAIN_STATUS, (long)natural_domain, 1) != -IPC_EFAULT)
              << 21;
    errors |= (unsigned long)!domain_exited(natural_domain, 37, 0) << 22;
    errors |= (unsigned long)(syscall3(SYS_WAITPID, natural_child, 0, 1) != -IPC_ECHILD) << 26;
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)natural_domain) != 0) << 23;
  }
  if (self > 0)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, self) != 0) << 36;

  unsigned long signal_domain = 0;
  long signal_child = syscall1(SYS_FORK_DOMAIN, (long)&signal_domain);
  if (signal_child == 0) {
    for (unsigned int attempt = 0; attempt < DOMAIN_CHILD_SLEEP_CYCLES; ++attempt) {
      unsigned long delay = DOMAIN_CHILD_SLEEP_NS;
      (void)nanosleep_ns(&delay);
    }
    _exit(97); // A missed signal must finish the test instead of hanging it.
  }
  if (signal_child <= 1 || !signal_domain) {
    errors |= 1UL << 47;
  } else {
    long signal_only = syscall2(SYS_CAP_DUPLICATE, (long)signal_domain, MOSS_CAP_DOMAIN_SIGNAL);
    errors |= (unsigned long)(signal_only <= 0) << 48;
    if (signal_only <= 0) {
      (void)syscall1(SYS_DOMAIN_TERMINATE, (long)signal_domain);
      (void)syscall1(SYS_DOMAIN_WAIT, (long)signal_domain);
    }
    if (signal_only > 0) {
      errors |= (unsigned long)(syscall1(SYS_DOMAIN_ID, signal_only) != -IPC_EACCES) << 49;
      errors |= (unsigned long)(syscall1(SYS_DOMAIN_WAIT, signal_only) != -IPC_EACCES) << 50;
      errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, signal_only) != -IPC_EACCES) << 51;
      errors |= (unsigned long)(syscall2(SYS_DOMAIN_SIGNAL, signal_only, 0) != 0) << 52;
      errors |= (unsigned long)(syscall2(SYS_DOMAIN_SIGNAL, signal_only, -1) != -IPC_EINVAL) << 53;
      errors |= (unsigned long)(syscall2(SYS_DOMAIN_SIGNAL, signal_only, SIGTERM) != 0) << 54;
      errors |= (unsigned long)(syscall1(SYS_DOMAIN_WAIT, (long)signal_domain) != 0) << 55;
      errors |= (unsigned long)!domain_exited(signal_domain, 0, SIGTERM) << 56;
      errors |= (unsigned long)(syscall2(SYS_DOMAIN_SIGNAL, signal_only, 0) != -IPC_ESRCH) << 57;
      errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, signal_only) != 0) << 58;
    }
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)signal_domain) != 0) << 59;
  }
  return errors;
}

unsigned long ipc_domain_selection(void) {
  struct moss_ipc_endpoints pair = {0};
  if (syscall1(SYS_IPC_CREATE, (long)&pair) != 0)
    return 1;
  unsigned long errors = (unsigned long)(syscall2(SYS_CAP_SET_INHERIT, (long)pair.send, 1) != 0);

  unsigned long empty_domain = 0;
  long empty_child = syscall1(SYS_FORK_DOMAIN, (long)&empty_domain);
  if (empty_child == 0)
    _exit(getppid() == 0 && syscall1(SYS_CAP_CLOSE, (long)pair.send) == -IPC_EBADF &&
                  syscall1(SYS_CAP_CLOSE, (long)pair.receive) == -IPC_EBADF
              ? 37
              : 96);
  errors |= (unsigned long)(empty_child <= 1 || !empty_domain) << 1;
  if (empty_domain)
    errors |= (unsigned long)!domain_exited(empty_domain, 37, 0) << 2;
  if (empty_child > 1)
    errors |= (unsigned long)(syscall3(SYS_WAITPID, empty_child, 0, 1) != -IPC_ECHILD) << 17;
  if (empty_domain)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)empty_domain) != 0) << 3;

  unsigned long inherited_domain = 0;
  long inherited_child = syscall1(SYS_FORK_DOMAIN_INHERIT, (long)&inherited_domain);
  if (inherited_child == 0) {
    if (getppid() != 0 || syscall1(SYS_CAP_CLOSE, (long)pair.receive) != -IPC_EBADF)
      _exit(96);
    char inherited_arg[MOSS_DECIMAL_BUFFER_SIZE];
    (void)ultoa(pair.send, inherited_arg, sizeof(inherited_arg));
    const char *args[] = {"cap-present", inherited_arg, 0};
    syscall3(SYS_EXECVE, (long)"/validation_child.elf", (long)args, 0);
    _exit(96);
  }
  errors |= (unsigned long)(inherited_child <= 1 || !inherited_domain) << 29;
  if (inherited_domain)
    errors |= (unsigned long)!domain_exited(inherited_domain, 37, 0) << 30;
  if (inherited_child > 1)
    errors |= (unsigned long)(syscall3(SYS_WAITPID, inherited_child, 0, 1) != -IPC_ECHILD) << 31;
  if (inherited_domain)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)inherited_domain) != 0) << 32;
  errors |= (unsigned long)(syscall1(SYS_FORK_DOMAIN_INHERIT, 0) != -IPC_EFAULT) << 33;

  const struct moss_fork_capability selected[] = {{pair.receive, MOSS_CAP_RECEIVE, 0}};
  unsigned long selected_domain = 0;
  long selected_child = syscall3(SYS_FORK_DOMAIN_SELECT, (long)&selected_domain, (long)selected, 1);
  if (selected_child == 0) {
    long denied_duplicate = syscall2(SYS_CAP_DUPLICATE, (long)pair.receive, MOSS_CAP_RECEIVE);
    long grandchild = syscall0(SYS_FORK);
    if (grandchild == 0)
      _exit(syscall1(SYS_CAP_CLOSE, (long)pair.receive) == -IPC_EBADF ? 37 : 96);
    if (denied_duplicate != -IPC_EACCES || grandchild <= 1 || !wait_exit(grandchild, 37) ||
        syscall1(SYS_CAP_CLOSE, (long)pair.send) != -IPC_EBADF)
      _exit(96);
    char selected_arg[MOSS_DECIMAL_BUFFER_SIZE];
    (void)ultoa(pair.receive, selected_arg, sizeof(selected_arg));
    const char *args[] = {"cap-present", selected_arg, 0};
    syscall3(SYS_EXECVE, (long)"/validation_child.elf", (long)args, 0);
    _exit(96);
  }
  errors |= (unsigned long)(selected_child <= 1 || !selected_domain) << 4;
  if (selected_domain)
    errors |= (unsigned long)!domain_exited(selected_domain, 37, 0) << 5;
  if (selected_child > 1)
    errors |= (unsigned long)(syscall3(SYS_WAITPID, selected_child, 0, 1) != -IPC_ECHILD) << 18;
  if (selected_domain)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)selected_domain) != 0) << 6;

  unsigned long rejected_domain = 0;
  const struct moss_fork_capability duplicated[] = {{pair.receive, MOSS_CAP_RECEIVE, 0},
                                                    {pair.receive, MOSS_CAP_RECEIVE, 0}};
  errors |=
      (unsigned long)(syscall3(SYS_FORK_DOMAIN_SELECT, (long)&rejected_domain, (long)duplicated, 2) != -IPC_EINVAL ||
                      rejected_domain != 0)
      << 7;
  long limited = syscall2(SYS_CAP_DUPLICATE, (long)pair.receive, MOSS_CAP_RECEIVE);
  errors |= (unsigned long)(limited <= 0) << 8;
  if (limited > 0) {
    const struct moss_fork_capability unauthorized[] = {{(unsigned long)limited, MOSS_CAP_RECEIVE, 0}};
    errors |= (unsigned long)(syscall3(SYS_FORK_DOMAIN_SELECT, (long)&rejected_domain, (long)unauthorized, 1) !=
                                  -IPC_EACCES ||
                              rejected_domain != 0)
              << 9;
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, limited) != 0) << 10;
  }
  long exec_limited = syscall2(SYS_CAP_DUPLICATE, (long)pair.receive, MOSS_CAP_RECEIVE);
  errors |= (unsigned long)(exec_limited <= 0) << 19;
  if (exec_limited > 0) {
    errors |= (unsigned long)(syscall2(SYS_CAP_SET_EXEC, exec_limited, 1) != -IPC_EACCES) << 20;
    errors |= (unsigned long)(syscall2(SYS_CAP_SET_EXEC, exec_limited, 0) != 0) << 21;
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, exec_limited) != 0) << 22;
  }
  errors |= (unsigned long)(syscall2(SYS_CAP_SET_EXEC, (long)pair.send, 2) != -IPC_EINVAL) << 23;
  errors |= (unsigned long)(syscall2(SYS_CAP_SET_EXEC, 0, 1) != -IPC_EBADF) << 24;
  errors |= (unsigned long)(syscall2(SYS_CAP_SET_INHERIT, (long)pair.receive, 1) != 0) << 25;
  errors |= (unsigned long)(syscall2(SYS_CAP_SET_EXEC, (long)pair.send, 0) != 0) << 26;
  errors |= (unsigned long)(syscall2(SYS_CAP_SET_EXEC, (long)pair.receive, 0) != 0 ||
                            syscall2(SYS_CAP_SET_EXEC, (long)pair.receive, 1) != 0)
            << 27;
  long exec_child = fork();
  if (exec_child == 0) {
    if (syscall2(SYS_CAP_SET_EXEC, (long)pair.send, 0) != 0)
      _exit(96);
    char closed_arg[MOSS_DECIMAL_BUFFER_SIZE], kept_arg[MOSS_DECIMAL_BUFFER_SIZE];
    (void)ultoa(pair.send, closed_arg, sizeof(closed_arg));
    (void)ultoa(pair.receive, kept_arg, sizeof(kept_arg));
    const char *args[] = {"cap-exec", closed_arg, kept_arg, 0};
    syscall3(SYS_EXECVE, (long)"/validation_child.elf", (long)args, 0);
    _exit(96);
  }
  errors |= (unsigned long)(exec_child <= 1 || !wait_exit(exec_child, 37)) << 28;
  errors |= (unsigned long)(syscall3(SYS_FORK_DOMAIN_SELECT, (long)&rejected_domain, 0, 1) != -IPC_EFAULT) << 11;
  // A bit outside the published selection flags must be rejected.
  const struct moss_fork_capability invalid_flags[] = {{pair.receive, MOSS_CAP_RECEIVE, MOSS_FORK_CAP_INHERIT << 1}};
  errors |=
      (unsigned long)(syscall3(SYS_FORK_DOMAIN_SELECT, (long)&rejected_domain, (long)invalid_flags, 1) != -IPC_EINVAL ||
                      rejected_domain != 0)
      << 14;
  const struct moss_fork_capability unauthorized_inherit[] = {{pair.receive, MOSS_CAP_RECEIVE, MOSS_FORK_CAP_INHERIT}};
  errors |= (unsigned long)(syscall3(SYS_FORK_DOMAIN_SELECT, (long)&rejected_domain, (long)unauthorized_inherit, 1) !=
                                -IPC_EACCES ||
                            rejected_domain != 0)
            << 15;
  const struct moss_fork_capability excess_rights[] = {{pair.receive, MOSS_CAP_RECEIVE | MOSS_CAP_MINT, 0}};
  errors |=
      (unsigned long)(syscall3(SYS_FORK_DOMAIN_SELECT, (long)&rejected_domain, (long)excess_rights, 1) != -IPC_EACCES ||
                      rejected_domain != 0)
      << 16;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0) << 12;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 13;

  // The two-second fallback bounds a missed termination; the one-second
  // parent poll leaves time to observe both live generations first.
  enum { SCOPE_CHILD_DELAY_NS = 10000000, SCOPE_CHILD_CYCLES = 200, SCOPE_STATUS_CYCLES = 100 };
  long scope = syscall0(SYS_DOMAIN_SCOPE_CREATE);
  errors |= (unsigned long)(scope <= 0) << 34;
  if (scope > 0) {
    long self = syscall0(SYS_DOMAIN_SELF);
    errors |= (unsigned long)(self <= 0) << 49;
    if (self > 0) {
      errors |= (unsigned long)(syscall2(SYS_DOMAIN_SCOPE_CONTAINS, scope, self) != 0) << 50;
      (void)syscall1(SYS_CAP_CLOSE, self);
    }
    long inspect = syscall2(SYS_CAP_DUPLICATE, scope, MOSS_CAP_DOMAIN_SCOPE_INSPECT);
    errors |= (unsigned long)(inspect <= 0) << 35;
    if (inspect > 0) {
      unsigned long rejected = 0;
      errors |= (unsigned long)(syscall1(SYS_DOMAIN_SCOPE_STATUS, inspect) != 0) << 36;
      errors |= (unsigned long)(syscall1(SYS_DOMAIN_SCOPE_TERMINATE, inspect) != -IPC_EACCES) << 37;
      errors |= (unsigned long)(syscall6(SYS_FORK_DOMAIN_SCOPED, (long)&rejected, 0, 0, inspect, 0, 0) != -IPC_EACCES ||
                                rejected != 0)
                << 38;
      (void)syscall1(SYS_CAP_CLOSE, inspect);
    }

    long other = syscall0(SYS_DOMAIN_SCOPE_CREATE);
    errors |= (unsigned long)(other <= 0) << 39;
    if (other > 0) {
      const struct moss_fork_capability alternative[] = {{(unsigned long)other, MOSS_CAP_DOMAIN_SCOPE_ASSIGN, 0}};
      unsigned long escape_domain = 0;
      long escape = syscall6(SYS_FORK_DOMAIN_SCOPED, (long)&escape_domain, (long)alternative, 1, scope, 0, 0);
      if (escape == 0) {
        unsigned long rejected = 0;
        long result = syscall6(SYS_FORK_DOMAIN_SCOPED, (long)&rejected, 0, 0, other, 0, 0);
        _exit(result == -IPC_EACCES && rejected == 0 ? 37 : 96);
      }
      errors |= (unsigned long)(escape <= 1 || !escape_domain) << 40;
      if (escape_domain) {
        errors |= (unsigned long)!domain_exited(escape_domain, 37, 0) << 41;
        (void)syscall1(SYS_CAP_CLOSE, (long)escape_domain);
      }
      (void)syscall1(SYS_CAP_CLOSE, other);
    }

    unsigned long live_domain = 0;
    long live = syscall6(SYS_FORK_DOMAIN_SCOPED, (long)&live_domain, 0, 0, scope, 0, 0);
    if (live == 0) {
      unsigned long grandchild_domain = 0;
      long grandchild = syscall1(SYS_FORK_DOMAIN, (long)&grandchild_domain);
      if (grandchild == 0) {
        for (unsigned int i = 0; i < SCOPE_CHILD_CYCLES; ++i) {
          unsigned long delay = SCOPE_CHILD_DELAY_NS;
          (void)nanosleep_ns(&delay);
        }
        _exit(97);
      }
      if (grandchild <= 1 || !grandchild_domain)
        _exit(96);
      (void)syscall1(SYS_CAP_CLOSE, (long)grandchild_domain);
      for (unsigned int i = 0; i < SCOPE_CHILD_CYCLES; ++i) {
        unsigned long delay = SCOPE_CHILD_DELAY_NS;
        (void)nanosleep_ns(&delay);
      }
      _exit(97);
    }
    errors |= (unsigned long)(live <= 1 || !live_domain) << 42;
    if (live_domain) {
      errors |= (unsigned long)(syscall2(SYS_DOMAIN_SCOPE_CONTAINS, scope, (long)live_domain) != 1) << 51;
      long observe = syscall2(SYS_CAP_DUPLICATE, (long)live_domain, MOSS_CAP_DOMAIN_OBSERVE);
      errors |= (unsigned long)(observe <= 0) << 52;
      if (observe > 0) {
        errors |= (unsigned long)(syscall2(SYS_DOMAIN_SCOPE_CONTAINS, scope, observe) != -IPC_EACCES) << 53;
        (void)syscall1(SYS_CAP_CLOSE, observe);
      }
    }
    long members = syscall1(SYS_DOMAIN_SCOPE_STATUS, scope);
    for (unsigned int i = 0; members >= 0 && members < 2 && i < SCOPE_STATUS_CYCLES; ++i) {
      unsigned long delay = SCOPE_CHILD_DELAY_NS;
      (void)nanosleep_ns(&delay);
      members = syscall1(SYS_DOMAIN_SCOPE_STATUS, scope);
    }
    errors |= (unsigned long)(members != 2) << 43;
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_SCOPE_TERMINATE, scope) != 0) << 44;
    if (live_domain) {
      errors |= (unsigned long)!domain_exited(live_domain, 0, SIGKILL) << 45;
      (void)syscall1(SYS_CAP_CLOSE, (long)live_domain);
    }
    members = syscall1(SYS_DOMAIN_SCOPE_STATUS, scope);
    for (unsigned int i = 0; members > 0 && i < SCOPE_STATUS_CYCLES; ++i) {
      unsigned long delay = SCOPE_CHILD_DELAY_NS;
      (void)nanosleep_ns(&delay);
      members = syscall1(SYS_DOMAIN_SCOPE_STATUS, scope);
    }
    errors |= (unsigned long)(members != 0) << 46;
    unsigned long rejected = 0;
    errors |= (unsigned long)(syscall6(SYS_FORK_DOMAIN_SCOPED, (long)&rejected, 0, 0, scope, 0, 0) != -IPC_EACCES ||
                              rejected != 0)
              << 47;
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, scope) != 0) << 48;
  }
  return errors;
}

unsigned long ipc_domain_wait_any(void) {
  enum { CHILD_DELAY_NS = 10000000, SLOW_CHILD_CYCLES = 100, FAST_CHILD_CYCLES = 5 };
  unsigned long slow_domain = 0;
  long slow = syscall1(SYS_FORK_DOMAIN, (long)&slow_domain);
  if (slow == 0) {
    for (unsigned int i = 0; i < SLOW_CHILD_CYCLES; ++i) {
      unsigned long delay = CHILD_DELAY_NS;
      (void)nanosleep_ns(&delay);
    }
    _exit(97); // A failed termination must not hang validation indefinitely.
  }
  if (slow <= 1 || !slow_domain)
    return 1;

  unsigned long fast_domain = 0;
  long fast = syscall1(SYS_FORK_DOMAIN, (long)&fast_domain);
  if (fast == 0) {
    for (unsigned int i = 0; i < FAST_CHILD_CYCLES; ++i) {
      unsigned long delay = CHILD_DELAY_NS;
      (void)nanosleep_ns(&delay);
    }
    _exit(37);
  }
  if (fast <= 1 || !fast_domain) {
    (void)syscall1(SYS_DOMAIN_TERMINATE, (long)slow_domain);
    (void)syscall1(SYS_DOMAIN_WAIT, (long)slow_domain);
    (void)syscall1(SYS_CAP_CLOSE, (long)slow_domain);
    return 2;
  }

  unsigned long errors = 0;
  const unsigned long domains[] = {slow_domain, fast_domain};
  struct moss_domain_exit status = {0};
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)domains, 2) != 1) << 0;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_STATUS, (long)fast_domain, (long)&status) != 0) << 20;
  errors |= (unsigned long)(status.code != 37 || status.signal != 0) << 1;
  errors |= (unsigned long)(syscall3(SYS_WAITPID, fast, 0, 1) != -IPC_ECHILD) << 17;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_STATUS, (long)fast_domain, 1) != -IPC_EFAULT) << 18;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)domains, 0) != -IPC_EINVAL) << 2;
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, 0, 1) != -IPC_EFAULT) << 3;
  const unsigned long bad[] = {0};
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)bad, 1) != -IPC_EBADF) << 4;

  long inspect = syscall2(SYS_CAP_DUPLICATE, (long)slow_domain, MOSS_CAP_DOMAIN_INSPECT);
  long observe = syscall2(SYS_CAP_DUPLICATE, (long)fast_domain, MOSS_CAP_DOMAIN_OBSERVE);
  errors |= (unsigned long)(inspect <= 0 || observe <= 0) << 5;
  if (inspect > 0) {
    const unsigned long unauthorized[] = {(unsigned long)inspect};
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)unauthorized, 1) != -IPC_EACCES) << 6;
  }
  if (observe > 0) {
    const unsigned long duplicate_target[] = {fast_domain, (unsigned long)observe};
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)duplicate_target, 2) != -IPC_EINVAL) << 7;
    errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)&observe, 1) != 0) << 8;
  }

  errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, (long)slow_domain) != 0) << 9;
  const unsigned long slow_only[] = {slow_domain};
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)slow_only, 1) != 0) << 10;
  errors |= (unsigned long)!domain_exited(slow_domain, 0, SIGKILL) << 11;
  status = (struct moss_domain_exit){0};
  errors |= (unsigned long)(syscall2(SYS_DOMAIN_WAIT_ANY, (long)domains, 2) != 0 ||
                            syscall2(SYS_DOMAIN_STATUS, (long)slow_domain, (long)&status) != 0 || status.code != 0 ||
                            status.signal != SIGKILL)
            << 16;
  errors |= (unsigned long)(syscall3(SYS_WAITPID, slow, 0, 1) != -IPC_ECHILD) << 19;
  if (inspect > 0)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, inspect) != 0) << 12;
  if (observe > 0)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, observe) != 0) << 13;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)slow_domain) != 0) << 14;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)fast_domain) != 0) << 15;
  return errors;
}
