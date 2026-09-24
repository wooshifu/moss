#pragma once

// RUN carries [opcode, argc, envc, NUL-terminated argv strings, then envp
// strings] and a SEND capability naming a sealable, unlisted File Service
// object. Loader seals it before reading; named or unavailable objects return
// NO_IMAGE without granting execution.
// Each count occupies one byte; all strings must fit the single IPC payload exactly.
// A successful reply gives the private supervisor observation, identity
// inspection and termination rights, plus the ability to delegate a reduced
// observation handle to the Process Compatibility Service.
enum { MOSS_LOADER_RUN = 1, MOSS_LOADER_RUN_HEADER_BYTES = 3 };
enum {
  MOSS_LOADER_OK = 0,
  MOSS_LOADER_BAD_REQUEST = 1,
  MOSS_LOADER_NO_IMAGE = 2,
  MOSS_LOADER_BAD_IMAGE = 3,
  MOSS_LOADER_UNAVAILABLE = 4
};
// Distinct from each other, normal success and execve failure status (127).
enum { MOSS_LOADER_PROBE_EXIT_CODE = 37, MOSS_LOADER_LIBC_PROBE_EXIT_CODE = 41 };
