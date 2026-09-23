#pragma once

// A single bounded file object is addressed by its endpoint capability.
// The first byte is an operation in requests and a status in replies.
enum { MOSS_FILE_READ = 1, MOSS_FILE_WRITE = 2 };
enum { MOSS_FILE_OK = 0, MOSS_FILE_BAD_REQUEST = 1 };
// Shared-page requests use a two-byte little-endian length after the opcode;
// shared-page read replies use the same length after the status byte.
enum { MOSS_FILE_MEMORY_HEADER_BYTES = 3 };
