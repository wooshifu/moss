#pragma once

// RUN carries [opcode] and a SEND capability naming the authorized File
// Service object. A successful reply gives the private supervisor observation,
// identity inspection and termination rights, plus the ability to delegate a
// reduced observation handle to the Process Compatibility Service.
enum { MOSS_LOADER_RUN = 1 };
enum {
  MOSS_LOADER_OK = 0,
  MOSS_LOADER_BAD_REQUEST = 1,
  MOSS_LOADER_NO_IMAGE = 2,
  MOSS_LOADER_BAD_IMAGE = 3,
  MOSS_LOADER_UNAVAILABLE = 4
};
// Distinct from normal success and the service's execve failure status (127).
enum { MOSS_LOADER_PROBE_EXIT_CODE = 37 };
