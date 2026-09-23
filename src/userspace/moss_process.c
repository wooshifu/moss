#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_process_protocol.h"
#include "syscall.h"

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

static int register_domain(unsigned long endpoint, unsigned long domain, unsigned char operation, unsigned long *id,
                           unsigned long *session) {
  if (!domain)
    return 0;
  struct moss_ipc_message request = {
      .size = 1,
      .capability = domain,
      .rights = MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT,
      .payload = {operation},
  };
  struct moss_ipc_message response = {0};
  long result = call(endpoint, &request, &response);
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
  if (!register_domain(root, domain, MOSS_PROCESS_REGISTER, &id, &session))
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
  long self = syscall0(SYS_DOMAIN_SELF);
  if (self <= 0)
    return 0;
  unsigned long parent_id = 0, parent_session = 0;
  if (!register_domain(root, (unsigned long)self, MOSS_PROCESS_REGISTER, &parent_id, &parent_session))
    return 0;
  int valid = parent_id > *last_id;
  *last_id = parent_id;
  unsigned long child_ids[2] = {0};
  unsigned long child_sessions[2] = {0};
  const int exit_codes[2] = {39, 40};
  long pipe_fds[2] = {-1, -1};
  if (syscall1(SYS_PIPE, (long)pipe_fds) != 0)
    valid = 0;
  for (unsigned int i = 0; valid && i < 2; ++i) {
    unsigned long domain = 0;
    long child = syscall1(SYS_FORK_DOMAIN, (long)&domain);
    if (child == 0) {
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
    if (i == 0 && pipe_fds[0] >= 0) {
      (void)syscall1(SYS_CLOSE, pipe_fds[0]);
      pipe_fds[0] = -1;
    }
    if (child <= 1 || !domain) {
      if (domain)
        (void)syscall1(SYS_CAP_CLOSE, (long)domain);
      valid = 0;
      break;
    }
    if (!register_domain(parent_session, domain, MOSS_PROCESS_REGISTER_CHILD, &child_ids[i], &child_sessions[i]) ||
        child_ids[i] <= *last_id) {
      valid = 0;
      break;
    }
    *last_id = child_ids[i];
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

  request = (struct moss_ipc_message){.size = 1, .payload = {MOSS_PROCESS_WAIT_ANY}};
  for (unsigned int reaped = 0, retry = 0; valid && reaped < 2 && retry < STATUS_RETRIES; ++retry) {
    response = (struct moss_ipc_message){0};
    result = call(parent_session, &request, &response);
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
  request.payload[0] = MOSS_PROCESS_RELEASE;
  response = (struct moss_ipc_message){0};
  result = call(parent_session, &request, &response);
  clean = no_capability(&response);
  valid &= result == 1 && response.payload[0] == MOSS_PROCESS_OK && clean;
  (void)syscall1(SYS_CAP_CLOSE, (long)parent_session);
  return valid;
}

int main(int argc, char **argv) {
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
  unsigned long last_id = 1;
  if (!observe_child(root, &last_id, 37) || !observe_child(root, &last_id, 38) || !observe_family(root, &last_id))
    return error();
  static const char message[] = "MOSS_PROCESS_READY\n";
  (void)write(STDOUT_FILENO, message, sizeof(message) - 1);
  return 0;
}
