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

static long open_file(unsigned long file, const struct moss_ipc_message *lookup, struct moss_ipc_message *opened) {
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) != 0 || now > LONG_MAX - FILE_OPEN_TIMEOUT_NS) {
    return -1;
  }
  // The namespace owns absolute path policy; the file service sees only a
  // name relative to this root mount.
  struct moss_ipc_message request = {.size = lookup->size - 1, .payload = {MOSS_FILE_OPEN}};
  request.payload[1] = lookup->payload[1] & MOSS_NAMESPACE_OPEN_CREATE ? MOSS_FILE_OPEN_CREATE : 0;
  memcpy(request.payload + 2, lookup->payload + 3, lookup->size - 3);
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
    } else if (request.size >= 5 && request.payload[0] == MOSS_NAMESPACE_OPEN &&
               !(request.payload[1] & ~(MOSS_NAMESPACE_OPEN_CREATE | MOSS_NAMESPACE_OPEN_TRANSFER))) {
      const unsigned char *path = request.payload + 2;
      unsigned long path_size = request.size - 2;
      // This namespace currently mounts one flat root filesystem. Reject
      // ambiguous spellings before the filesystem chooses an object.
      if (path[0] == '/' && path[1] != 0 && memchr(path, 0, path_size) == path + path_size - 1 &&
          !memchr(path + 1, '/', path_size - 2) && !(path_size == 3 && path[1] == '.') &&
          !(path_size == 4 && path[1] == '.' && path[2] == '.')) {
        response.payload[0] = MOSS_NAMESPACE_UNAVAILABLE;
        long result = open_file(file, &request, &opened);
        if (result == 1 && opened.payload[0] == MOSS_FILE_OK && opened.capability &&
            opened.rights == (MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE)) {
          response.payload[0] = MOSS_NAMESPACE_OK;
          response.capability = opened.capability;
          response.rights = MOSS_CAP_SEND |
                            (request.payload[1] & MOSS_NAMESPACE_OPEN_TRANSFER
                                 ? MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE
                                 : 0);
        } else if (result == 1 && opened.payload[0] == MOSS_FILE_NO_ENTRY && !opened.capability) {
          response.payload[0] = MOSS_NAMESPACE_NO_ENTRY;
        }
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
