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

static long file_call(unsigned long file, const struct moss_ipc_message *request, struct moss_ipc_message *response) {
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) != 0 || now > LONG_MAX - FILE_OPEN_TIMEOUT_NS) {
    return -1;
  }
  return syscall6(SYS_IPC_CALL, (long)file, (long)request, (long)response, (long)(now + FILE_OPEN_TIMEOUT_NS), 0, 0);
}

static long open_file(unsigned long file, const struct moss_ipc_message *lookup, struct moss_ipc_message *opened) {
  // The namespace owns absolute path policy; the file service sees only a
  // name relative to this root mount.
  struct moss_ipc_message request = {0};
  if (lookup->size == 4) {
    request.size = 1;
    request.payload[0] = MOSS_FILE_ROOT;
  } else {
    request.size = lookup->size - 1;
    request.payload[0] = MOSS_FILE_OPEN;
    request.payload[1] = (lookup->payload[1] & MOSS_NAMESPACE_OPEN_CREATE ? MOSS_FILE_OPEN_CREATE : 0) |
                         (lookup->payload[1] & MOSS_NAMESPACE_OPEN_EXCLUSIVE ? MOSS_FILE_OPEN_EXCLUSIVE : 0);
    memcpy(request.payload + 2, lookup->payload + 3, lookup->size - 3);
  }
  return file_call(file, &request, opened);
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
    } else if (request.size >= 4 &&
               (request.payload[0] == MOSS_NAMESPACE_OPEN || request.payload[0] == MOSS_NAMESPACE_STAT) &&
               (request.payload[0] == MOSS_NAMESPACE_OPEN || request.payload[1] == 0) &&
               !(request.payload[1] &
                 ~(MOSS_NAMESPACE_OPEN_CREATE | MOSS_NAMESPACE_OPEN_TRANSFER | MOSS_NAMESPACE_OPEN_EXCLUSIVE)) &&
               (!(request.payload[1] & MOSS_NAMESPACE_OPEN_EXCLUSIVE) ||
                (request.payload[1] & MOSS_NAMESPACE_OPEN_CREATE))) {
      const unsigned char *path = request.payload + 2;
      unsigned long path_size = request.size - 2;
      // This namespace currently mounts one flat root filesystem. Reject
      // ambiguous spellings before the filesystem chooses an object.
      int root = path_size == 2 && path[0] == '/' && path[1] == 0 && request.payload[1] == 0;
      int regular = path[0] == '/' && path[1] != 0 && memchr(path, 0, path_size) == path + path_size - 1 &&
                    !memchr(path + 1, '/', path_size - 2) && !(path_size == 3 && path[1] == '.') &&
                    !(path_size == 4 && path[1] == '.' && path[2] == '.');
      if (root || regular) {
        response.payload[0] = MOSS_NAMESPACE_UNAVAILABLE;
        long result = open_file(file, &request, &opened);
        if (result == 1 && opened.payload[0] == MOSS_FILE_OK && opened.capability &&
            opened.rights == (MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE)) {
          if (request.payload[0] == MOSS_NAMESPACE_STAT) {
            struct moss_ipc_message stat_request = {.size = 1, .payload = {MOSS_FILE_STAT}};
            struct moss_ipc_message stat_response = {0};
            long stat_result = file_call(opened.capability, &stat_request, &stat_response);
            if (stat_result == MOSS_FILE_STAT_REPLY_BYTES && stat_response.payload[0] == MOSS_FILE_OK &&
                !stat_response.capability && !stat_response.rights && moss_file_get_u64(stat_response.payload + 1) &&
                moss_file_get_u64(stat_response.payload + 9) <= MOSS_FILE_CONTENT_BUDGET_BYTES) {
              response.size = MOSS_NAMESPACE_STAT_REPLY_BYTES;
              response.payload[0] = MOSS_NAMESPACE_OK;
              response.payload[1] = root ? MOSS_NAMESPACE_KIND_DIRECTORY : MOSS_NAMESPACE_KIND_FILE;
              memcpy(response.payload + 2, stat_response.payload + 1, 16);
            }
            if (stat_response.capability)
              (void)syscall1(SYS_CAP_CLOSE, (long)stat_response.capability);
          } else {
            response.payload[0] = MOSS_NAMESPACE_OK;
            response.capability = opened.capability;
            response.rights =
                MOSS_CAP_SEND |
                (request.payload[1] & MOSS_NAMESPACE_OPEN_TRANSFER ? MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE : 0);
          }
        } else if (result == 1 && opened.payload[0] == MOSS_FILE_NO_ENTRY && !opened.capability) {
          response.payload[0] = MOSS_NAMESPACE_NO_ENTRY;
        } else if (result == 1 && opened.payload[0] == MOSS_FILE_EXISTS && !opened.capability && !opened.rights) {
          response.payload[0] = MOSS_NAMESPACE_EXISTS;
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
