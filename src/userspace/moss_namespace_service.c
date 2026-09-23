#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_namespace_protocol.h"
#include "syscall.h"

static unsigned long parse_handle(const char *text) {
  if (!text) {
    return 0;
  }
  char *end = NULL;
  errno = 0;
  unsigned long handle = strtoul(text, &end, 10);
  return errno || !handle || *end ? 0 : handle;
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
    if (request.capability) {
      // A lookup never accepts delegated authority. Release an unexpected
      // handle so an untrusted caller cannot exhaust this service's table.
      (void)syscall1(SYS_CAP_CLOSE, (long)request.capability);
    } else if (request.size >= 2 && request.payload[0] == MOSS_NAMESPACE_OPEN &&
               request.payload[request.size - 1] == 0) {
      if (request.size == 1 + sizeof(MOSS_SCRATCH_PATH) &&
          memcmp(request.payload + 1, MOSS_SCRATCH_PATH, sizeof(MOSS_SCRATCH_PATH)) == 0) {
        response.payload[0] = MOSS_NAMESPACE_OK;
        response.capability = file;
        response.rights = MOSS_CAP_SEND;
      } else {
        response.payload[0] = MOSS_NAMESPACE_NO_ENTRY;
      }
    }
    // The sender capability names the file-service incarnation. A restart
    // never changes the object behind an already transferred capability.
    (void)syscall2(SYS_IPC_REPLY, (long)reply, (long)&response);
  }
}
