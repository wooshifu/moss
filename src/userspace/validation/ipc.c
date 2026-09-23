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
  IPC_ESRCH = 3,
  IPC_EINTR = 4,
  IPC_EBADF = 9,
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
  if (child < 0)
    return 2;
  unsigned long errors = syscall1(SYS_CAP_CLOSE, (long)pair.receive) != 0;
  const struct moss_ipc_message request = {.size = 1, .payload = {19}};
  struct moss_ipc_message response = {0};
  long deadline = deadline_after(ipc_call_timeout_ns);
  errors |= (unsigned long)(deadline <= 0 || ipc_call(pair.send, &request, &response, deadline) != -IPC_EPIPE) << 1;
  errors |= (unsigned long)!wait_exit(child, 37) << 2;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)pair.send) != 0) << 3;
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
  enum { DOMAIN_CHILD_SLEEP_NS = 10000000, DOMAIN_CHILD_SLEEP_CYCLES = 100 };
  unsigned long errors = (unsigned long)(syscall1(SYS_FORK_DOMAIN, 0) != -IPC_EFAULT);
  unsigned long domain = 0;
  long child = syscall1(SYS_FORK_DOMAIN, (long)&domain);
  if (child == 0) {
    if (domain != 0)
      _exit(96); // The parent's newly installed authority must not appear in the child.
    for (unsigned int attempt = 0; attempt < DOMAIN_CHILD_SLEEP_CYCLES; ++attempt) {
      unsigned long delay = DOMAIN_CHILD_SLEEP_NS;
      (void)nanosleep_ns(&delay);
    }
    _exit(97); // Bound a failed termination test instead of hanging the suite.
  }
  if (child <= 1 || !domain) {
    if (child > 1)
      (void)wait_exit(child, 97);
    if (domain)
      (void)syscall1(SYS_CAP_CLOSE, (long)domain);
    return errors | 2;
  }

  long inspect = syscall2(SYS_CAP_DUPLICATE, (long)domain, MOSS_CAP_DOMAIN_INSPECT);
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_ID, (long)domain) != child) << 2;
  errors |= (unsigned long)(inspect <= 0) << 3;
  if (inspect > 0) {
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_ID, inspect) != child) << 4;
    errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, inspect) != -IPC_EACCES) << 5;
  }
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, (long)domain) != 0) << 6;
  errors |= (unsigned long)!wait_signal(child, SIGKILL) << 7;
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, (long)domain) != -IPC_ESRCH) << 8;
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_ID, (long)domain) != child) << 9;
  if (inspect > 0)
    errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, inspect) != 0) << 10;
  errors |= (unsigned long)(syscall1(SYS_CAP_CLOSE, (long)domain) != 0) << 11;
  errors |= (unsigned long)(syscall1(SYS_DOMAIN_TERMINATE, (long)domain) != -IPC_EBADF) << 12;
  return errors;
}
