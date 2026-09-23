#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_file_protocol.h"
#include "syscall.h"

// A stalled service must not leave a shell command waiting indefinitely.
#define FILE_CALL_TIMEOUT_NS 5000000000UL
// Native clock ID 1 is monotonic, as in clock_gettime_ns in syscall.h.
#define MOSS_MONOTONIC_CLOCK_ID 1

int main(int argc, char **argv) {
  const char *text = getenv("MOSS_FILE_CAP");
  if (!text || argc < 2) {
    return 2;
  }
  char *end = NULL;
  errno = 0;
  unsigned long file = strtoul(text, &end, 10);
  if (errno || !file || *end) {
    return 2;
  }

  struct moss_ipc_message request = {0};
  if (argc == 2 && strcmp(argv[1], "read") == 0) {
    request.size = 1;
    request.payload[0] = MOSS_FILE_READ;
  } else if (argc == 3 && strcmp(argv[1], "write") == 0) {
    size_t length = strlen(argv[2]);
    if (length > MOSS_IPC_MAX_MESSAGE - 1) {
      return 2;
    }
    request.size = length + 1;
    request.payload[0] = MOSS_FILE_WRITE;
    memcpy(request.payload + 1, argv[2], length);
  } else {
    return 2;
  }

  struct moss_ipc_message response = {0};
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_MONOTONIC_CLOCK_ID, (long)&now) != 0 || now > LONG_MAX - FILE_CALL_TIMEOUT_NS) {
    return 1;
  }
  long result =
      syscall6(SYS_IPC_CALL, (long)file, (long)&request, (long)&response, (long)(now + FILE_CALL_TIMEOUT_NS), 0, 0);
  if (result < 1 || response.payload[0] != MOSS_FILE_OK || response.capability) {
    static const char error[] = "MOSS_FILE_ERROR\n";
    (void)write(STDERR_FILENO, error, sizeof(error) - 1);
    if (response.capability) {
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    }
    return 1;
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
