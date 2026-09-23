#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_file_protocol.h"
#include "moss_namespace_protocol.h"
#include "syscall.h"

// The production file client waits five seconds for lookup; four seconds gives
// this service time to return an unavailable result if the file service stalls.
#define FILE_OPEN_TIMEOUT_NS 4000000000UL

static unsigned long parse_handle(const char *text) {
  if (!text) {
    return 0;
  }
  char *end = NULL;
  errno = 0;
  unsigned long handle = strtoul(text, &end, 10);
  return errno || !handle || *end ? 0 : handle;
}

static long open_file(unsigned long file, struct moss_ipc_message *opened) {
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) != 0 || now > LONG_MAX - FILE_OPEN_TIMEOUT_NS) {
    return -1;
  }
  const struct moss_ipc_message request = {.size = 1, .payload = {MOSS_FILE_OPEN}};
  return syscall6(SYS_IPC_CALL, (long)file, (long)&request, (long)opened, (long)(now + FILE_OPEN_TIMEOUT_NS), 0, 0);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    return 2;
  }
  unsigned long receive = parse_handle(argv[1]);
  unsigned long file = parse_handle(argv[2]);
  if (!receive || !file) {
    return 2;
  }

  for (;;) {
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    long received = syscall3(SYS_IPC_RECEIVE, (long)receive, (long)&request, (long)&reply);
    if (received == -EINTR) {
      continue;
    }
    if (received < 0) {
      return 1;
    }
    struct moss_ipc_message response = {.size = 1, .payload = {MOSS_NAMESPACE_BAD_REQUEST}};
    struct moss_ipc_message opened = {0};
    if (request.capability) {
      // A lookup never accepts delegated authority. Release an unexpected
      // handle so an untrusted caller cannot exhaust this service's table.
      (void)syscall1(SYS_CAP_CLOSE, (long)request.capability);
    } else if (request.size >= 2 && request.payload[0] == MOSS_NAMESPACE_OPEN &&
               request.payload[request.size - 1] == 0) {
      if (request.size == 1 + sizeof(MOSS_SCRATCH_PATH) &&
          memcmp(request.payload + 1, MOSS_SCRATCH_PATH, sizeof(MOSS_SCRATCH_PATH)) == 0) {
        response.payload[0] = MOSS_NAMESPACE_UNAVAILABLE;
        long result = open_file(file, &opened);
        if (result == 1 && opened.payload[0] == MOSS_FILE_OK && opened.capability &&
            opened.rights == (MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE)) {
          response.payload[0] = MOSS_NAMESPACE_OK;
          response.capability = opened.capability;
          response.rights = MOSS_CAP_SEND;
        }
      } else {
        response.payload[0] = MOSS_NAMESPACE_NO_ENTRY;
      }
    }
    // A returned file cap names its original service incarnation. Restarting
    // that service never rebinds a cap already transferred to a client.
    (void)syscall2(SYS_IPC_REPLY, (long)reply, (long)&response);
    if (opened.capability) {
      (void)syscall1(SYS_CAP_CLOSE, (long)opened.capability);
    }
  }
}
