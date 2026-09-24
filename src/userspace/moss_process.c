#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_process_protocol.h"
#include "syscall.h"

extern char **environ;

// Boot validation must fail within a bounded time if the service stops
// replying or a child never publishes its exit status.
static const unsigned long process_call_timeout_ns = 5000000000UL;
static const unsigned long status_retry_ns = 10000000UL;
enum { STATUS_RETRIES = 500 };

static int error(void) {
  static const char message[] = "MOSS_PROCESS_ERROR\n";
  (void)write(STDERR_FILENO, message, sizeof(message) - 1);
  return 1;
}

static long call(unsigned long endpoint, const struct moss_ipc_message *request, struct moss_ipc_message *response) {
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) != 0 || now > LONG_MAX - process_call_timeout_ns)
    return -1;
  return syscall6(SYS_IPC_CALL, (long)endpoint, (long)request, (long)response, (long)(now + process_call_timeout_ns), 0,
                  0);
}

static int no_capability(struct moss_ipc_message *response) {
  if (response->capability) {
    (void)syscall1(SYS_CAP_CLOSE, (long)response->capability);
    return 0;
  }
  return response->rights == 0;
}

static int new_session(unsigned long endpoint, unsigned long domain, unsigned char operation, unsigned long *id,
                       unsigned long *session) {
  if (!domain && operation != MOSS_PROCESS_PREPARE_CHILD)
    return 0;
  struct moss_ipc_message request = {
      .size = 1,
      .capability = domain,
      .rights = domain ? MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT | MOSS_CAP_DOMAIN_SIGNAL : 0,
      .payload = {operation},
  };
  struct moss_ipc_message response = {0};
  long result = call(endpoint, &request, &response);
  if (domain)
    (void)syscall1(SYS_CAP_CLOSE, (long)domain);
  if (result != MOSS_PROCESS_REPLY_VALUE_BYTES || response.payload[0] != MOSS_PROCESS_OK || !response.capability ||
      response.rights != (MOSS_CAP_SEND | MOSS_CAP_DUPLICATE)) {
    if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    return 0;
  }
  *id = moss_process_get_u64(response.payload + 1);
  *session = response.capability;
  return 1;
}

static int child_record_call(unsigned long parent_session, unsigned char operation, unsigned long child_id,
                             unsigned long domain, unsigned char expected) {
  struct moss_ipc_message request = {
      .size = MOSS_PROCESS_REPLY_VALUE_BYTES,
      .capability = domain,
      .rights = domain ? MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT | MOSS_CAP_DOMAIN_SIGNAL : 0,
      .payload = {operation},
  };
  moss_process_put_u64(request.payload + 1, child_id);
  struct moss_ipc_message response = {0};
  long result = call(parent_session, &request, &response);
  int clean = no_capability(&response);
  return result == 1 && response.payload[0] == expected && clean;
}

static int session_result(unsigned long session, unsigned char operation) {
  struct moss_ipc_message request = {.size = 1, .payload = {operation}};
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  int clean = no_capability(&response);
  return result == 1 && clean ? response.payload[0] : -1;
}

static int observe_child(unsigned long root, unsigned long *last_id, int exit_code) {
  unsigned long domain = 0;
  long child = syscall1(SYS_FORK_DOMAIN, (long)&domain);
  if (child == 0)
    _exit(exit_code);
  if (child <= 1 || !domain) {
    if (domain)
      (void)syscall1(SYS_CAP_CLOSE, (long)domain);
    return 0;
  }

  unsigned long id = 0, session = 0;
  if (!new_session(root, domain, MOSS_PROCESS_REGISTER, &id, &session))
    return 0;
  int valid = id > *last_id && syscall3(SYS_WAITPID, child, 0, 1) == -ECHILD;
  *last_id = id;
  // The root sender carries no record badge. Supplying the numeric ID in a
  // request must not turn that ambient value into observation authority.
  struct moss_ipc_message request = {.size = MOSS_PROCESS_REPLY_VALUE_BYTES, .payload = {MOSS_PROCESS_STATUS}};
  moss_process_put_u64(request.payload + 1, id);
  struct moss_ipc_message response = {0};
  long result = call(root, &request, &response);
  int clean = no_capability(&response);
  valid &= result == 1 && response.payload[0] == MOSS_PROCESS_BAD_REQUEST && clean;
  request = (struct moss_ipc_message){.size = 1, .payload = {MOSS_PROCESS_STATUS}};
  for (unsigned int retry = 0; valid && retry < STATUS_RETRIES; ++retry) {
    response = (struct moss_ipc_message){0};
    result = call(session, &request, &response);
    if (!no_capability(&response)) {
      valid = 0;
      break;
    }
    if (result == MOSS_PROCESS_REPLY_VALUE_BYTES && response.payload[0] == MOSS_PROCESS_EXITED) {
      uint64_t value = moss_process_get_u64(response.payload + 1);
      valid = (uint32_t)value == (uint32_t)exit_code && value >> 32 == 0;
      break;
    }
    if (result != 1 || response.payload[0] != MOSS_PROCESS_RUNNING) {
      valid = 0;
      break;
    }
    unsigned long delay = status_retry_ns;
    if (syscall1(SYS_NANOSLEEP, (long)&delay) != 0) {
      valid = 0;
      break;
    }
    if (retry == STATUS_RETRIES - 1)
      valid = 0;
  }
  request.payload[0] = MOSS_PROCESS_RELEASE;
  response = (struct moss_ipc_message){0};
  result = call(session, &request, &response);
  clean = no_capability(&response);
  valid &= result == 1 && response.payload[0] == MOSS_PROCESS_OK && clean;
  request.payload[0] = MOSS_PROCESS_STATUS;
  response = (struct moss_ipc_message){0};
  result = call(session, &request, &response);
  clean = no_capability(&response);
  valid &= result == 1 && response.payload[0] == MOSS_PROCESS_NO_ENTRY && clean;
  (void)syscall1(SYS_CAP_CLOSE, (long)session);
  return valid;
}

static int observe_family(unsigned long root, unsigned long *last_id) {
  unsigned long parent_session = getauxval(MOSS_AT_STARTUP_CAP);
  const int borrowed_session = parent_session != 0;
  unsigned long parent_id = 0;
  if (borrowed_session) {
    struct moss_ipc_message identity = {.size = 1, .payload = {MOSS_PROCESS_IDENTITY}};
    struct moss_ipc_message response = {0};
    long result = call(parent_session, &identity, &response);
    int clean = no_capability(&response);
    if (result != MOSS_PROCESS_REPLY_IDENTITY_BYTES || response.payload[0] != MOSS_PROCESS_OK || !clean)
      return 0;
    parent_id = moss_process_get_u64(response.payload + 1);
  } else {
    long self = syscall0(SYS_DOMAIN_SELF);
    if (self <= 0 || !new_session(root, (unsigned long)self, MOSS_PROCESS_REGISTER, &parent_id, &parent_session))
      return 0;
  }
  int valid = parent_id && (borrowed_session || parent_id > *last_id);
  if (!borrowed_session)
    *last_id = parent_id;
  unsigned long cancelled_id = 0, cancelled_session = 0;
  if (!new_session(parent_session, 0, MOSS_PROCESS_PREPARE_CHILD, &cancelled_id, &cancelled_session))
    valid = 0;
  if (cancelled_session) {
    valid &= cancelled_id > *last_id;
    *last_id = cancelled_id;
    struct moss_ipc_message request = {.size = 1, .payload = {MOSS_PROCESS_IDENTITY}};
    struct moss_ipc_message response = {0};
    long result = call(cancelled_session, &request, &response);
    int clean = no_capability(&response);
    valid &= result == MOSS_PROCESS_REPLY_IDENTITY_BYTES && response.payload[0] == MOSS_PROCESS_OK &&
             moss_process_get_u64(response.payload + 1) == cancelled_id &&
             moss_process_get_u64(response.payload + 9) == parent_id && clean;
    valid &= session_result(cancelled_session, MOSS_PROCESS_READY) == MOSS_PROCESS_RUNNING;
    request.payload[0] = MOSS_PROCESS_WAIT_ANY;
    response = (struct moss_ipc_message){0};
    result = call(parent_session, &request, &response);
    clean = no_capability(&response);
    valid &= result == 1 && response.payload[0] == MOSS_PROCESS_RUNNING && clean;
    valid &= child_record_call(parent_session, MOSS_PROCESS_WAIT_CHILD, cancelled_id, 0, MOSS_PROCESS_RUNNING);
    valid &= child_record_call(parent_session, MOSS_PROCESS_CANCEL_CHILD, cancelled_id, 0, MOSS_PROCESS_OK);
    valid &= child_record_call(parent_session, MOSS_PROCESS_WAIT_CHILD, cancelled_id, 0, MOSS_PROCESS_NO_ENTRY);
    request.payload[0] = MOSS_PROCESS_IDENTITY;
    response = (struct moss_ipc_message){0};
    result = call(cancelled_session, &request, &response);
    clean = no_capability(&response);
    valid &= result == 1 && response.payload[0] == MOSS_PROCESS_NO_ENTRY && clean;
    (void)syscall1(SYS_CAP_CLOSE, (long)cancelled_session);
  }
  unsigned long child_ids[2] = {0};
  unsigned long child_sessions[2] = {0};
  const int exit_codes[2] = {39, 40};
  long pipe_fds[2] = {-1, -1};
  if (syscall1(SYS_PIPE, (long)pipe_fds) != 0)
    valid = 0;
  for (unsigned int i = 0; valid && i < 2; ++i) {
    if (!new_session(parent_session, 0, MOSS_PROCESS_PREPARE_CHILD, &child_ids[i], &child_sessions[i])) {
      valid = 0;
      break;
    }
    if (child_ids[i] <= *last_id || syscall2(SYS_CAP_SET_INHERIT, (long)child_sessions[i], 1) != 0) {
      (void)child_record_call(parent_session, MOSS_PROCESS_CANCEL_CHILD, child_ids[i], 0, MOSS_PROCESS_OK);
      valid = 0;
      break;
    }
    *last_id = child_ids[i];
    unsigned long domain = 0;
    long child = syscall1(SYS_FORK_DOMAIN_INHERIT, (long)&domain);
    if (child == 0) {
      // This raw child uses only its reserved badge; it must not retain the
      // managed parent's session inherited by the native fork.
      if (borrowed_session)
        (void)syscall1(SYS_CAP_CLOSE, (long)parent_session);
      long self = syscall0(SYS_DOMAIN_SELF);
      int attached = self > 0 && child_record_call(child_sessions[i], MOSS_PROCESS_ATTACH_CHILD, child_ids[i],
                                                   (unsigned long)self, MOSS_PROCESS_OK);
      if (self > 0)
        (void)syscall1(SYS_CAP_CLOSE, self);
      if (!attached || session_result(child_sessions[i], MOSS_PROCESS_READY) != MOSS_PROCESS_OK)
        _exit(41);
      struct moss_ipc_message identity = {.size = 1, .payload = {MOSS_PROCESS_IDENTITY}};
      struct moss_ipc_message response = {0};
      long result = call(child_sessions[i], &identity, &response);
      int clean = no_capability(&response);
      if (syscall0(SYS_GETPPID) != 0 || result != MOSS_PROCESS_REPLY_IDENTITY_BYTES ||
          response.payload[0] != MOSS_PROCESS_OK || moss_process_get_u64(response.payload + 1) != child_ids[i] ||
          moss_process_get_u64(response.payload + 9) != parent_id || !clean)
        _exit(41);
      (void)syscall1(SYS_CLOSE, pipe_fds[1]);
      if (i == 0) {
        // Keep the first child running while the second exits. WAIT_ANY must
        // scan past that live child instead of hiding the second one's exit.
        unsigned char wake = 0;
        long read = syscall3(SYS_READ, pipe_fds[0], (long)&wake, 1);
        _exit(read == 1 && wake == 1 ? exit_codes[0] : 41);
      }
      _exit(exit_codes[1]);
    }
    // The child's table has its own flag; the next fork must not inherit this
    // child's identity through the parent's copy.
    valid &= syscall2(SYS_CAP_SET_INHERIT, (long)child_sessions[i], 0) == 0;
    if (i == 0 && pipe_fds[0] >= 0) {
      (void)syscall1(SYS_CLOSE, pipe_fds[0]);
      pipe_fds[0] = -1;
    }
    if (child <= 1 || !domain) {
      if (domain)
        (void)syscall1(SYS_CAP_CLOSE, (long)domain);
      (void)child_record_call(parent_session, MOSS_PROCESS_CANCEL_CHILD, child_ids[i], 0, MOSS_PROCESS_OK);
      valid = 0;
      break;
    }
    int attached = child_record_call(parent_session, MOSS_PROCESS_ATTACH_CHILD, child_ids[i], domain, MOSS_PROCESS_OK);
    if (!attached) {
      (void)syscall1(SYS_DOMAIN_TERMINATE, (long)domain);
      (void)child_record_call(parent_session, MOSS_PROCESS_CANCEL_CHILD, child_ids[i], 0, MOSS_PROCESS_OK);
    }
    (void)syscall1(SYS_CAP_CLOSE, (long)domain);
    if (!attached ||
        !child_record_call(parent_session, MOSS_PROCESS_CANCEL_CHILD, child_ids[i], 0, MOSS_PROCESS_NO_ENTRY)) {
      valid = 0;
      break;
    }
    valid &= session_result(child_sessions[i], MOSS_PROCESS_READY) == MOSS_PROCESS_OK;
    struct moss_ipc_message release = {.size = 1, .payload = {MOSS_PROCESS_RELEASE}};
    struct moss_ipc_message response = {0};
    long result = call(child_sessions[i], &release, &response);
    int clean = no_capability(&response);
    valid &= result == 1 && response.payload[0] == MOSS_PROCESS_BUSY && clean;
  }

  struct moss_ipc_message request = {.size = 1, .payload = {MOSS_PROCESS_RELEASE}};
  struct moss_ipc_message response = {0};
  long result = call(parent_session, &request, &response);
  int clean = no_capability(&response);
  valid &= result == 1 && response.payload[0] == MOSS_PROCESS_BUSY && clean;

  request = (struct moss_ipc_message){.size = MOSS_PROCESS_REPLY_VALUE_BYTES, .payload = {MOSS_PROCESS_WAIT_ANY}};
  moss_process_put_u64(request.payload + 1, parent_id);
  response = (struct moss_ipc_message){0};
  result = call(root, &request, &response);
  clean = no_capability(&response);
  valid &= result == 1 && response.payload[0] == MOSS_PROCESS_BAD_REQUEST && clean;

  valid &= child_record_call(parent_session, MOSS_PROCESS_WAIT_CHILD, child_ids[0], 0, MOSS_PROCESS_RUNNING);
  valid &= child_record_call(child_sessions[0], MOSS_PROCESS_WAIT_CHILD, child_ids[1], 0, MOSS_PROCESS_NO_ENTRY);
  request = (struct moss_ipc_message){.size = 1, .payload = {MOSS_PROCESS_WAIT_ANY}};
  for (unsigned int reaped = 0, retry = 0; valid && reaped < 2 && retry < STATUS_RETRIES; ++retry) {
    struct moss_ipc_message wait_request = request;
    if (reaped == 1) {
      wait_request.size = MOSS_PROCESS_REPLY_VALUE_BYTES;
      wait_request.payload[0] = MOSS_PROCESS_WAIT_CHILD;
      moss_process_put_u64(wait_request.payload + 1, child_ids[0]);
    }
    response = (struct moss_ipc_message){0};
    result = call(parent_session, &wait_request, &response);
    if (!no_capability(&response)) {
      valid = 0;
      break;
    }
    if (result == MOSS_PROCESS_REPLY_WAIT_BYTES && response.payload[0] == MOSS_PROCESS_EXITED) {
      uint64_t id = moss_process_get_u64(response.payload + 1);
      uint64_t status = moss_process_get_u64(response.payload + 9);
      unsigned int index = reaped == 0 ? 1 : 0;
      if (id != child_ids[index] || !child_sessions[index] || status != (uint32_t)exit_codes[index]) {
        valid = 0;
        break;
      }
      struct moss_ipc_message stale_request = {.size = 1, .payload = {MOSS_PROCESS_STATUS}};
      struct moss_ipc_message stale_response = {0};
      long stale_result = call(child_sessions[index], &stale_request, &stale_response);
      int stale_clean = no_capability(&stale_response);
      valid = stale_result == 1 && stale_response.payload[0] == MOSS_PROCESS_NO_ENTRY && stale_clean;
      (void)syscall1(SYS_CAP_CLOSE, (long)child_sessions[index]);
      child_sessions[index] = 0;
      if (reaped == 0) {
        const unsigned char wake = 1;
        valid &= syscall3(SYS_WRITE, pipe_fds[1], (long)&wake, 1) == 1;
        (void)syscall1(SYS_CLOSE, pipe_fds[1]);
        pipe_fds[1] = -1;
      }
      ++reaped;
      continue;
    }
    if (result != 1 || response.payload[0] != MOSS_PROCESS_RUNNING) {
      valid = 0;
      break;
    }
    unsigned long delay = status_retry_ns;
    if (syscall1(SYS_NANOSLEEP, (long)&delay) != 0)
      valid = 0;
  }
  if (valid) {
    response = (struct moss_ipc_message){0};
    result = call(parent_session, &request, &response);
    clean = no_capability(&response);
    valid = result == 1 && response.payload[0] == MOSS_PROCESS_NO_ENTRY && clean;
  }
  if (pipe_fds[0] >= 0)
    (void)syscall1(SYS_CLOSE, pipe_fds[0]);
  if (pipe_fds[1] >= 0)
    (void)syscall1(SYS_CLOSE, pipe_fds[1]);
  for (unsigned int i = 0; i < 2; ++i) {
    if (!child_sessions[i])
      continue;
    request.payload[0] = MOSS_PROCESS_RELEASE;
    response = (struct moss_ipc_message){0};
    (void)call(child_sessions[i], &request, &response);
    (void)no_capability(&response);
    (void)syscall1(SYS_CAP_CLOSE, (long)child_sessions[i]);
  }
  if (!borrowed_session) {
    request.payload[0] = MOSS_PROCESS_RELEASE;
    response = (struct moss_ipc_message){0};
    result = call(parent_session, &request, &response);
    clean = no_capability(&response);
    valid &= result == 1 && response.payload[0] == MOSS_PROCESS_OK && clean;
    (void)syscall1(SYS_CAP_CLOSE, (long)parent_session);
  }
  return valid;
}

static int managed_libc_probe(void) {
  unsigned long session = getauxval(MOSS_AT_STARTUP_CAP);
  pid_t parent_id = getpid();
  if (!session || parent_id <= 0 || getppid() <= 0)
    return 0;
  errno = 0;
  if (kill(parent_id, 0) != 0 || kill(getppid(), 0) != -1 || errno != ESRCH)
    return 0;
  pid_t child = fork();
  if (child == 0) {
    pid_t child_id = getpid();
    errno = 0;
    if (child_id <= 0 || getppid() != parent_id || syscall1(SYS_CAP_CLOSE, (long)session) != -EBADF ||
        kill(parent_id, 0) != -1 || errno != ESRCH || kill(child_id, 0) != 0)
      _exit(41);
    char child_text[21], parent_text[21]; // Decimal 64-bit values plus NUL.
    if (snprintf(child_text, sizeof(child_text), "%lu", (unsigned long)child_id) <= 0 ||
        snprintf(parent_text, sizeof(parent_text), "%lu", (unsigned long)parent_id) <= 0)
      _exit(42);
    char *const args[] = {"moss-process", "libc-child", child_text, parent_text, NULL};
    execve("/moss-process.elf", args, NULL);
    _exit(42);
  }
  int status = 0;
  if (child <= 0 || waitpid(child, &status, 0) != child || status != (37 << 8) ||
      waitpid(child, &status, WNOHANG) != -1 || errno != ECHILD)
    return 0;
  pid_t signaled = fork();
  if (signaled == 0) {
    unsigned long delay = 1000000000UL;
    (void)syscall1(SYS_NANOSLEEP, (long)&delay);
    _exit(97); // Bound a lost-signal regression instead of hanging the probe.
  }
  return signaled > 0 && kill(signaled, SIGTERM) == 0 && waitpid(signaled, &status, 0) == signaled &&
         WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM;
}

static int managed_group_probe(void) {
  pid_t self = getpid();
  pid_t group = getpgrp();
  pid_t session = getsid(0);
  if (self <= 0 || group <= 0 || session <= 0 || getpgid(0) != group || getpgid(self) != group ||
      getsid(self) != session)
    return 0;

  int wake[2], ready[2];
  if (pipe(wake) != 0)
    return 0;
  if (pipe(ready) != 0) {
    close(wake[0]);
    close(wake[1]);
    return 0;
  }
  pid_t children[2] = {-1, -1};
  for (unsigned int i = 0; i < 2; ++i) {
    children[i] = fork();
    if (children[i] == 0) {
      close(wake[1]);
      close(ready[0]);
      char byte = 0;
      pid_t group_id = i ? children[0] : getpid();
      if (read(wake[0], &byte, 1) != 1 || byte != '1' || getpgrp() != group_id || getsid(0) != session ||
          kill(0, 0) != 0 || kill(-group_id, 0) != 0)
        _exit(41);
      byte = '1';
      if (write(ready[1], &byte, 1) != 1)
        _exit(41);
      // Bound a lost group signal instead of hanging the boot probe.
      unsigned long delay = 1000000000UL;
      (void)syscall1(SYS_NANOSLEEP, (long)&delay);
      _exit(97);
    }
    if (children[i] < 0)
      break;
  }
  close(wake[0]);
  close(ready[1]);
  int status = 0;
  char wake_bytes[2] = {'1', '1'}, ready_byte = 0;
  int valid = children[0] > 0 && children[1] > 0 && setpgid(children[0], children[0]) == 0 &&
              setpgid(children[1], children[0]) == 0 && getpgid(children[0]) == children[0] &&
              getpgid(children[1]) == children[0] && getsid(children[1]) == session &&
              write(wake[1], wake_bytes, sizeof(wake_bytes)) == sizeof(wake_bytes) &&
              read(ready[0], &ready_byte, 1) == 1 && ready_byte == '1' && read(ready[0], &ready_byte, 1) == 1 &&
              ready_byte == '1' && waitpid(-children[0], &status, WNOHANG) == 0 && kill(-children[0], SIGTERM) == 0;
  int reaped[2] = {0};
  for (unsigned int i = 0; valid && i < 2; ++i) {
    pid_t done = waitpid(-children[0], &status, 0);
    valid = WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM;
    if (done == children[0] && !reaped[0])
      reaped[0] = 1;
    else if (done == children[1] && !reaped[1])
      reaped[1] = 1;
    else
      valid = 0;
  }
  valid &= reaped[0] && reaped[1];
  close(wake[1]);
  close(ready[0]);
  if (!valid) {
    for (unsigned int i = 0; i < 2; ++i) {
      if (children[i] > 0 && !reaped[i]) {
        (void)kill(children[i], SIGKILL);
        (void)waitpid(children[i], &status, 0);
      }
    }
  }
  if (!valid)
    return 0;

  pid_t child = fork();
  if (child == 0)
    _exit(getpgrp() == group && getsid(0) == session ? 29 : 41);
  if (child <= 0 || waitpid(0, &status, 0) != child || status != (29 << 8))
    return 0;

  child = fork();
  if (child == 0) {
    pid_t id = getpid();
    errno = 0;
    _exit(setsid() == id && getpgrp() == id && getsid(0) == id && setsid() == -1 && errno == EPERM ? 0 : 41);
  }
  return child > 0 && waitpid(child, &status, 0) == child && status == 0;
}

static int managed_orphan_probe(void) {
  // More orphan generations than registry slots expose records that nobody
  // can wait for after their original parent has exited.
  for (unsigned int cycle = 0; cycle < MOSS_PROCESS_RECORD_LIMIT; ++cycle) {
    int channel[2];
    if (pipe(channel) != 0)
      return 0;
    pid_t intermediate = fork();
    if (intermediate == 0) {
      close(channel[0]);
      pid_t orphan = fork();
      if (orphan == 0) {
        for (unsigned int retry = 0; retry < STATUS_RETRIES; ++retry) {
          pid_t parent = getppid();
          if (parent <= 0)
            _exit(1);
          if (parent == MOSS_PROCESS_INIT_ID) {
            const char adopted = '1';
            _exit(write(channel[1], &adopted, 1) == 1 ? 0 : 1);
          }
          unsigned long delay = status_retry_ns;
          (void)syscall1(SYS_NANOSLEEP, (long)&delay);
        }
        _exit(1);
      }
      _exit(orphan > 0 ? 0 : 1);
    }
    close(channel[1]);
    if (intermediate <= 0) {
      close(channel[0]);
      return 0;
    }
    char adopted = 0, extra = 0;
    ssize_t got = read(channel[0], &adopted, 1);
    ssize_t eof = read(channel[0], &extra, 1);
    close(channel[0]);
    int status = 0;
    // The orphan must see init before anyone reaps its former parent.
    pid_t waited = waitpid(intermediate, &status, 0);
    if (got != 1 || adopted != '1' || eof != 0 || waited != intermediate || status != 0)
      return 0;
    unsigned long delay = status_retry_ns;
    (void)syscall1(SYS_NANOSLEEP, (long)&delay);
  }
  return 1;
}

int main(int argc, char **argv) {
  if (argc == 4 && strcmp(argv[1], "libc-child") == 0) {
    char *end = NULL;
    unsigned long child_id = strtoul(argv[2], &end, 10);
    if (!child_id || *end)
      return 43;
    unsigned long parent_id = strtoul(argv[3], &end, 10);
    return parent_id && !*end && !environ[0] && getauxval(MOSS_AT_STARTUP_CAP) && getpid() == (pid_t)child_id &&
                   getppid() == (pid_t)parent_id
               ? 37
               : 43;
  }
  if (argc != 2 || strcmp(argv[1], "probe") != 0)
    return error();
  const char *value = getenv("MOSS_PROCESS_CAP");
  if (!value)
    return error();
  char *end = NULL;
  errno = 0;
  unsigned long root = strtoul(value, &end, 10);
  if (errno || !root || root > LONG_MAX || *end)
    return error();
  unsigned long last_id = MOSS_PROCESS_INIT_ID;
  if (!managed_libc_probe() || !managed_group_probe() || !managed_orphan_probe() ||
      !observe_child(root, &last_id, 37) || !observe_child(root, &last_id, 38) || !observe_family(root, &last_id))
    return error();
  static const char message[] = "MOSS_PROCESS_READY\n";
  (void)write(STDOUT_FILENO, message, sizeof(message) - 1);
  return 0;
}
