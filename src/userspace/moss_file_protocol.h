#pragma once

#include <limits.h>
#include <stdint.h>

// A zero badge marks lookup requests. The file service assigns a stable,
// nonzero badge to each live file and keeps its MINT authority private.
// The root directory has a distinct read-only object badge. File badges stay
// below it so a directory sender cannot perform unbadged OPEN requests.
#define MOSS_FILE_ROOT_BADGE LONG_MAX
enum { MOSS_FILE_SCRATCH_BADGE = 1 };
// OPEN carries [opcode, flags, relative NUL-terminated name]. CREATE adds a
// missing file without changing an existing file. EXCLUSIVE requires CREATE
// and rejects an existing file before returning its capability. UNLISTED
// requires CREATE and creates an object that cannot be looked up by name.
// WRITE requests a writable object and rejects immutable boot files at open.
// OPEN returns a sender to the namespace, which normally grants only SEND to
// the client; an explicit delegable lookup can retain TRANSFER|DUPLICATE.
// File operations then bypass the namespace service.
// The first byte is an operation in requests and a status in replies.
enum {
  MOSS_FILE_READ = 1,
  MOSS_FILE_WRITE = 2,
  MOSS_FILE_OPEN = 3,
  MOSS_FILE_RESIZE = 4,
  MOSS_FILE_SIZE = 5,
  MOSS_FILE_APPEND = 6,
  MOSS_FILE_STAT = 7,
  MOSS_FILE_ROOT = 8,
  MOSS_FILE_LIST = 9,
  MOSS_FILE_SEAL = 10,
  MOSS_FILE_SNAPSHOT = 11
};
enum { MOSS_FILE_OPEN_CREATE = 1U << 0, MOSS_FILE_OPEN_EXCLUSIVE = 1U << 1, MOSS_FILE_OPEN_UNLISTED = 1U << 2,
       MOSS_FILE_OPEN_WRITE = 1U << 3 };
enum {
  MOSS_FILE_OK = 0,
  MOSS_FILE_BAD_REQUEST = 1,
  MOSS_FILE_NO_ENTRY = 2,
  MOSS_FILE_UNAVAILABLE = 3,
  MOSS_FILE_EXISTS = 4,
  MOSS_FILE_END = 5,
  MOSS_FILE_READ_ONLY = 6,
  MOSS_FILE_NO_SPACE = 7
};
// Preserve the kernel dirent d_type values when libc moves to this service.
enum { MOSS_FILE_TYPE_DIRECTORY = 4, MOSS_FILE_TYPE_REGULAR = 8 };
// Until service resource accounting exists, bound client-triggered allocation.
// 16 MiB matches the current 4096-page native domain construction ceiling,
// allowing a static libc image through this volatile file service. Boot
// archive entries have a separate limit and do not consume that budget.
enum { MOSS_FILE_OBJECT_LIMIT = 16, MOSS_FILE_BOOT_ENTRY_LIMIT = 64, MOSS_FILE_CONTENT_BUDGET_BYTES = 4096 * 4096 };
// NO_SPACE means the content budget rejected a write, append or resize before
// mutation. Heap allocation failure remains UNAVAILABLE.
// SEAL is an idempotent one-byte request on an unlisted object sender. It
// permanently rejects WRITE, APPEND and RESIZE through every sender copy.
// Named files remain mutable for existing public clients.
// SNAPSHOT carries [opcode, reserved zero, relative NUL-terminated name] on
// the unbadged root sender. It returns an unlisted, sealed copy while the
// named source remains unchanged; ordinary named-file senders cannot clone.
// READ/WRITE transfer one shared page at an explicit byte offset. Their
// request carries [opcode, offset: u64 LE, count: u16 LE], and the reply
// carries [status, transferred: u16 LE]. RESIZE carries [opcode, size: u64 LE].
// SIZE returns [OK, size: u64 LE] through the file object's own capability.
// STAT returns [OK, file ID: u64 LE, size: u64 LE]. The ID is stable only
// within one file-service incarnation; old object caps cannot reach a restart.
// APPEND uses the I/O request with offset zero and returns [OK, start: u64 LE,
// transferred: u16 LE]. The service chooses start when it performs the write.
enum {
  MOSS_FILE_IO_HEADER_BYTES = 11,
  MOSS_FILE_IO_REPLY_BYTES = 3,
  MOSS_FILE_RESIZE_HEADER_BYTES = 9,
  MOSS_FILE_SIZE_REPLY_BYTES = 9,
  MOSS_FILE_APPEND_REPLY_BYTES = 11,
  MOSS_FILE_STAT_REPLY_BYTES = 17
};
// ROOT mints a sender for the root directory. LIST requires that sender and
// carries [opcode, cookie:u64 LE] plus a writable shared page. Its successful
// reply is [OK, type:u8, id:u64 LE, next cookie:u64 LE, name bytes:u16 LE];
// the page holds the NUL-terminated name. Cookie 0/1 yield . and ..; later
// cookies are file badges plus two. END has no entry and does not advance it.
enum { MOSS_FILE_LIST_BYTES = 9, MOSS_FILE_LIST_REPLY_BYTES = 20 };

static inline uint64_t moss_file_get_u64(const unsigned char *bytes) {
  uint64_t value = 0;
  for (unsigned int index = 0; index < 8; ++index) {
    value |= (uint64_t)bytes[index] << (index * 8);
  }
  return value;
}

static inline void moss_file_put_u64(unsigned char *bytes, uint64_t value) {
  for (unsigned int index = 0; index < 8; ++index) {
    bytes[index] = (unsigned char)(value >> (index * 8));
  }
}

static inline unsigned int moss_file_get_u16(const unsigned char *bytes) {
  return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8);
}

static inline void moss_file_put_u16(unsigned char *bytes, unsigned int value) {
  bytes[0] = (unsigned char)value;
  bytes[1] = (unsigned char)(value >> 8);
}
