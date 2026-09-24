#include <errno.h>
#include <limits.h>
#include <stdio.h>
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

static int transfer(unsigned long file, unsigned long memory, unsigned char operation, unsigned long offset,
                    unsigned int count, unsigned int *transferred) {
  struct moss_ipc_message request = {
      .size = MOSS_FILE_IO_HEADER_BYTES,
      .capability = memory,
      .rights = operation == MOSS_FILE_READ ? MOSS_CAP_MAP_WRITE : MOSS_CAP_MAP_READ,
      .payload = {operation},
  };
  moss_file_put_u64(request.payload + 1, offset);
  moss_file_put_u16(request.payload + 9, count);
  struct moss_ipc_message response = {0};
  long result = call(file, &request, &response);
  if (response.capability) {
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  }
  long expected = operation == MOSS_FILE_APPEND ? MOSS_FILE_APPEND_REPLY_BYTES : MOSS_FILE_IO_REPLY_BYTES;
  if (result != expected || response.payload[0] != MOSS_FILE_OK || response.capability || response.rights) {
    return 0;
  }
  if (operation == MOSS_FILE_APPEND) {
    uint64_t start = moss_file_get_u64(response.payload + 1);
    *transferred = moss_file_get_u16(response.payload + 9);
    if (start > MOSS_FILE_CONTENT_BUDGET_BYTES || *transferred > MOSS_FILE_CONTENT_BUDGET_BYTES - start) {
      return 0;
    }
  } else {
    *transferred = moss_file_get_u16(response.payload + 1);
  }
  return *transferred <= count && (operation == MOSS_FILE_READ || *transferred == count);
}

static int resize_file(unsigned long file, uint64_t size) {
  struct moss_ipc_message request = {.size = MOSS_FILE_RESIZE_HEADER_BYTES, .payload = {MOSS_FILE_RESIZE}};
  moss_file_put_u64(request.payload + 1, size);
  struct moss_ipc_message response = {0};
  long result = call(file, &request, &response);
  if (response.capability) {
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  }
  return result == 1 && response.payload[0] == MOSS_FILE_OK && !response.capability && !response.rights;
}

static int file_size(unsigned long file, uint64_t *size) {
  struct moss_ipc_message request = {.size = 1, .payload = {MOSS_FILE_SIZE}};
  struct moss_ipc_message response = {0};
  long result = call(file, &request, &response);
  if (response.capability) {
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  }
  if (result != MOSS_FILE_SIZE_REPLY_BYTES || response.payload[0] != MOSS_FILE_OK || response.capability ||
      response.rights) {
    return 0;
  }
  *size = moss_file_get_u64(response.payload + 1);
  return 1;
}

static int operate(unsigned long file, const char *write_text, size_t write_size, int append) {
  long memory = syscall1(SYS_MEM_CREATE, MOSS_MEM_OBJECT_BYTES);
  if (memory <= 0) {
    return error();
  }
  long mapped = syscall2(SYS_MEM_MAP, memory, MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE);
  if (mapped <= 0) {
    (void)syscall1(SYS_CAP_CLOSE, memory);
    return error();
  }

  int valid = 1;
  if (write_text) {
    // Replacement clears stale tail bytes. Append selects the current end in
    // the file service for each page. Preserve any completed pages if a later
    // call fails; retrying an ambiguous write could duplicate data.
    valid = append || resize_file(file, 0);
    for (size_t offset = 0; valid && offset < write_size;) {
      unsigned int count =
          write_size - offset < MOSS_MEM_OBJECT_BYTES ? (unsigned int)(write_size - offset) : MOSS_MEM_OBJECT_BYTES;
      memcpy((void *)mapped, write_text + offset, count);
      unsigned int transferred = 0;
      valid = transfer(file, (unsigned long)memory, append ? MOSS_FILE_APPEND : MOSS_FILE_WRITE, append ? 0 : offset,
                       count, &transferred);
      offset += transferred;
    }
    if (valid) {
      static const char write_success[] = "MOSS_FILE_WRITE_OK\n";
      static const char append_success[] = "MOSS_FILE_APPEND_OK\n";
      const char *message = append ? append_success : write_success;
      (void)write(STDOUT_FILENO, message, strlen(message));
    }
  } else {
    static const char prefix[] = "MOSS_FILE_READ=";
    int started = 0;
    for (unsigned long offset = 0; offset <= MOSS_FILE_CONTENT_BUDGET_BYTES;) {
      unsigned int count = MOSS_FILE_CONTENT_BUDGET_BYTES - offset < MOSS_MEM_OBJECT_BYTES
                               ? (unsigned int)(MOSS_FILE_CONTENT_BUDGET_BYTES - offset)
                               : MOSS_MEM_OBJECT_BYTES;
      unsigned int transferred = 0;
      valid = transfer(file, (unsigned long)memory, MOSS_FILE_READ, offset, count, &transferred);
      if (!valid) {
        break;
      }
      if (!started) {
        (void)write(STDOUT_FILENO, prefix, sizeof(prefix) - 1);
        started = 1;
      }
      if (transferred) {
        (void)write(STDOUT_FILENO, (const void *)mapped, transferred);
      }
      if (transferred < count || !count) {
        break;
      }
      offset += transferred;
    }
    if (valid) {
      (void)write(STDOUT_FILENO, "\n", 1);
    }
  }
  (void)syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES);
  (void)syscall1(SYS_CAP_CLOSE, memory);
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

  const char *path;
  const char *write_text = NULL;
  size_t write_size = 0;
  int resizing = 0;
  int sizing = 0;
  int appending = 0;
  uint64_t resize_size = 0;
  if ((argc == 2 || argc == 3) && strcmp(argv[1], "read") == 0) {
    path = argc == 3 ? argv[2] : MOSS_SCRATCH_PATH;
  } else if ((argc == 3 || argc == 4) && (strcmp(argv[1], "write") == 0 || strcmp(argv[1], "append") == 0)) {
    write_text = argv[2];
    write_size = strlen(write_text);
    if (write_size > MOSS_FILE_CONTENT_BUDGET_BYTES) {
      return 2;
    }
    appending = strcmp(argv[1], "append") == 0;
    path = argc == 4 ? argv[3] : MOSS_SCRATCH_PATH;
  } else if ((argc == 3 || argc == 4) && strcmp(argv[1], "resize") == 0) {
    char *size_end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(argv[2], &size_end, 10);
    if (errno || !argv[2][0] || *size_end || parsed > MOSS_FILE_CONTENT_BUDGET_BYTES) {
      return 2;
    }
    resizing = 1;
    resize_size = parsed;
    path = argc == 4 ? argv[3] : MOSS_SCRATCH_PATH;
  } else if ((argc == 2 || argc == 3) && strcmp(argv[1], "size") == 0) {
    sizing = 1;
    path = argc == 3 ? argv[2] : MOSS_SCRATCH_PATH;
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
  if (lookup != MOSS_NAMESPACE_OPEN_REPLY_BYTES || opened.payload[0] != MOSS_NAMESPACE_OK ||
      opened.payload[1] != MOSS_NAMESPACE_KIND_FILE || !opened.capability || opened.rights != MOSS_CAP_SEND) {
    if (opened.capability) {
      (void)syscall1(SYS_CAP_CLOSE, (long)opened.capability);
    }
    return error();
  }

  int result;
  if (sizing) {
    uint64_t size = 0;
    result = file_size(opened.capability, &size) ? 0 : error();
    if (!result) {
      char message[64];
      int length = snprintf(message, sizeof(message), "MOSS_FILE_SIZE=%llu\n", (unsigned long long)size);
      if (length <= 0 || (size_t)length >= sizeof(message)) {
        result = error();
      } else {
        (void)write(STDOUT_FILENO, message, (size_t)length);
      }
    }
  } else if (resizing) {
    result = resize_file(opened.capability, resize_size) ? 0 : error();
    if (!result) {
      static const char success[] = "MOSS_FILE_RESIZE_OK\n";
      (void)write(STDOUT_FILENO, success, sizeof(success) - 1);
    }
  } else {
    result = operate(opened.capability, write_text, write_size, appending);
  }
  (void)syscall1(SYS_CAP_CLOSE, (long)opened.capability);
  return result;
}
