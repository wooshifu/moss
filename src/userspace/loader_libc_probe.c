#include <stdlib.h>
#include <string.h>

#include "moss_loader_protocol.h"

// Volatile accesses make the probe read mapped TLS bytes instead of folding
// the expected values into constants during optimization.
static _Thread_local volatile int initialized_tls = 17;
static _Thread_local volatile int zero_tls;

int main(int argc, char **argv, char **envp) {
  if (argc != 2 || !argv || !argv[0] || !argv[1] || argv[2] || strcmp(argv[0], "loader-libc-probe") ||
      strcmp(argv[1], "from-supervisor") || !envp || !envp[0] || envp[1] || strcmp(envp[0], "MOSS_LOADER=ready") ||
      !getenv("MOSS_LOADER") || strcmp(getenv("MOSS_LOADER"), "ready") || initialized_tls != 17 || zero_tls != 0)
    return 1;
  initialized_tls = 23;
  zero_tls = 7;
  return initialized_tls == 23 && zero_tls == 7 ? MOSS_LOADER_LIBC_PROBE_EXIT_CODE : 2;
}
