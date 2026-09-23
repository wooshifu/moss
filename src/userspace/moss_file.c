#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_file_protocol.h"
#include "moss_namespace_protocol.h"
#include "syscall.h"

// Each service call has its own bounded wait; a stalled service cannot hang
// the shell indefinitely, and an ambiguous write is never retried here.
#define FILE_CALL_TIMEOUT_NS 5000000000UL
// Native clock ID 1 is monotonic, as in clock_gettime_ns in syscall.h.
#define MOSS_MONOTONIC_CLOCK_ID 1

static long call(unsigned long endpoint, const struct moss_ipc_message *request, struct moss_ipc_message *response) {
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_MONOTONIC_CLOCK_ID, (long)&now) != 0 || now > LONG_MAX - FILE_CALL_TIMEOUT_NS) {
    return -1;
  }
  return syscall6(SYS_IPC_CALL, (long)endpoint, (long)request, (long)response, (long)(now + FILE_CALL_TIMEOUT_NS), 0,
                  0);
}

static int error(void) {
  static const char message[] = "MOSS_FILE_ERROR\n";
  (void)write(STDERR_FILENO, message, sizeof(message) - 1);
  return 1;
}

int main(int argc, char **argv) {
  const char *text = getenv("MOSS_NAMESPACE_CAP");
  if (!text || argc < 2) {
    return 2;
  }
  char *end = NULL;
  errno = 0;
  unsigned long namespace = strtoul(text, &end, 10);
  if (errno || !namespace || *end) {
    return 2;
  }

  struct moss_ipc_message request = {0};
  const char *path;
  if ((argc == 2 || argc == 3) && strcmp(argv[1], "read") == 0) {
    request.size = 1;
    request.payload[0] = MOSS_FILE_READ;
    path = argc == 3 ? argv[2] : MOSS_SCRATCH_PATH;
  } else if ((argc == 3 || argc == 4) && strcmp(argv[1], "write") == 0) {
    size_t length = strlen(argv[2]);
    if (length > MOSS_IPC_MAX_MESSAGE - 1) {
      return 2;
    }
    request.size = length + 1;
    request.payload[0] = MOSS_FILE_WRITE;
    memcpy(request.payload + 1, argv[2], length);
    path = argc == 4 ? argv[3] : MOSS_SCRATCH_PATH;
  } else {
    return 2;
  }

  size_t path_size = strlen(path) + 1;
  if (path_size > MOSS_IPC_MAX_MESSAGE - 1) {
    return 2;
  }
  struct moss_ipc_message open = {.size = path_size + 1, .payload = {MOSS_NAMESPACE_OPEN}};
  memcpy(open.payload + 1, path, path_size);
  struct moss_ipc_message opened = {0};
  long lookup = call(namespace, &open, &opened);
  if (lookup != 1 || opened.payload[0] != MOSS_NAMESPACE_OK || !opened.capability || opened.rights != MOSS_CAP_SEND) {
    if (opened.capability) {
      (void)syscall1(SYS_CAP_CLOSE, (long)opened.capability);
    }
    return error();
  }

  struct moss_ipc_message response = {0};
  long result = call(opened.capability, &request, &response);
  (void)syscall1(SYS_CAP_CLOSE, (long)opened.capability);
  if (result < 1 || response.payload[0] != MOSS_FILE_OK || response.capability) {
    if (response.capability) {
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    }
    return error();
  }
  if (request.payload[0] == MOSS_FILE_READ) {
    static const char prefix[] = "MOSS_FILE_READ=";
    (void)write(STDOUT_FILENO, prefix, sizeof(prefix) - 1);
    (void)write(STDOUT_FILENO, response.payload + 1, response.size - 1);
    (void)write(STDOUT_FILENO, "\n", 1);
  } else {
    static const char success[] = "MOSS_FILE_WRITE_OK\n";
    (void)write(STDOUT_FILENO, success, sizeof(success) - 1);
  }
  return 0;
}
