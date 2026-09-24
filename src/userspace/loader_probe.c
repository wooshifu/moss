#define MOSS_SYSCALL_RAW_ONLY
#include "moss_loader_protocol.h"
#include "syscall.h"

enum { LOADER_PROBE_BAD_STARTUP_EXIT_CODE = 1 };

static int same(const char *left, const char *right) {
  while (*left && *left == *right) {
    ++left;
    ++right;
  }
  return *left == *right;
}

__attribute__((noreturn)) void _start(long argc, const char **argv, const char **envp) {
  const unsigned long *vector = envp ? (const unsigned long *)(envp + 2) : 0;
  int valid = argc == 2 && argv && envp && ((const unsigned long *)argv)[-1] == 2 && argv[0] && argv[1] && !argv[2] &&
              same(argv[0], "loader-probe") && same(argv[1], "from-supervisor") && envp[0] && !envp[1] &&
              same(envp[0], "MOSS_LOADER=ready") && vector && vector[0] == 0 && vector[1] == 0;
  (void)syscall1(SYS_EXIT, valid ? MOSS_LOADER_PROBE_EXIT_CODE : LOADER_PROBE_BAD_STARTUP_EXIT_CODE);
  for (;;) {
  }
}
