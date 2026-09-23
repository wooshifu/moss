#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_file_protocol.h"
#include "moss_namespace_protocol.h"
#include "syscall.h"

// Each service call has its own bounded wait; a stalled service cannot hang
// the shell indefinitely, and an ambiguous write is never retried here.
#define FILE_CALL_TIMEOUT_NS 5000000000UL

static long call(unsigned long endpoint, const struct moss_ipc_message *request, struct moss_ipc_message *response) {
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) != 0 || now > LONG_MAX - FILE_CALL_TIMEOUT_NS) {
    return -1;
  }
  return syscall6(SYS_IPC_CALL, (long)endpoint, (long)request, (long)response, (long)(now + FILE_CALL_TIMEOUT_NS), 0,
                  0);
}

static int error(void) {
  static const char message[] = "MOSS_FILE_ERROR\n";
  (void)write(STDERR_FILENO, message, sizeof(message) - 1);
  return 1;
}

static int operate(unsigned long file, struct moss_ipc_message *request, const char *write_text, size_t write_size) {
  int reading = request->payload[0] == MOSS_FILE_READ;
  int shared = reading || write_size > MOSS_IPC_MAX_MESSAGE - 1;
  long memory = 0;
  long mapped = 0;
  if (shared) {
    memory = syscall1(SYS_MEM_CREATE, MOSS_MEM_OBJECT_BYTES);
    if (memory <= 0) {
      return error();
    }
    mapped = syscall2(SYS_MEM_MAP, memory, MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE);
    if (mapped <= 0) {
      (void)syscall1(SYS_CAP_CLOSE, memory);
      return error();
    }
    request->capability = (unsigned long)memory;
    request->rights = reading ? MOSS_CAP_MAP_WRITE : MOSS_CAP_MAP_READ;
    if (!reading) {
      memcpy((void *)mapped, write_text, write_size);
      request->size = MOSS_FILE_MEMORY_HEADER_BYTES;
      request->payload[1] = (unsigned char)(write_size & 0xff);
      request->payload[2] = (unsigned char)(write_size >> 8);
    }
  }

  struct moss_ipc_message response = {0};
  long result = call(file, request, &response);
  if (memory > 0) {
    (void)syscall1(SYS_CAP_CLOSE, memory);
  }
  if (response.capability) {
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  }
  unsigned long length = (unsigned long)response.payload[1] | ((unsigned long)response.payload[2] << 8);
  int valid = result == (reading ? MOSS_FILE_MEMORY_HEADER_BYTES : 1) && response.payload[0] == MOSS_FILE_OK &&
              !response.capability && (!reading || length <= MOSS_MEM_OBJECT_BYTES);
  if (valid && reading) {
    static const char prefix[] = "MOSS_FILE_READ=";
    (void)write(STDOUT_FILENO, prefix, sizeof(prefix) - 1);
    (void)write(STDOUT_FILENO, (const void *)mapped, length);
    (void)write(STDOUT_FILENO, "\n", 1);
  } else if (valid) {
    static const char success[] = "MOSS_FILE_WRITE_OK\n";
    (void)write(STDOUT_FILENO, success, sizeof(success) - 1);
  }
  if (mapped > 0) {
    (void)syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES);
  }
  return valid ? 0 : error();
}

int main(int argc, char **argv) {
  const char *text = getenv("MOSS_NAMESPACE_CAP");
  if (!text || argc < 2) {
    return 2;
  }
  char *end = NULL;
  errno = 0;
  unsigned long namespace = strtoul(text, &end, 10);
  if (errno || !namespace || *end) {
    return 2;
  }

  struct moss_ipc_message request = {0};
  const char *path;
  const char *write_text = NULL;
  size_t write_size = 0;
  if ((argc == 2 || argc == 3) && strcmp(argv[1], "read") == 0) {
    request.size = 1;
    request.payload[0] = MOSS_FILE_READ;
    path = argc == 3 ? argv[2] : MOSS_SCRATCH_PATH;
  } else if ((argc == 3 || argc == 4) && strcmp(argv[1], "write") == 0) {
    write_text = argv[2];
    write_size = strlen(write_text);
    if (write_size > MOSS_MEM_OBJECT_BYTES) {
      return 2;
    }
    request.payload[0] = MOSS_FILE_WRITE;
    if (write_size <= MOSS_IPC_MAX_MESSAGE - 1) {
      request.size = write_size + 1;
      memcpy(request.payload + 1, write_text, write_size);
    }
    path = argc == 4 ? argv[3] : MOSS_SCRATCH_PATH;
  } else {
    return 2;
  }

  size_t path_size = strlen(path) + 1;
  if (path_size > MOSS_IPC_MAX_MESSAGE - 2) {
    return 2;
  }
  struct moss_ipc_message open = {.size = path_size + 2, .payload = {MOSS_NAMESPACE_OPEN}};
  open.payload[1] = write_text ? MOSS_NAMESPACE_OPEN_CREATE : 0;
  memcpy(open.payload + 2, path, path_size);
  struct moss_ipc_message opened = {0};
  long lookup = call(namespace, &open, &opened);
  if (lookup != 1 || opened.payload[0] != MOSS_NAMESPACE_OK || !opened.capability || opened.rights != MOSS_CAP_SEND) {
    if (opened.capability) {
      (void)syscall1(SYS_CAP_CLOSE, (long)opened.capability);
    }
    return error();
  }

  int result = operate(opened.capability, &request, write_text, write_size);
  (void)syscall1(SYS_CAP_CLOSE, (long)opened.capability);
  return result;
}
