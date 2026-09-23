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

  // ponytail: one shared page holds this volatile file; add multi-page
  // objects when a real filesystem protocol needs a larger transfer window.
  unsigned char file[MOSS_MEM_OBJECT_BYTES] = {0};
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
      unsigned long count = (unsigned long)request.payload[1] | ((unsigned long)request.payload[2] << 8);
      int reading = request.badge == MOSS_FILE_SCRATCH_BADGE && request.size == 1 &&
                    request.payload[0] == MOSS_FILE_READ && request.rights == MOSS_CAP_MAP_WRITE;
      int writing = request.badge == MOSS_FILE_SCRATCH_BADGE && request.size == MOSS_FILE_MEMORY_HEADER_BYTES &&
                    request.payload[0] == MOSS_FILE_WRITE && request.rights == MOSS_CAP_MAP_READ &&
                    count <= sizeof(file);
      if (reading || writing) {
        long mapped = syscall2(SYS_MEM_MAP, (long)request.capability, reading ? MOSS_CAP_MAP_WRITE : MOSS_CAP_MAP_READ);
        if (mapped > 0) {
          if (reading) {
            memcpy((void *)mapped, file, length);
            response.size = MOSS_FILE_MEMORY_HEADER_BYTES;
            response.payload[1] = (unsigned char)(length & 0xff);
            response.payload[2] = (unsigned char)(length >> 8);
          } else {
            memcpy(file, (const void *)mapped, count);
            length = count;
          }
          response.payload[0] = MOSS_FILE_OK;
          // This service maps a new client page for each operation. A failed
          // unmap must stop it before leaked mappings accumulate indefinitely.
          if (syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES) != 0) {
            return 1;
          }
        }
      }
      // Even a malformed request may carry a transferred handle. Release it
      // after use so clients cannot exhaust the service's capability table.
      (void)syscall1(SYS_CAP_CLOSE, (long)request.capability);
    } else if (request.badge == MOSS_FILE_SCRATCH_BADGE && request.size == 1 && request.payload[0] == MOSS_FILE_READ &&
               length <= MOSS_IPC_MAX_MESSAGE - 1) {
      response.payload[0] = MOSS_FILE_OK;
      response.size = length + 1;
      memcpy(response.payload + 1, file, length);
    } else if (request.badge == MOSS_FILE_SCRATCH_BADGE && request.size >= 1 && request.payload[0] == MOSS_FILE_WRITE) {
      length = request.size - 1;
      memcpy(file, request.payload + 1, length);
      response.payload[0] = MOSS_FILE_OK;
    }
    // A timed-out caller may have discarded its Reply; the file remains
    // usable for later requests regardless of that caller's outcome.
    (void)syscall2(SYS_IPC_REPLY, (long)reply, (long)&response);
  }
}
