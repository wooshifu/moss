#include "validation/internal.h"

static int decimal_equal(const char *actual, const char *expected) {
  while (*actual && *actual == *expected) {
    ++actual;
    ++expected;
  }
  return *actual == *expected;
}

unsigned long numbers_regression(void) {
  // These are the 64-bit unsigned/signed boundaries, rather than arbitrary
  // canaries. Signed subtraction stays representable while constructing MIN.
  const unsigned long unsigned_max = ~0UL;
  const long signed_min = -0x7fffffffffffffffL - 1;
  const long signed_max = 0x7fffffffffffffffL;
  char buffer[MOSS_DECIMAL_BUFFER_SIZE];
  unsigned long errors =
      ultoa(unsigned_max, buffer, sizeof(buffer)) != 20 || !decimal_equal(buffer, "18446744073709551615");
  errors |=
      (unsigned long)(ltoa(signed_min, buffer, sizeof(buffer)) != 20 || !decimal_equal(buffer, "-9223372036854775808"))
      << 1;
  errors |=
      (unsigned long)(ltoa(signed_max, buffer, sizeof(buffer)) != 19 || !decimal_equal(buffer, "9223372036854775807"))
      << 2;

  // A one-byte destination admits only NUL. Nonzero neighbours catch the old
  // zero-value fast path writing two bytes and signed formatting skipping NUL.
  unsigned char one[3] = {0xa5, 0xa5, 0xa5};
  errors |= (unsigned long)(ultoa(0, (char *)one + 1, 1) != 0 || one[0] != 0xa5 || one[1] != 0 || one[2] != 0xa5) << 3;
  one[1] = 0xa5;
  errors |=
      (unsigned long)(ltoa(signed_min, (char *)one + 1, 1) != 0 || one[0] != 0xa5 || one[1] != 0 || one[2] != 0xa5)
      << 4;
  one[1] = 0xa5;
  errors |= (unsigned long)(ultoa(unsigned_max, (char *)one + 1, 0) != 0 || ltoa(signed_min, (char *)one + 1, 0) != 0 ||
                            one[1] != 0xa5)
            << 5;
  // Four bytes admit a three-character prefix and NUL, never the low digits.
  errors |= (unsigned long)(ultoa(12345, buffer, 4) != 3 || !decimal_equal(buffer, "123") ||
                            ltoa(signed_min, buffer, 4) != 3 || !decimal_equal(buffer, "-92"))
            << 6;

  long ends[2];
  if (pipe(ends) != 0) {
    return errors | (1UL << 7);
  }
  long child = fork();
  if (child == 0) {
    if (close((int)ends[0]) != 0 || dup2((int)ends[1], 1) != 1 || close((int)ends[1]) != 0) {
      _exit(99);
    }
    // Capture the real output helpers through Moss write syscalls. Width 22
    // adds two spaces to each 20-character boundary value; small width 3 also
    // covers zero/sign padding. The complete output fits one 4096-byte pipe.
    print_ulong(unsigned_max);
    print("\n");
    print_num_padded(unsigned_max, 22);
    print("\n");
    print_long(signed_min);
    print("\n");
    print_snum_padded(signed_min, 22);
    print("\n");
    print_num_padded(0, 3);
    print("\n");
    print_snum_padded(-1, 3);
    print("\n");
    _exit(37);
  }
  int failed = close((int)ends[1]) != 0 || child < 0;
  const char expected[] = "18446744073709551615\n"
                          "  18446744073709551615\n"
                          "-9223372036854775808\n"
                          "  -9223372036854775808\n"
                          "  0\n"
                          " -1\n";
  unsigned long received = 0;
  char chunk[32];
  long count;
  // Drain even mismatched output before reaping, so regressions cannot strand
  // the writer behind pipe backpressure. The chunk size is a bounded stack choice.
  while ((count = read((int)ends[0], chunk, sizeof(chunk))) > 0) {
    for (long i = 0; i < count; ++i, ++received) {
      failed |= received >= sizeof(expected) - 1 || chunk[i] != expected[received];
    }
  }
  failed |= count < 0 || received != sizeof(expected) - 1;
  failed |= close((int)ends[0]) != 0;
  if (child > 0) {
    failed |= !wait_exit(child, 37);
  }
  return errors | ((unsigned long)(failed != 0) << 7);
}

unsigned long user_ranges(void) {
  unsigned check = 0;
  unsigned long failures = 0;
#define CHECK_RANGE(expr)                                                                                              \
  do {                                                                                                                 \
    if (!(expr))                                                                                                       \
      failures |= 1UL << check;                                                                                        \
    ++check;                                                                                                           \
  } while (0)
  // 2^47 is the exclusive user limit; 0x1000 lies in inherited kernel mappings.
  // Near-end and negative ranges test wrap/overflow. 0x180000000 (6 GiB) is
  // the kernel-installed SIGRETURN_PAGE, which mmap/munmap must preserve.
  // 0x32 adds unsupported MAP_FIXED, and prot bit 8 is outside R/W/X.
  CHECK_RANGE(syscall1(SYS_DEBUG_PRINT, 0) == -22);
  CHECK_RANGE(syscall1(SYS_DEBUG_PRINT, 0x800000000000L) == -14);
  CHECK_RANGE(syscall3(SYS_WRITE, 1, 0x800000000000L, 0) == -22); // current Moss zero-count ABI
  CHECK_RANGE(syscall3(SYS_WRITE, 1, 0x1000, 1) == -14);
  CHECK_RANGE(syscall3(SYS_WRITE, 1, -2L, 4) == -14);
  CHECK_RANGE(syscall3(SYS_CLOCK_GETTIME, 0, (long)"read-only", 0) == -14);
  CHECK_RANGE(syscall6(SYS_MMAP, 0x1000, 4096, 3, 0x22, -1, 0) == -22);
  CHECK_RANGE(syscall6(SYS_MMAP, 0x800000000000L - 4096, 8192, 3, 0x22, -1, 0) == -22);
  CHECK_RANGE(syscall6(SYS_MMAP, 0x180000000L, 4096, 3, 0x22, -1, 0) == -22);
  CHECK_RANGE(syscall6(SYS_MMAP, 0, 4096, 3, 0x32, -1, 0) == -22); // MAP_FIXED unsupported
  CHECK_RANGE(syscall6(SYS_MMAP, 0, 4096, 8, 0x22, -1, 0) == -22);
  CHECK_RANGE(syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 4096) == -22);
  CHECK_RANGE(syscall6(SYS_MMAP, 0, 0x7fffffffffffffffL, 3, 0x22, -1, 0) == -12);
  CHECK_RANGE(syscall2(SYS_MUNMAP, 0x180000000L, 4096) == -22);
  CHECK_RANGE(syscall2(SYS_MUNMAP, -4096L, 8192) == -22);
  char too_long[1024]; // Native MAX_PATH_LEN bytes, with no terminating NUL.
  for (unsigned i = 0; i < sizeof(too_long); ++i) {
    too_long[i] = 'a';
  }
  CHECK_RANGE(syscall3(SYS_OPEN, (long)too_long, 0, 0) == -36);
  long first = syscall6(SYS_MMAP, 0, 4096, 3, 0x22, -1, 0);
  CHECK_RANGE(first > 0);
  if (first > 0) {
    long second = syscall6(SYS_MMAP, first + 4096, 4096, 3, 0x22, -1, 0);
    CHECK_RANGE(second == first + 4096);
    if (second == first + 4096) {
      // Page offsets 4094/4092/4093 leave 2/4/3 bytes respectively in the
      // first VMA, forcing the u32 mask, u64 clock and path to cross into the next.
      unsigned char *mask = (unsigned char *)(first + 4094);
      mask[0] = 1;
      mask[1] = 0;
      mask[2] = 0;
      mask[3] = 0;
      CHECK_RANGE(syscall3(20, 0, 4, (long)mask) == 0);                  // copy_from_user across VMAs
      CHECK_RANGE(syscall3(SYS_CLOCK_GETTIME, 0, first + 4092, 0) == 0); // Moss writes one u64, crossing VMAs
      const char name[] = "/missing-range-test";
      char *path = (char *)(first + 4093);
      for (unsigned i = 0; i < sizeof(name); ++i) {
        path[i] = name[i];
      }
      CHECK_RANGE(syscall3(SYS_OPEN, (long)path, 0, 0) == -2); // string spans two VMAs
    }
    if (second > 0) {
      CHECK_RANGE(syscall2(SYS_MUNMAP, second, 4096) == 0);
    }
    CHECK_RANGE(syscall3(SYS_CLOCK_GETTIME, 0, first + 4092, 0) == -14); // second VMA now absent
    long collision = syscall6(SYS_MMAP, first, 4096, 3, 0x22, -1, 0);
    CHECK_RANGE(collision > 0 && collision != first);
    if (collision > 0 && collision != first) {
      CHECK_RANGE(syscall2(SYS_MUNMAP, collision, 4096) == 0);
    }
    CHECK_RANGE(syscall2(SYS_MUNMAP, first, 4096) == 0);
  }
  long none = syscall6(SYS_MMAP, 0, 4096, 0, 0x22, -1, 0);
  CHECK_RANGE(none > 0);
  if (none > 0) {
    CHECK_RANGE(syscall3(SYS_WRITE, 1, none, 1) == -14);
    CHECK_RANGE(syscall3(SYS_CLOCK_GETTIME, 0, none, 0) == -14);
    CHECK_RANGE(syscall2(SYS_MUNMAP, none, 4096) == 0);
  }
#undef CHECK_RANGE
  return failures;
}
