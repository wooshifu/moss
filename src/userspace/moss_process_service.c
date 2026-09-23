#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_process_protocol.h"
#include "syscall.h"

struct Record {
  unsigned long id;
  unsigned long parent_id;
  unsigned long native_id;
  unsigned long domain;
};

static struct Record records[MOSS_PROCESS_RECORD_LIMIT];

static unsigned long parse_handle(const char *text) {
  if (!text)
    return 0;
  char *end = NULL;
  errno = 0;
  unsigned long handle = strtoul(text, &end, 10);
  return errno || !handle || handle > LONG_MAX || *end ? 0 : handle;
}

static struct Record *find_record(unsigned long id) {
  for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
    if (records[i].id == id)
      return &records[i];
  }
  return NULL;
}

static struct Record *free_record(unsigned long native_id) {
  struct Record *free = NULL;
  for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
    if (records[i].id && records[i].native_id == native_id)
      return NULL;
    if (!records[i].id && !free)
      free = &records[i];
  }
  return free;
}

static int has_children(unsigned long parent_id) {
  for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
    if (records[i].id && records[i].parent_id == parent_id)
      return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  unsigned long receive = parse_handle(argv[1]);
  unsigned long mint = parse_handle(argv[2]);
  if (!receive || !mint)
    return 2;

  // IDs are never recycled while this service incarnation is alive. A stale
  // badged sender cannot select a later record in a reused slot; a restart
  // creates a new endpoint, so its old sender cannot reach the new registry.
  // Reserve 1 for a future compatibility init registration. The supervisor
  // still owns the native initial domain in this first service incarnation.
  unsigned long next_id = 2;
  for (;;) {
    struct moss_ipc_message request = {0};
    unsigned long reply = 0;
    long received = syscall3(SYS_IPC_RECEIVE, (long)receive, (long)&request, (long)&reply);
    if (received == -EINTR)
      continue;
    if (received < 0)
      return 1;

    struct moss_ipc_message response = {.size = 1, .payload = {MOSS_PROCESS_BAD_REQUEST}};
    struct Record *registered = NULL;
    struct Record *released = NULL;
    long session = 0;
    const int register_root = request.badge == 0 && request.payload[0] == MOSS_PROCESS_REGISTER;
    const int register_child = request.badge != 0 && request.payload[0] == MOSS_PROCESS_REGISTER_CHILD;
    if (request.size == 1 && (register_root || register_child) && request.capability &&
        request.rights == (MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT)) {
      struct Record *parent = register_child ? find_record(request.badge) : NULL;
      long native_id = syscall1(SYS_DOMAIN_ID, (long)request.capability);
      if (register_child && !parent) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
      } else if (register_child) {
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
      if ((!register_child || parent) && native_id > 0 && next_id <= LONG_MAX) {
        registered = free_record((unsigned long)native_id);
        if (registered)
          session = syscall2(SYS_IPC_MINT_BADGE, (long)mint, (long)next_id);
      }
      if (session > 0) {
        registered->id = next_id++;
        registered->parent_id = parent ? parent->id : 0;
        registered->native_id = (unsigned long)native_id;
        registered->domain = request.capability;
        request.capability = 0;
        response.size = MOSS_PROCESS_REPLY_VALUE_BYTES;
        response.payload[0] = MOSS_PROCESS_OK;
        moss_process_put_u64(response.payload + 1, registered->id);
        response.capability = (unsigned long)session;
        response.rights = MOSS_CAP_SEND | MOSS_CAP_DUPLICATE;
      } else if (!register_child || parent) {
        response.payload[0] = native_id <= 0 ? MOSS_PROCESS_BAD_REQUEST : MOSS_PROCESS_UNAVAILABLE;
      }
    } else if (request.badge && request.size == 1 && !request.capability && !request.rights) {
      struct Record *record = find_record(request.badge);
      if (!record) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
      } else if (request.payload[0] == MOSS_PROCESS_STATUS) {
        // This single-threaded service must not wait for one child's exit and
        // stall every other registered client's request.
        struct moss_domain_exit status = {0};
        long result = syscall2(SYS_DOMAIN_STATUS, (long)record->domain, (long)&status);
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
        int pending = 0;
        int failed = 0;
        // Scan all children: a running first child must not hide another
        // child's exit from wait-any.
        for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
          struct Record *child = &records[i];
          if (!child->id || child->parent_id != record->id)
            continue;
          pending = 1;
          struct moss_domain_exit status = {0};
          long result = syscall2(SYS_DOMAIN_STATUS, (long)child->domain, (long)&status);
          if (result == 0) {
            response.size = MOSS_PROCESS_REPLY_WAIT_BYTES;
            response.payload[0] = MOSS_PROCESS_EXITED;
            moss_process_put_u64(response.payload + 1, child->id);
            moss_process_put_u64(response.payload + 9, ((uint64_t)status.signal << 32) | (uint32_t)status.code);
            released = child;
            break;
          }
          if (result != -EAGAIN)
            failed = 1;
        }
        if (!released)
          response.payload[0] = failed    ? MOSS_PROCESS_UNAVAILABLE
                                : pending ? MOSS_PROCESS_RUNNING
                                          : MOSS_PROCESS_NO_ENTRY;
      } else if (request.payload[0] == MOSS_PROCESS_RELEASE) {
        if (has_children(record->id)) {
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
    if (registered && sent != 0) {
      (void)syscall1(SYS_CAP_CLOSE, (long)registered->domain);
      *registered = (struct Record){0};
    }
    if (released && sent == 0) {
      (void)syscall1(SYS_CAP_CLOSE, (long)released->domain);
      *released = (struct Record){0};
    }
    if (session > 0)
      (void)syscall1(SYS_CAP_CLOSE, session);
  }
}
