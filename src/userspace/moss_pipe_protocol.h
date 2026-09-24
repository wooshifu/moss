#pragma once

#include <stdint.h>

// A root sender creates a pipe and receives a SEND-only control capability.
// The control capability obtains one SEND-only capability for each end. A
// holder must cancel an incomplete pair or close each issued end exactly once.
// The pipe service never reuses a badge during its lifetime.
enum {
  MOSS_PIPE_CREATE = 1,
  MOSS_PIPE_READ_END = 2,
  MOSS_PIPE_WRITE_END = 3,
  MOSS_PIPE_CANCEL = 4,
  MOSS_PIPE_READ = 5,
  MOSS_PIPE_WRITE = 6,
  MOSS_PIPE_CLOSE = 7
};

enum {
  MOSS_PIPE_OK = 0,
  MOSS_PIPE_BAD_REQUEST = 1,
  MOSS_PIPE_UNAVAILABLE = 2,
  MOSS_PIPE_WOULD_BLOCK = 3,
  MOSS_PIPE_BROKEN = 4,
  MOSS_PIPE_NO_ENTRY = 5
};

// READ/WRITE carry [opcode, count:u16 LE] and a one-page Memory Object with
// MAP_WRITE/MAP_READ respectively. A successful reply is [OK, count:u16 LE].
// Writes are all-or-nothing within one page; a timed-out write is uncertain.
// WOULD_BLOCK never changes the pipe. READ returns OK with count zero at EOF;
// WRITE returns BROKEN when the read end is closed.
enum { MOSS_PIPE_IO_BYTES = 3, MOSS_PIPE_IO_REPLY_BYTES = 3 };
// Bound service-owned ring storage until pipes have per-client quotas.
enum { MOSS_PIPE_OBJECT_LIMIT = 16 };

static inline unsigned int moss_pipe_get_u16(const unsigned char *bytes) {
  return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8);
}

static inline void moss_pipe_put_u16(unsigned char *bytes, unsigned int value) {
  bytes[0] = (unsigned char)value;
  bytes[1] = (unsigned char)(value >> 8);
}
