#define MOSS_SYSCALL_RAW_ONLY
#include "moss_loader_protocol.h"
#include "syscall.h"

__attribute__((noreturn)) void _start(void) {
  (void)syscall1(SYS_EXIT, MOSS_LOADER_PROBE_EXIT_CODE);
  for (;;) {
  }
}
