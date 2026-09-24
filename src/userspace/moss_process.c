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
#include "moss_console_protocol.h"
#include "moss_file_protocol.h"
#include "moss_namespace_protocol.h"
#include "moss_pipe_protocol.h"
#include "moss_process_protocol.h"
#include "syscall.h"

extern char **environ;

// Boot validation must fail within a bounded time if the service stops
// replying or a child never publishes its exit status.
static const unsigned long process_call_timeout_ns = 5000000000UL;
static const unsigned long status_retry_ns = 10000000UL;
// Give the service time to accept a wait before the cancellation probe expires.
static const unsigned long fd_wait_cancel_timeout_ns = 1000000000UL;
// The child pauses briefly so the parent usually reaches FD_WAIT first. The
// readiness contract still holds if the producer wins that scheduling race.
enum { STATUS_RETRIES = 500, FD_WAIT_PROBE_DELAY_US = 50000 };

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

static int fd_constructor_closed;
__attribute__((constructor)) static void check_fd_before_main(void) {
  const char *text = getenv("MOSS_FD_PROBE_CLOEXEC");
  if (!text)
    return;
  // This constructor runs before mlibc finishes startup; strtoul aborts here.
  // The probe only accepts a bounded decimal descriptor number.
  unsigned long number = 0;
  for (const char *digit = text; *digit; ++digit) {
    if (*digit < '0' || *digit > '9' || number > (MOSS_PROCESS_FD_LIMIT - 1UL - (*digit - '0')) / 10UL)
      return;
    number = number * 10UL + (*digit - '0');
  }
  unsigned long session = getauxval(MOSS_AT_STARTUP_CAP);
  if (!number || !session)
    return;
  struct moss_ipc_message request = {.size = MOSS_PROCESS_REPLY_VALUE_BYTES, .payload = {MOSS_PROCESS_FD_CLOSE}};
  moss_process_put_u64(request.payload + 1, number);
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  int clean = no_capability(&response);
  fd_constructor_closed = result == 1 && response.payload[0] == MOSS_PROCESS_BAD_DESCRIPTOR && clean;
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

static int observe_child(unsigned long root, unsigned long parent_session, unsigned long *last_id, int exit_code,
                         int cancel_after_exit) {
  unsigned long domain = 0;
  long child = syscall1(SYS_FORK_DOMAIN, (long)&domain);
  if (child == 0)
    _exit(exit_code);
  if (child <= 1 || !domain) {
    if (domain)
      (void)syscall1(SYS_CAP_CLOSE, (long)domain);
    return 0;
  }

  struct moss_ipc_message request = {
      .size = 1,
      .capability = domain,
      .rights = MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT | MOSS_CAP_DOMAIN_SIGNAL,
      .payload = {MOSS_PROCESS_REGISTER},
  };
  struct moss_ipc_message response = {0};
  long result = call(root, &request, &response);
  if (result != 1 || response.payload[0] != MOSS_PROCESS_BAD_REQUEST || !no_capability(&response)) {
    (void)syscall1(SYS_CAP_CLOSE, (long)domain);
    return 0;
  }

  unsigned long id = 0, session = 0;
  if (!new_session(parent_session, domain, MOSS_PROCESS_REGISTER_CHILD, &id, &session))
    return 0;
  int valid = id > *last_id && syscall3(SYS_WAITPID, child, 0, 1) == -ECHILD;
  *last_id = id;
  // The root sender carries no record badge. Supplying the numeric ID in a
  // request must not turn that ambient value into observation authority.
  request = (struct moss_ipc_message){.size = MOSS_PROCESS_REPLY_VALUE_BYTES, .payload = {MOSS_PROCESS_STATUS}};
  moss_process_put_u64(request.payload + 1, id);
  response = (struct moss_ipc_message){0};
  result = call(root, &request, &response);
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
  valid &= result == 1 && response.payload[0] == MOSS_PROCESS_BUSY && clean;
  if (cancel_after_exit) {
    valid &= child_record_call(parent_session, MOSS_PROCESS_CANCEL_CHILD, id, 0, MOSS_PROCESS_OK);
  } else {
    request = (struct moss_ipc_message){.size = MOSS_PROCESS_REPLY_VALUE_BYTES, .payload = {MOSS_PROCESS_WAIT_CHILD}};
    moss_process_put_u64(request.payload + 1, id);
    response = (struct moss_ipc_message){0};
    result = call(parent_session, &request, &response);
    clean = no_capability(&response);
    valid &= result == MOSS_PROCESS_REPLY_WAIT_BYTES && response.payload[0] == MOSS_PROCESS_EXITED &&
             moss_process_get_u64(response.payload + 1) == id &&
             moss_process_get_u64(response.payload + 9) == (uint32_t)exit_code && clean;
  }
  request.payload[0] = MOSS_PROCESS_STATUS;
  request.size = 1;
  response = (struct moss_ipc_message){0};
  result = call(session, &request, &response);
  clean = no_capability(&response);
  valid &= result == 1 && response.payload[0] == MOSS_PROCESS_NO_ENTRY && clean;
  (void)syscall1(SYS_CAP_CLOSE, (long)session);
  return valid;
}

static int observe_family(unsigned long root, unsigned long *last_id) {
  unsigned long parent_session = getauxval(MOSS_AT_STARTUP_CAP);
  if (!parent_session)
    return 0;
  unsigned long parent_id = 0;
  struct moss_ipc_message identity = {.size = 1, .payload = {MOSS_PROCESS_IDENTITY}};
  struct moss_ipc_message identity_response = {0};
  long identity_result = call(parent_session, &identity, &identity_response);
  int identity_clean = no_capability(&identity_response);
  if (identity_result != MOSS_PROCESS_REPLY_IDENTITY_BYTES || identity_response.payload[0] != MOSS_PROCESS_OK ||
      !identity_clean)
    return 0;
  parent_id = moss_process_get_u64(identity_response.payload + 1);
  int valid = parent_id != 0;
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
  // Preserve the former registry-sized churn run as an adoption regression.
  enum { ORPHAN_CYCLES = 16 };
  for (unsigned int cycle = 0; cycle < ORPHAN_CYCLES; ++cycle) {
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

static int managed_fanout_probe(void) {
  // More live children than the former 16-record table exercises growth.
  enum { FANOUT_CHILDREN = 20 };
  int channel[2];
  if (pipe(channel) != 0)
    return 0;
  pid_t children[FANOUT_CHILDREN];
  unsigned int count = 0;
  for (; count < FANOUT_CHILDREN; ++count) {
    pid_t child = fork();
    if (child == 0) {
      close(channel[1]);
      char byte;
      _exit(read(channel[0], &byte, 1) == 0 ? 0 : 1);
    }
    if (child < 0)
      break;
    children[count] = child;
  }
  close(channel[0]);
  close(channel[1]);
  int valid = count == FANOUT_CHILDREN;
  for (unsigned int i = 0; i < count; ++i) {
    int status = 0;
    if (waitpid(children[i], &status, 0) != children[i] || status != 0)
      valid = 0;
  }
  return valid;
}

static int fd_open(unsigned long session, const char *path, unsigned char flags, unsigned long *number) {
  size_t path_size = strlen(path) + 1;
  if (path_size > MOSS_IPC_MAX_MESSAGE - 2)
    return 0;
  struct moss_ipc_message request = {.size = path_size + 2, .payload = {MOSS_PROCESS_FD_OPEN}};
  request.payload[1] = flags;
  memcpy(request.payload + 2, path, path_size);
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  if (!no_capability(&response) || result != MOSS_PROCESS_REPLY_VALUE_BYTES || response.payload[0] != MOSS_PROCESS_OK)
    return 0;
  *number = moss_process_get_u64(response.payload + 1);
  return *number >= MOSS_PROCESS_FD_FIRST && *number < MOSS_PROCESS_FD_LIMIT;
}

static int fd_install(unsigned long session, unsigned long file, unsigned char flags, unsigned long *number) {
  struct moss_ipc_message request = {.size = MOSS_PROCESS_FD_INSTALL_BYTES,
                                     .capability = file,
                                     .rights = MOSS_CAP_SEND,
                                     .payload = {MOSS_PROCESS_FD_INSTALL, flags}};
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  if (!no_capability(&response) || result != MOSS_PROCESS_REPLY_VALUE_BYTES || response.payload[0] != MOSS_PROCESS_OK)
    return 0;
  *number = moss_process_get_u64(response.payload + 1);
  return *number >= MOSS_PROCESS_FD_FIRST && *number < MOSS_PROCESS_FD_LIMIT;
}

static int fd_rejected(unsigned long session, const struct moss_ipc_message *request, unsigned char status) {
  struct moss_ipc_message response = {0};
  long result = call(session, request, &response);
  return no_capability(&response) && result == 1 && response.payload[0] == status;
}

static unsigned long opened_file_cap(unsigned long namespace, const char *path, unsigned char flags,
                                     unsigned int rights) {
  size_t path_size = strlen(path) + 1;
  if (path_size > MOSS_IPC_MAX_MESSAGE - 2)
    return 0;
  struct moss_ipc_message request = {.size = path_size + 2, .payload = {MOSS_NAMESPACE_OPEN}};
  request.payload[1] = flags;
  memcpy(request.payload + 2, path, path_size);
  struct moss_ipc_message response = {0};
  long result = call(namespace, &request, &response);
  if (result == MOSS_NAMESPACE_OPEN_REPLY_BYTES && response.payload[0] == MOSS_NAMESPACE_OK &&
      response.payload[1] == MOSS_NAMESPACE_KIND_FILE && response.capability && response.rights == rights)
    return response.capability;
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  return 0;
}

static int fd_command(unsigned long session, unsigned char operation, unsigned long number, unsigned long *value) {
  struct moss_ipc_message request = {.size = MOSS_PROCESS_REPLY_VALUE_BYTES, .payload = {operation}};
  moss_process_put_u64(request.payload + 1, number);
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  if (!no_capability(&response) || response.payload[0] != MOSS_PROCESS_OK)
    return 0;
  if (operation == MOSS_PROCESS_FD_CLOSE)
    return result == 1;
  if (result != MOSS_PROCESS_REPLY_VALUE_BYTES)
    return 0;
  *value = moss_process_get_u64(response.payload + 1);
  return *value >= MOSS_PROCESS_FD_FIRST && *value < MOSS_PROCESS_FD_LIMIT;
}

static int fd_dup_to(unsigned long session, unsigned long source, unsigned long target) {
  struct moss_ipc_message request = {.size = MOSS_PROCESS_FD_DUP_TO_BYTES, .payload = {MOSS_PROCESS_FD_DUP_TO}};
  moss_process_put_u64(request.payload + 1, source);
  moss_process_put_u64(request.payload + 9, target);
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  return no_capability(&response) && result == MOSS_PROCESS_REPLY_VALUE_BYTES &&
         response.payload[0] == MOSS_PROCESS_OK && moss_process_get_u64(response.payload + 1) == target;
}

static int fd_flags(unsigned long session, unsigned char operation, unsigned long number, unsigned char *flags) {
  struct moss_ipc_message request = {.size = operation == MOSS_PROCESS_FD_SET_FLAGS ? MOSS_PROCESS_FD_SET_FLAGS_BYTES
                                                                                    : MOSS_PROCESS_REPLY_VALUE_BYTES,
                                     .payload = {operation}};
  moss_process_put_u64(request.payload + 1, number);
  if (operation == MOSS_PROCESS_FD_SET_FLAGS)
    request.payload[9] = *flags;
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  if (!no_capability(&response) || response.payload[0] != MOSS_PROCESS_OK)
    return 0;
  if (operation == MOSS_PROCESS_FD_SET_FLAGS)
    return result == 1;
  if (result != MOSS_PROCESS_REPLY_VALUE_BYTES || moss_process_get_u64(response.payload + 1) > 1)
    return 0;
  *flags = (unsigned char)moss_process_get_u64(response.payload + 1);
  return 1;
}

static int fd_status(unsigned long session, unsigned long number, unsigned long *status) {
  struct moss_ipc_message request = {.size = MOSS_PROCESS_REPLY_VALUE_BYTES, .payload = {MOSS_PROCESS_FD_GET_STATUS}};
  moss_process_put_u64(request.payload + 1, number);
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  if (!no_capability(&response) || result != MOSS_PROCESS_REPLY_VALUE_BYTES || response.payload[0] != MOSS_PROCESS_OK)
    return 0;
  *status = moss_process_get_u64(response.payload + 1);
  return 1;
}

static int fd_stat(unsigned long session, unsigned long number, unsigned char *kind, unsigned long *id,
                   unsigned long *size) {
  struct moss_ipc_message request = {.size = MOSS_PROCESS_REPLY_VALUE_BYTES, .payload = {MOSS_PROCESS_FD_STAT}};
  moss_process_put_u64(request.payload + 1, number);
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  if (!no_capability(&response) || result != MOSS_PROCESS_FD_STAT_REPLY_BYTES || response.payload[0] != MOSS_PROCESS_OK)
    return 0;
  *kind = response.payload[1];
  *id = moss_process_get_u64(response.payload + 2);
  *size = moss_process_get_u64(response.payload + 10);
  return *kind >= MOSS_PROCESS_FD_KIND_FILE && *kind <= MOSS_PROCESS_FD_KIND_DIRECTORY;
}

static int path_stat(unsigned long session, const char *path, unsigned char *kind, unsigned long *id,
                     unsigned long *size) {
  size_t path_size = strlen(path) + 1;
  if (path_size > MOSS_IPC_MAX_MESSAGE - 2)
    return 0;
  struct moss_ipc_message request = {.size = path_size + 2, .payload = {MOSS_PROCESS_PATH_STAT}};
  memcpy(request.payload + 2, path, path_size);
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  if (!no_capability(&response) || result != MOSS_PROCESS_FD_STAT_REPLY_BYTES || response.payload[0] != MOSS_PROCESS_OK)
    return 0;
  *kind = response.payload[1];
  *id = moss_process_get_u64(response.payload + 2);
  *size = moss_process_get_u64(response.payload + 10);
  return (*kind == MOSS_PROCESS_FD_KIND_FILE || *kind == MOSS_PROCESS_FD_KIND_DIRECTORY) && *id;
}

static int fd_readdir(unsigned long session, unsigned long number, unsigned long memory, const char *page,
                      unsigned char *type, unsigned long *id, unsigned long *cookie, unsigned int *name_size) {
  struct moss_ipc_message request = {.size = MOSS_PROCESS_REPLY_VALUE_BYTES,
                                     .capability = memory,
                                     .rights = MOSS_CAP_MAP_WRITE | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE,
                                     .payload = {MOSS_PROCESS_FD_READDIR}};
  moss_process_put_u64(request.payload + 1, number);
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  if (!no_capability(&response) || result != MOSS_PROCESS_FD_READDIR_REPLY_BYTES ||
      response.payload[0] != MOSS_PROCESS_OK)
    return 0;
  *type = response.payload[1];
  *id = moss_process_get_u64(response.payload + 2);
  *cookie = moss_process_get_u64(response.payload + 10);
  *name_size = moss_file_get_u16(response.payload + 18);
  return !*name_size || (*name_size <= MOSS_IPC_MAX_MESSAGE - 2 && page[*name_size - 1] == 0);
}

static int fd_dup_min(unsigned long session, unsigned long source, unsigned long minimum, unsigned char flags,
                      unsigned long *target) {
  struct moss_ipc_message request = {.size = MOSS_PROCESS_FD_DUP_MIN_BYTES, .payload = {MOSS_PROCESS_FD_DUP_MIN}};
  moss_process_put_u64(request.payload + 1, source);
  moss_process_put_u64(request.payload + 9, minimum);
  request.payload[17] = flags;
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  if (!no_capability(&response) || result != MOSS_PROCESS_REPLY_VALUE_BYTES || response.payload[0] != MOSS_PROCESS_OK)
    return 0;
  *target = moss_process_get_u64(response.payload + 1);
  return *target >= minimum && *target >= MOSS_PROCESS_FD_FIRST && *target < MOSS_PROCESS_FD_LIMIT;
}

static int fd_seek(unsigned long session, unsigned long number, unsigned char whence, unsigned long *position) {
  struct moss_ipc_message request = {.size = MOSS_PROCESS_FD_SEEK_BYTES, .payload = {MOSS_PROCESS_FD_SEEK}};
  moss_process_put_u64(request.payload + 1, number);
  moss_process_put_u64(request.payload + 9, 0);
  request.payload[17] = whence;
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  if (!no_capability(&response) || result != MOSS_PROCESS_REPLY_VALUE_BYTES || response.payload[0] != MOSS_PROCESS_OK)
    return 0;
  *position = moss_process_get_u64(response.payload + 1);
  return 1;
}

static int fd_io(unsigned long session, unsigned char operation, unsigned long number, unsigned long memory,
                 unsigned int count, unsigned int *transferred) {
  // Forwarding through the process service needs both delegation rights on
  // the service's received handle; the file service receives only MAP_*.
  struct moss_ipc_message request = {.size = MOSS_PROCESS_FD_IO_BYTES,
                                     .capability = memory,
                                     .rights =
                                         (operation == MOSS_PROCESS_FD_WRITE ? MOSS_CAP_MAP_READ : MOSS_CAP_MAP_WRITE) |
                                         MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE,
                                     .payload = {operation}};
  moss_process_put_u64(request.payload + 1, number);
  moss_file_put_u16(request.payload + 9, count);
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  if (!no_capability(&response) || result != MOSS_PROCESS_FD_IO_REPLY_BYTES || response.payload[0] != MOSS_PROCESS_OK)
    return 0;
  *transferred = moss_file_get_u16(response.payload + 1);
  return *transferred <= count && (operation == MOSS_PROCESS_FD_READ || *transferred == count);
}

static int fd_exec_child(const char *kept_text) {
  if (!fd_constructor_closed) {
    fprintf(stderr, "MOSS_FD_CLOEXEC_EARLY_FAILED\n");
    return 0;
  }
  char *end = NULL;
  errno = 0;
  unsigned long kept = strtoul(kept_text, &end, 10);
  unsigned long session = getauxval(MOSS_AT_STARTUP_CAP);
  if (errno || !kept || *end || !session)
    return 0;
  long memory = syscall1(SYS_MEM_CREATE, MOSS_MEM_OBJECT_BYTES);
  long mapped = memory > 0 ? syscall2(SYS_MEM_MAP, memory, MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE) : 0;
  unsigned int transferred = 0;
  int valid = mapped > 0 && fd_io(session, MOSS_PROCESS_FD_READ, kept, memory, 1, &transferred) && transferred == 1 &&
              *(unsigned char *)mapped == 'c';
  if (mapped > 0)
    (void)syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES);
  if (memory > 0)
    (void)syscall1(SYS_CAP_CLOSE, memory);
  return valid;
}

static int fd_directory_probe(unsigned long session, unsigned long memory, const char *page, unsigned long note_id) {
  unsigned long directory = 0, duplicate = 0, id = 0, size = 0, cookie = 0;
  unsigned char kind = 0, type = 0;
  unsigned int name_size = 0;
  int valid = fd_open(session, "/", MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_CLOEXEC, &directory) &&
              fd_stat(session, directory, &kind, &id, &size) && kind == MOSS_PROCESS_FD_KIND_DIRECTORY &&
              id == MOSS_FILE_ROOT_BADGE && size == 0;
  if (valid) {
    struct moss_ipc_message writable = {.size = 2 + sizeof("/"), .payload = {MOSS_PROCESS_FD_OPEN}};
    writable.payload[1] = MOSS_PROCESS_FD_WRITABLE;
    memcpy(writable.payload + 2, "/", sizeof("/"));
    struct moss_ipc_message create = {.size = 2 + sizeof("."), .payload = {MOSS_PROCESS_FD_OPEN}};
    create.payload[1] = MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_CREATE;
    memcpy(create.payload + 2, ".", sizeof("."));
    valid = fd_rejected(session, &writable, MOSS_PROCESS_IS_DIRECTORY) &&
            fd_rejected(session, &create, MOSS_PROCESS_IS_DIRECTORY) &&
            fd_readdir(session, directory, memory, page, &type, &id, &cookie, &name_size) &&
            type == MOSS_FILE_TYPE_DIRECTORY && id == MOSS_FILE_ROOT_BADGE && cookie == 1 && name_size == sizeof(".") &&
            strcmp(page, ".") == 0 && fd_command(session, MOSS_PROCESS_FD_DUP, directory, &duplicate);
  }
  if (valid)
    valid = fd_readdir(session, duplicate, memory, page, &type, &id, &cookie, &name_size) &&
            type == MOSS_FILE_TYPE_DIRECTORY && id == MOSS_FILE_ROOT_BADGE && cookie == 2 &&
            name_size == sizeof("..") && strcmp(page, "..") == 0;
  int saw_scratch = 0, saw_note = 0, saw_exclusive = 0, ended = 0;
  unsigned long previous = cookie;
  for (unsigned int index = 0; valid && index <= MOSS_FILE_OBJECT_LIMIT; ++index) {
    valid = fd_readdir(session, directory, memory, page, &type, &id, &cookie, &name_size);
    if (!valid)
      break;
    if (!name_size) {
      ended = cookie == previous;
      break;
    }
    valid = type == MOSS_FILE_TYPE_REGULAR && cookie > previous && id > 0 && id < MOSS_FILE_ROOT_BADGE &&
            strcmp(page, "loader-probe") != 0 && strcmp(page, "loader-bad") != 0;
    previous = cookie;
    if (strcmp(page, "scratch") == 0)
      saw_scratch = id == MOSS_FILE_SCRATCH_BADGE;
    else if (strcmp(page, "note") == 0)
      saw_note = id == note_id;
    else if (strcmp(page, "fd-exclusive") == 0)
      saw_exclusive = 1;
  }
  valid = valid && ended && saw_scratch && saw_note && saw_exclusive;
  if (valid)
    valid = fd_seek(session, duplicate, MOSS_PROCESS_FD_SEEK_SET, &cookie) && cookie == 0 &&
            fd_readdir(session, directory, memory, page, &type, &id, &cookie, &name_size) &&
            type == MOSS_FILE_TYPE_DIRECTORY && strcmp(page, ".") == 0 && cookie == 1;
  if (duplicate)
    valid &= fd_command(session, MOSS_PROCESS_FD_CLOSE, duplicate, NULL);
  if (directory)
    valid &= fd_command(session, MOSS_PROCESS_FD_CLOSE, directory, NULL);
  return valid;
}

static int fd_view_probe(void) {
  unsigned long session = getauxval(MOSS_AT_STARTUP_CAP);
  const char *namespace_text = getenv("MOSS_NAMESPACE_CAP");
  char *namespace_end = NULL;
  errno = 0;
  unsigned long namespace = namespace_text ? strtoul(namespace_text, &namespace_end, 10) : 0;
  if (errno || !namespace || *namespace_end)
    return 0;
  unsigned long first = 0, duplicate = 0, imported = 0, victim = 0, spare = 0, minimum_first = 0, minimum_second = 0,
                position = 0;
  unsigned long file_id = 0, object_id = 0, object_size = 0;
  unsigned char flags = 0;
  unsigned char kind = 0;
  unsigned int transferred = 0;
  if (!session || !fd_open(session, "/note",
                           MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_WRITABLE | MOSS_PROCESS_FD_CREATE |
                               MOSS_PROCESS_FD_TRUNCATE | MOSS_PROCESS_FD_CLOEXEC,
                           &first))
    return 0;
  long memory = syscall1(SYS_MEM_CREATE, MOSS_MEM_OBJECT_BYTES);
  long mapped = memory > 0 ? syscall2(SYS_MEM_MAP, memory, MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE) : 0;
  int valid = mapped > 0;
  if (valid) {
    struct moss_ipc_message missing = {.size = 2 + sizeof("/fd-absent"), .payload = {MOSS_PROCESS_FD_OPEN}};
    missing.payload[1] = MOSS_PROCESS_FD_READABLE;
    memcpy(missing.payload + 2, "/fd-absent", sizeof("/fd-absent"));
    struct moss_ipc_message bad_fd = {.size = MOSS_PROCESS_REPLY_VALUE_BYTES, .payload = {MOSS_PROCESS_FD_CLOSE}};
    moss_process_put_u64(bad_fd.payload + 1, MOSS_PROCESS_FD_LIMIT);
    valid = fd_rejected(session, &missing, MOSS_PROCESS_NOT_FOUND) &&
            fd_rejected(session, &bad_fd, MOSS_PROCESS_BAD_DESCRIPTOR);
  }
  if (valid)
    valid = fd_flags(session, MOSS_PROCESS_FD_GET_FLAGS, first, &flags) && flags == 1 &&
            fd_status(session, first, &position) && position == (MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_WRITABLE);
  if (valid) {
    memcpy((void *)mapped, "abc", 3);
    valid = fd_io(session, MOSS_PROCESS_FD_WRITE, first, memory, 3, &transferred) && transferred == 3;
  }
  if (valid)
    valid = fd_stat(session, first, &kind, &file_id, &object_size) && kind == MOSS_PROCESS_FD_KIND_FILE &&
            file_id > 0 && object_size == 3;
  if (valid)
    valid = path_stat(session, "/note", &kind, &object_id, &object_size) && kind == MOSS_PROCESS_FD_KIND_FILE &&
            object_id == file_id && object_size == 3 && path_stat(session, "/", &kind, &object_id, &object_size) &&
            kind == MOSS_PROCESS_FD_KIND_DIRECTORY && object_id == MOSS_FILE_ROOT_BADGE && object_size == 0;
  if (valid) {
    unsigned long relative = 0, dot = 0;
    valid = path_stat(session, "note", &kind, &object_id, &object_size) && kind == MOSS_PROCESS_FD_KIND_FILE &&
            object_id == file_id && object_size == 3 && path_stat(session, "./note", &kind, &object_id, &object_size) &&
            object_id == file_id && path_stat(session, "..", &kind, &object_id, &object_size) &&
            kind == MOSS_PROCESS_FD_KIND_DIRECTORY && object_id == MOSS_FILE_ROOT_BADGE &&
            fd_open(session, "../note", MOSS_PROCESS_FD_READABLE, &relative) &&
            fd_stat(session, relative, &kind, &object_id, &object_size) && kind == MOSS_PROCESS_FD_KIND_FILE &&
            object_id == file_id && fd_open(session, ".", MOSS_PROCESS_FD_READABLE, &dot) &&
            fd_stat(session, dot, &kind, &object_id, &object_size) && kind == MOSS_PROCESS_FD_KIND_DIRECTORY &&
            object_id == MOSS_FILE_ROOT_BADGE;
    if (dot)
      valid &= fd_command(session, MOSS_PROCESS_FD_CLOSE, dot, NULL);
    if (relative)
      valid &= fd_command(session, MOSS_PROCESS_FD_CLOSE, relative, NULL);
  }
  if (valid) {
    struct moss_ipc_message missing = {.size = 2 + sizeof("/path-stat-absent"), .payload = {MOSS_PROCESS_PATH_STAT}};
    memcpy(missing.payload + 2, "/path-stat-absent", sizeof("/path-stat-absent"));
    struct moss_ipc_message nested = {.size = 2 + sizeof("/note/child"), .payload = {MOSS_PROCESS_PATH_STAT}};
    memcpy(nested.payload + 2, "/note/child", sizeof("/note/child"));
    valid = fd_rejected(session, &missing, MOSS_PROCESS_NOT_FOUND) &&
            fd_rejected(session, &nested, MOSS_PROCESS_BAD_REQUEST);
  }
  unsigned long exclusive = 0;
  if (valid)
    valid = fd_open(session, "/fd-exclusive",
                    MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_WRITABLE | MOSS_PROCESS_FD_CREATE |
                        MOSS_PROCESS_FD_EXCLUSIVE,
                    &exclusive);
  if (valid)
    valid = fd_directory_probe(session, (unsigned long)memory, (const char *)mapped, file_id);
  if (valid) {
    struct moss_ipc_message existing = {.size = 2 + sizeof("/note"), .payload = {MOSS_PROCESS_FD_OPEN}};
    existing.payload[1] = MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_WRITABLE | MOSS_PROCESS_FD_CREATE |
                          MOSS_PROCESS_FD_EXCLUSIVE | MOSS_PROCESS_FD_TRUNCATE;
    memcpy(existing.payload + 2, "/note", sizeof("/note"));
    valid = fd_rejected(session, &existing, MOSS_PROCESS_EXISTS) &&
            fd_seek(session, first, MOSS_PROCESS_FD_SEEK_SET, &position) && position == 0 &&
            fd_io(session, MOSS_PROCESS_FD_READ, first, memory, 3, &transferred) && transferred == 3 &&
            memcmp((const void *)mapped, "abc", 3) == 0;
  }
  if (exclusive)
    valid &= fd_command(session, MOSS_PROCESS_FD_CLOSE, exclusive, NULL);
  if (valid) {
    valid = fd_seek(session, first, MOSS_PROCESS_FD_SEEK_END, &position) && position == 3;
  }
  if (valid) {
    valid = fd_seek(session, first, MOSS_PROCESS_FD_SEEK_SET, &position) && position == 0;
  }
  if (valid) {
    valid = fd_command(session, MOSS_PROCESS_FD_DUP, first, &duplicate);
  }
  if (valid)
    valid = fd_stat(session, duplicate, &kind, &object_id, &object_size) && kind == MOSS_PROCESS_FD_KIND_FILE &&
            object_id == file_id && object_size == 3;
  if (valid) {
    valid = fd_io(session, MOSS_PROCESS_FD_READ, duplicate, memory, 1, &transferred) && transferred == 1 &&
            *(unsigned char *)mapped == 'a' && fd_io(session, MOSS_PROCESS_FD_READ, first, memory, 1, &transferred) &&
            transferred == 1 && *(unsigned char *)mapped == 'b';
  }
  if (valid) {
    unsigned long bare = opened_file_cap(namespace, "/note", 0, MOSS_CAP_SEND);
    struct moss_ipc_message request = {.size = MOSS_PROCESS_FD_INSTALL_BYTES,
                                       .capability = bare,
                                       .rights = MOSS_CAP_SEND,
                                       .payload = {MOSS_PROCESS_FD_INSTALL, MOSS_PROCESS_FD_READABLE}};
    struct moss_ipc_message response = {0};
    valid = bare && call(session, &request, &response) == -EACCES && no_capability(&response);
    if (bare)
      (void)syscall1(SYS_CAP_CLOSE, (long)bare);
  }
  if (valid) {
    unsigned long file = opened_file_cap(namespace, "/note", MOSS_NAMESPACE_OPEN_TRANSFER,
                                         MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE);
    valid = file && fd_install(session, file, MOSS_PROCESS_FD_READABLE, &imported);
    if (file)
      (void)syscall1(SYS_CAP_CLOSE, (long)file);
    struct moss_ipc_message wrong_access = {.size = MOSS_PROCESS_FD_IO_BYTES,
                                            .capability = (unsigned long)memory,
                                            .rights = MOSS_CAP_MAP_READ | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE,
                                            .payload = {MOSS_PROCESS_FD_WRITE}};
    moss_process_put_u64(wrong_access.payload + 1, imported);
    moss_file_put_u16(wrong_access.payload + 9, 1);
    valid = valid && fd_rejected(session, &wrong_access, MOSS_PROCESS_BAD_DESCRIPTOR);
    valid = valid && fd_io(session, MOSS_PROCESS_FD_READ, imported, memory, 1, &transferred) && transferred == 1 &&
            *(unsigned char *)mapped == 'a';
  }
  if (valid) {
    valid = fd_open(session, "/scratch", MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_CLOEXEC, &victim) &&
            fd_stat(session, victim, &kind, &object_id, &object_size) && kind == MOSS_PROCESS_FD_KIND_FILE &&
            object_id != file_id && fd_dup_to(session, duplicate, victim) && fd_dup_to(session, first, first) &&
            fd_stat(session, victim, &kind, &object_id, &object_size) && object_id == file_id && object_size == 3 &&
            fd_flags(session, MOSS_PROCESS_FD_GET_FLAGS, victim, &flags) && flags == 0 &&
            fd_flags(session, MOSS_PROCESS_FD_GET_FLAGS, first, &flags) && flags == 1;
  }
  if (valid) {
    flags = 1;
    valid = fd_flags(session, MOSS_PROCESS_FD_SET_FLAGS, victim, &flags) &&
            fd_flags(session, MOSS_PROCESS_FD_GET_FLAGS, victim, &flags) && flags == 1;
    flags = 0;
    valid = valid && fd_flags(session, MOSS_PROCESS_FD_SET_FLAGS, victim, &flags) &&
            fd_flags(session, MOSS_PROCESS_FD_GET_FLAGS, victim, &flags) && flags == 0;
    flags = 2;
    valid = valid && !fd_flags(session, MOSS_PROCESS_FD_SET_FLAGS, victim, &flags) &&
            fd_flags(session, MOSS_PROCESS_FD_GET_FLAGS, victim, &flags) && flags == 0;
  }
  if (valid) {
    valid = fd_dup_min(session, duplicate, MOSS_PROCESS_FD_LIMIT - 2, 1, &minimum_first) &&
            minimum_first == MOSS_PROCESS_FD_LIMIT - 2 &&
            fd_flags(session, MOSS_PROCESS_FD_GET_FLAGS, minimum_first, &flags) && flags == 1 &&
            fd_status(session, minimum_first, &position) &&
            position == (MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_WRITABLE) &&
            fd_dup_min(session, duplicate, MOSS_PROCESS_FD_LIMIT - 2, 0, &minimum_second) &&
            minimum_second == MOSS_PROCESS_FD_LIMIT - 1 &&
            fd_flags(session, MOSS_PROCESS_FD_GET_FLAGS, minimum_second, &flags) && flags == 0 &&
            !fd_dup_min(session, duplicate, MOSS_PROCESS_FD_LIMIT, 0, &position);
  }
  if (valid) {
    struct moss_ipc_message full = {.size = MOSS_PROCESS_FD_DUP_MIN_BYTES, .payload = {MOSS_PROCESS_FD_DUP_MIN}};
    moss_process_put_u64(full.payload + 1, duplicate);
    moss_process_put_u64(full.payload + 9, MOSS_PROCESS_FD_LIMIT - 2);
    valid = fd_rejected(session, &full, MOSS_PROCESS_TOO_MANY_FILES);
  }
  if (minimum_second) {
    valid &= fd_command(session, MOSS_PROCESS_FD_CLOSE, minimum_second, NULL);
    minimum_second = 0;
  }
  if (valid) {
    const unsigned long free_target = MOSS_PROCESS_FD_LIMIT - 1;
    valid = fd_dup_to(session, duplicate, free_target);
    if (valid)
      spare = free_target;
    valid = valid && !fd_dup_to(session, duplicate, MOSS_PROCESS_FD_LIMIT);
  }
  if (valid) {
    pid_t child = fork();
    if (child == 0) {
      char environment[64], kept_text[32];
      int env_size = snprintf(environment, sizeof(environment), "MOSS_FD_PROBE_CLOEXEC=%lu", first);
      int kept_size = snprintf(kept_text, sizeof(kept_text), "%lu", victim);
      if (env_size <= 0 || (size_t)env_size >= sizeof(environment) || kept_size <= 0 ||
          (size_t)kept_size >= sizeof(kept_text))
        _exit(43);
      char *const argv[] = {"moss-process.elf", "fd-exec-child", kept_text, NULL};
      char *const env[] = {environment, NULL};
      execve("/moss-process.elf", argv, env);
      _exit(43);
    }
    int status = 0;
    pid_t waited = child > 0 ? waitpid(child, &status, 0) : -1;
    valid = child > 0 && waited == child && status == (37 << 8) &&
            fd_io(session, MOSS_PROCESS_FD_READ, first, memory, 1, &transferred) && transferred == 0;
  }
  if (spare)
    valid &= fd_command(session, MOSS_PROCESS_FD_CLOSE, spare, NULL);
  if (minimum_first)
    valid &= fd_command(session, MOSS_PROCESS_FD_CLOSE, minimum_first, NULL);
  if (victim)
    valid &= fd_command(session, MOSS_PROCESS_FD_CLOSE, victim, NULL);
  if (imported)
    valid &= fd_command(session, MOSS_PROCESS_FD_CLOSE, imported, NULL);
  if (duplicate)
    valid &= fd_command(session, MOSS_PROCESS_FD_CLOSE, duplicate, NULL);
  unsigned long filled_through = MOSS_PROCESS_FD_FIRST - 1;
  if (valid) {
    for (unsigned long target = MOSS_PROCESS_FD_FIRST; target < MOSS_PROCESS_FD_LIMIT; ++target) {
      if (!fd_dup_to(session, first, target)) {
        valid = 0;
        break;
      }
      filled_through = target;
    }
  }
  if (valid) {
    struct moss_ipc_message truncate = {.size = 2 + sizeof("/note"), .payload = {MOSS_PROCESS_FD_OPEN}};
    truncate.payload[1] = MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_WRITABLE | MOSS_PROCESS_FD_TRUNCATE;
    memcpy(truncate.payload + 2, "/note", sizeof("/note"));
    struct moss_ipc_message pipe = {.size = 1, .payload = {MOSS_PROCESS_FD_PIPE}};
    valid = fd_rejected(session, &truncate, MOSS_PROCESS_TOO_MANY_FILES) &&
            fd_rejected(session, &pipe, MOSS_PROCESS_TOO_MANY_FILES) &&
            fd_seek(session, first, MOSS_PROCESS_FD_SEEK_SET, &position) && position == 0 &&
            fd_io(session, MOSS_PROCESS_FD_READ, first, memory, 3, &transferred) && transferred == 3 &&
            memcmp((const void *)mapped, "abc", 3) == 0;
  }
  for (unsigned long target = MOSS_PROCESS_FD_FIRST; target <= filled_through; ++target) {
    if (target != first)
      valid &= fd_command(session, MOSS_PROCESS_FD_CLOSE, target, NULL);
  }
  valid &= fd_command(session, MOSS_PROCESS_FD_CLOSE, first, NULL);
  if (mapped > 0)
    (void)syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES);
  if (memory > 0)
    (void)syscall1(SYS_CAP_CLOSE, memory);
  return valid;
}

static int pipe_status(unsigned long endpoint, unsigned char operation) {
  struct moss_ipc_message request = {.size = 1, .payload = {operation}};
  struct moss_ipc_message response = {0};
  long result = call(endpoint, &request, &response);
  int clean = no_capability(&response);
  return clean && result == 1 ? response.payload[0] : -1;
}

static int pipe_finish(unsigned long reader, unsigned char commit) {
  struct moss_ipc_message request = {.size = MOSS_PIPE_READ_FINISH_BYTES, .payload = {MOSS_PIPE_READ_FINISH, commit}};
  struct moss_ipc_message response = {0};
  long result = call(reader, &request, &response);
  return no_capability(&response) && result == 1 && response.payload[0] == MOSS_PIPE_OK;
}

static int pipe_capability(unsigned long endpoint, unsigned char operation, unsigned long *capability) {
  struct moss_ipc_message request = {.size = 1, .payload = {operation}};
  struct moss_ipc_message response = {0};
  long result = call(endpoint, &request, &response);
  if (result == 1 && response.payload[0] == MOSS_PIPE_OK && response.capability && response.rights == MOSS_CAP_SEND) {
    *capability = response.capability;
    return 1;
  }
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  return 0;
}

static int pipe_io(unsigned long endpoint, unsigned char operation, unsigned long memory, unsigned int count,
                   unsigned char expected, unsigned int transferred) {
  struct moss_ipc_message request = {.size = MOSS_PIPE_IO_BYTES,
                                     .capability = memory,
                                     .rights = operation == MOSS_PIPE_READ || operation == MOSS_PIPE_READ_PREPARE
                                                   ? MOSS_CAP_MAP_WRITE
                                                   : MOSS_CAP_MAP_READ,
                                     .payload = {operation}};
  moss_pipe_put_u16(request.payload + 1, count);
  struct moss_ipc_message response = {0};
  long result = call(endpoint, &request, &response);
  if (!no_capability(&response) || response.payload[0] != expected)
    return 0;
  if (expected != MOSS_PIPE_OK)
    return result == 1;
  return result == MOSS_PIPE_IO_REPLY_BYTES && moss_pipe_get_u16(response.payload + 1) == transferred;
}

static int pipe_probe(void) {
  const char *text = getenv("MOSS_PIPE_CAP");
  char *end = NULL;
  errno = 0;
  unsigned long root = text ? strtoul(text, &end, 10) : 0;
  if (errno || !root || root > LONG_MAX || *end)
    return 0;
  unsigned long control = 0, reader = 0, writer = 0;
  long memory = syscall1(SYS_MEM_CREATE, MOSS_MEM_OBJECT_BYTES);
  long mapped = memory > 0 ? syscall2(SYS_MEM_MAP, memory, MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE) : 0;
  int valid = mapped > 0 && pipe_capability(root, MOSS_PIPE_CREATE, &control) &&
              pipe_capability(control, MOSS_PIPE_READ_END, &reader) &&
              pipe_capability(control, MOSS_PIPE_WRITE_END, &writer);
  if (valid) {
    unsigned char *page = (unsigned char *)mapped;
    for (unsigned int index = 0; index < MOSS_MEM_OBJECT_BYTES; ++index)
      page[index] = (unsigned char)index;
    valid = pipe_io(writer, MOSS_PIPE_WRITE, memory, MOSS_MEM_OBJECT_BYTES, MOSS_PIPE_OK, MOSS_MEM_OBJECT_BYTES) &&
            pipe_io(writer, MOSS_PIPE_WRITE, memory, 1, MOSS_PIPE_WOULD_BLOCK, 0) &&
            pipe_io(reader, MOSS_PIPE_READ, memory, 1, MOSS_PIPE_OK, 1) && page[0] == 0 &&
            pipe_io(writer, MOSS_PIPE_WRITE, memory, 2, MOSS_PIPE_WOULD_BLOCK, 0);
    if (valid) {
      page[0] = '!';
      valid = pipe_io(writer, MOSS_PIPE_WRITE, memory, 1, MOSS_PIPE_OK, 1) &&
              pipe_status(writer, MOSS_PIPE_CLOSE) == MOSS_PIPE_OK &&
              pipe_io(reader, MOSS_PIPE_READ, memory, MOSS_MEM_OBJECT_BYTES, MOSS_PIPE_OK, MOSS_MEM_OBJECT_BYTES);
    }
    if (valid) {
      for (unsigned int index = 0; index < MOSS_MEM_OBJECT_BYTES - 1; ++index)
        valid &= page[index] == (unsigned char)(index + 1);
      valid &= page[MOSS_MEM_OBJECT_BYTES - 1] == '!';
      valid &= pipe_io(reader, MOSS_PIPE_READ, memory, 1, MOSS_PIPE_OK, 0) &&
               pipe_status(reader, MOSS_PIPE_CLOSE) == MOSS_PIPE_OK &&
               pipe_io(reader, MOSS_PIPE_READ, memory, 1, MOSS_PIPE_NO_ENTRY, 0);
    }
  }
  if (valid) {
    (void)syscall1(SYS_CAP_CLOSE, (long)reader);
    (void)syscall1(SYS_CAP_CLOSE, (long)writer);
    (void)syscall1(SYS_CAP_CLOSE, (long)control);
    reader = writer = control = 0;
    *(unsigned char *)mapped = 'z';
    valid =
        pipe_capability(root, MOSS_PIPE_CREATE, &control) && pipe_capability(control, MOSS_PIPE_READ_END, &reader) &&
        pipe_capability(control, MOSS_PIPE_WRITE_END, &writer) &&
        pipe_io(writer, MOSS_PIPE_WRITE, memory, 1, MOSS_PIPE_OK, 1) &&
        pipe_io(reader, MOSS_PIPE_READ_PREPARE, memory, 1, MOSS_PIPE_OK, 1) && *(unsigned char *)mapped == 'z' &&
        pipe_io(reader, MOSS_PIPE_READ, memory, 1, MOSS_PIPE_WOULD_BLOCK, 0) && pipe_finish(reader, 0) &&
        pipe_io(reader, MOSS_PIPE_READ_PREPARE, memory, 1, MOSS_PIPE_OK, 1) && *(unsigned char *)mapped == 'z' &&
        pipe_finish(reader, 1) && pipe_io(reader, MOSS_PIPE_READ, memory, 1, MOSS_PIPE_WOULD_BLOCK, 0) &&
        pipe_status(reader, MOSS_PIPE_CLOSE) == MOSS_PIPE_OK && pipe_status(writer, MOSS_PIPE_CLOSE) == MOSS_PIPE_OK;
  }
  if (valid) {
    (void)syscall1(SYS_CAP_CLOSE, (long)reader);
    (void)syscall1(SYS_CAP_CLOSE, (long)writer);
    (void)syscall1(SYS_CAP_CLOSE, (long)control);
    reader = writer = control = 0;
    valid = pipe_capability(root, MOSS_PIPE_CREATE, &control) &&
            pipe_capability(control, MOSS_PIPE_READ_END, &reader) &&
            pipe_status(control, MOSS_PIPE_CANCEL) == MOSS_PIPE_OK &&
            pipe_io(reader, MOSS_PIPE_READ, memory, 1, MOSS_PIPE_NO_ENTRY, 0);
  }
  if (!valid && control)
    (void)pipe_status(control, MOSS_PIPE_CANCEL);
  if (reader)
    (void)syscall1(SYS_CAP_CLOSE, (long)reader);
  if (writer)
    (void)syscall1(SYS_CAP_CLOSE, (long)writer);
  if (control)
    (void)syscall1(SYS_CAP_CLOSE, (long)control);
  if (mapped > 0)
    (void)syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES);
  if (memory > 0)
    (void)syscall1(SYS_CAP_CLOSE, memory);
  return valid;
}

static int fd_pipe(unsigned long session, unsigned long *reader, unsigned long *writer) {
  struct moss_ipc_message request = {.size = 1, .payload = {MOSS_PROCESS_FD_PIPE}};
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  if (!no_capability(&response) || result != MOSS_PROCESS_FD_PIPE_REPLY_BYTES || response.payload[0] != MOSS_PROCESS_OK)
    return 0;
  *reader = moss_process_get_u64(response.payload + 1);
  *writer = moss_process_get_u64(response.payload + 9);
  return *reader < MOSS_PROCESS_FD_LIMIT && *writer > *reader && *writer < MOSS_PROCESS_FD_LIMIT;
}

static int fd_io_rejected(unsigned long session, unsigned char operation, unsigned long number, unsigned long memory,
                          unsigned int count, unsigned char status) {
  struct moss_ipc_message request = {.size = MOSS_PROCESS_FD_IO_BYTES,
                                     .capability = memory,
                                     .rights =
                                         (operation == MOSS_PROCESS_FD_WRITE ? MOSS_CAP_MAP_READ : MOSS_CAP_MAP_WRITE) |
                                         MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE,
                                     .payload = {operation}};
  moss_process_put_u64(request.payload + 1, number);
  moss_pipe_put_u16(request.payload + 9, count);
  return fd_rejected(session, &request, status);
}

static int fd_wait(unsigned long session, unsigned long number, unsigned char direction, unsigned int count) {
  struct moss_ipc_message request = {.size = MOSS_PROCESS_FD_WAIT_BYTES, .payload = {MOSS_PROCESS_FD_WAIT}};
  moss_process_put_u64(request.payload + 1, number);
  request.payload[9] = direction;
  moss_file_put_u16(request.payload + 10, count);
  struct moss_ipc_message response = {0};
  long result = call(session, &request, &response);
  return no_capability(&response) && result == 1 && response.payload[0] == MOSS_PROCESS_OK;
}

static int fd_wait_expires(unsigned long session, unsigned long number) {
  struct moss_ipc_message request = {.size = MOSS_PROCESS_FD_WAIT_BYTES, .payload = {MOSS_PROCESS_FD_WAIT}};
  moss_process_put_u64(request.payload + 1, number);
  request.payload[9] = MOSS_PROCESS_FD_WAIT_READ;
  moss_file_put_u16(request.payload + 10, 1);
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) != 0 || now > LONG_MAX - fd_wait_cancel_timeout_ns)
    return 0;
  struct moss_ipc_message response = {0};
  long result = syscall6(SYS_IPC_CALL, (long)session, (long)&request, (long)&response,
                         (long)(now + fd_wait_cancel_timeout_ns), 0, 0);
  return no_capability(&response) && result == -ETIMEDOUT;
}

static int fd_wait_child(const char *number_text, const char *direction_text) {
  char *end = NULL;
  errno = 0;
  unsigned long number = strtoul(number_text, &end, 10);
  unsigned long session = getauxval(MOSS_AT_STARTUP_CAP);
  int writing = strcmp(direction_text, "write") == 0;
  if (errno || !number || number >= MOSS_PROCESS_FD_LIMIT || *end || !session ||
      (!writing && strcmp(direction_text, "read") != 0))
    return 0;
  usleep(FD_WAIT_PROBE_DELAY_US);
  long memory = syscall1(SYS_MEM_CREATE, MOSS_MEM_OBJECT_BYTES);
  long page = memory > 0 ? syscall2(SYS_MEM_MAP, memory, MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE) : 0;
  if (page > 0 && writing)
    *(unsigned char *)page = 'w';
  unsigned int transferred = 0;
  int valid = page > 0 &&
              fd_io(session, writing ? MOSS_PROCESS_FD_WRITE : MOSS_PROCESS_FD_READ, number, memory, 1, &transferred) &&
              transferred == 1 && (writing || *(unsigned char *)page == 'x');
  if (page > 0)
    (void)syscall2(SYS_MUNMAP, page, MOSS_MEM_OBJECT_BYTES);
  if (memory > 0)
    (void)syscall1(SYS_CAP_CLOSE, memory);
  return valid;
}

static int fd_pipe_probe(void) {
  unsigned long session = getauxval(MOSS_AT_STARTUP_CAP);
  unsigned long reader = 0, writer = 0, duplicate = 0, flags = 0;
  unsigned long object_id = 0, object_size = 0;
  unsigned char kind = 0;
  unsigned int transferred = 0;
  long memory = syscall1(SYS_MEM_CREATE, MOSS_MEM_OBJECT_BYTES);
  long mapped = memory > 0 ? syscall2(SYS_MEM_MAP, memory, MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE) : 0;
  int valid = session && mapped > 0 && fd_pipe(session, &reader, &writer) && reader >= MOSS_PROCESS_FD_FIRST &&
              fd_status(session, reader, &flags) && flags == MOSS_PROCESS_FD_READABLE &&
              fd_status(session, writer, &flags) && flags == MOSS_PROCESS_FD_WRITABLE &&
              fd_stat(session, reader, &kind, &object_id, &object_size) && kind == MOSS_PROCESS_FD_KIND_PIPE &&
              object_id == 0 && object_size == 0 && fd_stat(session, writer, &kind, &object_id, &object_size) &&
              kind == MOSS_PROCESS_FD_KIND_PIPE && object_id == 0 && object_size == 0;
  if (valid) {
    memcpy((void *)mapped, "pipe", 4);
    valid = fd_io(session, MOSS_PROCESS_FD_WRITE, writer, memory, 4, &transferred) && transferred == 4 &&
            fd_command(session, MOSS_PROCESS_FD_DUP, reader, &duplicate) &&
            fd_command(session, MOSS_PROCESS_FD_CLOSE, reader, NULL);
    if (valid)
      reader = 0;
  }
  if (valid)
    valid = fd_io(session, MOSS_PROCESS_FD_READ, duplicate, memory, 2, &transferred) && transferred == 2 &&
            memcmp((const void *)mapped, "pi", 2) == 0 &&
            fd_io(session, MOSS_PROCESS_FD_READ, duplicate, memory, 2, &transferred) && transferred == 2 &&
            memcmp((const void *)mapped, "pe", 2) == 0 &&
            fd_io_rejected(session, MOSS_PROCESS_FD_READ, duplicate, memory, 1, MOSS_PROCESS_WOULD_BLOCK);
  if (valid)
    valid = fd_wait_expires(session, duplicate);
  if (valid) {
    char writer_text[24];
    snprintf(writer_text, sizeof(writer_text), "%lu", writer);
    pid_t child = fork();
    if (child == 0) {
      char *const child_argv[] = {"moss-process", "fd-wait-child", writer_text, "write", NULL};
      execve("/moss-process.elf", child_argv, environ);
      _exit(43);
    }
    valid = child > 0 && fd_wait(session, duplicate, MOSS_PROCESS_FD_WAIT_READ, 1) &&
            fd_io(session, MOSS_PROCESS_FD_READ, duplicate, memory, 1, &transferred) && transferred == 1 &&
            *(unsigned char *)mapped == 'w';
    int status = 0;
    if (child > 0)
      valid &= waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 37;
  }
  if (valid) {
    memset((void *)mapped, 'x', MOSS_MEM_OBJECT_BYTES);
    valid = fd_io(session, MOSS_PROCESS_FD_WRITE, writer, memory, MOSS_MEM_OBJECT_BYTES, &transferred) &&
            transferred == MOSS_MEM_OBJECT_BYTES;
  }
  if (valid) {
    char reader_text[24];
    snprintf(reader_text, sizeof(reader_text), "%lu", duplicate);
    pid_t child = fork();
    if (child == 0) {
      char *const child_argv[] = {"moss-process", "fd-wait-child", reader_text, "read", NULL};
      execve("/moss-process.elf", child_argv, environ);
      _exit(43);
    }
    valid = child > 0 && fd_wait(session, writer, MOSS_PROCESS_FD_WAIT_WRITE, 1);
    if (valid) {
      *(unsigned char *)mapped = 'y';
      valid = fd_io(session, MOSS_PROCESS_FD_WRITE, writer, memory, 1, &transferred) && transferred == 1 &&
              fd_io(session, MOSS_PROCESS_FD_READ, duplicate, memory, MOSS_MEM_OBJECT_BYTES, &transferred) &&
              transferred == MOSS_MEM_OBJECT_BYTES;
    }
    int status = 0;
    if (child > 0)
      valid &= waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 37;
  }
  if (valid) {
    struct moss_ipc_message seek = {.size = MOSS_PROCESS_FD_SEEK_BYTES, .payload = {MOSS_PROCESS_FD_SEEK}};
    moss_process_put_u64(seek.payload + 1, duplicate);
    seek.payload[17] = MOSS_PROCESS_FD_SEEK_SET;
    valid = fd_rejected(session, &seek, MOSS_PROCESS_NOT_SEEKABLE) &&
            fd_command(session, MOSS_PROCESS_FD_CLOSE, writer, NULL) &&
            fd_wait(session, duplicate, MOSS_PROCESS_FD_WAIT_READ, 1);
    if (valid)
      writer = 0;
  }
  if (valid)
    valid = fd_io(session, MOSS_PROCESS_FD_READ, duplicate, memory, 1, &transferred) && transferred == 0 &&
            fd_command(session, MOSS_PROCESS_FD_CLOSE, duplicate, NULL);
  if (valid)
    duplicate = 0;
  if (valid)
    valid = fd_pipe(session, &reader, &writer) && fd_command(session, MOSS_PROCESS_FD_CLOSE, reader, NULL);
  if (valid)
    reader = 0;
  if (valid)
    valid = fd_wait(session, writer, MOSS_PROCESS_FD_WAIT_WRITE, 1) &&
            fd_io_rejected(session, MOSS_PROCESS_FD_WRITE, writer, memory, 1, MOSS_PROCESS_BROKEN_PIPE) &&
            fd_command(session, MOSS_PROCESS_FD_CLOSE, writer, NULL);
  if (valid)
    writer = 0;
  if (reader)
    (void)fd_command(session, MOSS_PROCESS_FD_CLOSE, reader, NULL);
  if (writer)
    (void)fd_command(session, MOSS_PROCESS_FD_CLOSE, writer, NULL);
  if (duplicate)
    (void)fd_command(session, MOSS_PROCESS_FD_CLOSE, duplicate, NULL);
  if (mapped > 0)
    (void)syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES);
  if (memory > 0)
    (void)syscall1(SYS_CAP_CLOSE, memory);
  return valid;
}

static int console_feed(unsigned long input, unsigned char byte) {
  struct moss_ipc_message request = {.size = 2, .payload = {MOSS_CONSOLE_FEED, byte}};
  struct moss_ipc_message response = {0};
  long result = call(input, &request, &response);
  return no_capability(&response) && result == 1 && response.payload[0] == MOSS_CONSOLE_OK;
}

static int console_fd_probe(void) {
  unsigned long session = getauxval(MOSS_AT_STARTUP_CAP);
  const char *text = getenv("MOSS_CONSOLE_INPUT_CAP");
  char *end = NULL;
  errno = 0;
  unsigned long input = text ? strtoul(text, &end, 10) : 0;
  if (errno || !session || !input || input > LONG_MAX || *end)
    return 0;
  long memory = syscall1(SYS_MEM_CREATE, MOSS_MEM_OBJECT_BYTES);
  long mapped = memory > 0 ? syscall2(SYS_MEM_MAP, memory, MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE) : 0;
  unsigned long status = 0;
  unsigned long object_id = 0, object_size = 0;
  unsigned char kind = 0;
  unsigned int transferred = 0;
  int valid = mapped > 0 && fd_status(session, 0, &status) && status == MOSS_PROCESS_FD_READABLE &&
              fd_status(session, 1, &status) && status == MOSS_PROCESS_FD_WRITABLE && fd_status(session, 2, &status) &&
              status == MOSS_PROCESS_FD_WRITABLE && fd_stat(session, 0, &kind, &object_id, &object_size) &&
              kind == MOSS_PROCESS_FD_KIND_CONSOLE && object_id == 0 && object_size == 0 &&
              fd_stat(session, 1, &kind, &object_id, &object_size) && kind == MOSS_PROCESS_FD_KIND_CONSOLE &&
              object_id == 0 && object_size == 0 &&
              fd_io_rejected(session, MOSS_PROCESS_FD_READ, 0, memory, 1, MOSS_PROCESS_WOULD_BLOCK);
  if (valid)
    valid = console_feed(input, '@') && console_feed(input, '!') &&
            fd_io(session, MOSS_PROCESS_FD_READ, 0, memory, 1, &transferred) && transferred == 1 &&
            *(unsigned char *)mapped == '@' && fd_io(session, MOSS_PROCESS_FD_READ, 0, memory, 1, &transferred) &&
            transferred == 1 && *(unsigned char *)mapped == '!';
  if (valid)
    valid = syscall2(SYS_CAP_SET_INHERIT, (long)input, 1) == 0;
  if (valid) {
    pid_t child = fork();
    if (child == 0) {
      usleep(FD_WAIT_PROBE_DELAY_US);
      _exit(console_feed(input, '?') ? 37 : 43);
    }
    valid = child > 0 && fd_wait(session, 0, MOSS_PROCESS_FD_WAIT_READ, 1) &&
            fd_io(session, MOSS_PROCESS_FD_READ, 0, memory, 1, &transferred) && transferred == 1 &&
            *(unsigned char *)mapped == '?';
    int status = 0;
    if (child > 0)
      valid &= waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 37;
  }
  if (valid) {
    static const char message[] = "MOSS_CONSOLE_OBJECT_WRITE\n";
    memcpy((void *)mapped, message, sizeof(message) - 1);
    valid = fd_dup_to(session, 1, 0) && fd_status(session, 0, &status) && status == MOSS_PROCESS_FD_WRITABLE &&
            fd_io(session, MOSS_PROCESS_FD_WRITE, 0, memory, sizeof(message) - 1, &transferred) &&
            transferred == sizeof(message) - 1 && fd_command(session, MOSS_PROCESS_FD_CLOSE, 0, NULL) &&
            fd_io_rejected(session, MOSS_PROCESS_FD_WRITE, 0, memory, 1, MOSS_PROCESS_BAD_DESCRIPTOR);
  }
  if (valid) {
    unsigned long reader = MOSS_PROCESS_FD_LIMIT, writer = MOSS_PROCESS_FD_LIMIT;
    valid = fd_pipe(session, &reader, &writer) && reader == 0 && writer >= MOSS_PROCESS_FD_FIRST &&
            fd_command(session, MOSS_PROCESS_FD_CLOSE, reader, NULL) &&
            fd_command(session, MOSS_PROCESS_FD_CLOSE, writer, NULL);
  }
  if (mapped > 0)
    (void)syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES);
  if (memory > 0)
    (void)syscall1(SYS_CAP_CLOSE, memory);
  return valid;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "fd-exec-child") == 0)
    return fd_exec_child(argv[2]) ? 37 : 43;
  if (argc == 4 && strcmp(argv[1], "fd-wait-child") == 0)
    return fd_wait_child(argv[2], argv[3]) ? 37 : 43;
  if (argc == 2 && strcmp(argv[1], "fd-probe") == 0) {
    if (!fd_view_probe())
      return error();
    static const char message[] = "MOSS_FD_READY\n";
    (void)write(STDOUT_FILENO, message, sizeof(message) - 1);
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "pipe-probe") == 0) {
    if (!pipe_probe())
      return error();
    static const char message[] = "MOSS_PIPE_READY\n";
    (void)write(STDOUT_FILENO, message, sizeof(message) - 1);
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "fd-pipe-probe") == 0) {
    if (!fd_pipe_probe())
      return error();
    static const char message[] = "MOSS_FD_PIPE_READY\n";
    (void)write(STDOUT_FILENO, message, sizeof(message) - 1);
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "console-fd-probe") == 0) {
    if (!console_fd_probe())
      return error();
    static const char message[] = "MOSS_CONSOLE_READY\n";
    (void)write(STDOUT_FILENO, message, sizeof(message) - 1);
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "crash-survivor") == 0) {
    pid_t orphan = fork();
    if (orphan < 0)
      return error();
    if (orphan > 0)
      return 0;
    // The marker must prove that the service adopted a live grandchild;
    // otherwise the crash probe could pass by killing only a shell child.
    int adopted = 0;
    for (unsigned int retry = 0; retry < STATUS_RETRIES; ++retry) {
      if (getppid() == MOSS_PROCESS_INIT_ID) {
        adopted = 1;
        break;
      }
      unsigned long delay = status_retry_ns;
      (void)syscall1(SYS_NANOSLEEP, (long)&delay);
    }
    if (!adopted)
      _exit(1);
    static const char started[] = "MOSS_OLD_CHILD_STARTED\n";
    static const char survived[] = "MOSS_OLD_CHILD_SURVIVED\n";
    (void)write(STDOUT_FILENO, started, sizeof(started) - 1);
    // The boot probe waits beyond this delay after crashing the old service.
    unsigned long delay = 3000000000UL;
    (void)syscall1(SYS_NANOSLEEP, (long)&delay);
    (void)write(STDOUT_FILENO, survived, sizeof(survived) - 1);
    return 0;
  }
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
  unsigned long parent_session = getauxval(MOSS_AT_STARTUP_CAP);
  if (!managed_libc_probe() || !managed_group_probe() || !managed_orphan_probe() || !managed_fanout_probe() ||
      !observe_child(root, parent_session, &last_id, 37, 1) || !observe_child(root, parent_session, &last_id, 38, 0) ||
      !observe_family(root, &last_id))
    return error();
  static const char message[] = "MOSS_PROCESS_READY\n";
  (void)write(STDOUT_FILENO, message, sizeof(message) - 1);
  return 0;
}
