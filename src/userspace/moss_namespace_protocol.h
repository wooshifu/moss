#pragma once

#define MOSS_SCRATCH_PATH "/scratch"

// OPEN carries [opcode, flags, absolute NUL-terminated flat-root file path].
// A successful lookup returns a direct file endpoint capability.
enum { MOSS_NAMESPACE_OPEN = 1 };
enum { MOSS_NAMESPACE_OPEN_CREATE = 1U << 0 };
enum {
  MOSS_NAMESPACE_OK = 0,
  MOSS_NAMESPACE_BAD_REQUEST = 1,
  MOSS_NAMESPACE_NO_ENTRY = 2,
  MOSS_NAMESPACE_UNAVAILABLE = 3
};
