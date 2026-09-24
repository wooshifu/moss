#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_code_authority_protocol.h"
#include "syscall.h"

static long parse_handle(const char *text) {
  if (!text || *text < '0' || *text > '9') {
    return 0;
  }
  char *end = NULL;
  errno = 0;
  unsigned long value = strtoul(text, &end, 10);
  return errno || !value || value > LONG_MAX || *end ? 0 : (long)value;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    return 2;
  }
  long receive = parse_handle(argv[1]);
  long approver = parse_handle(argv[2]);
  if (!receive || !approver) {
    return 2;
  }

  for (;;) {
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    long received = syscall3(SYS_IPC_RECEIVE, receive, (long)&request, (long)&reply);
    if (received == -EINTR) {
      continue;
    }
    if (received < 0) {
      return 1;
    }

    struct moss_ipc_message response = {.size = 1, .payload = {MOSS_CODE_BAD_REQUEST}};
    long approved = 0;
    if (request.size == 1 && request.payload[0] == MOSS_CODE_APPROVE && request.badge == 0 && request.capability &&
        request.rights == MOSS_CAP_MAP_READ && syscall1(SYS_CODE_PAGE_COUNT, (long)request.capability) > 0) {
      approved = syscall2(SYS_CODE_APPROVE, approver, (long)request.capability);
      response.payload[0] = approved > 0 ? MOSS_CODE_OK : MOSS_CODE_UNAVAILABLE;
      if (approved > 0) {
        response.capability = (unsigned long)approved;
        response.rights = MOSS_CAP_CODE_EXEC | MOSS_CAP_CODE_IDENTIFY | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE;
      }
    }
    if (request.capability) {
      (void)syscall1(SYS_CAP_CLOSE, (long)request.capability);
    }
    if (syscall2(SYS_IPC_REPLY, (long)reply, (long)&response) != 0) {
      (void)syscall1(SYS_CAP_CLOSE, (long)reply);
    }
    if (approved > 0) {
      (void)syscall1(SYS_CAP_CLOSE, approved);
    }
  }
}
