#include "syscall.h"

// 95/96 identify startup-vector/FP-reset failures; 37 is the success marker
// shared with validation.c. They are fixture protocol values, not errno codes.

void _start(long argc, const char **argv) {
  if (argc != 1 || !argv || !argv[0] || argv[1] || argv[0][0] != 'e' || argv[0][1] != 'x' || argv[0][2] != 'e' ||
      argv[0][3] != 'c' || argv[0][4]) {
    _exit(95);
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
    _exit(96);
#endif
  // The parent must observe this exit code only after a successful real exec.
  _exit(37);
}
