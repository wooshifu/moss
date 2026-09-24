#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_file_protocol.h"
#include "syscall.h"

enum { FILE_NAME_BYTES = MOSS_IPC_MAX_MESSAGE - 2 };
_Static_assert(MOSS_MEM_OBJECT_BYTES == 4096, "file protocol page size");

struct FileObject {
  struct FileObject *next;
  unsigned long badge;
  unsigned long length;
  unsigned long capacity;
  unsigned long name_size;
  char name[FILE_NAME_BYTES];
  unsigned char *data;
};

static int resize_file(struct FileObject *file, unsigned long new_length, unsigned long *allocated) {
  if (new_length > MOSS_FILE_CONTENT_BUDGET_BYTES) {
    return 0;
  }
  unsigned long new_capacity =
      ((new_length + MOSS_MEM_OBJECT_BYTES - 1) / MOSS_MEM_OBJECT_BYTES) * MOSS_MEM_OBJECT_BYTES;
  if (new_capacity > file->capacity) {
    unsigned long growth = new_capacity - file->capacity;
    if (growth > MOSS_FILE_CONTENT_BUDGET_BYTES - *allocated) {
      return 0;
    }
    unsigned char *data = realloc(file->data, new_capacity);
    if (!data) {
      return 0;
    }
    file->data = data;
    file->capacity = new_capacity;
    *allocated += growth;
  } else if (new_capacity < file->capacity) {
    if (!new_capacity) {
      free(file->data);
      file->data = NULL;
      *allocated -= file->capacity;
      file->capacity = 0;
    } else {
      // A failed best-effort shrink must not make truncation fail or claim
      // that memory was returned to the service budget.
      unsigned char *data = realloc(file->data, new_capacity);
      if (data) {
        file->data = data;
        *allocated -= file->capacity - new_capacity;
        file->capacity = new_capacity;
      }
    }
  }
  if (new_length > file->length) {
    // Sparse writes and extension must never expose prior file contents.
    memset(file->data + file->length, 0, new_length - file->length);
  }
  file->length = new_length;
  return 1;
}

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
  unsigned long allocated = 0;
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
      int valid_header = request.size == MOSS_FILE_IO_HEADER_BYTES;
      uint64_t offset = valid_header ? moss_file_get_u64(request.payload + 1) : 0;
      unsigned int count = valid_header ? moss_file_get_u16(request.payload + 9) : 0;
      int valid_count = valid_header && count <= MOSS_MEM_OBJECT_BYTES;
      int reading = file && valid_count && request.payload[0] == MOSS_FILE_READ && request.rights == MOSS_CAP_MAP_WRITE;
      int writing = file && valid_count && request.payload[0] == MOSS_FILE_WRITE &&
                    request.rights == MOSS_CAP_MAP_READ && offset <= MOSS_FILE_CONTENT_BUDGET_BYTES &&
                    count <= MOSS_FILE_CONTENT_BUDGET_BYTES - offset;
      if (reading || writing) {
        long mapped = syscall2(SYS_MEM_MAP, (long)request.capability, reading ? MOSS_CAP_MAP_WRITE : MOSS_CAP_MAP_READ);
        if (mapped > 0) {
          unsigned int transferred = 0;
          if (reading) {
            if (offset < file->length) {
              unsigned long available = file->length - (unsigned long)offset;
              transferred = available < count ? (unsigned int)available : count;
              memcpy((void *)mapped, file->data + offset, transferred);
            }
          } else if (!count || (offset + count <= file->length) ||
                     resize_file(file, (unsigned long)offset + count, &allocated)) {
            if (count) {
              memcpy(file->data + offset, (const void *)mapped, count);
            }
            transferred = count;
          } else {
            response.payload[0] = MOSS_FILE_UNAVAILABLE;
          }
          if (reading || !count || transferred == count) {
            response.payload[0] = MOSS_FILE_OK;
            response.size = MOSS_FILE_IO_REPLY_BYTES;
            moss_file_put_u16(response.payload + 1, transferred);
          }
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
               !(request.payload[1] & ~(MOSS_FILE_OPEN_CREATE | MOSS_FILE_OPEN_UNLISTED)) &&
               valid_name(request.payload + 2, request.size - 2)) {
      const unsigned char *name = request.payload + 2;
      unsigned long name_size = request.size - 2;
      int unlisted = !!(request.payload[1] & MOSS_FILE_OPEN_UNLISTED);
      file = unlisted ? NULL : find_name(files, name, name_size);
      struct FileObject *created = NULL;
      if (!file && (request.payload[1] & MOSS_FILE_OPEN_CREATE) && next_badge != 0 &&
          file_count < MOSS_FILE_OBJECT_LIMIT) {
        created = calloc(1, sizeof(*created));
        if (created) {
          created->badge = next_badge;
          // A zero name size is impossible for public OPEN. Keep the object
          // in the badge list without granting root senders a lookup path.
          if (!unlisted) {
            created->name_size = name_size;
            memcpy(created->name, name, name_size);
          }
          file = created;
        }
      }
      if (!file) {
        response.payload[0] = request.payload[1] & MOSS_FILE_OPEN_CREATE ? MOSS_FILE_UNAVAILABLE : MOSS_FILE_NO_ENTRY;
      } else {
        minted = syscall2(SYS_IPC_MINT_BADGE, (long)mint, (long)file->badge);
        if (minted > 0) {
          if (created) {
            // Publish a named file before replying so a timed-out client can
            // reopen its identity. Unlisted objects are deliberately reachable
            // only through the returned capability.
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
    } else if (file && request.size == MOSS_FILE_RESIZE_HEADER_BYTES && request.payload[0] == MOSS_FILE_RESIZE) {
      uint64_t length = moss_file_get_u64(request.payload + 1);
      response.payload[0] =
          length <= MOSS_FILE_CONTENT_BUDGET_BYTES && resize_file(file, (unsigned long)length, &allocated)
              ? MOSS_FILE_OK
              : MOSS_FILE_UNAVAILABLE;
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
