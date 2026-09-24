#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_file_protocol.h"
#include "syscall.h"

enum { FILE_NAME_BYTES = MOSS_IPC_MAX_MESSAGE - 2 };
_Static_assert(MOSS_MEM_OBJECT_BYTES == 4096, "file protocol page size");

// CPIO newc stores each numeric field as eight ASCII hex digits and pads
// records to four-byte boundaries. The service owns boot path parsing.
struct NewcHeader {
  char magic[6], ino[8], mode[8], uid[8], gid[8], nlink[8], mtime[8], filesize[8];
  char devmajor[8], devminor[8], rdevmajor[8], rdevminor[8], namesize[8], check[8];
};
_Static_assert(sizeof(struct NewcHeader) == 110, "CPIO newc header size");
enum { CPIO_TYPE_MASK = 0170000, CPIO_REGULAR = 0100000, CPIO_ALIGNMENT = 4 };

struct FileObject {
  struct FileObject *next;
  unsigned long badge;
  unsigned long length;
  unsigned long capacity;
  unsigned long name_size;
  unsigned int sealed;
  char name[FILE_NAME_BYTES];
  unsigned char *data;
  const unsigned char *boot_data;
};

static unsigned char resize_file(struct FileObject *file, unsigned long new_length, unsigned long *allocated) {
  if (file->boot_data)
    return MOSS_FILE_READ_ONLY;
  if (new_length > MOSS_FILE_CONTENT_BUDGET_BYTES)
    return MOSS_FILE_NO_SPACE;
  unsigned long new_capacity =
      ((new_length + MOSS_MEM_OBJECT_BYTES - 1) / MOSS_MEM_OBJECT_BYTES) * MOSS_MEM_OBJECT_BYTES;
  if (new_capacity > file->capacity) {
    unsigned long growth = new_capacity - file->capacity;
    if (growth > MOSS_FILE_CONTENT_BUDGET_BYTES - *allocated)
      return MOSS_FILE_NO_SPACE;
    unsigned char *data = realloc(file->data, new_capacity);
    if (!data)
      return MOSS_FILE_UNAVAILABLE;
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
  return MOSS_FILE_OK;
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

static int hex8(const char field[8], unsigned long *value) {
  unsigned long parsed = 0;
  for (unsigned int i = 0; i < 8; ++i) {
    unsigned char digit = (unsigned char)field[i];
    if (digit >= '0' && digit <= '9')
      digit -= '0';
    else if (digit >= 'a' && digit <= 'f')
      digit = digit - 'a' + 10;
    else if (digit >= 'A' && digit <= 'F')
      digit = digit - 'A' + 10;
    else
      return 0;
    parsed = (parsed << 4) | digit;
  }
  *value = parsed;
  return 1;
}

static unsigned long cpio_align(unsigned long value) {
  return (value + CPIO_ALIGNMENT - 1) & ~(unsigned long)(CPIO_ALIGNMENT - 1);
}

static int load_boot_files(const unsigned char *archive, unsigned long size, struct FileObject **files,
                           unsigned long *boot_count, unsigned long *next_badge) {
  unsigned long offset = 0;
  while (size - offset >= sizeof(struct NewcHeader)) {
    const struct NewcHeader *header = (const struct NewcHeader *)(archive + offset);
    unsigned long name_bytes = 0, file_bytes = 0, mode = 0;
    if (memcmp(header->magic, "070701", 6) != 0 || !hex8(header->namesize, &name_bytes) ||
        !hex8(header->filesize, &file_bytes) || !hex8(header->mode, &mode) ||
        name_bytes == 0 || name_bytes > size - offset - sizeof(*header))
      return 0;
    unsigned long record_size = size - offset;
    unsigned long data_offset = cpio_align(sizeof(*header) + name_bytes);
    if (data_offset > record_size || file_bytes > record_size - data_offset)
      return 0;
    unsigned long next_offset = cpio_align(data_offset + file_bytes);
    if (next_offset > record_size)
      return 0;
    const unsigned char *name = archive + offset + sizeof(*header);
    if (name[name_bytes - 1] != 0 || memchr(name, 0, name_bytes - 1))
      return 0;
    if (name_bytes == sizeof("TRAILER!!!") && memcmp(name, "TRAILER!!!", sizeof("TRAILER!!!")) == 0)
      return file_bytes == 0;
    if (name_bytes > 2 && name[0] == '.' && name[1] == '/') {
      name += 2;
      name_bytes -= 2;
    }
    if (name_bytes > 1 && name[0] == '/') {
      ++name;
      --name_bytes;
    }
    // The current namespace publishes flat root entries; deeper archive paths
    // remain hidden until it gains directories.
    if ((mode & CPIO_TYPE_MASK) == CPIO_REGULAR && valid_name(name, name_bytes)) {
      // The bootstrap parser accepts at most 64 entries. Keep that bound
      // separate from the client-created file budget.
      if (*boot_count == MOSS_FILE_BOOT_ENTRY_LIMIT || find_name(*files, name, name_bytes))
        return 0;
      struct FileObject *file = calloc(1, sizeof(*file));
      if (!file)
        return 0;
      file->badge = (*next_badge)++;
      file->name_size = name_bytes;
      memcpy(file->name, name, name_bytes);
      file->length = file_bytes;
      file->boot_data = archive + offset + data_offset;
      file->next = *files;
      *files = file;
      ++*boot_count;
    }
    offset += next_offset;
  }
  return 0; // A valid newc archive ends with TRAILER!!!.
}

static void list_root(struct FileObject *files, unsigned long cookie, unsigned char *page,
                      struct moss_ipc_message *response) {
  const char *name = NULL;
  unsigned long id = MOSS_FILE_ROOT_BADGE;
  unsigned long next_cookie = cookie + 1;
  unsigned int name_size = 0;
  unsigned char type = MOSS_FILE_TYPE_DIRECTORY;
  if (cookie < 2) {
    name = cookie ? ".." : ".";
    name_size = cookie ? sizeof("..") : sizeof(".");
  } else {
    struct FileObject *next = NULL;
    // Badges are never recycled, so insertion cannot shift an existing
    // cursor. Unlisted Loader images have no name and stay invisible here.
    for (struct FileObject *file = files; file; file = file->next) {
      if (file->name_size && file->badge > cookie - 2 && (!next || file->badge < next->badge))
        next = file;
    }
    if (!next) {
      response->payload[0] = MOSS_FILE_END;
      return;
    }
    name = next->name;
    name_size = next->name_size;
    id = next->badge;
    next_cookie = id + 2;
    type = MOSS_FILE_TYPE_REGULAR;
  }
  memcpy(page, name, name_size);
  response->size = MOSS_FILE_LIST_REPLY_BYTES;
  response->payload[0] = MOSS_FILE_OK;
  response->payload[1] = type;
  moss_file_put_u64(response->payload + 2, id);
  moss_file_put_u64(response->payload + 10, next_cookie);
  moss_file_put_u16(response->payload + 18, name_size);
}

int main(int argc, char **argv) {
  if (argc != 5 || !argv[1] || !argv[2] || !argv[3] || !argv[4]) {
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
  errno = 0;
  unsigned long archive_cap = strtoul(argv[3], &end, 10);
  if (errno || !archive_cap || *end)
    return 2;
  errno = 0;
  unsigned long archive_size = strtoul(argv[4], &end, 10);
  if (errno || !archive_size || *end)
    return 2;
  long mapped_archive = syscall2(SYS_MEM_MAP, archive_cap, MOSS_CAP_MAP_READ);
  (void)syscall1(SYS_CAP_CLOSE, archive_cap);
  if (mapped_archive <= 0)
    return 2;

  // Existing clients can always reopen /scratch after a volatile service
  // restart. New files are created explicitly and live until this service dies.
  static struct FileObject scratch = {
      .badge = MOSS_FILE_SCRATCH_BADGE, .name_size = sizeof("scratch"), .name = "scratch"};
  struct FileObject *files = &scratch;
  unsigned long volatile_count = 1;
  unsigned long boot_count = 0;
  unsigned long allocated = 0;
  // Never recycle a badge while old file capabilities may still exist.
  unsigned long next_badge = MOSS_FILE_SCRATCH_BADGE + 1;
  if (!load_boot_files((const unsigned char *)mapped_archive, archive_size, &files, &boot_count, &next_badge))
    return 2;
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
    struct FileObject *snapshot = NULL;
    struct FileObject *file = find_badge(files, request.badge);
    if (request.capability && request.badge == MOSS_FILE_ROOT_BADGE && request.size == MOSS_FILE_LIST_BYTES &&
        request.payload[0] == MOSS_FILE_LIST && request.rights == MOSS_CAP_MAP_WRITE) {
      long mapped = syscall2(SYS_MEM_MAP, (long)request.capability, MOSS_CAP_MAP_WRITE);
      if (mapped > 0) {
        list_root(files, moss_file_get_u64(request.payload + 1), (unsigned char *)mapped, &response);
        if (syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES) != 0)
          return 1;
      }
      (void)syscall1(SYS_CAP_CLOSE, (long)request.capability);
    } else if (request.capability) {
      int valid_header = request.size == MOSS_FILE_IO_HEADER_BYTES;
      uint64_t offset = valid_header ? moss_file_get_u64(request.payload + 1) : 0;
      unsigned int count = valid_header ? moss_file_get_u16(request.payload + 9) : 0;
      int valid_count = valid_header && count <= MOSS_MEM_OBJECT_BYTES;
      int reading = file && valid_count && request.payload[0] == MOSS_FILE_READ && request.rights == MOSS_CAP_MAP_WRITE;
      int write_request = file && !file->boot_data && !file->sealed && valid_count &&
                          request.payload[0] == MOSS_FILE_WRITE && request.rights == MOSS_CAP_MAP_READ;
      int writing =
          write_request && offset <= MOSS_FILE_CONTENT_BUDGET_BYTES && count <= MOSS_FILE_CONTENT_BUDGET_BYTES - offset;
      int appending = file && !file->boot_data && !file->sealed && valid_count &&
                      request.payload[0] == MOSS_FILE_APPEND && request.rights == MOSS_CAP_MAP_READ && offset == 0;
      if (reading || writing || appending) {
        long mapped = syscall2(SYS_MEM_MAP, (long)request.capability, reading ? MOSS_CAP_MAP_WRITE : MOSS_CAP_MAP_READ);
        if (mapped > 0) {
          unsigned int transferred = 0;
          unsigned long write_offset = appending ? file->length : (unsigned long)offset;
          unsigned char resize_status = MOSS_FILE_OK;
          if (!reading && count && write_offset + count > file->length)
            resize_status = resize_file(file, write_offset + count, &allocated);
          if (reading) {
            if (offset < file->length) {
              unsigned long available = file->length - (unsigned long)offset;
              transferred = available < count ? (unsigned int)available : count;
              const unsigned char *data = file->boot_data ? file->boot_data : file->data;
              memcpy((void *)mapped, data + offset, transferred);
            }
          } else if (resize_status == MOSS_FILE_OK) {
            if (count) {
              memcpy(file->data + write_offset, (const void *)mapped, count);
            }
            transferred = count;
          } else {
            response.payload[0] = resize_status;
          }
          if (reading || !count || transferred == count) {
            response.payload[0] = MOSS_FILE_OK;
            response.size = appending ? MOSS_FILE_APPEND_REPLY_BYTES : MOSS_FILE_IO_REPLY_BYTES;
            if (appending) {
              moss_file_put_u64(response.payload + 1, write_offset);
              moss_file_put_u16(response.payload + 9, transferred);
            } else {
              moss_file_put_u16(response.payload + 1, transferred);
            }
          }
          // This service maps a new client page for each operation. A failed
          // unmap must stop it before leaked mappings accumulate indefinitely.
          if (syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES) != 0) {
            return 1;
          }
        }
      } else if (write_request) {
        response.payload[0] = MOSS_FILE_NO_SPACE;
      }
      // Even a malformed request may carry a transferred handle. Release it
      // after use so clients cannot exhaust the service's capability table.
      (void)syscall1(SYS_CAP_CLOSE, (long)request.capability);
    } else if (request.badge == 0 && request.size == 1 && request.payload[0] == MOSS_FILE_ROOT && !request.rights) {
      minted = syscall2(SYS_IPC_MINT_BADGE, (long)mint, MOSS_FILE_ROOT_BADGE);
      if (minted > 0) {
        response.payload[0] = MOSS_FILE_OK;
        response.capability = (unsigned long)minted;
        response.rights = MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE;
      } else {
        response.payload[0] = MOSS_FILE_UNAVAILABLE;
      }
    } else if (request.badge == 0 && request.size >= 4 && request.payload[0] == MOSS_FILE_OPEN &&
               !(request.payload[1] &
                 ~(MOSS_FILE_OPEN_CREATE | MOSS_FILE_OPEN_EXCLUSIVE | MOSS_FILE_OPEN_UNLISTED | MOSS_FILE_OPEN_WRITE)) &&
               (!(request.payload[1] & (MOSS_FILE_OPEN_EXCLUSIVE | MOSS_FILE_OPEN_UNLISTED)) ||
                (request.payload[1] & MOSS_FILE_OPEN_CREATE)) &&
               !(request.payload[1] & MOSS_FILE_OPEN_EXCLUSIVE && request.payload[1] & MOSS_FILE_OPEN_UNLISTED) &&
               valid_name(request.payload + 2, request.size - 2)) {
      const unsigned char *name = request.payload + 2;
      unsigned long name_size = request.size - 2;
      int unlisted = !!(request.payload[1] & MOSS_FILE_OPEN_UNLISTED);
      // Lookup and creation stay in this serial loop so exclusive creators
      // cannot both observe a missing name before either publishes its badge.
      file = unlisted ? NULL : find_name(files, name, name_size);
      struct FileObject *created = NULL;
      // Directory cookies add two synthetic entries to each file badge.
      if (!file && (request.payload[1] & MOSS_FILE_OPEN_CREATE) && next_badge < MOSS_FILE_ROOT_BADGE - 2 &&
          volatile_count < MOSS_FILE_OBJECT_LIMIT) {
        created = calloc(1, sizeof(*created));
        if (created) {
          created->badge = next_badge;
          // An unlisted object stays addressable by badge but has no root name.
          if (!unlisted) {
            created->name_size = name_size;
            memcpy(created->name, name, name_size);
          }
          file = created;
        }
      }
      if (file && !created && (request.payload[1] & MOSS_FILE_OPEN_EXCLUSIVE)) {
        response.payload[0] = MOSS_FILE_EXISTS;
      } else if (file && file->boot_data && (request.payload[1] & MOSS_FILE_OPEN_WRITE)) {
        response.payload[0] = MOSS_FILE_READ_ONLY;
      } else if (!file) {
        response.payload[0] = request.payload[1] & MOSS_FILE_OPEN_CREATE ? MOSS_FILE_UNAVAILABLE : MOSS_FILE_NO_ENTRY;
      } else {
        minted = syscall2(SYS_IPC_MINT_BADGE, (long)mint, (long)file->badge);
        if (minted > 0) {
          if (created) {
            // Named files publish before replying so a timed-out caller can
            // reopen one identity. Unlisted objects remain capability-only.
            created->next = files;
            files = created;
            ++volatile_count;
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
    } else if (file && request.size == 1 && request.payload[0] == MOSS_FILE_SIZE && !request.rights) {
      response.payload[0] = MOSS_FILE_OK;
      response.size = MOSS_FILE_SIZE_REPLY_BYTES;
      moss_file_put_u64(response.payload + 1, file->length);
    } else if ((file || request.badge == MOSS_FILE_ROOT_BADGE) && request.size == 1 &&
               request.payload[0] == MOSS_FILE_STAT && !request.rights) {
      response.payload[0] = MOSS_FILE_OK;
      response.size = MOSS_FILE_STAT_REPLY_BYTES;
      moss_file_put_u64(response.payload + 1, file ? file->badge : MOSS_FILE_ROOT_BADGE);
      moss_file_put_u64(response.payload + 9, file ? file->length : 0);
    } else if (file && !file->name_size && request.size == 1 && request.payload[0] == MOSS_FILE_SEAL &&
               !request.rights) {
      // The single receiver orders this transition after earlier writes and
      // before every later request through any sender copy. Public names stay
      // mutable because a reader must not be able to deny writers service.
      file->sealed = 1;
      response.payload[0] = MOSS_FILE_OK;
    } else if (request.badge == 0 && request.size >= 4 && request.payload[0] == MOSS_FILE_SNAPSHOT &&
               request.payload[1] == 0 && valid_name(request.payload + 2, request.size - 2) && !request.rights) {
      file = find_name(files, request.payload + 2, request.size - 2);
      if (!file) {
        response.payload[0] = MOSS_FILE_NO_ENTRY;
      } else {
        response.payload[0] = MOSS_FILE_UNAVAILABLE;
        if (next_badge < MOSS_FILE_ROOT_BADGE - 2 && volatile_count < MOSS_FILE_OBJECT_LIMIT) {
          struct FileObject *copy = calloc(1, sizeof(*copy));
          if (copy && resize_file(copy, file->length, &allocated) == MOSS_FILE_OK) {
            if (file->length) {
              memcpy(copy->data, file->boot_data ? file->boot_data : file->data, file->length);
            }
            copy->badge = next_badge;
            copy->sealed = 1;
            minted = syscall2(SYS_IPC_MINT_BADGE, (long)mint, (long)copy->badge);
            if (minted > 0) {
              copy->next = files;
              files = copy;
              snapshot = copy;
              ++volatile_count;
              ++next_badge;
              response.payload[0] = MOSS_FILE_OK;
              response.capability = (unsigned long)minted;
              response.rights = MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE;
            }
          }
          if (!snapshot && copy) {
            allocated -= copy->capacity;
            free(copy->data);
            free(copy);
          }
        }
      }
    } else if (file && request.size == MOSS_FILE_RESIZE_HEADER_BYTES && request.payload[0] == MOSS_FILE_RESIZE) {
      uint64_t length = moss_file_get_u64(request.payload + 1);
      if (file->sealed) {
        response.payload[0] = MOSS_FILE_BAD_REQUEST;
      } else {
        response.payload[0] = length > MOSS_FILE_CONTENT_BUDGET_BYTES
                                  ? MOSS_FILE_NO_SPACE
                                  : resize_file(file, (unsigned long)length, &allocated);
      }
    }
    // A timed-out caller does not invalidate the existing source file.
    long replied = syscall2(SYS_IPC_REPLY, (long)reply, (long)&response);
    if (snapshot && replied != 0) {
      // A failed handoff must not strand a private clone in the bounded file
      // table or content budget. This single receiver cannot change the list
      // head between linking the clone and finishing its Reply.
      files = snapshot->next;
      --volatile_count;
      allocated -= snapshot->capacity;
      free(snapshot->data);
      free(snapshot);
    }
    // Reply transfer snapshots the handle. Keep no local object handle after
    // the reply, including when the caller timed out before delivery.
    if (minted > 0) {
      (void)syscall1(SYS_CAP_CLOSE, minted);
    }
  }
}
