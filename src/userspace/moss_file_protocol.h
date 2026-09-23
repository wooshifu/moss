#pragma once

// Zero is the unbadged endpoint. This service currently has one bounded file;
// its minted badge identifies /scratch independently of client payloads.
enum { MOSS_FILE_SCRATCH_BADGE = 1 };
// Requests reach the service through its endpoint and identify the file by badge.
// The first byte is an operation in requests and a status in replies.
enum { MOSS_FILE_READ = 1, MOSS_FILE_WRITE = 2 };
enum { MOSS_FILE_OK = 0, MOSS_FILE_BAD_REQUEST = 1 };
// Shared-page requests use a two-byte little-endian length after the opcode;
// shared-page read replies use the same length after the status byte.
enum { MOSS_FILE_MEMORY_HEADER_BYTES = 3 };
