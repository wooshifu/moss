#pragma once

#define MOSS_SCRATCH_PATH "/scratch"

// OPEN and STAT carry [opcode, flags, absolute NUL-terminated flat-root path].
// Root-leading slash and dot segments resolve within the root mount; a path
// through a regular file is rejected until directory traversal exists.
// The root directory is read-only: CREATE or WRITE reports IS_DIRECTORY, and
// other nonzero namespace flags are rejected. WRITE rejects immutable files
// before a writable descriptor can be installed. A successful OPEN returns [OK, kind]
// and a direct object capability. TRANSFER
// explicitly permits handing that authority to a POSIX descriptor view;
// synchronous IPC requires both TRANSFER and DUPLICATE on the source handle.
// STAT requires zero flags and returns [OK, kind:u8, file ID:u64 LE, size:u64 LE]
// without returning a capability.
enum { MOSS_NAMESPACE_OPEN = 1, MOSS_NAMESPACE_STAT = 2 };
enum { MOSS_NAMESPACE_KIND_FILE = 1, MOSS_NAMESPACE_KIND_DIRECTORY = 4 };
enum { MOSS_NAMESPACE_OPEN_REPLY_BYTES = 2, MOSS_NAMESPACE_STAT_REPLY_BYTES = 18 };
enum { MOSS_NAMESPACE_OPEN_CREATE = 1U << 0, MOSS_NAMESPACE_OPEN_TRANSFER = 1U << 1,
       MOSS_NAMESPACE_OPEN_EXCLUSIVE = 1U << 2, MOSS_NAMESPACE_OPEN_WRITE = 1U << 3 };
enum {
  MOSS_NAMESPACE_OK = 0,
  MOSS_NAMESPACE_BAD_REQUEST = 1,
  MOSS_NAMESPACE_NO_ENTRY = 2,
  MOSS_NAMESPACE_UNAVAILABLE = 3,
  MOSS_NAMESPACE_EXISTS = 4,
  MOSS_NAMESPACE_IS_DIRECTORY = 5,
  MOSS_NAMESPACE_READ_ONLY = 6
};
