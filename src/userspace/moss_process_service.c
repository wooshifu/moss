#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_process_protocol.h"
#include "syscall.h"

struct Record {
  unsigned long id;
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
    if (request.badge == 0 && request.size == 1 && request.payload[0] == MOSS_PROCESS_REGISTER && request.capability &&
        request.rights == (MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT)) {
      long native_id = syscall1(SYS_DOMAIN_ID, (long)request.capability);
      if (native_id > 0 && next_id <= LONG_MAX) {
        registered = free_record((unsigned long)native_id);
        if (registered)
          session = syscall2(SYS_IPC_MINT_BADGE, (long)mint, (long)next_id);
      }
      if (session > 0) {
        registered->id = next_id++;
        registered->native_id = (unsigned long)native_id;
        registered->domain = request.capability;
        request.capability = 0;
        response.size = MOSS_PROCESS_REPLY_VALUE_BYTES;
        response.payload[0] = MOSS_PROCESS_OK;
        moss_process_put_u64(response.payload + 1, registered->id);
        response.capability = (unsigned long)session;
        response.rights = MOSS_CAP_SEND | MOSS_CAP_DUPLICATE;
      } else {
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
      } else if (request.payload[0] == MOSS_PROCESS_RELEASE) {
        response.payload[0] = MOSS_PROCESS_OK;
        released = record;
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
