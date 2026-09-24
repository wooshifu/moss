#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_console_protocol.h"
#include "moss_file_protocol.h"
#include "moss_namespace_protocol.h"
#include "moss_pipe_protocol.h"
#include "moss_process_protocol.h"
#include "syscall.h"

enum {
  DESCRIPTION_FILE = MOSS_PROCESS_FD_KIND_FILE,
  DESCRIPTION_PIPE = MOSS_PROCESS_FD_KIND_PIPE,
  DESCRIPTION_CONSOLE = MOSS_PROCESS_FD_KIND_CONSOLE
};
enum { BACKEND_WOULD_BLOCK = 2, BACKEND_BROKEN_PIPE = 3 };

struct OpenDescription {
  unsigned long object;
  unsigned long offset;
  unsigned long references;
  unsigned char flags;
  unsigned char uncertain;
  unsigned char kind;
  unsigned char stream;
};

struct Descriptor {
  struct Descriptor *next;
  struct OpenDescription *description;
  unsigned long number;
  unsigned char close_on_exec;
};

struct Record {
  struct Record *next;
  unsigned long id;
  unsigned long parent_id;
  unsigned long group_id;
  unsigned long session_id;
  // A reserved child has an identity before fork and no domain until attach.
  unsigned long domain;
  unsigned long reservation_deadline_ns;
  unsigned char orphaned;
  struct Descriptor *descriptors;
};

// Retain empty nodes for reuse: reply rollback keeps record pointers until
// after SYS_IPC_REPLY, so allocations must not move live records.
static struct Record *records;
static void pipe_close(unsigned long endpoint);

static void release_description(struct OpenDescription *description) {
  if (description && --description->references == 0) {
    if (description->kind == DESCRIPTION_PIPE)
      pipe_close(description->object);
    (void)syscall1(SYS_CAP_CLOSE, (long)description->object);
    free(description);
  }
}

static void drop_descriptors(struct Record *record) {
  struct Descriptor *entry = record->descriptors;
  record->descriptors = NULL;
  while (entry) {
    struct Descriptor *next = entry->next;
    release_description(entry->description);
    free(entry);
    entry = next;
  }
}

static void clear_record(struct Record *record) {
  struct Record *next = record->next;
  drop_descriptors(record);
  *record = (struct Record){.next = next};
}

static struct Descriptor *find_descriptor(struct Record *record, unsigned long number) {
  if (!record || number >= MOSS_PROCESS_FD_LIMIT)
    return NULL;
  for (struct Descriptor *entry = record->descriptors; entry; entry = entry->next) {
    if (entry->number == number)
      return entry;
  }
  return NULL;
}

static struct Descriptor *add_descriptor_at(struct Record *record, struct OpenDescription *description,
                                            unsigned long number) {
  if (number >= MOSS_PROCESS_FD_LIMIT || find_descriptor(record, number))
    return NULL;
  struct Descriptor *entry = calloc(1, sizeof(*entry));
  if (!entry)
    return NULL;
  entry->number = number;
  entry->description = description;
  ++description->references;
  entry->next = record->descriptors;
  record->descriptors = entry;
  return entry;
}

static unsigned long first_free_descriptor(struct Record *record, unsigned long minimum) {
  unsigned long number = minimum;
  while (number < MOSS_PROCESS_FD_LIMIT && find_descriptor(record, number))
    ++number;
  return number;
}

static int seed_console_descriptors(struct Record *record, unsigned long console) {
  // The supervisor's console authority becomes three independent open
  // descriptions before any managed child can inherit descriptor numbers.
  for (unsigned long stream = 0; stream < MOSS_PROCESS_FD_FIRST; ++stream) {
    long object = syscall2(SYS_CAP_DUPLICATE, (long)console, MOSS_CAP_SEND);
    struct OpenDescription *description = object > 0 ? calloc(1, sizeof(*description)) : NULL;
    if (!description) {
      if (object > 0)
        (void)syscall1(SYS_CAP_CLOSE, object);
      drop_descriptors(record);
      return 0;
    }
    description->object = (unsigned long)object;
    description->references = 0;
    description->flags = stream == 0 ? MOSS_PROCESS_FD_READABLE : MOSS_PROCESS_FD_WRITABLE;
    description->kind = DESCRIPTION_CONSOLE;
    description->stream = (unsigned char)stream;
    if (!add_descriptor_at(record, description, stream)) {
      (void)syscall1(SYS_CAP_CLOSE, object);
      free(description);
      drop_descriptors(record);
      return 0;
    }
  }
  return 1;
}

static struct Descriptor *add_descriptor_from(struct Record *record, struct OpenDescription *description,
                                              unsigned long minimum, int *full) {
  unsigned long number = first_free_descriptor(record, minimum);
  *full = number >= MOSS_PROCESS_FD_LIMIT;
  return *full ? NULL : add_descriptor_at(record, description, number);
}

static void remove_descriptor(struct Record *record, struct Descriptor *entry) {
  for (struct Descriptor **slot = &record->descriptors; *slot; slot = &(*slot)->next) {
    if (*slot == entry) {
      *slot = entry->next;
      release_description(entry->description);
      free(entry);
      return;
    }
  }
}

static int clone_descriptors(const struct Record *parent, struct Record *child) {
  // Fork copies descriptor numbers while retaining each shared open offset.
  struct Descriptor **tail = &child->descriptors;
  for (const struct Descriptor *entry = parent->descriptors; entry; entry = entry->next) {
    struct Descriptor *copy = calloc(1, sizeof(*copy));
    if (!copy) {
      drop_descriptors(child);
      return 0;
    }
    copy->number = entry->number;
    copy->description = entry->description;
    copy->close_on_exec = entry->close_on_exec;
    ++copy->description->references;
    *tail = copy;
    tail = &copy->next;
  }
  return 1;
}

static unsigned long parse_handle(const char *text) {
  if (!text)
    return 0;
  char *end = NULL;
  errno = 0;
  unsigned long handle = strtoul(text, &end, 10);
  return errno || !handle || handle > LONG_MAX || *end ? 0 : handle;
}

static struct Record *find_record(unsigned long id) {
  if (!id)
    return NULL;
  for (struct Record *record = records; record; record = record->next) {
    if (record->id == id)
      return record;
  }
  return NULL;
}

static struct Record *free_record(void) {
  for (struct Record *record = records; record; record = record->next) {
    if (!record->id)
      return record;
  }
  struct Record *record = calloc(1, sizeof(*record));
  if (record) {
    record->next = records;
    records = record;
  }
  return record;
}

static int domain_available(unsigned long domain) {
  for (struct Record *record = records; record; record = record->next) {
    if (record->id && record->domain) {
      // A comparison error cannot prove uniqueness, so reject the domain.
      if (syscall2(SYS_DOMAIN_SAME, (long)domain, (long)record->domain) != 0)
        return 0;
    }
  }
  return 1;
}

static int has_children(unsigned long parent_id) {
  for (struct Record *record = records; record; record = record->next) {
    if (record->id && record->parent_id == parent_id)
      return 1;
  }
  return 0;
}

static int record_running(const struct Record *record) {
  // A delegated sender can outlive its domain; stale badges lose control authority.
  if (!record || !record->domain)
    return 0;
  struct moss_domain_exit status = {0};
  return syscall2(SYS_DOMAIN_STATUS, (long)record->domain, (long)&status) == -EAGAIN;
}

static int group_exists(unsigned long group_id, unsigned long session_id) {
  for (struct Record *record = records; record; record = record->next) {
    if (record->id && record->domain && record->group_id == group_id && record->session_id == session_id)
      return 1;
  }
  return 0;
}

static int group_id_used(unsigned long group_id) {
  for (struct Record *record = records; record; record = record->next) {
    if (record->id && record->domain && record->group_id == group_id)
      return 1;
  }
  return 0;
}

static struct Record *select_exited_child(struct Record *parent, unsigned long group_id, int group_only,
                                          struct moss_ipc_message *response) {
  int pending = 0;
  int failed = 0;
  // A running first child must not hide another child's exit.
  for (struct Record *child = records; child; child = child->next) {
    if (!child->id || child->parent_id != parent->id || (group_only && child->group_id != group_id))
      continue;
    pending = 1;
    // Reservations count as children but cannot have an exit status yet.
    if (!child->domain)
      continue;
    struct moss_domain_exit status = {0};
    long result = syscall2(SYS_DOMAIN_STATUS, (long)child->domain, (long)&status);
    if (result == 0) {
      response->size = MOSS_PROCESS_REPLY_WAIT_BYTES;
      response->payload[0] = MOSS_PROCESS_EXITED;
      moss_process_put_u64(response->payload + 1, child->id);
      moss_process_put_u64(response->payload + 9, ((uint64_t)status.signal << 32) | (uint32_t)status.code);
      return child;
    }
    if (result != -EAGAIN)
      failed = 1;
  }
  response->payload[0] = failed ? MOSS_PROCESS_UNAVAILABLE : pending ? MOSS_PROCESS_RUNNING : MOSS_PROCESS_NO_ENTRY;
  return NULL;
}

static void adopt_children(unsigned long parent_id) {
  for (struct Record *child = records; child; child = child->next) {
    if (!child->id || child->parent_id != parent_id)
      continue;
    // The child can attach itself with its own badged session even if its
    // parent exits during the fork handshake. A lease reclaims unused ones.
    child->parent_id = MOSS_PROCESS_INIT_ID;
    child->orphaned = 1;
  }
}

static void refresh_orphans(void) {
  // The service has no exit notification yet. Sweep its live registry on
  // each request so a child observes reparenting before its next getppid().
  // ponytail: Parentage scans are quadratic; index them if large live process
  // sets make this sweep measurable.
  for (struct Record *parent = records; parent; parent = parent->next) {
    if (!parent->id || !parent->domain)
      continue;
    int children = parent->id != MOSS_PROCESS_INIT_ID && has_children(parent->id);
    if (!children && !parent->descriptors)
      continue;
    struct moss_domain_exit status = {0};
    if (syscall2(SYS_DOMAIN_STATUS, (long)parent->domain, (long)&status) == 0) {
      // Wait records outlive their domains; file capabilities must not.
      drop_descriptors(parent);
      if (children)
        adopt_children(parent->id);
    }
  }
  for (struct Record *orphan = records; orphan; orphan = orphan->next) {
    if (!orphan->id || !orphan->orphaned || !orphan->domain)
      continue;
    // Native init cannot wait through this service yet; retain live orphans
    // but reclaim their exited records on the next request.
    struct moss_domain_exit status = {0};
    if (syscall2(SYS_DOMAIN_STATUS, (long)orphan->domain, (long)&status) == 0) {
      (void)syscall1(SYS_CAP_CLOSE, (long)orphan->domain);
      clear_record(orphan);
    }
  }
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) == 0) {
    for (struct Record *reserved = records; reserved; reserved = reserved->next) {
      if (reserved->id && !reserved->domain && reserved->reservation_deadline_ns &&
          now >= reserved->reservation_deadline_ns)
        clear_record(reserved);
    }
  }
}

// Keep a stalled backend below the five-second managed call deadline so one
// object request cannot indefinitely hold this single-threaded registry.
#define PROCESS_BACKEND_CALL_TIMEOUT_NS 1000000000UL

static long service_call(unsigned long endpoint, const struct moss_ipc_message *request,
                         struct moss_ipc_message *response) {
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) != 0 ||
      now > LONG_MAX - PROCESS_BACKEND_CALL_TIMEOUT_NS)
    return -EIO;
  return syscall6(SYS_IPC_CALL, (long)endpoint, (long)request, (long)response,
                  (long)(now + PROCESS_BACKEND_CALL_TIMEOUT_NS), 0, 0);
}

static void pipe_close(unsigned long endpoint) {
  struct moss_ipc_message request = {.size = 1, .payload = {MOSS_PIPE_CLOSE}};
  struct moss_ipc_message response = {0};
  (void)service_call(endpoint, &request, &response);
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
}

static int pipe_get_cap(unsigned long endpoint, unsigned char operation, unsigned long *capability) {
  struct moss_ipc_message request = {.size = 1, .payload = {operation}};
  struct moss_ipc_message response = {0};
  long result = service_call(endpoint, &request, &response);
  if (result == 1 && response.payload[0] == MOSS_PIPE_OK && response.capability && response.rights == MOSS_CAP_SEND) {
    *capability = response.capability;
    return 1;
  }
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  return 0;
}

static int pipe_create_pair(unsigned long root, unsigned long *reader, unsigned long *writer) {
  unsigned long control = 0;
  int created = pipe_get_cap(root, MOSS_PIPE_CREATE, &control);
  int complete = created && pipe_get_cap(control, MOSS_PIPE_READ_END, reader) &&
                 pipe_get_cap(control, MOSS_PIPE_WRITE_END, writer);
  if (created && !complete) {
    struct moss_ipc_message request = {.size = 1, .payload = {MOSS_PIPE_CANCEL}};
    struct moss_ipc_message response = {0};
    (void)service_call(control, &request, &response);
    if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  }
  if (control)
    (void)syscall1(SYS_CAP_CLOSE, (long)control);
  if (!complete) {
    if (*reader)
      (void)syscall1(SYS_CAP_CLOSE, (long)*reader);
    if (*writer)
      (void)syscall1(SYS_CAP_CLOSE, (long)*writer);
    *reader = *writer = 0;
  }
  return complete;
}

static int file_resize(unsigned long file, unsigned long length) {
  struct moss_ipc_message request = {.size = MOSS_FILE_RESIZE_HEADER_BYTES, .payload = {MOSS_FILE_RESIZE}};
  moss_file_put_u64(request.payload + 1, length);
  struct moss_ipc_message response = {0};
  long result = service_call(file, &request, &response);
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  return result == 1 && response.payload[0] == MOSS_FILE_OK && !response.capability && !response.rights;
}

static int file_stat(unsigned long file, unsigned long *id, unsigned long *size) {
  struct moss_ipc_message request = {.size = 1, .payload = {MOSS_FILE_STAT}};
  struct moss_ipc_message response = {0};
  long result = service_call(file, &request, &response);
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  if (result != MOSS_FILE_STAT_REPLY_BYTES || response.payload[0] != MOSS_FILE_OK || response.capability ||
      response.rights)
    return 0;
  unsigned long file_id = moss_file_get_u64(response.payload + 1);
  unsigned long file_size = moss_file_get_u64(response.payload + 9);
  if (!file_id || file_size > MOSS_FILE_CONTENT_BUDGET_BYTES)
    return 0;
  if (id)
    *id = file_id;
  *size = file_size;
  return 1;
}

// -1 means the write may have completed without a reply; callers poison that
// open description instead of retrying at an offset whose state is unknown.
static int file_transfer(struct OpenDescription *description, unsigned long memory, unsigned int count, int writing,
                         unsigned long *position, unsigned int *transferred) {
  unsigned char operation =
      writing ? (description->flags & MOSS_PROCESS_FD_APPEND ? MOSS_FILE_APPEND : MOSS_FILE_WRITE) : MOSS_FILE_READ;
  struct moss_ipc_message request = {.size = MOSS_FILE_IO_HEADER_BYTES,
                                     .capability = memory,
                                     .rights = writing ? MOSS_CAP_MAP_READ : MOSS_CAP_MAP_WRITE,
                                     .payload = {operation}};
  moss_file_put_u64(request.payload + 1, operation == MOSS_FILE_APPEND ? 0 : description->offset);
  moss_file_put_u16(request.payload + 9, count);
  struct moss_ipc_message response = {0};
  long result = service_call(description->object, &request, &response);
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  if (result == 1 && (response.payload[0] == MOSS_FILE_UNAVAILABLE || response.payload[0] == MOSS_FILE_BAD_REQUEST) &&
      !response.capability && !response.rights)
    return 0;
  long expected = operation == MOSS_FILE_APPEND ? MOSS_FILE_APPEND_REPLY_BYTES : MOSS_FILE_IO_REPLY_BYTES;
  if (result != expected || response.payload[0] != MOSS_FILE_OK || response.capability || response.rights)
    return -1;
  *position = operation == MOSS_FILE_APPEND ? moss_file_get_u64(response.payload + 1) : description->offset;
  *transferred = moss_file_get_u16(response.payload + (operation == MOSS_FILE_APPEND ? 9 : 1));
  if (*transferred > count || (writing && *transferred != count) ||
      (*position > MOSS_FILE_CONTENT_BUDGET_BYTES ? writing || *transferred != 0
                                                  : *transferred > MOSS_FILE_CONTENT_BUDGET_BYTES - *position))
    return -1;
  return 1;
}

// WOULD_BLOCK and BROKEN are complete, non-mutating pipe responses. A lost
// write reply remains uncertain because the pipe may already contain bytes.
// A prepared read keeps bytes in the backend until the client's reply outcome
// is known. On an uncertain backend response, retire this service epoch.
static int pipe_transfer(struct OpenDescription *description, unsigned long memory, unsigned int count, int writing,
                         unsigned int *transferred) {
  struct moss_ipc_message request = {.size = MOSS_PIPE_IO_BYTES,
                                     .capability = memory,
                                     .rights = writing ? MOSS_CAP_MAP_READ : MOSS_CAP_MAP_WRITE,
                                     .payload = {writing ? MOSS_PIPE_WRITE : MOSS_PIPE_READ_PREPARE}};
  moss_pipe_put_u16(request.payload + 1, count);
  struct moss_ipc_message response = {0};
  long result = service_call(description->object, &request, &response);
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  if (response.capability || response.rights)
    return -1;
  if (result == 1) {
    if (response.payload[0] == MOSS_PIPE_WOULD_BLOCK)
      return BACKEND_WOULD_BLOCK;
    if (response.payload[0] == MOSS_PIPE_BROKEN)
      return BACKEND_BROKEN_PIPE;
    if (response.payload[0] == MOSS_PIPE_BAD_REQUEST || response.payload[0] == MOSS_PIPE_UNAVAILABLE ||
        response.payload[0] == MOSS_PIPE_NO_ENTRY)
      return 0;
  }
  if (result != MOSS_PIPE_IO_REPLY_BYTES || response.payload[0] != MOSS_PIPE_OK)
    return -1;
  *transferred = moss_pipe_get_u16(response.payload + 1);
  return *transferred <= count && (!writing || *transferred == count) ? 1 : -1;
}

static int finish_object_read(struct OpenDescription *description, int commit) {
  int pipe = description->kind == DESCRIPTION_PIPE;
  struct moss_ipc_message request = {
      .size = pipe ? MOSS_PIPE_READ_FINISH_BYTES : MOSS_CONSOLE_READ_FINISH_BYTES,
      .payload = {pipe ? MOSS_PIPE_READ_FINISH : MOSS_CONSOLE_READ_FINISH, (unsigned char)commit}};
  struct moss_ipc_message response = {0};
  long result = service_call(description->object, &request, &response);
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  return result == 1 && response.payload[0] == (pipe ? MOSS_PIPE_OK : MOSS_CONSOLE_OK) && !response.capability &&
         !response.rights;
}

static int console_transfer(struct OpenDescription *description, unsigned long memory, unsigned int count, int writing,
                            unsigned int *transferred) {
  struct moss_ipc_message request = {
      .size = MOSS_CONSOLE_IO_BYTES,
      .capability = memory,
      .rights = writing ? MOSS_CAP_MAP_READ : MOSS_CAP_MAP_WRITE,
      .payload = {writing ? MOSS_CONSOLE_WRITE : MOSS_CONSOLE_READ_PREPARE, description->stream}};
  moss_console_put_u16(request.payload + 2, count);
  struct moss_ipc_message response = {0};
  long result = service_call(description->object, &request, &response);
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  if (response.capability || response.rights)
    return -1;
  if (result == 1) {
    if (response.payload[0] == MOSS_CONSOLE_WOULD_BLOCK)
      return BACKEND_WOULD_BLOCK;
    if (response.payload[0] == MOSS_CONSOLE_BAD_REQUEST || response.payload[0] == MOSS_CONSOLE_UNAVAILABLE)
      return 0;
  }
  if (result != MOSS_CONSOLE_IO_REPLY_BYTES || response.payload[0] != MOSS_CONSOLE_OK)
    return -1;
  *transferred = moss_console_get_u16(response.payload + 1);
  return *transferred <= count ? 1 : -1;
}

// Publish descriptor and offset changes only after the caller receives the
// reply. Prepared input is finalized then; a completed write with a lost
// reply instead poisons its shared offset.
struct FdAction {
  struct Record *owner;
  struct Descriptor *added;
  struct Descriptor *added_second;
  struct Descriptor *closing;
  struct Descriptor *replaced;
  struct Descriptor *flagged;
  struct OpenDescription *previous_description;
  unsigned char previous_cloexec;
  struct OpenDescription *offset_description;
  struct OpenDescription *prepared_read;
  unsigned long next_offset;
  int completed_write;
  int fatal_backend;
};

static void fd_reply_value(struct moss_ipc_message *response, unsigned long value) {
  response->size = MOSS_PROCESS_REPLY_VALUE_BYTES;
  response->payload[0] = MOSS_PROCESS_OK;
  moss_process_put_u64(response->payload + 1, value);
}

static void handle_fd_request(struct Record *owner, unsigned long namespace, unsigned long pipe,
                              struct moss_ipc_message *request, struct moss_ipc_message *response,
                              struct FdAction *action) {
  if (!record_running(owner)) {
    response->payload[0] = MOSS_PROCESS_NO_ENTRY;
    return;
  }
  action->owner = owner;
  unsigned char operation = request->payload[0];
  if (operation == MOSS_PROCESS_FD_EXEC) {
    if (request->size == 1 && !request->capability && !request->rights) {
      // Exec already committed before this request. A lost reply must not
      // preserve marked descriptors; retrying the request is idempotent.
      for (struct Descriptor *entry = owner->descriptors; entry;) {
        struct Descriptor *next = entry->next;
        if (entry->close_on_exec)
          remove_descriptor(owner, entry);
        entry = next;
      }
      response->payload[0] = MOSS_PROCESS_OK;
    }
    return;
  }
  if (operation == MOSS_PROCESS_FD_PIPE) {
    if (request->size != 1 || request->capability || request->rights)
      return;
    unsigned long read_number = first_free_descriptor(owner, 0);
    unsigned long write_number = first_free_descriptor(owner, read_number + 1);
    if (write_number >= MOSS_PROCESS_FD_LIMIT) {
      response->payload[0] = MOSS_PROCESS_TOO_MANY_FILES;
      return;
    }
    // Reserve both local entries before CREATE so allocation failure cannot
    // leave a live pipe whose endpoints have no descriptor owners.
    struct Descriptor *read_entry = calloc(1, sizeof(*read_entry));
    struct Descriptor *write_entry = calloc(1, sizeof(*write_entry));
    struct OpenDescription *read_description = calloc(1, sizeof(*read_description));
    struct OpenDescription *write_description = calloc(1, sizeof(*write_description));
    if (!read_entry || !write_entry || !read_description || !write_description) {
      response->payload[0] = MOSS_PROCESS_UNAVAILABLE;
    } else {
      unsigned long reader = 0, writer = 0;
      if (pipe_create_pair(pipe, &reader, &writer)) {
        read_description->object = reader;
        read_description->references = 1;
        read_description->flags = MOSS_PROCESS_FD_READABLE;
        read_description->kind = DESCRIPTION_PIPE;
        write_description->object = writer;
        write_description->references = 1;
        write_description->flags = MOSS_PROCESS_FD_WRITABLE;
        write_description->kind = DESCRIPTION_PIPE;
        read_entry->number = read_number;
        read_entry->description = read_description;
        write_entry->number = write_number;
        write_entry->description = write_description;
        read_entry->next = write_entry;
        write_entry->next = owner->descriptors;
        owner->descriptors = read_entry;
        action->added = read_entry;
        action->added_second = write_entry;
        response->size = MOSS_PROCESS_FD_PIPE_REPLY_BYTES;
        response->payload[0] = MOSS_PROCESS_OK;
        moss_process_put_u64(response->payload + 1, read_number);
        moss_process_put_u64(response->payload + 9, write_number);
        return;
      }
      response->payload[0] = MOSS_PROCESS_UNAVAILABLE;
    }
    free(read_entry);
    free(write_entry);
    free(read_description);
    free(write_description);
    return;
  }
  if (operation == MOSS_PROCESS_FD_OPEN) {
    unsigned char flags = request->payload[1];
    const unsigned char *path = request->payload + 2;
    unsigned long path_size = request->size >= 2 ? request->size - 2 : 0;
    if (request->size < 5 || request->capability || request->rights ||
        flags &
            ~(MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_WRITABLE | MOSS_PROCESS_FD_CREATE | MOSS_PROCESS_FD_TRUNCATE |
              MOSS_PROCESS_FD_APPEND | MOSS_PROCESS_FD_CLOEXEC | MOSS_PROCESS_FD_EXCLUSIVE) ||
        !(flags & (MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_WRITABLE)) ||
        ((flags & MOSS_PROCESS_FD_EXCLUSIVE) && !(flags & MOSS_PROCESS_FD_CREATE)) ||
        ((flags & (MOSS_PROCESS_FD_TRUNCATE | MOSS_PROCESS_FD_APPEND)) && !(flags & MOSS_PROCESS_FD_WRITABLE)) ||
        path[0] != '/' || memchr(path, 0, path_size) != path + path_size - 1)
      return;
    unsigned long number = first_free_descriptor(owner, 0);
    if (number == MOSS_PROCESS_FD_LIMIT) {
      response->payload[0] = MOSS_PROCESS_TOO_MANY_FILES;
      return;
    }
    // Allocate before CREATE/TRUNCATE: allocation failure must not mutate a
    // file when no descriptor can be delivered. This request loop is serial.
    struct Descriptor *entry = calloc(1, sizeof(*entry));
    struct OpenDescription *description = calloc(1, sizeof(*description));
    if (!entry || !description) {
      free(entry);
      free(description);
      response->payload[0] = MOSS_PROCESS_UNAVAILABLE;
      return;
    }
    struct moss_ipc_message lookup = {.size = request->size, .payload = {MOSS_NAMESPACE_OPEN}};
    lookup.payload[1] = (flags & MOSS_PROCESS_FD_CREATE ? MOSS_NAMESPACE_OPEN_CREATE : 0) |
                        (flags & MOSS_PROCESS_FD_EXCLUSIVE ? MOSS_NAMESPACE_OPEN_EXCLUSIVE : 0);
    memcpy(lookup.payload + 2, path, path_size);
    struct moss_ipc_message opened = {0};
    long result = service_call(namespace, &lookup, &opened);
    if (result == 1 && opened.payload[0] == MOSS_NAMESPACE_NO_ENTRY && !opened.capability && !opened.rights) {
      response->payload[0] = MOSS_PROCESS_NOT_FOUND;
    } else if (result == 1 && opened.payload[0] == MOSS_NAMESPACE_EXISTS && !opened.capability && !opened.rights) {
      response->payload[0] = MOSS_PROCESS_EXISTS;
    } else if (result == 1 && opened.payload[0] == MOSS_NAMESPACE_OK && opened.capability &&
               opened.rights == MOSS_CAP_SEND &&
               (!(flags & MOSS_PROCESS_FD_TRUNCATE) || file_resize(opened.capability, 0))) {
      description->object = opened.capability;
      description->flags = flags & (MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_WRITABLE | MOSS_PROCESS_FD_APPEND);
      description->references = 1;
      description->kind = DESCRIPTION_FILE;
      entry->number = number;
      entry->description = description;
      entry->close_on_exec = !!(flags & MOSS_PROCESS_FD_CLOEXEC);
      entry->next = owner->descriptors;
      owner->descriptors = entry;
      opened.capability = 0;
      action->added = entry;
      fd_reply_value(response, number);
    } else {
      response->payload[0] = MOSS_PROCESS_UNAVAILABLE;
    }
    if (opened.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)opened.capability);
    if (!action->added) {
      free(entry);
      free(description);
    }
    return;
  }

  if (operation == MOSS_PROCESS_FD_INSTALL) {
    unsigned char flags = request->payload[1];
    if (request->size != MOSS_PROCESS_FD_INSTALL_BYTES || !request->capability || request->rights != MOSS_CAP_SEND ||
        flags &
            ~(MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_WRITABLE | MOSS_PROCESS_FD_APPEND | MOSS_PROCESS_FD_CLOEXEC) ||
        !(flags & (MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_WRITABLE)) ||
        ((flags & MOSS_PROCESS_FD_APPEND) && !(flags & MOSS_PROCESS_FD_WRITABLE)))
      return;
    struct OpenDescription *description = calloc(1, sizeof(*description));
    if (!description) {
      response->payload[0] = MOSS_PROCESS_UNAVAILABLE;
      return;
    }
    description->object = request->capability;
    description->flags = flags & ~MOSS_PROCESS_FD_CLOEXEC;
    description->kind = DESCRIPTION_FILE;
    int full = 0;
    action->added = add_descriptor_from(owner, description, 0, &full);
    if (!action->added) {
      free(description);
      response->payload[0] = full ? MOSS_PROCESS_TOO_MANY_FILES : MOSS_PROCESS_UNAVAILABLE;
      return;
    }
    action->added->close_on_exec = !!(flags & MOSS_PROCESS_FD_CLOEXEC);
    request->capability = 0;
    fd_reply_value(response, action->added->number);
    return;
  }

  if (request->size < MOSS_PROCESS_REPLY_VALUE_BYTES ||
      (request->capability && operation != MOSS_PROCESS_FD_READ && operation != MOSS_PROCESS_FD_WRITE))
    return;
  unsigned long number = moss_process_get_u64(request->payload + 1);
  struct Descriptor *entry = find_descriptor(owner, number);
  if (!entry) {
    response->payload[0] = MOSS_PROCESS_BAD_DESCRIPTOR;
    return;
  }
  struct OpenDescription *description = entry->description;
  if (operation == MOSS_PROCESS_FD_CLOSE || operation == MOSS_PROCESS_FD_DUP) {
    if (request->size != MOSS_PROCESS_REPLY_VALUE_BYTES || request->capability || request->rights)
      return;
    if (operation == MOSS_PROCESS_FD_CLOSE) {
      action->closing = entry;
      response->payload[0] = MOSS_PROCESS_OK;
    } else {
      int full = 0;
      action->added = add_descriptor_from(owner, description, 0, &full);
      if (action->added)
        fd_reply_value(response, action->added->number);
      else
        response->payload[0] = full ? MOSS_PROCESS_TOO_MANY_FILES : MOSS_PROCESS_UNAVAILABLE;
    }
    return;
  }

  if (operation == MOSS_PROCESS_FD_DUP_TO) {
    if (request->size != MOSS_PROCESS_FD_DUP_TO_BYTES || request->capability || request->rights)
      return;
    unsigned long target = moss_process_get_u64(request->payload + 9);
    if (target >= MOSS_PROCESS_FD_LIMIT) {
      response->payload[0] = MOSS_PROCESS_BAD_REQUEST;
      return;
    }
    if (target != number) {
      struct Descriptor *occupied = find_descriptor(owner, target);
      if (occupied) {
        // The service processes one request at a time; retain the old target
        // until reply delivery decides whether this replacement commits.
        ++description->references;
        action->replaced = occupied;
        action->previous_description = occupied->description;
        action->previous_cloexec = occupied->close_on_exec;
        occupied->description = description;
        occupied->close_on_exec = 0;
      } else {
        action->added = add_descriptor_at(owner, description, target);
        if (!action->added) {
          response->payload[0] = MOSS_PROCESS_UNAVAILABLE;
          return;
        }
      }
    }
    fd_reply_value(response, target);
    return;
  }

  if (operation == MOSS_PROCESS_FD_GET_FLAGS || operation == MOSS_PROCESS_FD_SET_FLAGS) {
    if (request->capability || request->rights ||
        request->size !=
            (operation == MOSS_PROCESS_FD_GET_FLAGS ? MOSS_PROCESS_REPLY_VALUE_BYTES : MOSS_PROCESS_FD_SET_FLAGS_BYTES))
      return;
    if (operation == MOSS_PROCESS_FD_GET_FLAGS) {
      fd_reply_value(response, entry->close_on_exec);
    } else if (request->payload[9] > 1) {
      response->payload[0] = MOSS_PROCESS_BAD_REQUEST;
    } else {
      action->flagged = entry;
      action->previous_cloexec = entry->close_on_exec;
      entry->close_on_exec = request->payload[9];
      response->payload[0] = MOSS_PROCESS_OK;
    }
    return;
  }

  if (operation == MOSS_PROCESS_FD_GET_STATUS) {
    if (request->size != MOSS_PROCESS_REPLY_VALUE_BYTES || request->capability || request->rights)
      return;
    fd_reply_value(response,
                   description->flags & (MOSS_PROCESS_FD_READABLE | MOSS_PROCESS_FD_WRITABLE | MOSS_PROCESS_FD_APPEND));
    return;
  }

  if (operation == MOSS_PROCESS_FD_STAT) {
    if (request->size != MOSS_PROCESS_REPLY_VALUE_BYTES || request->capability || request->rights)
      return;
    unsigned long id = 0, size = 0;
    if (description->kind == DESCRIPTION_FILE && !file_stat(description->object, &id, &size)) {
      response->payload[0] = MOSS_PROCESS_UNAVAILABLE;
      return;
    }
    response->size = MOSS_PROCESS_FD_STAT_REPLY_BYTES;
    response->payload[0] = MOSS_PROCESS_OK;
    response->payload[1] = description->kind;
    moss_process_put_u64(response->payload + 2, id);
    moss_process_put_u64(response->payload + 10, size);
    return;
  }

  if (operation == MOSS_PROCESS_FD_DUP_MIN) {
    if (request->size != MOSS_PROCESS_FD_DUP_MIN_BYTES || request->capability || request->rights ||
        request->payload[17] > 1)
      return;
    unsigned long minimum = moss_process_get_u64(request->payload + 9);
    if (minimum >= MOSS_PROCESS_FD_LIMIT) {
      response->payload[0] = MOSS_PROCESS_BAD_REQUEST;
      return;
    }
    int full = 0;
    action->added = add_descriptor_from(owner, description, minimum, &full);
    if (!action->added) {
      response->payload[0] = full ? MOSS_PROCESS_TOO_MANY_FILES : MOSS_PROCESS_UNAVAILABLE;
      return;
    }
    action->added->close_on_exec = request->payload[17];
    fd_reply_value(response, action->added->number);
    return;
  }

  if (operation == MOSS_PROCESS_FD_READ || operation == MOSS_PROCESS_FD_WRITE) {
    int writing = operation == MOSS_PROCESS_FD_WRITE;
    if (request->size != MOSS_PROCESS_FD_IO_BYTES || !request->capability ||
        request->rights !=
            ((writing ? MOSS_CAP_MAP_READ : MOSS_CAP_MAP_WRITE) | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE))
      return;
    if (!(description->flags & (writing ? MOSS_PROCESS_FD_WRITABLE : MOSS_PROCESS_FD_READABLE))) {
      response->payload[0] = MOSS_PROCESS_BAD_DESCRIPTOR;
      return;
    }
    if (description->uncertain) {
      response->payload[0] = MOSS_PROCESS_UNAVAILABLE;
      return;
    }
    unsigned int count = moss_file_get_u16(request->payload + 9);
    if (count > MOSS_MEM_OBJECT_BYTES)
      return;
    unsigned long position = 0;
    unsigned int transferred = 0;
    int result = description->kind == DESCRIPTION_PIPE
                     ? pipe_transfer(description, request->capability, count, writing, &transferred)
                 : description->kind == DESCRIPTION_CONSOLE
                     ? console_transfer(description, request->capability, count, writing, &transferred)
                     : file_transfer(description, request->capability, count, writing, &position, &transferred);
    if (result == 1) {
      response->size = MOSS_PROCESS_FD_IO_REPLY_BYTES;
      response->payload[0] = MOSS_PROCESS_OK;
      moss_file_put_u16(response->payload + 1, transferred);
      action->offset_description = description;
      action->next_offset = description->kind == DESCRIPTION_FILE ? position + transferred : 0;
      action->completed_write = writing;
      if (description->kind != DESCRIPTION_FILE && !writing && transferred)
        action->prepared_read = description;
    } else {
      response->payload[0] = result == BACKEND_WOULD_BLOCK   ? MOSS_PROCESS_WOULD_BLOCK
                             : result == BACKEND_BROKEN_PIPE ? MOSS_PROCESS_BROKEN_PIPE
                                                             : MOSS_PROCESS_UNAVAILABLE;
      if (result < 0 && writing)
        description->uncertain = 1;
      if (result < 0 && !writing && description->kind != DESCRIPTION_FILE)
        action->fatal_backend = 1;
    }
    return;
  }

  if (operation == MOSS_PROCESS_FD_SEEK) {
    if (request->size != MOSS_PROCESS_FD_SEEK_BYTES || request->capability || request->rights || description->uncertain)
      return;
    if (description->kind != DESCRIPTION_FILE) {
      response->payload[0] = MOSS_PROCESS_NOT_SEEKABLE;
      return;
    }
    unsigned char whence = request->payload[17];
    unsigned long base = 0;
    if (whence == MOSS_PROCESS_FD_SEEK_CUR)
      base = description->offset;
    else if (whence == MOSS_PROCESS_FD_SEEK_END) {
      if (!file_stat(description->object, NULL, &base)) {
        response->payload[0] = MOSS_PROCESS_UNAVAILABLE;
        return;
      }
    } else if (whence != MOSS_PROCESS_FD_SEEK_SET) {
      return;
    }
    uint64_t bits = moss_process_get_u64(request->payload + 9);
    unsigned long position;
    if (bits >> 63) {
      // Work in unsigned arithmetic so INT64_MIN has a representable magnitude.
      uint64_t magnitude = ~bits + 1;
      if (base < magnitude)
        return;
      position = base - magnitude;
    } else {
      if (base > LONG_MAX - bits)
        return;
      position = base + bits;
    }
    action->offset_description = description;
    action->next_offset = position;
    fd_reply_value(response, position);
  }
}

static int finish_fd_action(struct FdAction *action, long sent) {
  if (!action->owner)
    return 1;
  if (action->added && sent != 0)
    remove_descriptor(action->owner, action->added);
  if (action->added_second && sent != 0)
    remove_descriptor(action->owner, action->added_second);
  if (action->closing && sent == 0)
    remove_descriptor(action->owner, action->closing);
  if (action->replaced) {
    if (sent == 0) {
      release_description(action->previous_description);
    } else {
      release_description(action->replaced->description);
      action->replaced->description = action->previous_description;
      action->replaced->close_on_exec = action->previous_cloexec;
    }
  }
  if (action->flagged && sent != 0)
    action->flagged->close_on_exec = action->previous_cloexec;
  if (action->offset_description) {
    if (sent == 0)
      action->offset_description->offset = action->next_offset;
    else if (action->completed_write)
      action->offset_description->uncertain = 1;
  }
  if (action->fatal_backend)
    return 0;
  // The backend holds the bytes until the client's one-shot reply commits.
  // An uncertain FINISH outcome retires the whole epoch before another read.
  return !action->prepared_read || finish_object_read(action->prepared_read, sent == 0);
}

int main(int argc, char **argv) {
  if (argc != 7)
    return 2;
  unsigned long receive = parse_handle(argv[1]);
  unsigned long mint = parse_handle(argv[2]);
  unsigned long scope = parse_handle(argv[3]);
  unsigned long namespace = parse_handle(argv[4]);
  unsigned long pipe = parse_handle(argv[5]);
  unsigned long console = parse_handle(argv[6]);
  if (!receive || !mint || !scope || !namespace || !pipe || !console)
    return 2;

  // IDs are never recycled while this service incarnation is alive. A stale
  // badged sender cannot select a later record in a reused slot; a restart
  // creates a new endpoint, so its old sender cannot reach the new registry.
  // The supervisor registers first, so compatibility init retains ID 1.
  unsigned long next_id = MOSS_PROCESS_INIT_ID;
  for (;;) {
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    long received = syscall3(SYS_IPC_RECEIVE, (long)receive, (long)&request, (long)&reply);
    if (received == -EINTR)
      continue;
    if (received < 0)
      return 1;

    refresh_orphans();

    struct moss_ipc_message response = {.size = 1, .payload = {MOSS_PROCESS_BAD_REQUEST}};
    struct Record *registered = NULL;
    struct Record *attached = NULL;
    struct Record *released = NULL;
    struct Record *changed_group = NULL;
    struct FdAction fd_action = {0};
    unsigned long old_group_id = 0, old_session_id = 0;
    long session = 0;
    // The unbadged sender bootstraps init once; later callers must prove parentage with a badge.
    const int register_root =
        request.badge == 0 && request.payload[0] == MOSS_PROCESS_REGISTER && next_id == MOSS_PROCESS_INIT_ID;
    const int register_child = request.badge != 0 && request.payload[0] == MOSS_PROCESS_REGISTER_CHILD;
    const int prepare_child = request.badge != 0 && request.payload[0] == MOSS_PROCESS_PREPARE_CHILD;
    if (request.size == 1 &&
        (((register_root || register_child) && request.capability &&
          request.rights == (MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT | MOSS_CAP_DOMAIN_SIGNAL)) ||
         (prepare_child && !request.capability && !request.rights))) {
      struct Record *parent = register_child || prepare_child ? find_record(request.badge) : NULL;
      long valid_domain =
          prepare_child ? 1 : syscall2(SYS_DOMAIN_SAME, (long)request.capability, (long)request.capability);
      if (register_child && valid_domain == 1)
        valid_domain = syscall2(SYS_DOMAIN_SCOPE_CONTAINS, (long)scope, (long)request.capability);
      unsigned long reservation_deadline_ns = 0;
      int deadline_ready = 1;
      if (prepare_child) {
        unsigned long now = 0;
        deadline_ready = syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) == 0 &&
                         now <= LONG_MAX - MOSS_PROCESS_RESERVATION_TIMEOUT_NS;
        if (deadline_ready)
          reservation_deadline_ns = now + MOSS_PROCESS_RESERVATION_TIMEOUT_NS;
      }
      if ((register_child || prepare_child) && (!parent || !parent->domain)) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
        parent = NULL;
      } else if (parent) {
        struct moss_domain_exit parent_status = {0};
        long state = syscall2(SYS_DOMAIN_STATUS, (long)parent->domain, (long)&parent_status);
        if (state == 0) {
          response.payload[0] = MOSS_PROCESS_NO_ENTRY;
          parent = NULL;
        } else if (state != -EAGAIN) {
          response.payload[0] = MOSS_PROCESS_UNAVAILABLE;
          parent = NULL;
        }
      }
      if ((register_root || parent) && valid_domain == 1 && deadline_ready && next_id <= LONG_MAX &&
          (prepare_child || domain_available(request.capability))) {
        registered = free_record();
        if (registered)
          session = syscall2(SYS_IPC_MINT_BADGE, (long)mint, (long)next_id);
      }
      if (session > 0 && parent && !clone_descriptors(parent, registered)) {
        (void)syscall1(SYS_CAP_CLOSE, session);
        session = 0;
      }
      if (session > 0 && register_root && !seed_console_descriptors(registered, console)) {
        (void)syscall1(SYS_CAP_CLOSE, session);
        session = 0;
      }
      if (session > 0) {
        registered->id = next_id++;
        registered->parent_id = parent ? parent->id : 0;
        registered->group_id = parent ? parent->group_id : registered->id;
        registered->session_id = parent ? parent->session_id : registered->id;
        registered->domain = prepare_child ? 0 : request.capability;
        registered->reservation_deadline_ns = reservation_deadline_ns;
        if (!prepare_child)
          request.capability = 0;
        response.size = MOSS_PROCESS_REPLY_VALUE_BYTES;
        response.payload[0] = MOSS_PROCESS_OK;
        moss_process_put_u64(response.payload + 1, registered->id);
        response.capability = (unsigned long)session;
        response.rights = MOSS_CAP_SEND | MOSS_CAP_DUPLICATE;
      } else if (register_root || parent) {
        response.payload[0] = valid_domain != 1 ? MOSS_PROCESS_BAD_REQUEST : MOSS_PROCESS_UNAVAILABLE;
      }
    } else if (request.badge && request.size == MOSS_PROCESS_REPLY_VALUE_BYTES &&
               request.payload[0] == MOSS_PROCESS_ATTACH_CHILD && request.capability &&
               request.rights == (MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT | MOSS_CAP_DOMAIN_SIGNAL)) {
      struct Record *child = find_record(moss_process_get_u64(request.payload + 1));
      if (!child || (child->parent_id != request.badge && child->id != request.badge)) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
      } else if (syscall2(SYS_DOMAIN_SCOPE_CONTAINS, (long)scope, (long)request.capability) != 1) {
        response.payload[0] = MOSS_PROCESS_BAD_REQUEST;
      } else if (child->domain) {
        // Parent and child may attach concurrently. Only a repeat for the
        // same domain is idempotent.
        response.payload[0] = syscall2(SYS_DOMAIN_SAME, (long)request.capability, (long)child->domain) == 1
                                  ? MOSS_PROCESS_OK
                                  : MOSS_PROCESS_BAD_REQUEST;
      } else if (syscall2(SYS_DOMAIN_SAME, (long)request.capability, (long)request.capability) != 1 ||
                 !domain_available(request.capability)) {
        response.payload[0] = MOSS_PROCESS_BAD_REQUEST;
      } else {
        attached = child;
        child->domain = request.capability;
        request.capability = 0;
        response.payload[0] = MOSS_PROCESS_OK;
      }
    } else if (request.badge && request.size == MOSS_PROCESS_REPLY_VALUE_BYTES &&
               request.payload[0] == MOSS_PROCESS_CANCEL_CHILD && !request.capability && !request.rights) {
      struct Record *child = find_record(moss_process_get_u64(request.payload + 1));
      if (!child || child->parent_id != request.badge) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
      } else {
        // A lost attach reply may make fork fail after the child attached.
        // Its parent may discard the invisible record once the domain exited.
        struct moss_domain_exit status = {0};
        long state = child->domain ? syscall2(SYS_DOMAIN_STATUS, (long)child->domain, (long)&status) : 0;
        if (state == 0) {
          response.payload[0] = MOSS_PROCESS_OK;
          released = child;
        } else {
          response.payload[0] = state == -EAGAIN ? MOSS_PROCESS_NO_ENTRY : MOSS_PROCESS_UNAVAILABLE;
        }
      }
    } else if (request.badge && request.size == MOSS_PROCESS_REPLY_VALUE_BYTES &&
               request.payload[0] == MOSS_PROCESS_WAIT_CHILD && !request.capability && !request.rights) {
      struct Record *parent = find_record(request.badge);
      struct Record *child = find_record(moss_process_get_u64(request.payload + 1));
      if (!parent || !child || child->parent_id != parent->id) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
      } else if (!child->domain) {
        response.payload[0] = MOSS_PROCESS_RUNNING;
      } else {
        struct moss_domain_exit status = {0};
        long result = syscall2(SYS_DOMAIN_STATUS, (long)child->domain, (long)&status);
        if (result == 0) {
          response.size = MOSS_PROCESS_REPLY_WAIT_BYTES;
          response.payload[0] = MOSS_PROCESS_EXITED;
          moss_process_put_u64(response.payload + 1, child->id);
          moss_process_put_u64(response.payload + 9, ((uint64_t)status.signal << 32) | (uint32_t)status.code);
          released = child;
        } else {
          response.payload[0] = result == -EAGAIN ? MOSS_PROCESS_RUNNING : MOSS_PROCESS_UNAVAILABLE;
        }
      }
    } else if (request.badge && request.size == MOSS_PROCESS_REPLY_VALUE_BYTES &&
               request.payload[0] == MOSS_PROCESS_WAIT_GROUP && !request.capability && !request.rights) {
      struct Record *parent = find_record(request.badge);
      if (!parent) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
      } else {
        unsigned long group_id = moss_process_get_u64(request.payload + 1);
        if (group_id > LONG_MAX)
          response.payload[0] = MOSS_PROCESS_BAD_REQUEST;
        else
          released = select_exited_child(parent, group_id ? group_id : parent->group_id, 1, &response);
      }
    } else if (request.badge && request.size == MOSS_PROCESS_REPLY_VALUE_BYTES &&
               request.payload[0] == MOSS_PROCESS_GET_GROUP && !request.capability && !request.rights) {
      struct Record *caller = find_record(request.badge);
      unsigned long target_id = moss_process_get_u64(request.payload + 1);
      struct Record *target = target_id ? find_record(target_id) : caller;
      if (!record_running(caller) || !target || (target != caller && target->parent_id != caller->id) ||
          !record_running(target)) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
      } else {
        response.size = MOSS_PROCESS_REPLY_GROUP_BYTES;
        response.payload[0] = MOSS_PROCESS_OK;
        moss_process_put_u64(response.payload + 1, target->group_id);
        moss_process_put_u64(response.payload + 9, target->session_id);
      }
    } else if (request.badge && request.size == MOSS_PROCESS_SET_GROUP_REQUEST_BYTES &&
               request.payload[0] == MOSS_PROCESS_SET_GROUP && !request.capability && !request.rights) {
      struct Record *caller = find_record(request.badge);
      unsigned long target_id = moss_process_get_u64(request.payload + 1);
      unsigned long group_id = moss_process_get_u64(request.payload + 9);
      struct Record *target = target_id ? find_record(target_id) : caller;
      if (target_id > LONG_MAX || group_id > LONG_MAX) {
        response.payload[0] = MOSS_PROCESS_BAD_REQUEST;
      } else if (!record_running(caller) || !target || (target != caller && target->parent_id != caller->id) ||
                 !record_running(target)) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
      } else {
        if (!group_id)
          group_id = target->id;
        if (target->session_id != caller->session_id || target->session_id == target->id ||
            (group_id != target->id && !group_exists(group_id, caller->session_id))) {
          response.payload[0] = MOSS_PROCESS_DENIED;
        } else {
          changed_group = target;
          old_group_id = target->group_id;
          old_session_id = target->session_id;
          target->group_id = group_id;
          response.payload[0] = MOSS_PROCESS_OK;
        }
      }
    } else if (request.badge && request.size == 1 && request.payload[0] == MOSS_PROCESS_NEW_SESSION &&
               !request.capability && !request.rights) {
      struct Record *caller = find_record(request.badge);
      // A new session cannot reuse a group ID still held by another member.
      if (!record_running(caller)) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
      } else if (group_id_used(caller->id)) {
        response.payload[0] = MOSS_PROCESS_DENIED;
      } else {
        changed_group = caller;
        old_group_id = caller->group_id;
        old_session_id = caller->session_id;
        caller->group_id = caller->id;
        caller->session_id = caller->id;
        response.size = MOSS_PROCESS_REPLY_VALUE_BYTES;
        response.payload[0] = MOSS_PROCESS_OK;
        moss_process_put_u64(response.payload + 1, caller->id);
      }
    } else if (request.badge && request.size == MOSS_PROCESS_SIGNAL_REQUEST_BYTES &&
               request.payload[0] == MOSS_PROCESS_SIGNAL && !request.capability && !request.rights) {
      struct Record *caller = find_record(request.badge);
      struct Record *target = find_record(moss_process_get_u64(request.payload + 1));
      if (!record_running(caller) || !target || (target != caller && target->parent_id != caller->id)) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
      } else if (!target->domain) {
        response.payload[0] = MOSS_PROCESS_RUNNING;
      } else {
        uint64_t signo = moss_process_get_u64(request.payload + 9);
        long signaled = signo <= LONG_MAX ? syscall2(SYS_DOMAIN_SIGNAL, (long)target->domain, (long)signo) : -EINVAL;
        response.payload[0] = signaled == 0         ? MOSS_PROCESS_OK
                              : signaled == -ESRCH  ? MOSS_PROCESS_NO_ENTRY
                              : signaled == -EINVAL ? MOSS_PROCESS_BAD_REQUEST
                                                    : MOSS_PROCESS_UNAVAILABLE;
      }
    } else if (request.badge && request.size == MOSS_PROCESS_SIGNAL_REQUEST_BYTES &&
               request.payload[0] == MOSS_PROCESS_SIGNAL_GROUP && !request.capability && !request.rights) {
      struct Record *caller = find_record(request.badge);
      unsigned long group_id = moss_process_get_u64(request.payload + 1);
      unsigned long signo = moss_process_get_u64(request.payload + 9);
      if (!record_running(caller)) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
      } else if (group_id > LONG_MAX || signo >= MOSS_PROCESS_SIGNAL_LIMIT) {
        response.payload[0] = MOSS_PROCESS_BAD_REQUEST;
      } else {
        if (!group_id)
          group_id = caller->group_id;
        int sent_signal = 0, failed = 0;
        for (struct Record *target = records; target; target = target->next) {
          if (!target->id || !target->domain || target->group_id != group_id ||
              (target != caller && target->parent_id != caller->id))
            continue;
          long result = syscall2(SYS_DOMAIN_SIGNAL, (long)target->domain, (long)signo);
          if (result == 0)
            sent_signal = 1;
          else if (result != -ESRCH)
            failed = 1;
        }
        response.payload[0] = sent_signal ? MOSS_PROCESS_OK : failed ? MOSS_PROCESS_UNAVAILABLE : MOSS_PROCESS_NO_ENTRY;
      }
    } else if (request.badge && request.size && request.payload[0] >= MOSS_PROCESS_FD_OPEN &&
               request.payload[0] <= MOSS_PROCESS_FD_STAT) {
      handle_fd_request(find_record(request.badge), namespace, pipe, &request, &response, &fd_action);
    } else if (request.badge && request.size == 1 && !request.capability && !request.rights) {
      struct Record *record = find_record(request.badge);
      if (!record) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
      } else if (request.payload[0] == MOSS_PROCESS_READY) {
        response.payload[0] = record->domain ? MOSS_PROCESS_OK : MOSS_PROCESS_RUNNING;
      } else if (request.payload[0] == MOSS_PROCESS_IDENTITY) {
        response.size = MOSS_PROCESS_REPLY_IDENTITY_BYTES;
        response.payload[0] = MOSS_PROCESS_OK;
        moss_process_put_u64(response.payload + 1, record->id);
        moss_process_put_u64(response.payload + 9, record->parent_id);
      } else if (request.payload[0] == MOSS_PROCESS_STATUS) {
        // This single-threaded service must not wait for one child's exit and
        // stall every other registered client's request.
        struct moss_domain_exit status = {0};
        long result = record->domain ? syscall2(SYS_DOMAIN_STATUS, (long)record->domain, (long)&status) : -EAGAIN;
        if (result == -EAGAIN) {
          response.payload[0] = MOSS_PROCESS_RUNNING;
        } else if (result == 0) {
          response.size = MOSS_PROCESS_REPLY_VALUE_BYTES;
          response.payload[0] = MOSS_PROCESS_EXITED;
          moss_process_put_u64(response.payload + 1, ((uint64_t)status.signal << 32) | (uint32_t)status.code);
        } else {
          response.payload[0] = MOSS_PROCESS_UNAVAILABLE;
        }
      } else if (request.payload[0] == MOSS_PROCESS_WAIT_ANY) {
        released = select_exited_child(record, 0, 0, &response);
      } else if (request.payload[0] == MOSS_PROCESS_RELEASE) {
        // A child cannot discard the parent's wait record with its own badge.
        if (record->parent_id || has_children(record->id)) {
          response.payload[0] = MOSS_PROCESS_BUSY;
        } else {
          response.payload[0] = MOSS_PROCESS_OK;
          released = record;
        }
      }
    }
    if (request.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)request.capability);
    long sent = syscall2(SYS_IPC_REPLY, (long)reply, (long)&response);
    if (!finish_fd_action(&fd_action, sent))
      return 1;
    if (changed_group && sent != 0) {
      changed_group->group_id = old_group_id;
      changed_group->session_id = old_session_id;
    }
    if (registered && sent != 0) {
      // A failed bootstrap reply delivered no badge, so init may retry as ID 1.
      if (registered->id == MOSS_PROCESS_INIT_ID)
        next_id = MOSS_PROCESS_INIT_ID;
      if (registered->domain)
        (void)syscall1(SYS_CAP_CLOSE, (long)registered->domain);
      clear_record(registered);
    }
    if (attached && sent != 0) {
      // Keep the reservation retryable if the parent did not get the reply.
      (void)syscall1(SYS_CAP_CLOSE, (long)attached->domain);
      attached->domain = 0;
    }
    if (attached && sent == 0)
      attached->reservation_deadline_ns = 0;
    if (released && sent == 0) {
      if (released->id != MOSS_PROCESS_INIT_ID)
        adopt_children(released->id);
      if (released->domain)
        (void)syscall1(SYS_CAP_CLOSE, (long)released->domain);
      clear_record(released);
    }
    if (session > 0)
      (void)syscall1(SYS_CAP_CLOSE, session);
  }
}
