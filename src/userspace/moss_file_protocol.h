#pragma once

#include <stdint.h>

// A zero badge marks lookup requests. The file service assigns a stable,
// nonzero badge to each live file and keeps its MINT authority private.
enum { MOSS_FILE_SCRATCH_BADGE = 1 };
// OPEN carries [opcode, flags, relative NUL-terminated name]. CREATE adds a
// missing file without changing an existing file. OPEN returns an object
// sender; the public namespace transfers only SEND to clients. File
// operations then bypass the namespace service.
// The first byte is an operation in requests and a status in replies.
enum {
  MOSS_FILE_READ = 1,
  MOSS_FILE_WRITE = 2,
  MOSS_FILE_OPEN = 3,
  MOSS_FILE_RESIZE = 4,
  MOSS_FILE_SEAL = 5,
  MOSS_FILE_SNAPSHOT = 6,
};
// UNLISTED creates a file object without inserting a name into the root
// index. Only the returned object capability can address it; even another
// holder of the root sender cannot reopen it by guessing the supplied name.
enum { MOSS_FILE_OPEN_CREATE = 1U << 0, MOSS_FILE_OPEN_UNLISTED = 1U << 1 };
// SEAL is an idempotent one-byte request on an unlisted object sender. It
// permanently rejects WRITE and RESIZE, including through duplicated senders.
// Named files remain mutable for existing public clients.
// SNAPSHOT carries [opcode, reserved zero, relative NUL-terminated name] on
// the root sender. Ordinary named-file SEND handles cannot allocate clones.
// It returns an unlisted, sealed object fixed at request handling.
enum { MOSS_FILE_OK = 0, MOSS_FILE_BAD_REQUEST = 1, MOSS_FILE_NO_ENTRY = 2, MOSS_FILE_UNAVAILABLE = 3 };
// Until service resource accounting exists, bound client-triggered allocation.
// 16 MiB matches the current 4096-page native domain construction ceiling,
// allowing a static libc image through this volatile file service.
enum { MOSS_FILE_OBJECT_LIMIT = 16, MOSS_FILE_CONTENT_BUDGET_BYTES = 4096 * 4096 };
// READ/WRITE transfer one shared page at an explicit byte offset. Their
// request carries [opcode, offset: u64 LE, count: u16 LE], and the reply
// carries [status, transferred: u16 LE]. RESIZE carries [opcode, size: u64 LE].
enum { MOSS_FILE_IO_HEADER_BYTES = 11, MOSS_FILE_IO_REPLY_BYTES = 3, MOSS_FILE_RESIZE_HEADER_BYTES = 9 };

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
