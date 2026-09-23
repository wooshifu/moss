#pragma once

// A single bounded file object is addressed by its endpoint capability.
// The first byte is an operation in requests and a status in replies.
enum { MOSS_FILE_READ = 1, MOSS_FILE_WRITE = 2 };
enum { MOSS_FILE_OK = 0, MOSS_FILE_BAD_REQUEST = 1 };
