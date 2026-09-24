#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "syscall.h"

static int error(void) {
  static const char message[] = "MOSS_DOMAIN_ERROR\n";
  (void)write(STDERR_FILENO, message, sizeof(message) - 1);
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 3 || strcmp(argv[1], "terminate") != 0)
    return error();

  const char *variable;
  if (strcmp(argv[2], "file") == 0)
    variable = "MOSS_FILE_DOMAIN_CAP";
  else if (strcmp(argv[2], "namespace") == 0)
    variable = "MOSS_NAMESPACE_DOMAIN_CAP";
  else if (strcmp(argv[2], "process") == 0)
    variable = "MOSS_PROCESS_DOMAIN_CAP";
  else if (strcmp(argv[2], "code") == 0)
    variable = "MOSS_CODE_DOMAIN_CAP";
  else if (strcmp(argv[2], "supervisor") == 0)
    variable = "MOSS_SUPERVISOR_DOMAIN_CAP";
  else
    return error();

  const char *value = getenv(variable);
  if (!value || *value < '0' || *value > '9')
    return error();
  char *end = NULL;
  errno = 0;
  unsigned long handle = strtoul(value, &end, 10);
  if (errno || !handle || handle > LONG_MAX || *end || syscall1(SYS_DOMAIN_TERMINATE, (long)handle) != 0)
    return error();

  return 0;
}
