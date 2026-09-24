#pragma once

#include <stdint.h>

// The unbadged REGISTER operation bootstraps init once as ID 1. Later domains
// require REGISTER_CHILD or PREPARE_CHILD through a parent's badged sender.
// A registration transfers OBSERVE|INSPECT|SIGNAL authority for one native domain.
// The returned sender's kernel-authenticated badge is the compatibility identity;
// numeric IDs in payloads never authorize status, parentage, or wait operations.
// REGISTER_CHILD requires the parent's badged sender and creates a child
// relationship. WAIT_ANY uses that sender to reap one exited child without
// blocking the service; RUNNING means the caller can poll again.
enum {
  MOSS_PROCESS_REGISTER = 1,
  MOSS_PROCESS_STATUS = 2,
  MOSS_PROCESS_RELEASE = 3,
  MOSS_PROCESS_REGISTER_CHILD = 4,
  MOSS_PROCESS_WAIT_ANY = 5,
  MOSS_PROCESS_PREPARE_CHILD = 6,
  MOSS_PROCESS_ATTACH_CHILD = 7,
  MOSS_PROCESS_CANCEL_CHILD = 8,
  MOSS_PROCESS_IDENTITY = 9,
  MOSS_PROCESS_READY = 10,
  MOSS_PROCESS_WAIT_CHILD = 11,
  MOSS_PROCESS_SIGNAL = 12,
  MOSS_PROCESS_GET_GROUP = 13,
  MOSS_PROCESS_SET_GROUP = 14,
  MOSS_PROCESS_NEW_SESSION = 15,
  MOSS_PROCESS_WAIT_GROUP = 16,
  MOSS_PROCESS_SIGNAL_GROUP = 17
};
enum {
  MOSS_PROCESS_OK = 0,
  MOSS_PROCESS_BAD_REQUEST = 1,
  MOSS_PROCESS_NO_ENTRY = 2,
  MOSS_PROCESS_UNAVAILABLE = 3,
  MOSS_PROCESS_RUNNING = 4,
  MOSS_PROCESS_EXITED = 5,
  MOSS_PROCESS_BUSY = 6,
  MOSS_PROCESS_DENIED = 7
};
enum { MOSS_PROCESS_INIT_ID = 1 };
// Native process::sig::NSIG is 32: zero probes existence, 1..31 are signals.
enum { MOSS_PROCESS_SIGNAL_LIMIT = 32 };
// Match the managed IPC call bound so an abandoned fork reservation cannot
// occupy a record indefinitely after its parent or child exits.
#define MOSS_PROCESS_RESERVATION_TIMEOUT_NS 5000000000UL
enum {
  MOSS_PROCESS_REPLY_VALUE_BYTES = 9,
  MOSS_PROCESS_REPLY_WAIT_BYTES = 17,
  MOSS_PROCESS_SIGNAL_REQUEST_BYTES = 17,
  MOSS_PROCESS_SET_GROUP_REQUEST_BYTES = 17,
  MOSS_PROCESS_REPLY_GROUP_BYTES = 17,
  MOSS_PROCESS_REPLY_IDENTITY_BYTES = MOSS_PROCESS_REPLY_WAIT_BYTES
};

// REGISTER returns [OK, ID:u64 LE]. STATUS returns [EXITED, status:u64 LE].
// WAIT_ANY returns [EXITED, child ID:u64 LE, status:u64 LE] and atomically
// reaps that child after a successful reply. Status packs signal in the high
// 32 bits and exit code in the low 32 bits. Other replies are one byte.
// PREPARE_CHILD returns a child ID and a badged session to inherit across a
// native fork. ATTACH_CHILD transfers the new domain under the parent's or
// child's session and accepts a repeat for the same domain. CANCEL_CHILD
// removes an unattached reservation or a child whose attached domain exited.
// IDENTITY returns [OK, ID:u64 LE, parent ID:u64 LE] through the child's session.
// READY returns RUNNING until the reserved identity is attached. WAIT_CHILD
// selects one child by ID under the parent's badge and uses WAIT_ANY's reply.
// SIGNAL accepts [operation, target ID:u64 LE, signal:u64 LE]. Only a record's
// own badge or its parent's badge may signal it; a bare numeric ID has no authority.
// GET_GROUP returns [OK, group ID:u64 LE, session ID:u64 LE] for self or a child.
// SET_GROUP accepts [operation, target ID:u64 LE, group ID:u64 LE], with zero
// selecting the caller/target respectively. NEW_SESSION returns [OK, ID:u64 LE].
// WAIT_GROUP selects children by group ID, with zero selecting the caller's
// current group; the service still requires the parent's badge to reap them.
// SIGNAL_GROUP selects the caller's group for ID zero. It can signal only the
// caller and its direct children that belong to that group.
static inline uint64_t moss_process_get_u64(const unsigned char *bytes) {
  uint64_t value = 0;
  for (unsigned int index = 0; index < 8; ++index)
    value |= (uint64_t)bytes[index] << (index * 8);
  return value;
}

static inline void moss_process_put_u64(unsigned char *bytes, uint64_t value) {
  for (unsigned int index = 0; index < 8; ++index)
    bytes[index] = (unsigned char)(value >> (index * 8));
}
