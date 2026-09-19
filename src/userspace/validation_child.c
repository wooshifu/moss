#include "syscall.h"

// 94 marks a forbidden access that unexpectedly returned; 95/96/97 identify
// startup-vector, FP-reset and boundary-plan failures. 37 is the shared success
// marker. A SIGSEGV exit is encoded as (-11)&0xff = 245 by the Moss wait ABI.
enum { FORBIDDEN_ACCESS_EXIT = 94, STARTUP_EXIT = 95, FP_EXIT = 96, BOUNDARY_EXIT = 97, SUCCESS_EXIT = 37 };
enum { SEGFAULT_EXIT = 245 };

// Keep this geometry synchronized with gen_validation_initramfs.py. The two
// additional PT_LOAD ranges are far beyond the linked child image but remain in
// the ordinary user-code area that begins at 8 GiB. Non-power-of-two file/BSS
// tails make endpoint rounding errors observable.
enum { PAGE_BYTES = 4096, RX_PREFIX_BYTES = 0x124, RW_PREFIX_BYTES = 0x2A0 };
static const unsigned long RX_PAGE = 0x0000000200020000UL;
static const unsigned long RX_ADDRESS = 0x0000000200020000UL + RX_PREFIX_BYTES;
static const unsigned long RX_FILE_BYTES = PAGE_BYTES + 37;
static const unsigned long RX_MEMORY_BYTES = PAGE_BYTES + 37 + 211;
static const unsigned long RW_PAGE = 0x0000000200040000UL;
static const unsigned long RW_ADDRESS = 0x0000000200040000UL + RW_PREFIX_BYTES;
static const unsigned long RW_FILE_BYTES = 83;
static const unsigned long RW_MEMORY_BYTES = PAGE_BYTES + 257;

// Distinct nonzero bytes identify each file prefix and payload; zero is reserved
// for the BSS and rounded page tails supplied by the loader.
enum { RX_PREFIX_MARKER = 0xA1, RX_FILE_MARKER = 0xB2, RW_PREFIX_MARKER = 0xC3, RW_FILE_MARKER = 0xD4 };
#if defined(__x86_64__)
enum { RETURN_CODE_BYTES = 6 }; // mov eax, 42; ret
#else
enum { RETURN_CODE_BYTES = 8 }; // Two fixed-width AArch64/RV64 instructions.
#endif

static int text_equal(const char *left, const char *right) {
  while (*left && *left == *right) {
    ++left;
    ++right;
  }
  return *left == *right;
}

static int wait_exit(long child, int code) {
  int status = 0;
  return child > 1 && waitpid(child, &status, 0) == child && ((status >> 8) & 255) == code;
}

static int access_faults(unsigned long address, int execute) {
  long child = fork();
  if (child == 0) {
    if (execute) {
      ((void (*)(void))address)();
    } else {
#if defined(__aarch64__)
      asm volatile("strb wzr, [%0]" : : "r"(address) : "memory");
#elif defined(__x86_64__)
      asm volatile("movb $0, (%0)" : : "r"(address) : "memory");
#else
      asm volatile("sb zero, 0(%0)" : : "r"(address) : "memory");
#endif
    }
    _exit(FORBIDDEN_ACCESS_EXIT);
  }
  return wait_exit(child, SEGFAULT_EXIT);
}

static int boundary_load_plan(void) {
  const volatile unsigned char *rx_page = (const volatile unsigned char *)RX_PAGE;
  const volatile unsigned char *rx = (const volatile unsigned char *)RX_ADDRESS;
  volatile unsigned char *rw_page = (volatile unsigned char *)RW_PAGE;
  volatile unsigned char *rw = (volatile unsigned char *)RW_ADDRESS;

  unsigned long errors = 0;
  errors |= (unsigned long)(rx_page[0] != RX_PREFIX_MARKER || rx_page[RX_PREFIX_BYTES - 1] != RX_PREFIX_MARKER) << 0;
  errors |= (unsigned long)(rx[RETURN_CODE_BYTES] != RX_FILE_MARKER || rx[RX_FILE_BYTES - 1] != RX_FILE_MARKER) << 1;
  errors |= (unsigned long)(rx[RX_FILE_BYTES] != 0 || rx[RX_MEMORY_BYTES - 1] != 0 || rx_page[2 * PAGE_BYTES - 1] != 0)
            << 2;
  errors |= (unsigned long)(((long (*)(void))RX_ADDRESS)() != 42) << 3;
  errors |= (unsigned long)!access_faults(RX_ADDRESS + RETURN_CODE_BYTES, 0) << 4;

  errors |= (unsigned long)(rw_page[0] != RW_PREFIX_MARKER || rw_page[RW_PREFIX_BYTES - 1] != RW_PREFIX_MARKER) << 5;
  errors |= (unsigned long)(rw[RETURN_CODE_BYTES] != RW_FILE_MARKER || rw[RW_FILE_BYTES - 1] != RW_FILE_MARKER) << 6;
  errors |= (unsigned long)(rw[RW_FILE_BYTES] != 0 || rw[RW_MEMORY_BYTES - 1] != 0 || rw_page[2 * PAGE_BYTES - 1] != 0)
            << 7;
  // A direct store proves the RW segment's final writable permission and BSS residency.
  rw[RW_FILE_BYTES] = 0x5E;
  errors |= (unsigned long)(rw[RW_FILE_BYTES] != 0x5E) << 8;
  errors |= (unsigned long)!access_faults(RW_ADDRESS, 1) << 9;
  return errors == 0;
}

void _start(long argc, const char **argv) {
  if (argc != 1 || !argv || !argv[0] || argv[1]) {
    _exit(STARTUP_EXIT);
  }
  if (text_equal(argv[0], "boundary")) {
    _exit(boundary_load_plan() ? SUCCESS_EXIT : BOUNDARY_EXIT);
  }
  if (!text_equal(argv[0], "exec")) {
    _exit(STARTUP_EXIT);
  }
#if defined(__x86_64__)
  unsigned short cw, status;
  unsigned mxcsr;
  unsigned long vector[2];
  asm volatile("fnstcw %0; fnstsw %1; stmxcsr %2; movdqu %%xmm15, %3"
               : "=m"(cw), "=m"(status), "=m"(mxcsr), "=m"(vector)
               :
               : "memory");
  // x87 control 0x37f and MXCSR 0x1f80 are the default masked, nearest-rounding
  // states; exec must clear the inherited x87 status and XMM15 contents too.
  if (cw != 0x37f || status != 0 || mxcsr != 0x1f80 || vector[0] || vector[1])
    _exit(FP_EXIT);
#endif
  // The parent must observe this exit code only after a successful real exec.
  _exit(SUCCESS_EXIT);
}
