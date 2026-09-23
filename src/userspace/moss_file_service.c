#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_file_protocol.h"
#include "syscall.h"

int main(int argc, char **argv) {
  if (argc != 2 || !argv[1]) {
    return 2;
  }
  char *end = NULL;
  errno = 0;
  unsigned long receive = strtoul(argv[1], &end, 10);
  if (errno || !receive || *end) {
    return 2;
  }

  // ponytail: one control message holds this volatile file; use a Memory
  // Object data plane when files need to exceed the bounded control payload.
  unsigned char file[MOSS_IPC_MAX_MESSAGE - 1] = {0};
  unsigned long length = 0;
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
    struct moss_ipc_message response = {.size = 1, .payload = {MOSS_FILE_BAD_REQUEST}};
    if (request.capability) {
      // A file request never accepts delegated authority; release unexpected
      // handles so an untrusted client cannot fill the service's handle table.
      (void)syscall1(SYS_CAP_CLOSE, (long)request.capability);
    } else if (request.size == 1 && request.payload[0] == MOSS_FILE_READ) {
      response.payload[0] = MOSS_FILE_OK;
      response.size = length + 1;
      memcpy(response.payload + 1, file, length);
    } else if (request.size >= 1 && request.payload[0] == MOSS_FILE_WRITE) {
      length = request.size - 1;
      memcpy(file, request.payload + 1, length);
      response.payload[0] = MOSS_FILE_OK;
    }
    // A timed-out caller may have discarded its Reply; the file remains
    // usable for later requests regardless of that caller's outcome.
    (void)syscall2(SYS_IPC_REPLY, (long)reply, (long)&response);
  }
}
