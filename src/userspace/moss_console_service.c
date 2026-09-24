#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_console_protocol.h"
#include "syscall.h"

enum { INPUT_BADGE = 1 };
// ponytail: A physical reader would race the legacy shell for fd 0; start a
// supervised reader domain when managed libc switches to this descriptor view.
static unsigned char input[MOSS_CONSOLE_RING_BYTES];
static unsigned int head;
static unsigned int length;

static unsigned long parse_handle(const char *text) {
  if (!text)
    return 0;
  char *end = NULL;
  errno = 0;
  unsigned long handle = strtoul(text, &end, 10);
  return errno || !handle || handle > LONG_MAX || *end ? 0 : handle;
}

int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  unsigned long receive = parse_handle(argv[1]);
  unsigned long mint = parse_handle(argv[2]);
  if (!receive || !mint)
    return 2;

  for (;;) {
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    long received = syscall3(SYS_IPC_RECEIVE, (long)receive, (long)&request, (long)&reply);
    if (received == -EINTR)
      continue;
    if (received < 0)
      return 1;

    struct moss_ipc_message response = {.size = 1, .payload = {MOSS_CONSOLE_BAD_REQUEST}};
    long minted = 0;
    unsigned int consumed = 0;
    int fed = 0;
    if (request.badge == 0 && request.size == 1 && request.payload[0] == MOSS_CONSOLE_INPUT_CAP &&
        !request.capability && !request.rights) {
      minted = syscall2(SYS_IPC_MINT_BADGE, (long)mint, INPUT_BADGE);
      if (minted > 0) {
        response.payload[0] = MOSS_CONSOLE_OK;
        response.capability = (unsigned long)minted;
        response.rights = MOSS_CAP_SEND | MOSS_CAP_DUPLICATE;
      } else {
        response.payload[0] = MOSS_CONSOLE_UNAVAILABLE;
      }
    } else if (request.badge == INPUT_BADGE && request.size == 2 && request.payload[0] == MOSS_CONSOLE_FEED &&
               !request.capability && !request.rights) {
      if (length == MOSS_CONSOLE_RING_BYTES) {
        response.payload[0] = MOSS_CONSOLE_WOULD_BLOCK;
      } else {
        response.payload[0] = MOSS_CONSOLE_OK;
        fed = 1;
      }
    } else if (request.badge == 0 && request.capability && request.size == MOSS_CONSOLE_IO_BYTES) {
      unsigned char operation = request.payload[0];
      unsigned int stream = request.payload[1];
      unsigned int count = moss_console_get_u16(request.payload + 2);
      int reading = operation == MOSS_CONSOLE_READ && stream == 0 && request.rights == MOSS_CAP_MAP_WRITE;
      int writing =
          operation == MOSS_CONSOLE_WRITE && (stream == 1 || stream == 2) && request.rights == MOSS_CAP_MAP_READ;
      if ((reading || writing) && count <= MOSS_MEM_OBJECT_BYTES) {
        if (!count) {
          response.size = MOSS_CONSOLE_IO_REPLY_BYTES;
          response.payload[0] = MOSS_CONSOLE_OK;
          moss_console_put_u16(response.payload + 1, 0);
        } else if (reading && !length) {
          response.payload[0] = MOSS_CONSOLE_WOULD_BLOCK;
        } else {
          long mapped =
              syscall2(SYS_MEM_MAP, (long)request.capability, reading ? MOSS_CAP_MAP_WRITE : MOSS_CAP_MAP_READ);
          if (mapped > 0) {
            long transferred = 0;
            if (reading) {
              consumed = count < length ? count : length;
              unsigned int first =
                  consumed < MOSS_CONSOLE_RING_BYTES - head ? consumed : MOSS_CONSOLE_RING_BYTES - head;
              memcpy((void *)mapped, input + head, first);
              memcpy((unsigned char *)mapped + first, input, consumed - first);
              transferred = consumed;
            } else {
              transferred = syscall3(SYS_WRITE, stream, mapped, count);
            }
            if (transferred >= 0 && transferred <= count) {
              response.size = MOSS_CONSOLE_IO_REPLY_BYTES;
              response.payload[0] = MOSS_CONSOLE_OK;
              moss_console_put_u16(response.payload + 1, (unsigned int)transferred);
            } else {
              response.payload[0] = MOSS_CONSOLE_UNAVAILABLE;
            }
            if (syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES) != 0)
              return 1;
          } else {
            response.payload[0] = MOSS_CONSOLE_UNAVAILABLE;
          }
        }
      }
    }

    if (request.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)request.capability);
    long sent = syscall2(SYS_IPC_REPLY, (long)reply, (long)&response);
    // Input ownership changes only after the immediate IPC reply commits;
    // a timed-out feeder must be able to retry without duplicating a byte.
    if (sent == 0 && fed) {
      input[(head + length) % MOSS_CONSOLE_RING_BYTES] = request.payload[1];
      ++length;
    }
    if (sent == 0 && consumed) {
      head = (head + consumed) % MOSS_CONSOLE_RING_BYTES;
      length -= consumed;
    }
    if (minted > 0)
      (void)syscall1(SYS_CAP_CLOSE, minted);
  }
}
