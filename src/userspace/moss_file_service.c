#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_file_protocol.h"
#include "syscall.h"

enum { FILE_NAME_BYTES = MOSS_IPC_MAX_MESSAGE - 2 };
// Until per-service resource accounting exists, sixteen one-page files bound
// client-triggered content allocation to 64 KiB per service incarnation.
enum { FILE_OBJECT_LIMIT = 16 };

struct FileObject {
  struct FileObject *next;
  unsigned long badge;
  unsigned long length;
  unsigned long name_size;
  char name[FILE_NAME_BYTES];
  unsigned char data[MOSS_MEM_OBJECT_BYTES];
};

static int valid_name(const unsigned char *name, unsigned long size) {
  if (size < 2 || size > FILE_NAME_BYTES || name[0] == 0 || memchr(name, 0, size) != name + size - 1 ||
      memchr(name, '/', size - 1)) {
    return 0;
  }
  return !(size == 2 && name[0] == '.') && !(size == 3 && name[0] == '.' && name[1] == '.');
}

// ponytail: linear scans suit the current volatile root; index names when
// lookup cost becomes measurable with a larger file set.
static struct FileObject *find_name(struct FileObject *files, const unsigned char *name, unsigned long size) {
  for (struct FileObject *file = files; file; file = file->next) {
    if (file->name_size == size && memcmp(file->name, name, size) == 0) {
      return file;
    }
  }
  return NULL;
}

static struct FileObject *find_badge(struct FileObject *files, unsigned long badge) {
  for (struct FileObject *file = files; file; file = file->next) {
    if (file->badge == badge) {
      return file;
    }
  }
  return NULL;
}

int main(int argc, char **argv) {
  if (argc != 3 || !argv[1] || !argv[2]) {
    return 2;
  }
  char *end = NULL;
  errno = 0;
  unsigned long receive = strtoul(argv[1], &end, 10);
  if (errno || !receive || *end) {
    return 2;
  }
  errno = 0;
  unsigned long mint = strtoul(argv[2], &end, 10);
  if (errno || !mint || *end) {
    return 2;
  }

  // Existing clients can always reopen /scratch after a volatile service
  // restart. New files are created explicitly and live until this service dies.
  static struct FileObject scratch = {
      .badge = MOSS_FILE_SCRATCH_BADGE, .name_size = sizeof("scratch"), .name = "scratch"};
  struct FileObject *files = &scratch;
  unsigned long file_count = 1;
  // Never recycle a badge while old file capabilities may still exist.
  unsigned long next_badge = MOSS_FILE_SCRATCH_BADGE + 1;
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
    long minted = 0;
    struct FileObject *file = find_badge(files, request.badge);
    if (request.capability) {
      unsigned long count = (unsigned long)request.payload[1] | ((unsigned long)request.payload[2] << 8);
      int reading =
          file && request.size == 1 && request.payload[0] == MOSS_FILE_READ && request.rights == MOSS_CAP_MAP_WRITE;
      int writing = file && request.size == MOSS_FILE_MEMORY_HEADER_BYTES && request.payload[0] == MOSS_FILE_WRITE &&
                    request.rights == MOSS_CAP_MAP_READ && count <= sizeof(file->data);
      if (reading || writing) {
        long mapped = syscall2(SYS_MEM_MAP, (long)request.capability, reading ? MOSS_CAP_MAP_WRITE : MOSS_CAP_MAP_READ);
        if (mapped > 0) {
          if (reading) {
            memcpy((void *)mapped, file->data, file->length);
            response.size = MOSS_FILE_MEMORY_HEADER_BYTES;
            response.payload[1] = (unsigned char)(file->length & 0xff);
            response.payload[2] = (unsigned char)(file->length >> 8);
          } else {
            memcpy(file->data, (const void *)mapped, count);
            file->length = count;
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
    } else if (request.badge == 0 && request.size >= 4 && request.payload[0] == MOSS_FILE_OPEN &&
               !(request.payload[1] & ~MOSS_FILE_OPEN_CREATE) && valid_name(request.payload + 2, request.size - 2)) {
      const unsigned char *name = request.payload + 2;
      unsigned long name_size = request.size - 2;
      file = find_name(files, name, name_size);
      struct FileObject *created = NULL;
      if (!file && (request.payload[1] & MOSS_FILE_OPEN_CREATE) && next_badge != 0 && file_count < FILE_OBJECT_LIMIT) {
        created = calloc(1, sizeof(*created));
        if (created) {
          created->badge = next_badge;
          created->name_size = name_size;
          memcpy(created->name, name, name_size);
          file = created;
        }
      }
      if (!file) {
        response.payload[0] = request.payload[1] & MOSS_FILE_OPEN_CREATE ? MOSS_FILE_UNAVAILABLE : MOSS_FILE_NO_ENTRY;
      } else {
        minted = syscall2(SYS_IPC_MINT_BADGE, (long)mint, (long)file->badge);
        if (minted > 0) {
          if (created) {
            // Publish before replying so a timed-out client can reopen the
            // same object, rather than creating a second identity on retry.
            created->next = files;
            files = created;
            ++file_count;
            ++next_badge;
          }
          response.payload[0] = MOSS_FILE_OK;
          response.capability = (unsigned long)minted;
          // Forwarding a reduced SEND handle requires TRANSFER and DUPLICATE.
          response.rights = MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE;
        } else {
          free(created);
          response.payload[0] = MOSS_FILE_UNAVAILABLE;
        }
      }
    } else if (file && request.size == 1 && request.payload[0] == MOSS_FILE_READ &&
               file->length <= MOSS_IPC_MAX_MESSAGE - 1) {
      response.payload[0] = MOSS_FILE_OK;
      response.size = file->length + 1;
      memcpy(response.payload + 1, file->data, file->length);
    } else if (file && request.size >= 1 && request.payload[0] == MOSS_FILE_WRITE) {
      file->length = request.size - 1;
      memcpy(file->data, request.payload + 1, file->length);
      response.payload[0] = MOSS_FILE_OK;
    }
    // A timed-out caller may have discarded its Reply; the file remains
    // usable for later requests regardless of that caller's outcome.
    (void)syscall2(SYS_IPC_REPLY, (long)reply, (long)&response);
    // Reply transfer snapshots the handle. Keep no local object handle after
    // the reply, including when the caller timed out before delivery.
    if (minted > 0) {
      (void)syscall1(SYS_CAP_CLOSE, minted);
    }
  }
}
