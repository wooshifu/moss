#pragma once

// The unbadged root sender serves managed stdio requests. Only a badged
// input sender may feed bytes into the receive ring. The supervisor lends
// it to the validation shell until a physical-input domain owns the feed.
enum {
  MOSS_CONSOLE_INPUT_CAP = 1,
  MOSS_CONSOLE_READ = 2,
  MOSS_CONSOLE_WRITE = 3,
  MOSS_CONSOLE_FEED = 4,
  MOSS_CONSOLE_READ_PREPARE = 5,
  MOSS_CONSOLE_READ_FINISH = 6,
  MOSS_CONSOLE_WAIT = 7
};

enum { MOSS_CONSOLE_OK = 0, MOSS_CONSOLE_BAD_REQUEST = 1, MOSS_CONSOLE_WOULD_BLOCK = 2, MOSS_CONSOLE_UNAVAILABLE = 3 };

// READ/WRITE carry [opcode, stream, count:u16 LE] and a one-page Memory
// Object with MAP_WRITE/MAP_READ. Stream 0 is input, 1/2 are output.
// A successful transfer replies [OK, count:u16 LE]. FEED carries one byte
// from the input sender; it returns WOULD_BLOCK when the ring is full.
enum { MOSS_CONSOLE_IO_BYTES = 4, MOSS_CONSOLE_IO_REPLY_BYTES = 3 };
// PREPARE copies a nonempty read without consuming it. FINISH carries
// [opcode, commit:u8] and consumes the reserved bytes only after its reply
// reaches the Process Service; commit=0 leaves them available for retry.
enum { MOSS_CONSOLE_READ_FINISH_BYTES = 2 };
// WAIT carries [opcode, input stream 0] and a transferred SEND-only Reply.
// A wake asks the caller to retry the read; it does not reserve input.
enum { MOSS_CONSOLE_WAIT_BYTES = 2 };
enum { MOSS_CONSOLE_RING_BYTES = 4096 };

static inline unsigned int moss_console_get_u16(const unsigned char *bytes) {
  return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8);
}

static inline void moss_console_put_u16(unsigned char *bytes, unsigned int value) {
  bytes[0] = (unsigned char)value;
  bytes[1] = (unsigned char)(value >> 8);
}
