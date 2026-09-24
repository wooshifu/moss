#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_process_protocol.h"
#include "syscall.h"

struct Record {
  unsigned long id;
  unsigned long parent_id;
  unsigned long group_id;
  unsigned long session_id;
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

static int record_running(const struct Record *record) {
  // A delegated sender can outlive its domain; stale badges lose control authority.
  if (!record || !record->domain)
    return 0;
  struct moss_domain_exit status = {0};
  return syscall2(SYS_DOMAIN_STATUS, (long)record->domain, (long)&status) == -EAGAIN;
}

static int group_exists(unsigned long group_id, unsigned long session_id) {
  for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
    if (records[i].id && records[i].domain && records[i].group_id == group_id && records[i].session_id == session_id)
      return 1;
  }
  return 0;
}

static int group_id_used(unsigned long group_id) {
  for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
    if (records[i].id && records[i].domain && records[i].group_id == group_id)
      return 1;
  }
  return 0;
}

static struct Record *select_exited_child(struct Record *parent, unsigned long group_id, int group_only,
                                          struct moss_ipc_message *response) {
  int pending = 0;
  int failed = 0;
  // A running first child must not hide another child's exit.
  for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
    struct Record *child = &records[i];
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
    struct Record *changed_group = NULL;
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
        for (unsigned int i = 0; i < MOSS_PROCESS_RECORD_LIMIT; ++i) {
          struct Record *target = &records[i];
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
