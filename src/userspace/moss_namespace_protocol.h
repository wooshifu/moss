#pragma once

#define MOSS_SCRATCH_PATH "/scratch"

// The namespace returns a file endpoint capability only for a valid OPEN.
enum { MOSS_NAMESPACE_OPEN = 1 };
enum { MOSS_NAMESPACE_OK = 0, MOSS_NAMESPACE_BAD_REQUEST = 1, MOSS_NAMESPACE_NO_ENTRY = 2 };
