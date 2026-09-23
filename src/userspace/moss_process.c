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

  struct moss_ipc_message request = {
      .size = 1,
      .capability = domain,
      .rights = MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT,
      .payload = {MOSS_PROCESS_REGISTER},
  };
  struct moss_ipc_message response = {0};
  long result = call(root, &request, &response);
  (void)syscall1(SYS_CAP_CLOSE, (long)domain);
  if (result != MOSS_PROCESS_REPLY_VALUE_BYTES || response.payload[0] != MOSS_PROCESS_OK || !response.capability ||
      response.rights != (MOSS_CAP_SEND | MOSS_CAP_DUPLICATE)) {
    if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    return 0;
  }
  unsigned long id = moss_process_get_u64(response.payload + 1);
  unsigned long session = response.capability;
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
  valid &= result == 1 && response.payload[0] == MOSS_PROCESS_OK && clean;
  request.payload[0] = MOSS_PROCESS_STATUS;
  response = (struct moss_ipc_message){0};
  result = call(session, &request, &response);
  clean = no_capability(&response);
  valid &= result == 1 && response.payload[0] == MOSS_PROCESS_NO_ENTRY && clean;
  (void)syscall1(SYS_CAP_CLOSE, (long)session);
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
  if (!observe_child(root, &last_id, 37) || !observe_child(root, &last_id, 38))
    return error();
  static const char message[] = "MOSS_PROCESS_READY\n";
  (void)write(STDOUT_FILENO, message, sizeof(message) - 1);
  return 0;
}
