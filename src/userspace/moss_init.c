#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_code_authority_protocol.h"
#include "moss_file_protocol.h"
#include "moss_loader_protocol.h"
#include "moss_process_protocol.h"
#include "syscall.h"

// One second between launches bounds a failing service's restart rate.
#define RESTART_DELAY_NS 1000000000UL
// Match the native IPC validation bound so a stalled service cannot hold boot indefinitely.
#define CODE_CALL_TIMEOUT_NS 5000000000UL

struct Service {
  pid_t pid;
  long send;
  long domain;
};

struct LoaderImages {
  long probe;
  long libc_probe;
  long bad;
  long named_source;
  long named_snapshot;
};

enum ServiceLoss { FILE_LOST, NAMESPACE_LOST, PROCESS_LOST, SUPERVISOR_FAILURE };

static void report(int fd, const char *message) { (void)write(fd, message, strlen(message)); }

static void report_started(const char *name, pid_t pid) {
  char message[80];
  int size = snprintf(message, sizeof(message), "moss-init: %s started pid=%ld\n", name, (long)pid);
  if (size > 0 && (size_t)size < sizeof(message)) {
    (void)write(STDOUT_FILENO, message, (size_t)size);
  }
}

static void reap_finished_children(void) {
  int status;
  pid_t child;
  do {
    child = waitpid(-1, &status, WNOHANG);
  } while (child > 0 || (child < 0 && errno == EINTR));
}

static void child_exited(int signo) {
  (void)signo;
  int saved_errno = errno;
  // Reap in the handler so an orphan cannot exit between a drain and the
  // supervisor's domain wait. Supervised domains retire independently.
  reap_finished_children();
  errno = saved_errno;
}

static pid_t fork_domain(long *domain, const struct moss_fork_capability *handles, size_t count) {
  *domain = 0;
  return (pid_t)syscall3(SYS_FORK_DOMAIN_SELECT, (long)domain, (long)handles, (long)count);
}

static void stop_child(pid_t child, long domain) {
  if (child > 0) {
    long result = domain > 0 ? syscall1(SYS_DOMAIN_TERMINATE, domain) : -EBADF;
    if (result != 0 && result != -ESRCH) {
      // Losing the authority for a live supervised child leaves no safe
      // recovery owner. Exiting init enters the kernel's fatal reset path.
      report(STDERR_FILENO, "moss-init: child domain termination failed\n");
      _exit(1);
    }
    // Native domains auto-retire their diagnostic PID after publishing exit.
    do {
      result = syscall1(SYS_DOMAIN_WAIT, domain);
    } while (result == -EINTR);
    if (result != 0) {
      report(STDERR_FILENO, "moss-init: child domain wait failed\n");
      _exit(1);
    }
  }
  if (domain > 0) {
    (void)syscall1(SYS_CAP_CLOSE, domain);
  }
}

static void stop_service(struct Service *service) {
  stop_child(service->pid, service->domain);
  if (service->send > 0) {
    (void)syscall1(SYS_CAP_CLOSE, service->send);
  }
  service->pid = 0;
  service->send = 0;
  service->domain = 0;
}

static void close_loader_images(struct LoaderImages *images) {
  if (images->probe > 0)
    (void)syscall1(SYS_CAP_CLOSE, images->probe);
  if (images->libc_probe > 0)
    (void)syscall1(SYS_CAP_CLOSE, images->libc_probe);
  if (images->bad > 0)
    (void)syscall1(SYS_CAP_CLOSE, images->bad);
  if (images->named_source > 0) {
    (void)syscall1(SYS_CAP_CLOSE, images->named_source);
  }
  if (images->named_snapshot > 0) {
    (void)syscall1(SYS_CAP_CLOSE, images->named_snapshot);
  }
  *images = (struct LoaderImages){0};
}

static int restart_delay(void) {
  // The native syscall accepts nanoseconds; this mlibc port has no
  // nanosleep sysdep yet.
  unsigned long delay = RESTART_DELAY_NS;
  unsigned long remaining = 0;
  long result;
  while ((result = syscall2(SYS_NANOSLEEP, (long)&delay, (long)&remaining)) == -EINTR) {
    delay = remaining;
  }
  return result == 0 ? 0 : -1;
}

static int start_endpoint_service(struct Service *service, const char *program) {
  struct moss_ipc_endpoints endpoints = {0};
  if (syscall1(SYS_IPC_CREATE, (long)&endpoints) != 0) {
    return -1;
  }
  long mint = 0;
  long receive = syscall2(SYS_CAP_DUPLICATE, (long)endpoints.receive, MOSS_CAP_RECEIVE | MOSS_CAP_DUPLICATE);
  if (receive <= 0) {
    goto fail;
  }
  // The service owns mint authority. The supervisor retains only an
  // attenuated sender for clients and recovery.
  mint = syscall2(SYS_CAP_DUPLICATE, (long)endpoints.send,
                  MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE | MOSS_CAP_MINT);
  if (mint <= 0) {
    goto fail;
  }
  char receive_arg[32], mint_arg[32]; // Each holds a decimal 64-bit handle.
  int receive_size = snprintf(receive_arg, sizeof(receive_arg), "%lu", (unsigned long)receive);
  int mint_size = snprintf(mint_arg, sizeof(mint_arg), "%lu", (unsigned long)mint);
  if (receive_size < 0 || (size_t)receive_size >= sizeof(receive_arg) || mint_size < 0 ||
      (size_t)mint_size >= sizeof(mint_arg)) {
    goto fail;
  }

  long domain = 0;
  const struct moss_fork_capability handles[] = {
      {(unsigned long)receive, MOSS_CAP_RECEIVE, 0},
      {(unsigned long)mint, MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE | MOSS_CAP_MINT, 0}};
  pid_t child = fork_domain(&domain, handles, sizeof(handles) / sizeof(handles[0]));
  if (child == 0) {
    char *const argv[] = {(char *)program, receive_arg, mint_arg, NULL};
    execve(program, argv, NULL);
    _exit(127);
  }
  (void)syscall1(SYS_CAP_CLOSE, mint);
  (void)syscall1(SYS_CAP_CLOSE, receive);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.receive);
  if (child < 0) {
    stop_child(child, domain);
    (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
    return -1;
  }
  long send = syscall2(SYS_CAP_DUPLICATE, (long)endpoints.send, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
  if (send <= 0) {
    stop_child(child, domain);
    return -1;
  }
  service->pid = child;
  service->send = send;
  service->domain = domain;
  return 0;

fail:
  if (mint > 0) {
    (void)syscall1(SYS_CAP_CLOSE, mint);
  }
  if (receive > 0) {
    (void)syscall1(SYS_CAP_CLOSE, receive);
  }
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.receive);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
  return -1;
}

static int start_code_service(struct Service *service, long approver) {
  struct moss_ipc_endpoints endpoints = {0};
  if (syscall1(SYS_IPC_CREATE, (long)&endpoints) != 0)
    return -1;
  char receive_arg[32], approver_arg[32]; // Each holds a decimal 64-bit handle.
  int receive_size = snprintf(receive_arg, sizeof(receive_arg), "%lu", endpoints.receive);
  int approver_size = snprintf(approver_arg, sizeof(approver_arg), "%lu", (unsigned long)approver);
  if (receive_size < 0 || (size_t)receive_size >= sizeof(receive_arg) || approver_size < 0 ||
      (size_t)approver_size >= sizeof(approver_arg))
    goto fail;

  // The child receives approval only. Its issuer lifetime cannot erase an
  // approval, and the supervisor alone retains this scope's revoke right.
  const struct moss_fork_capability handles[] = {{endpoints.receive, MOSS_CAP_RECEIVE, 0},
                                                 {(unsigned long)approver, MOSS_CAP_CODE_APPROVE, 0}};
  long domain = 0;
  pid_t child = fork_domain(&domain, handles, sizeof(handles) / sizeof(handles[0]));
  if (child == 0) {
    char *const argv[] = {"code-authority-service", receive_arg, approver_arg, NULL};
    execve("/code-authority-service.elf", argv, NULL);
    _exit(127);
  }
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.receive);
  if (child < 0) {
    stop_child(child, domain);
    (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
    return -1;
  }
  // The trusted loader needs a selected sender for each incarnation. Keep
  // delegation private to init; ordinary shell descendants never receive it.
  long send = syscall2(SYS_CAP_DUPLICATE, (long)endpoints.send, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
  if (send <= 0) {
    stop_child(child, domain);
    return -1;
  }
  service->pid = child;
  service->send = send;
  service->domain = domain;
  return 0;

fail:
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.receive);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
  return -1;
}

static long request_code_probe(long send) {
  // Bootstrap probes only the supervisor's own text, never caller-supplied
  // bytes. The private sender is this temporary policy's authorization.
  unsigned long page = (unsigned long)start_code_service & ~(MOSS_MEM_OBJECT_BYTES - 1UL);
  long version = syscall1(SYS_CODE_SNAPSHOT, (long)page);
  if (version <= 0)
    return -1;
  unsigned long now = 0;
  long approved = -1;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) == 0 && now <= LONG_MAX - CODE_CALL_TIMEOUT_NS) {
    const struct moss_ipc_message request = {
        .size = 1, .capability = (unsigned long)version, .rights = MOSS_CAP_MAP_READ, .payload = {MOSS_CODE_APPROVE}};
    struct moss_ipc_message response = {0};
    long result =
        syscall6(SYS_IPC_CALL, send, (long)&request, (long)&response, (long)(now + CODE_CALL_TIMEOUT_NS), 0, 0);
    if (result == 1 && response.size == 1 && response.payload[0] == MOSS_CODE_OK && response.capability &&
        response.rights == (MOSS_CAP_CODE_EXEC | MOSS_CAP_CODE_IDENTIFY | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE))
      approved = (long)response.capability;
    else if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  }
  (void)syscall1(SYS_CAP_CLOSE, version);
  return approved;
}

static int launch_code_service(struct Service *service, long approver, long *probe) {
  if (start_code_service(service, approver) != 0)
    return -1;
  *probe = request_code_probe(service->send);
  if (*probe <= 0) {
    stop_service(service);
    return -1;
  }
  report_started("code authority service", service->pid);
  return 0;
}

static long call_service(long send, const struct moss_ipc_message *request, struct moss_ipc_message *response) {
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) != 0 || now > LONG_MAX - CODE_CALL_TIMEOUT_NS)
    return -1;
  return syscall6(SYS_IPC_CALL, send, (long)request, (long)response, (long)(now + CODE_CALL_TIMEOUT_NS), 0, 0);
}

static int seed_loader_file(long file, const char *name, const char *source, int unlisted, long *image) {
  *image = 0;
  int fd = source ? open(source, O_RDONLY) : -1;
  if (source && fd < 0)
    return -1;
  struct stat source_status = {0};
  if (source && (fstat(fd, &source_status) != 0 || source_status.st_size <= 0 ||
                 source_status.st_size > MOSS_FILE_CONTENT_BUDGET_BYTES)) {
    (void)close(fd);
    return -1;
  }
  struct moss_ipc_message request = {
      .size = strlen(name) + 3,
      .payload = {MOSS_FILE_OPEN, MOSS_FILE_OPEN_CREATE | (unlisted ? MOSS_FILE_OPEN_UNLISTED : 0)}};
  memcpy(request.payload + 2, name, strlen(name) + 1);
  struct moss_ipc_message response = {0};
  long sent = call_service(file, &request, &response);
  if (sent != 1 || response.size != 1 || response.payload[0] != MOSS_FILE_OK || !response.capability ||
      response.rights != (MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE)) {
    if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    if (fd >= 0)
      (void)close(fd);
    return -1;
  }
  long object = (long)response.capability;
  // Reserve the known immutable seed length once: per-page realloc would
  // repeatedly copy the growing libc image during each service recovery.
  if (source) {
    request = (struct moss_ipc_message){.size = MOSS_FILE_RESIZE_HEADER_BYTES, .payload = {MOSS_FILE_RESIZE}};
    moss_file_put_u64(request.payload + 1, (uint64_t)source_status.st_size);
    response = (struct moss_ipc_message){0};
    sent = call_service(object, &request, &response);
    if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    if (sent != 1 || response.size != 1 || response.payload[0] != MOSS_FILE_OK || response.capability ||
        response.rights) {
      (void)syscall1(SYS_CAP_CLOSE, object);
      (void)close(fd);
      return -1;
    }
  }
  long memory = syscall1(SYS_MEM_CREATE, MOSS_MEM_OBJECT_BYTES);
  long mapped = memory > 0 ? syscall2(SYS_MEM_MAP, memory, MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE) : -1;
  int valid = mapped > 0;
  size_t offset = 0;
  while (valid) {
    ssize_t count;
    if (source) {
      count = read(fd, (void *)mapped, MOSS_MEM_OBJECT_BYTES);
      if (count < 0 && errno == EINTR)
        continue;
    } else {
      memcpy((void *)mapped, "BAD", 3);
      count = offset ? 0 : 3;
    }
    if (count < 0 || (size_t)count > MOSS_FILE_CONTENT_BUDGET_BYTES - offset) {
      valid = 0;
      break;
    }
    if (!count)
      break;
    request = (struct moss_ipc_message){.size = MOSS_FILE_IO_HEADER_BYTES,
                                        .capability = (unsigned long)memory,
                                        .rights = MOSS_CAP_MAP_READ,
                                        .payload = {MOSS_FILE_WRITE}};
    moss_file_put_u64(request.payload + 1, offset);
    moss_file_put_u16(request.payload + 9, (unsigned int)count);
    response = (struct moss_ipc_message){0};
    sent = call_service(object, &request, &response);
    if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    if (sent != MOSS_FILE_IO_REPLY_BYTES || response.payload[0] != MOSS_FILE_OK || response.capability ||
        response.rights || moss_file_get_u16(response.payload + 1) != (unsigned int)count) {
      valid = 0;
      break;
    }
    offset += (size_t)count;
  }
  if (source && offset != (size_t)source_status.st_size)
    valid = 0;
  if (valid && offset && unlisted) {
    // A root sender can create an unlisted object, but must not be able to
    // recover its authority by a later name lookup.
    request = (struct moss_ipc_message){.size = strlen(name) + 3, .payload = {MOSS_FILE_OPEN}};
    memcpy(request.payload + 2, name, strlen(name) + 1);
    response = (struct moss_ipc_message){0};
    sent = call_service(file, &request, &response);
    valid = sent == 1 && response.size == 1 && response.payload[0] == MOSS_FILE_NO_ENTRY && !response.capability &&
            !response.rights;
    if (response.capability) {
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    }
  }
  if (valid && offset && unlisted) {
    request = (struct moss_ipc_message){.size = 1, .payload = {MOSS_FILE_SEAL}};
    response = (struct moss_ipc_message){0};
    sent = call_service(object, &request, &response);
    valid = sent == 1 && response.size == 1 && response.payload[0] == MOSS_FILE_OK && !response.capability &&
            !response.rights;
    if (response.capability) {
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    }
  }
  if (valid && offset && unlisted) {
    // Deliberately try to corrupt the sealed ELF while we still own a sender
    // and mapped page. A bad WRITE implementation makes startup fail here.
    memcpy((void *)mapped, "BAD", 3);
    request = (struct moss_ipc_message){.size = MOSS_FILE_IO_HEADER_BYTES,
                                        .capability = (unsigned long)memory,
                                        .rights = MOSS_CAP_MAP_READ,
                                        .payload = {MOSS_FILE_WRITE}};
    moss_file_put_u64(request.payload + 1, 0);
    moss_file_put_u16(request.payload + 9, 3);
    response = (struct moss_ipc_message){0};
    sent = call_service(object, &request, &response);
    valid = sent == 1 && response.size == 1 && response.payload[0] == MOSS_FILE_BAD_REQUEST && !response.capability &&
            !response.rights;
    if (response.capability) {
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    }
  }
  if (valid && offset && unlisted) {
    request = (struct moss_ipc_message){.size = MOSS_FILE_RESIZE_HEADER_BYTES, .payload = {MOSS_FILE_RESIZE}};
    moss_file_put_u64(request.payload + 1, 0);
    response = (struct moss_ipc_message){0};
    sent = call_service(object, &request, &response);
    valid = sent == 1 && response.size == 1 && response.payload[0] == MOSS_FILE_BAD_REQUEST && !response.capability &&
            !response.rights;
    if (response.capability) {
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    }
  }
  if (mapped > 0 && syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES) != 0)
    _exit(1); // Init cannot safely retry with an accumulating leaked mapping.
  if (memory > 0)
    (void)syscall1(SYS_CAP_CLOSE, memory);
  if (fd >= 0)
    (void)close(fd);
  if (!valid || !offset) {
    (void)syscall1(SYS_CAP_CLOSE, object);
    return -1;
  }
  *image = object;
  return 0;
}

static int start_loader_service(const struct Service *code, long factory, struct Service *service) {
  struct moss_ipc_endpoints endpoints = {0};
  if (syscall1(SYS_IPC_CREATE, (long)&endpoints) != 0)
    return -1;
  char receive_arg[32], code_arg[32], factory_arg[32];
  int sizes[] = {snprintf(receive_arg, sizeof(receive_arg), "%lu", endpoints.receive),
                 snprintf(code_arg, sizeof(code_arg), "%lu", (unsigned long)code->send),
                 snprintf(factory_arg, sizeof(factory_arg), "%lu", (unsigned long)factory)};
  if (sizes[0] < 0 || (size_t)sizes[0] >= sizeof(receive_arg) || sizes[1] < 0 || (size_t)sizes[1] >= sizeof(code_arg) ||
      sizes[2] < 0 || (size_t)sizes[2] >= sizeof(factory_arg))
    goto fail;
  // This child accepts explicitly transferred file objects, requests code
  // approval and spawns domains; it cannot discover unrelated files or mint.
  const struct moss_fork_capability handles[] = {{endpoints.receive, MOSS_CAP_RECEIVE, 0},
                                                 {(unsigned long)code->send, MOSS_CAP_SEND, 0},
                                                 {(unsigned long)factory, MOSS_CAP_DOMAIN_SPAWN, 0}};
  long domain = 0;
  pid_t child = fork_domain(&domain, handles, sizeof(handles) / sizeof(handles[0]));
  if (child == 0) {
    char *const argv[] = {"loader-service", receive_arg, code_arg, factory_arg, NULL};
    execve("/loader-service.elf", argv, NULL);
    _exit(127);
  }
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.receive);
  if (child < 0) {
    stop_child(child, domain);
    (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
    return -1;
  }
  long send = syscall2(SYS_CAP_DUPLICATE, (long)endpoints.send, MOSS_CAP_SEND);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
  if (send <= 0) {
    stop_child(child, domain);
    return -1;
  }
  *service = (struct Service){.pid = child, .send = send, .domain = domain};
  return 0;
fail:
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.receive);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
  return -1;
}

static int request_loader(long send, long image, const char *program, unsigned char expected, long *domain) {
  struct moss_ipc_message request = {.size = MOSS_LOADER_RUN_HEADER_BYTES,
                                     .capability = (unsigned long)image,
                                     .rights = MOSS_CAP_SEND,
                                     .payload = {MOSS_LOADER_RUN, 2, 1}};
  const char *const strings[] = {program, "from-supervisor", "MOSS_LOADER=ready"};
  for (size_t index = 0; index < sizeof(strings) / sizeof(strings[0]); ++index) {
    size_t length = strlen(strings[index]) + 1;
    if (length > sizeof(request.payload) - request.size)
      return -1;
    memcpy(request.payload + request.size, strings[index], length);
    request.size += length;
  }
  struct moss_ipc_message response = {0};
  long sent = call_service(send, &request, &response);
  if (sent == 1 && response.size == 1 && response.payload[0] == expected &&
      ((expected == MOSS_LOADER_OK && response.capability &&
        response.rights == (MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT | MOSS_CAP_DOMAIN_TERMINATE |
                            MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE)) ||
       (expected != MOSS_LOADER_OK && !response.capability && !response.rights))) {
    *domain = (long)response.capability;
    return 0;
  }
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  return -1;
}

static int run_loader_probe(long send, long image, const char *program, int expected_exit) {
  long domain = 0;
  if (request_loader(send, image, program, MOSS_LOADER_OK, &domain) != 0)
    return -1;
  long waited;
  do {
    waited = syscall1(SYS_DOMAIN_WAIT, domain);
  } while (waited == -EINTR);
  struct moss_domain_exit status = {0};
  int valid = waited == 0 && syscall2(SYS_DOMAIN_STATUS, domain, (long)&status) == 0 && status.code == expected_exit &&
              status.signal == 0;
  if (!valid) {
    (void)syscall1(SYS_DOMAIN_TERMINATE, domain);
    (void)syscall1(SYS_DOMAIN_WAIT, domain);
  }
  (void)syscall1(SYS_CAP_CLOSE, domain);
  return valid ? 0 : -1;
}

static int write_file_prefix(long file, const unsigned char *bytes, unsigned int count, unsigned char expected) {
  long memory = syscall1(SYS_MEM_CREATE, MOSS_MEM_OBJECT_BYTES);
  if (memory <= 0) {
    return -1;
  }
  long mapped = syscall2(SYS_MEM_MAP, memory, MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE);
  if (mapped <= 0) {
    (void)syscall1(SYS_CAP_CLOSE, memory);
    return -1;
  }
  memcpy((void *)mapped, bytes, count);
  struct moss_ipc_message request = {.size = MOSS_FILE_IO_HEADER_BYTES,
                                     .capability = (unsigned long)memory,
                                     .rights = MOSS_CAP_MAP_READ,
                                     .payload = {MOSS_FILE_WRITE}};
  moss_file_put_u64(request.payload + 1, 0);
  moss_file_put_u16(request.payload + 9, count);
  struct moss_ipc_message response = {0};
  long sent = call_service(file, &request, &response);
  int valid = sent == (expected == MOSS_FILE_OK ? MOSS_FILE_IO_REPLY_BYTES : 1) &&
              response.size == (expected == MOSS_FILE_OK ? MOSS_FILE_IO_REPLY_BYTES : 1) &&
              response.payload[0] == expected && !response.capability && !response.rights &&
              (expected != MOSS_FILE_OK || moss_file_get_u16(response.payload + 1) == count);
  if (response.capability) {
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  }
  if (syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES) != 0) {
    _exit(1); // Init cannot safely retry with an accumulating leaked mapping.
  }
  (void)syscall1(SYS_CAP_CLOSE, memory);
  return valid ? 0 : -1;
}

static int prepare_named_snapshot(long file_root, struct LoaderImages *images) {
  static const char name[] = "loader-named-probe";
  struct moss_ipc_message request = {.size = sizeof(name) + 2, .payload = {MOSS_FILE_OPEN}};
  memcpy(request.payload + 2, name, sizeof(name));
  struct moss_ipc_message response = {0};
  long sent = call_service(file_root, &request, &response);
  long named = (long)response.capability;
  int valid = sent == 1 && response.size == 1 && response.payload[0] == MOSS_FILE_OK && named > 0 &&
              response.rights == (MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE);
  long snapshot = 0;
  if (valid) {
    request = (struct moss_ipc_message){.size = 1, .payload = {MOSS_FILE_SNAPSHOT}};
    response = (struct moss_ipc_message){0};
    sent = call_service(named, &request, &response);
    valid = sent == 1 && response.size == 1 && response.payload[0] == MOSS_FILE_BAD_REQUEST && !response.capability &&
            !response.rights;
    if (response.capability) {
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    }
  }
  if (valid) {
    request = (struct moss_ipc_message){.size = sizeof(name) + 2, .payload = {MOSS_FILE_SNAPSHOT, 0}};
    memcpy(request.payload + 2, name, sizeof(name));
    response = (struct moss_ipc_message){0};
    sent = call_service(file_root, &request, &response);
    snapshot = (long)response.capability;
    valid = sent == 1 && response.size == 1 && response.payload[0] == MOSS_FILE_OK && snapshot > 0 &&
            response.rights == (MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE);
  }
  if (valid) {
    // ponytail: keep one clone per File Service incarnation until object
    // release exists; a clone per Loader restart would exhaust 16 slots.
    (void)syscall1(SYS_CAP_CLOSE, images->named_source);
    images->named_source = named;
    images->named_snapshot = snapshot;
    return 0;
  }
  if (snapshot > 0) {
    (void)syscall1(SYS_CAP_CLOSE, snapshot);
  }
  if (named > 0) {
    (void)syscall1(SYS_CAP_CLOSE, named);
  }
  return -1;
}

static int verify_named_snapshot(long named, long snapshot, long loader) {
  static const unsigned char bad_magic[] = {'B', 'A', 'D', '!'};
  static const unsigned char elf_magic[] = {0x7f, 'E', 'L', 'F'};
  int sealed = write_file_prefix(snapshot, bad_magic, sizeof(bad_magic), MOSS_FILE_BAD_REQUEST) == 0;
  int changed = sealed && write_file_prefix(named, bad_magic, sizeof(bad_magic), MOSS_FILE_OK) == 0;
  // The probe validates argv[0] as part of the Loader startup ABI.
  int ran = changed && run_loader_probe(loader, snapshot, "loader-probe", MOSS_LOADER_PROBE_EXIT_CODE) == 0;
  // The named source stays writable for later opens and Loader restarts.
  int restored = !changed || write_file_prefix(named, elf_magic, sizeof(elf_magic), MOSS_FILE_OK) == 0;
  return sealed && changed && ran && restored ? 0 : -1;
}

static int launch_loader_service(const struct Service *code, const struct LoaderImages *images, long file_root,
                                 long factory, struct Service *loader) {
  if (start_loader_service(code, factory, loader) != 0)
    return -1;
  struct moss_ipc_message open = {.size = sizeof("scratch") + 2, .payload = {MOSS_FILE_OPEN}};
  memcpy(open.payload + 2, "scratch", sizeof("scratch"));
  struct moss_ipc_message opened = {0};
  long result = call_service(file_root, &open, &opened);
  long named = (long)opened.capability;
  int named_valid = result == 1 && opened.size == 1 && opened.payload[0] == MOSS_FILE_OK && named > 0 &&
                    opened.rights == (MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE);
  long domain = 0;
  // A public file is readable, but its readers must not be able to seal it.
  // This request checks that Loader rejects it before reading any content.
  named_valid = named_valid && request_loader(loader->send, named, "loader-named", MOSS_LOADER_NO_IMAGE, &domain) == 0;
  if (named > 0) {
    (void)syscall1(SYS_CAP_CLOSE, named);
  }
  struct moss_ipc_message malformed = {.size = MOSS_LOADER_RUN_HEADER_BYTES,
                                       .capability = (unsigned long)images->probe,
                                       .rights = MOSS_CAP_SEND,
                                       .payload = {MOSS_LOADER_RUN, 1, 0}};
  struct moss_ipc_message rejected = {0};
  long sent = call_service(loader->send, &malformed, &rejected);
  int valid = named_valid && sent == 1 && rejected.size == 1 && rejected.payload[0] == MOSS_LOADER_BAD_REQUEST &&
              !rejected.capability && !rejected.rights;
  if (rejected.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)rejected.capability);
  valid =
      valid && request_loader(loader->send, images->bad, "loader-bad", MOSS_LOADER_BAD_IMAGE, &domain) == 0 &&
      run_loader_probe(loader->send, images->probe, "loader-probe", MOSS_LOADER_PROBE_EXIT_CODE) == 0 &&
      run_loader_probe(loader->send, images->libc_probe, "loader-libc-probe", MOSS_LOADER_LIBC_PROBE_EXIT_CODE) == 0 &&
      verify_named_snapshot(images->named_source, images->named_snapshot, loader->send) == 0;
  if (!valid) {
    stop_service(loader);
    return -1;
  }
  report_started("loader service", loader->pid);
  return 0;
}

static int verify_loader_process_bridge(long loader, long image, long process) {
  long domain = 0;
  if (request_loader(loader, image, "loader-probe", MOSS_LOADER_OK, &domain) != 0)
    return -1;

  struct moss_ipc_message request = {.size = 1,
                                     .capability = (unsigned long)domain,
                                     .rights = MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT,
                                     .payload = {MOSS_PROCESS_REGISTER}};
  struct moss_ipc_message response = {0};
  long received = call_service(process, &request, &response);
  long session = (long)response.capability;
  uint64_t identity = received == MOSS_PROCESS_REPLY_VALUE_BYTES ? moss_process_get_u64(response.payload + 1) : 0;
  int valid = received == MOSS_PROCESS_REPLY_VALUE_BYTES && response.size == MOSS_PROCESS_REPLY_VALUE_BYTES &&
              response.payload[0] == MOSS_PROCESS_OK && session > 0 && identity != 0 &&
              response.rights == (MOSS_CAP_SEND | MOSS_CAP_DUPLICATE);

  if (valid) {
    request = (struct moss_ipc_message){.size = 1, .payload = {MOSS_PROCESS_IDENTITY}};
    response = (struct moss_ipc_message){0};
    received = call_service(session, &request, &response);
    valid = received == MOSS_PROCESS_REPLY_IDENTITY_BYTES && response.size == MOSS_PROCESS_REPLY_IDENTITY_BYTES &&
            response.payload[0] == MOSS_PROCESS_OK && moss_process_get_u64(response.payload + 1) == identity &&
            moss_process_get_u64(response.payload + 9) == 0 && !response.capability && !response.rights;
  }
  if (valid) {
    long waited;
    do {
      waited = syscall1(SYS_DOMAIN_WAIT, domain);
    } while (waited == -EINTR);
    valid = waited == 0;
  }
  if (valid) {
    request.payload[0] = MOSS_PROCESS_STATUS;
    response = (struct moss_ipc_message){0};
    received = call_service(session, &request, &response);
    valid = received == MOSS_PROCESS_REPLY_VALUE_BYTES && response.size == MOSS_PROCESS_REPLY_VALUE_BYTES &&
            response.payload[0] == MOSS_PROCESS_EXITED &&
            moss_process_get_u64(response.payload + 1) == MOSS_LOADER_PROBE_EXIT_CODE && !response.capability &&
            !response.rights;
  }
  if (valid) {
    request.payload[0] = MOSS_PROCESS_RELEASE;
    response = (struct moss_ipc_message){0};
    received = call_service(session, &request, &response);
    valid = received == 1 && response.size == 1 && response.payload[0] == MOSS_PROCESS_OK && !response.capability &&
            !response.rights;
  }
  if (response.capability && response.capability != (unsigned long)session)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  if (session > 0)
    (void)syscall1(SYS_CAP_CLOSE, session);
  if (!valid) {
    (void)syscall1(SYS_DOMAIN_TERMINATE, domain);
    (void)syscall1(SYS_DOMAIN_WAIT, domain);
  }
  (void)syscall1(SYS_CAP_CLOSE, domain);
  if (valid)
    report(STDOUT_FILENO, "moss-init: loader process bridge ready\n");
  return valid ? 0 : -1;
}

static int start_namespace_service(const struct Service *file, struct Service *service) {
  struct moss_ipc_endpoints endpoints = {0};
  if (syscall1(SYS_IPC_CREATE, (long)&endpoints) != 0) {
    return -1;
  }
  long receive = 0;
  // Selecting a child capability needs DUPLICATE. The namespace can ask the file
  // service to open an object, but cannot mint or transfer this root sender.
  long file_cap = syscall2(SYS_CAP_DUPLICATE, file->send, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE);
  if (file_cap <= 0) {
    goto fail;
  }
  receive = syscall2(SYS_CAP_DUPLICATE, (long)endpoints.receive, MOSS_CAP_RECEIVE | MOSS_CAP_DUPLICATE);
  if (receive <= 0) {
    goto fail;
  }
  char receive_arg[32], file_arg[32]; // Each holds a decimal 64-bit handle.
  int receive_size = snprintf(receive_arg, sizeof(receive_arg), "%lu", (unsigned long)receive);
  int file_size = snprintf(file_arg, sizeof(file_arg), "%lu", (unsigned long)file_cap);
  if (receive_size < 0 || (size_t)receive_size >= sizeof(receive_arg) || file_size < 0 ||
      (size_t)file_size >= sizeof(file_arg)) {
    goto fail;
  }

  // The selected child handles keep their numbers for argv. The parent closes
  // its temporary file handle before it creates a shell.
  long domain = 0;
  const struct moss_fork_capability handles[] = {{(unsigned long)receive, MOSS_CAP_RECEIVE, 0},
                                                 {(unsigned long)file_cap, MOSS_CAP_SEND, 0}};
  pid_t child = fork_domain(&domain, handles, sizeof(handles) / sizeof(handles[0]));
  if (child == 0) {
    char *const argv[] = {"namespace-service", receive_arg, file_arg, NULL};
    execve("/namespace-service.elf", argv, NULL);
    _exit(127);
  }
  (void)syscall1(SYS_CAP_CLOSE, file_cap);
  (void)syscall1(SYS_CAP_CLOSE, receive);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.receive);
  if (child < 0) {
    stop_child(child, domain);
    (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
    return -1;
  }

  long send = syscall2(SYS_CAP_DUPLICATE, (long)endpoints.send, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
  if (send <= 0) {
    if (send > 0) {
      (void)syscall1(SYS_CAP_CLOSE, send);
    }
    stop_child(child, domain);
    return -1;
  }
  service->pid = child;
  service->send = send;
  service->domain = domain;
  return 0;

fail:
  if (file_cap > 0) {
    (void)syscall1(SYS_CAP_CLOSE, file_cap);
  }
  if (receive > 0) {
    (void)syscall1(SYS_CAP_CLOSE, receive);
  }
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.receive);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
  return -1;
}

static pid_t start_shell(long namespace_capability, long process_capability, long file_domain, long namespace_domain,
                         long process_domain, long code_domain, long loader_domain, long supervisor_domain,
                         long *domain) {
  *domain = 0;
  if (namespace_capability <= 0 || process_capability <= 0 || file_domain <= 0 || namespace_domain <= 0 ||
      process_domain <= 0 || code_domain <= 0 || loader_domain <= 0 || supervisor_domain <= 0)
    return -1;
  char namespace_env[64]; // Environment key plus decimal 64-bit handle.
  char process_env[64], file_domain_env[64], namespace_domain_env[64], process_domain_env[64], code_domain_env[64],
      loader_domain_env[64], supervisor_domain_env[64];
  int size =
      snprintf(namespace_env, sizeof(namespace_env), "MOSS_NAMESPACE_CAP=%lu", (unsigned long)namespace_capability);
  if (size < 0 || (size_t)size >= sizeof(namespace_env)) {
    return -1;
  }
  size = snprintf(process_env, sizeof(process_env), "MOSS_PROCESS_CAP=%lu", (unsigned long)process_capability);
  if (size < 0 || (size_t)size >= sizeof(process_env))
    return -1;
  size = snprintf(file_domain_env, sizeof(file_domain_env), "MOSS_FILE_DOMAIN_CAP=%lu", (unsigned long)file_domain);
  if (size < 0 || (size_t)size >= sizeof(file_domain_env))
    return -1;
  size = snprintf(namespace_domain_env, sizeof(namespace_domain_env), "MOSS_NAMESPACE_DOMAIN_CAP=%lu",
                  (unsigned long)namespace_domain);
  if (size < 0 || (size_t)size >= sizeof(namespace_domain_env))
    return -1;
  size = snprintf(process_domain_env, sizeof(process_domain_env), "MOSS_PROCESS_DOMAIN_CAP=%lu",
                  (unsigned long)process_domain);
  if (size < 0 || (size_t)size >= sizeof(process_domain_env))
    return -1;
  size = snprintf(code_domain_env, sizeof(code_domain_env), "MOSS_CODE_DOMAIN_CAP=%lu", (unsigned long)code_domain);
  if (size < 0 || (size_t)size >= sizeof(code_domain_env))
    return -1;
  size = snprintf(loader_domain_env, sizeof(loader_domain_env), "MOSS_LOADER_DOMAIN_CAP=%lu",
                  (unsigned long)loader_domain);
  if (size < 0 || (size_t)size >= sizeof(loader_domain_env))
    return -1;
  size = snprintf(supervisor_domain_env, sizeof(supervisor_domain_env), "MOSS_SUPERVISOR_DOMAIN_CAP=%lu",
                  (unsigned long)supervisor_domain);
  if (size < 0 || (size_t)size >= sizeof(supervisor_domain_env))
    return -1;
  // The privileged management shell receives attenuated termination rights.
  // Commands need INHERIT because ash forks before executing them; stable
  // handle numbers let their environment refer to the same local authority.
  const struct moss_fork_capability handles[] = {
      {(unsigned long)namespace_capability, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)process_capability, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)file_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)namespace_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)process_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)code_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)loader_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)supervisor_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT}};
  pid_t child = fork_domain(domain, handles, sizeof(handles) / sizeof(handles[0]));
  if (child == 0) {
    char *const argv[] = {"ash", "-i", NULL};
    char *const env[] = {"PATH=/",
                         "HOME=/",
                         "TERM=dumb",
                         "PS1=moss$ ",
                         "PS2=> ",
                         namespace_env,
                         process_env,
                         file_domain_env,
                         namespace_domain_env,
                         process_domain_env,
                         code_domain_env,
                         loader_domain_env,
                         supervisor_domain_env,
                         NULL};
    execve("/busybox.elf", argv, env);
    _exit(127);
  }
  return child;
}

static enum ServiceLoss supervise(struct Service *file, struct Service *namespace, struct Service *process,
                                  struct Service *code, struct Service *loader, long approver, long revoker,
                                  long *code_probe, long factory, const struct LoaderImages *images,
                                  long supervisor_domain, pid_t *shell, long *shell_domain) {
  for (;;) {
    const unsigned long domains[] = {(unsigned long)file->domain,    (unsigned long)namespace->domain,
                                     (unsigned long)process->domain, (unsigned long)code->domain,
                                     (unsigned long)loader->domain,  (unsigned long)*shell_domain};
    long exited = syscall2(SYS_DOMAIN_WAIT_ANY, (long)domains, 6);
    if (exited == -EINTR) {
      continue;
    }
    if (exited < 0 || exited > 5) {
      char message[80];
      int size = snprintf(message, sizeof(message), "moss-init: domain wait failed: %ld\n", exited);
      if (size > 0 && (size_t)size < sizeof(message))
        (void)write(STDERR_FILENO, message, (size_t)size);
      return SUPERVISOR_FAILURE;
    }
    if (exited == 0) {
      file->pid = 0;
      report(STDOUT_FILENO, "moss-init: file service died\n");
      return FILE_LOST;
    }
    if (exited == 1) {
      namespace->pid = 0;
      report(STDOUT_FILENO, "moss-init: namespace service died\n");
      return NAMESPACE_LOST;
    }
    if (exited == 2) {
      process->pid = 0;
      report(STDOUT_FILENO, "moss-init: process service died\n");
      return PROCESS_LOST;
    }
    if (exited == 3) {
      code->pid = 0;
      report(STDOUT_FILENO, "moss-init: code authority service died\n");
      // The loader's selected approval sender names this incarnation only.
      stop_service(loader);
      // The issuer is gone; only the independently held revoker can retire
      // its surviving approval before a replacement receives approval power.
      if (syscall2(SYS_CODE_REVOKE, revoker, *code_probe) != 0)
        return SUPERVISOR_FAILURE;
      (void)syscall1(SYS_CAP_CLOSE, *code_probe);
      *code_probe = 0;
      stop_service(code);
      if (restart_delay() != 0 || launch_code_service(code, approver, code_probe) != 0) {
        report(STDERR_FILENO, "moss-init: code authority service launch failed\n");
        return SUPERVISOR_FAILURE;
      }
      if (launch_loader_service(code, images, file->send, factory, loader) != 0) {
        report(STDERR_FILENO, "moss-init: loader service launch failed\n");
        return SUPERVISOR_FAILURE;
      }
      // The management shell's old termination handle names the dead
      // incarnation. Refresh it without restarting independent services.
      stop_child(*shell, *shell_domain);
      *shell = 0;
      *shell_domain = 0;
      report(STDOUT_FILENO, "moss-init: restarting shell\n");
      *shell = start_shell(namespace->send, process->send, file->domain, namespace->domain, process->domain,
                           code->domain, loader->domain, supervisor_domain, shell_domain);
      if (*shell < 0)
        return SUPERVISOR_FAILURE;
    }
    if (exited == 4) {
      loader->pid = 0;
      report(STDOUT_FILENO, "moss-init: loader service died\n");
      stop_service(loader);
      if (restart_delay() != 0 || launch_loader_service(code, images, file->send, factory, loader) != 0) {
        report(STDERR_FILENO, "moss-init: loader service launch failed\n");
        return SUPERVISOR_FAILURE;
      }
      stop_child(*shell, *shell_domain);
      *shell = 0;
      *shell_domain = 0;
      report(STDOUT_FILENO, "moss-init: restarting shell\n");
      *shell = start_shell(namespace->send, process->send, file->domain, namespace->domain, process->domain,
                           code->domain, loader->domain, supervisor_domain, shell_domain);
      if (*shell < 0)
        return SUPERVISOR_FAILURE;
    }
    if (exited == 5) {
      *shell = 0;
      (void)syscall1(SYS_CAP_CLOSE, *shell_domain);
      *shell_domain = 0;
      report(STDOUT_FILENO, "moss-init: restarting shell\n");
      if (restart_delay() != 0) {
        return SUPERVISOR_FAILURE;
      }
      *shell = start_shell(namespace->send, process->send, file->domain, namespace->domain, process->domain,
                           code->domain, loader->domain, supervisor_domain, shell_domain);
      if (*shell < 0) {
        report(STDERR_FILENO, "moss-init: shell launch failed\n");
        *shell = 0;
        return NAMESPACE_LOST;
      }
    }
  }
}

int main(void) {
  // POSIX shell descendants can be reparented to init. Native supervised
  // domains use capability observation and leave no waitpid zombie.
  struct sigaction action = {.sa_handler = child_exited};
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGCHLD, &action, NULL) != 0) {
    return 1;
  }
  long supervisor_domain = syscall0(SYS_DOMAIN_SELF);
  if (supervisor_domain <= 0) {
    report(STDERR_FILENO, "moss-init: self domain acquisition failed\n");
    return 1;
  }
  report(STDOUT_FILENO, "moss-init: supervisor ready\n");
  long authority = syscall0(SYS_CODE_AUTHORITY);
  long approver =
      authority > 0 ? syscall2(SYS_CAP_DUPLICATE, authority, MOSS_CAP_CODE_APPROVE | MOSS_CAP_DUPLICATE) : -1;
  long revoker = authority > 0 ? syscall2(SYS_CAP_DUPLICATE, authority, MOSS_CAP_CODE_REVOKE) : -1;
  if (authority > 0)
    (void)syscall1(SYS_CAP_CLOSE, authority);
  if (approver <= 0 || revoker <= 0) {
    report(STDERR_FILENO, "moss-init: code authority acquisition failed\n");
    return 1;
  }
  long root_factory = syscall0(SYS_DOMAIN_FACTORY);
  long factory =
      root_factory > 0 ? syscall2(SYS_CAP_DUPLICATE, root_factory, MOSS_CAP_DOMAIN_SPAWN | MOSS_CAP_DUPLICATE) : -1;
  if (root_factory > 0)
    (void)syscall1(SYS_CAP_CLOSE, root_factory);
  if (factory <= 0) {
    report(STDERR_FILENO, "moss-init: domain factory acquisition failed\n");
    return 1;
  }
  struct Service code = {0};
  long code_probe = 0;
  while (launch_code_service(&code, approver, &code_probe) != 0) {
    report(STDERR_FILENO, "moss-init: code authority service launch failed\n");
    if (restart_delay() != 0)
      return 1;
  }
  for (;;) {
    struct Service file = {0};
    if (start_endpoint_service(&file, "/file-service.elf") != 0) {
      report(STDERR_FILENO, "moss-init: file service launch failed\n");
      if (restart_delay() != 0) {
        return 1;
      }
      continue;
    }
    report_started("file service", file.pid);
    // The kernel filesystem is a temporary bootstrap source. Normal native
    // image reads below use capability-addressed File Service objects.
    struct LoaderImages images = {0};
    if (seed_loader_file(file.send, "loader-probe", "/loader_probe.elf", 1, &images.probe) != 0 ||
        seed_loader_file(file.send, "loader-libc-probe", "/loader_libc_probe.elf", 1, &images.libc_probe) != 0 ||
        seed_loader_file(file.send, "loader-bad", NULL, 1, &images.bad) != 0 ||
        seed_loader_file(file.send, "loader-named-probe", "/loader_probe.elf", 0, &images.named_source) != 0 ||
        prepare_named_snapshot(file.send, &images) != 0) {
      report(STDERR_FILENO, "moss-init: loader image seeding failed\n");
      close_loader_images(&images);
      stop_service(&file);
      if (restart_delay() != 0)
        return 1;
      continue;
    }
    for (;;) {
      struct Service namespace = {0};
      if (start_namespace_service(&file, &namespace) != 0) {
        report(STDERR_FILENO, "moss-init: namespace service launch failed\n");
        break;
      }
      report_started("namespace service", namespace.pid);
      struct Service loader = {0};
      if (launch_loader_service(&code, &images, file.send, factory, &loader) != 0) {
        report(STDERR_FILENO, "moss-init: loader service launch failed\n");
        stop_service(&namespace);
        break;
      }
      enum ServiceLoss lost;
      for (;;) {
        struct Service process = {0};
        if (start_endpoint_service(&process, "/process-service.elf") != 0) {
          report(STDERR_FILENO, "moss-init: process service launch failed\n");
          lost = PROCESS_LOST;
        } else {
          report_started("process service", process.pid);
          long shell_domain = 0;
          pid_t shell = 0;
          if (verify_loader_process_bridge(loader.send, images.probe, process.send) != 0) {
            report(STDERR_FILENO, "moss-init: loader process bridge failed\n");
            lost = SUPERVISOR_FAILURE;
          } else {
            shell = start_shell(namespace.send, process.send, file.domain, namespace.domain, process.domain,
                                code.domain, loader.domain, supervisor_domain, &shell_domain);
            if (shell < 0) {
              report(STDERR_FILENO, "moss-init: shell launch failed\n");
              lost = PROCESS_LOST;
            } else {
              lost = supervise(&file, &namespace, &process, &code, &loader, approver, revoker, &code_probe, factory,
                               &images, supervisor_domain, &shell, &shell_domain);
            }
          }
          stop_child(shell, shell_domain);
          stop_service(&process);
        }
        if (lost != PROCESS_LOST)
          break;
        if (restart_delay() != 0) {
          stop_service(&loader);
          stop_service(&namespace);
          close_loader_images(&images);
          stop_service(&file);
          return 1;
        }
      }
      stop_service(&loader);
      stop_service(&namespace);
      if (lost == SUPERVISOR_FAILURE) {
        close_loader_images(&images);
        stop_service(&file);
        return 1;
      }
      if (lost == FILE_LOST) {
        break;
      }
      // Namespace policy can restart while the independent file object and
      // capabilities already transferred to clients remain valid.
      if (restart_delay() != 0) {
        close_loader_images(&images);
        stop_service(&file);
        return 1;
      }
    }
    close_loader_images(&images);
    stop_service(&file);
    if (restart_delay() != 0) {
      return 1;
    }
  }
}
