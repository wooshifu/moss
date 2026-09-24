#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_process_protocol.h"
#include "syscall.h"

struct Record {
  unsigned long id;
  unsigned long parent_id;
  // A reserved child has an identity before fork and no domain until attach.
  unsigned long domain;
  unsigned long reservation_deadline_ns;
  unsigned char orphaned;
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

static struct Record *free_record(void) {
  for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
    if (!records[i].id)
      return &records[i];
  }
  return NULL;
}

static int domain_available(unsigned long domain) {
  for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
    if (records[i].id && records[i].domain) {
      // A comparison error cannot prove uniqueness, so reject the domain.
      if (syscall2(SYS_DOMAIN_SAME, (long)domain, (long)records[i].domain) != 0)
        return 0;
    }
  }
  return 1;
}

static int has_children(unsigned long parent_id) {
  for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
    if (records[i].id && records[i].parent_id == parent_id)
      return 1;
  }
  return 0;
}

static void adopt_children(unsigned long parent_id) {
  for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
    struct Record *child = &records[i];
    if (!child->id || child->parent_id != parent_id)
      continue;
    // The child can attach itself with its own badged session even if its
    // parent exits during the fork handshake. A lease reclaims unused ones.
    child->parent_id = MOSS_PROCESS_INIT_ID;
    child->orphaned = 1;
  }
}

static void refresh_orphans(void) {
  // The service has no exit notification yet. Sweep its bounded registry on
  // each request so a child observes reparenting before its next getppid().
  // ponytail: Index parentage if the fixed record limit grows.
  for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
    struct Record *parent = &records[i];
    if (!parent->id || parent->id == MOSS_PROCESS_INIT_ID || !parent->domain || !has_children(parent->id))
      continue;
    struct moss_domain_exit status = {0};
    if (syscall2(SYS_DOMAIN_STATUS, (long)parent->domain, (long)&status) == 0)
      adopt_children(parent->id);
  }
  for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
    struct Record *orphan = &records[i];
    if (!orphan->id || !orphan->orphaned || !orphan->domain)
      continue;
    // Native init cannot wait through this service yet; retain live orphans
    // but reclaim their exited records on the next request.
    struct moss_domain_exit status = {0};
    if (syscall2(SYS_DOMAIN_STATUS, (long)orphan->domain, (long)&status) == 0) {
      (void)syscall1(SYS_CAP_CLOSE, (long)orphan->domain);
      *orphan = (struct Record){0};
    }
  }
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) == 0) {
    for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
      struct Record *reserved = &records[i];
      if (reserved->id && !reserved->domain && reserved->reservation_deadline_ns &&
          now >= reserved->reservation_deadline_ns)
        *reserved = (struct Record){0};
    }
  }
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
    long session = 0;
    const int register_root = request.badge == 0 && request.payload[0] == MOSS_PROCESS_REGISTER;
    const int register_child = request.badge != 0 && request.payload[0] == MOSS_PROCESS_REGISTER_CHILD;
    const int prepare_child = request.badge != 0 && request.payload[0] == MOSS_PROCESS_PREPARE_CHILD;
    if (request.size == 1 &&
        (((register_root || register_child) && request.capability &&
          request.rights == (MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT | MOSS_CAP_DOMAIN_SIGNAL)) ||
         (prepare_child && !request.capability && !request.rights))) {
      struct Record *parent = register_child || prepare_child ? find_record(request.badge) : NULL;
      long valid_domain =
          prepare_child ? 1 : syscall2(SYS_DOMAIN_SAME, (long)request.capability, (long)request.capability);
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
      if (session > 0) {
        registered->id = next_id++;
        registered->parent_id = parent ? parent->id : 0;
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
      if (!child || child->parent_id != request.badge || child->domain) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
      } else {
        response.payload[0] = MOSS_PROCESS_OK;
        released = child;
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
    } else if (request.badge && request.size == MOSS_PROCESS_SIGNAL_REQUEST_BYTES &&
               request.payload[0] == MOSS_PROCESS_SIGNAL && !request.capability && !request.rights) {
      struct Record *caller = find_record(request.badge);
      struct Record *target = find_record(moss_process_get_u64(request.payload + 1));
      if (!caller || !caller->domain || !target || (target != caller && target->parent_id != caller->id)) {
        response.payload[0] = MOSS_PROCESS_NO_ENTRY;
      } else {
        struct moss_domain_exit caller_status = {0};
        // A delegated sender may outlive its domain; its stale badge must not
        // retain signal authority after that domain exits.
        if (syscall2(SYS_DOMAIN_STATUS, (long)caller->domain, (long)&caller_status) != -EAGAIN) {
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
      }
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
        int pending = 0;
        int failed = 0;
        // Scan all children: a running first child must not hide another
        // child's exit from wait-any.
        for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
          struct Record *child = &records[i];
          if (!child->id || child->parent_id != record->id)
            continue;
          pending = 1;
          // Reservations count as children but cannot have an exit status yet.
          if (!child->domain)
            continue;
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
    if (registered && sent != 0) {
      if (registered->domain)
        (void)syscall1(SYS_CAP_CLOSE, (long)registered->domain);
      *registered = (struct Record){0};
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
      *released = (struct Record){0};
    }
    if (session > 0)
      (void)syscall1(SYS_CAP_CLOSE, session);
  }
}
