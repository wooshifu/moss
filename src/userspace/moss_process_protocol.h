#pragma once

#include <stdint.h>

// A registration transfers OBSERVE|INSPECT authority for one native domain.
// The returned sender's kernel-assigned badge is the compatibility identity;
// numeric IDs in payloads never authorize status or release operations.
enum { MOSS_PROCESS_REGISTER = 1, MOSS_PROCESS_STATUS = 2, MOSS_PROCESS_RELEASE = 3 };
enum {
  MOSS_PROCESS_OK = 0,
  MOSS_PROCESS_BAD_REQUEST = 1,
  MOSS_PROCESS_NO_ENTRY = 2,
  MOSS_PROCESS_UNAVAILABLE = 3,
  MOSS_PROCESS_RUNNING = 4,
  MOSS_PROCESS_EXITED = 5
};
// ponytail: a fixed table bounds orphaned registrations until sender-lifetime
// notifications or a service-side lease can reclaim clients that die abruptly.
enum { MOSS_PROCESS_REPLY_VALUE_BYTES = 9, MOSS_PROCESS_RECORD_LIMIT = 16 };

// REGISTER returns [OK, ID:u64 LE]. EXITED returns [EXITED,
// (signal:u32 << 32 | exit_code:u32):u64 LE]. Other replies are one byte.
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
