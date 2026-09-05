#include "syscall.h"

void _start(void) {
  // The parent must observe this exit code only after a successful real exec.
  _exit(37);
}
