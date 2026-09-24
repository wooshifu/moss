#pragma once

#define MOSS_SCRATCH_PATH "/scratch"

// OPEN carries [opcode, flags, absolute NUL-terminated flat-root path].
// The root path "/" is read-only and requires zero namespace flags; its
// returned sender names the root directory object.
// A successful lookup returns a direct file endpoint capability. TRANSFER
// explicitly permits handing that authority to a POSIX descriptor view;
// synchronous IPC requires both TRANSFER and DUPLICATE on the source handle.
enum { MOSS_NAMESPACE_OPEN = 1 };
enum { MOSS_NAMESPACE_OPEN_CREATE = 1U << 0, MOSS_NAMESPACE_OPEN_TRANSFER = 1U << 1,
       MOSS_NAMESPACE_OPEN_EXCLUSIVE = 1U << 2 };
enum {
  MOSS_NAMESPACE_OK = 0,
  MOSS_NAMESPACE_BAD_REQUEST = 1,
  MOSS_NAMESPACE_NO_ENTRY = 2,
  MOSS_NAMESPACE_UNAVAILABLE = 3,
  MOSS_NAMESPACE_EXISTS = 4
};
