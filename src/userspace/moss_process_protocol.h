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
  MOSS_PROCESS_SIGNAL_GROUP = 17,
  MOSS_PROCESS_FD_OPEN = 18,
  MOSS_PROCESS_FD_CLOSE = 19,
  MOSS_PROCESS_FD_DUP = 20,
  MOSS_PROCESS_FD_READ = 21,
  MOSS_PROCESS_FD_WRITE = 22,
  MOSS_PROCESS_FD_SEEK = 23,
  MOSS_PROCESS_FD_EXEC = 24,
  MOSS_PROCESS_FD_DUP_TO = 25
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
// Match the current kernel compatibility limit while reserving 0..2 for the
// console entries that the unified descriptor view will also own.
enum { MOSS_PROCESS_FD_LIMIT = 256, MOSS_PROCESS_FD_FIRST = 3 };
enum {
  MOSS_PROCESS_FD_READABLE = 1U << 0,
  MOSS_PROCESS_FD_WRITABLE = 1U << 1,
  MOSS_PROCESS_FD_CREATE = 1U << 2,
  MOSS_PROCESS_FD_TRUNCATE = 1U << 3,
  MOSS_PROCESS_FD_APPEND = 1U << 4,
  MOSS_PROCESS_FD_CLOEXEC = 1U << 5
};
enum { MOSS_PROCESS_FD_SEEK_SET = 0, MOSS_PROCESS_FD_SEEK_CUR = 1, MOSS_PROCESS_FD_SEEK_END = 2 };
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
enum {
  MOSS_PROCESS_FD_IO_BYTES = 11,
  MOSS_PROCESS_FD_IO_REPLY_BYTES = 3,
  MOSS_PROCESS_FD_SEEK_BYTES = 18,
  MOSS_PROCESS_FD_DUP_TO_BYTES = 17
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
// FD_OPEN carries [opcode, flags, absolute NUL-terminated namespace path] and
// returns [OK, descriptor:u64 LE]. FD_CLOSE and FD_DUP carry [opcode,
// descriptor:u64 LE]; DUP returns the new descriptor. FD_DUP_TO carries
// [opcode, source:u64 LE, target:u64 LE] and returns the target. A different
// source replaces an occupied target and clears its close-on-exec flag;
// duplicating a descriptor onto itself preserves that flag. FD_READ/WRITE carry
// [opcode, descriptor:u64 LE, count:u16 LE] plus a transferred Memory Object
// with MAP_WRITE|TRANSFER|DUPLICATE or MAP_READ|TRANSFER|DUPLICATE respectively,
// and return
// [OK, transferred:u16 LE]. FD_SEEK carries [opcode, descriptor:u64 LE,
// offset:i64 LE, whence] and returns [OK, position:u64 LE]. FD_CLOEXEC is a
// descriptor flag: DUP clears it, fork copies it, and FD_EXEC closes marked
// entries after a successful exec. FD_EXEC carries only its opcode.
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
