#pragma once

// A zero badge marks lookup requests. Only the file service's MINT authority
// can issue the nonzero badge that identifies /scratch for read/write requests.
enum { MOSS_FILE_SCRATCH_BADGE = 1 };
// OPEN returns a file sender to the namespace, which transfers only SEND to
// the client. File operations then bypass the namespace service.
// The first byte is an operation in requests and a status in replies.
enum { MOSS_FILE_READ = 1, MOSS_FILE_WRITE = 2, MOSS_FILE_OPEN = 3 };
enum { MOSS_FILE_OK = 0, MOSS_FILE_BAD_REQUEST = 1 };
// Shared-page requests use a two-byte little-endian length after the opcode;
// shared-page read replies use the same length after the status byte.
enum { MOSS_FILE_MEMORY_HEADER_BYTES = 3 };
