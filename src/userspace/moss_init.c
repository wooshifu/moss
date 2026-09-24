#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_code_authority_protocol.h"
#include "moss_console_protocol.h"
#include "moss_file_protocol.h"
#include "moss_loader_protocol.h"
#include "moss_process_protocol.h"
#include "syscall.h"

// One second between launches bounds a failing service's restart rate.
#define RESTART_DELAY_NS 1000000000UL
// A failed scope drain cannot be followed by a new compatibility namespace.
#define SCOPE_SHUTDOWN_TIMEOUT_NS 5000000000UL
#define SCOPE_SHUTDOWN_POLL_NS 10000000UL
// Match the native IPC validation bound so a stalled service cannot hold boot indefinitely.
#define CODE_CALL_TIMEOUT_NS 5000000000UL

struct Service {
  pid_t pid;
  long send;
  long domain;
};

struct LoaderImages {
  long probe;
  long bad;
};

enum ServiceLoss { FILE_LOST, NAMESPACE_LOST, PROCESS_LOST, PIPE_LOST, CONSOLE_LOST, SUPERVISOR_FAILURE };

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

static pid_t fork_domain(long *domain, const struct moss_fork_capability *handles, size_t count, long scope) {
  *domain = 0;
  if (scope > 0)
    return (pid_t)syscall6(SYS_FORK_DOMAIN_SCOPED, (long)domain, (long)handles, (long)count, scope, 0, 0);
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
  if (images->bad > 0)
    (void)syscall1(SYS_CAP_CLOSE, images->bad);
  *images = (struct LoaderImages){0};
}

static int retire_scope(long scope) {
  if (syscall1(SYS_DOMAIN_SCOPE_TERMINATE, scope) != 0)
    return -1;
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) != 0 || now > LONG_MAX - SCOPE_SHUTDOWN_TIMEOUT_NS)
    return -1;
  unsigned long deadline = now + SCOPE_SHUTDOWN_TIMEOUT_NS;
  for (;;) {
    long live = syscall1(SYS_DOMAIN_SCOPE_STATUS, scope);
    if (live == 0)
      return 0;
    if (live < 0 || syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) != 0 || now >= deadline)
      return -1;
    unsigned long delay = SCOPE_SHUTDOWN_POLL_NS;
    (void)syscall1(SYS_NANOSLEEP, (long)&delay);
  }
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

static long process_call(long session, const struct moss_ipc_message *request, struct moss_ipc_message *response) {
  // A dead or wedged registry must not stall init's recovery loop forever.
  static const unsigned long call_timeout_ns = MOSS_PROCESS_RESERVATION_TIMEOUT_NS;
  unsigned long now = 0;
  if (syscall2(SYS_CLOCK_GETTIME, MOSS_CLOCK_MONOTONIC, (long)&now) != 0 || now > LONG_MAX - call_timeout_ns)
    return -1;
  return syscall6(SYS_IPC_CALL, session, (long)request, (long)response, (long)(now + call_timeout_ns), 0, 0);
}

static int process_reply_ok(long result, struct moss_ipc_message *response) {
  if (response->capability) {
    (void)syscall1(SYS_CAP_CLOSE, (long)response->capability);
    return 0;
  }
  return result == 1 && response->rights == 0 && response->payload[0] == MOSS_PROCESS_OK;
}

static long register_supervisor(long root) {
  long self = syscall0(SYS_DOMAIN_SELF);
  if (self <= 0)
    return 0;
  struct moss_ipc_message request = {.size = 1,
                                     .capability = (unsigned long)self,
                                     .rights =
                                         MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT | MOSS_CAP_DOMAIN_SIGNAL,
                                     .payload = {MOSS_PROCESS_REGISTER}};
  struct moss_ipc_message response = {0};
  long result = process_call(root, &request, &response);
  (void)syscall1(SYS_CAP_CLOSE, self);
  if (result == MOSS_PROCESS_REPLY_VALUE_BYTES && response.payload[0] == MOSS_PROCESS_OK &&
      moss_process_get_u64(response.payload + 1) == MOSS_PROCESS_INIT_ID && response.capability &&
      response.rights == (MOSS_CAP_SEND | MOSS_CAP_DUPLICATE))
    return (long)response.capability;
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  return 0;
}

static int console_input_sender(long root, long *input) {
  *input = 0;
  struct moss_ipc_message request = {.size = 1, .payload = {MOSS_CONSOLE_INPUT_CAP}};
  struct moss_ipc_message response = {0};
  long result = process_call(root, &request, &response);
  if (result == 1 && response.payload[0] == MOSS_CONSOLE_OK && response.capability &&
      response.rights == (MOSS_CAP_SEND | MOSS_CAP_DUPLICATE)) {
    *input = (long)response.capability;
    return 1;
  }
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  return 0;
}

static int process_rejects_unscoped_child(long session, long domain) {
  // A leaked parent badge must not register a domain outside this epoch's
  // cleanup scope. The supervisor itself is deliberately unscoped.
  struct moss_ipc_message request = {.size = 1,
                                     .capability = (unsigned long)domain,
                                     .rights =
                                         MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT | MOSS_CAP_DOMAIN_SIGNAL,
                                     .payload = {MOSS_PROCESS_REGISTER_CHILD}};
  struct moss_ipc_message response = {0};
  long result = process_call(session, &request, &response);
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  return result == 1 && response.payload[0] == MOSS_PROCESS_BAD_REQUEST && !response.capability && !response.rights;
}

static int stale_process_session_closed(long session) {
  struct moss_ipc_message request = {.size = 1, .payload = {MOSS_PROCESS_STATUS}};
  struct moss_ipc_message response = {0};
  long result = process_call(session, &request, &response);
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  // An old badge names its original endpoint; it cannot join a new registry.
  return result == -EPIPE && !response.capability;
}

static int process_child_request(long parent_session, unsigned char operation, unsigned long child_id,
                                 unsigned long domain) {
  struct moss_ipc_message request = {
      .size = MOSS_PROCESS_REPLY_VALUE_BYTES,
      .capability = domain,
      .rights = domain ? MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT | MOSS_CAP_DOMAIN_SIGNAL : 0,
      .payload = {operation}};
  moss_process_put_u64(request.payload + 1, child_id);
  struct moss_ipc_message response = {0};
  return process_reply_ok(process_call(parent_session, &request, &response), &response);
}

static int reap_shell(long parent_session, unsigned long child_id) {
  struct moss_ipc_message request = {.size = MOSS_PROCESS_REPLY_VALUE_BYTES, .payload = {MOSS_PROCESS_WAIT_CHILD}};
  moss_process_put_u64(request.payload + 1, child_id);
  struct moss_ipc_message response = {0};
  long result = process_call(parent_session, &request, &response);
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  return result == MOSS_PROCESS_REPLY_WAIT_BYTES && response.payload[0] == MOSS_PROCESS_EXITED &&
         moss_process_get_u64(response.payload + 1) == child_id && !response.capability && !response.rights;
}

static int start_endpoint_service(struct Service *service, const char *program, long scope, long namespace_capability,
                                  long pipe_capability, long console_capability, long archive_capability,
                                  unsigned long archive_size) {
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
  char receive_arg[32], mint_arg[32], scope_arg[32], namespace_arg[32], pipe_arg[32], console_arg[32],
      archive_arg[32], archive_size_arg[32]; // Decimal 64-bit handles and archive size.
  int receive_size = snprintf(receive_arg, sizeof(receive_arg), "%lu", (unsigned long)receive);
  int mint_size = snprintf(mint_arg, sizeof(mint_arg), "%lu", (unsigned long)mint);
  int scope_size = scope > 0 ? snprintf(scope_arg, sizeof(scope_arg), "%lu", (unsigned long)scope) : 0;
  int namespace_size = namespace_capability > 0
                           ? snprintf(namespace_arg, sizeof(namespace_arg), "%lu", (unsigned long)namespace_capability)
                           : 0;
  int pipe_size = pipe_capability > 0 ? snprintf(pipe_arg, sizeof(pipe_arg), "%lu", (unsigned long)pipe_capability) : 0;
  int console_size =
      console_capability > 0 ? snprintf(console_arg, sizeof(console_arg), "%lu", (unsigned long)console_capability) : 0;
  int archive_arg_size = archive_capability > 0
                             ? snprintf(archive_arg, sizeof(archive_arg), "%lu", (unsigned long)archive_capability)
                             : 0;
  int archive_size_size = archive_capability > 0
                              ? snprintf(archive_size_arg, sizeof(archive_size_arg), "%lu", archive_size)
                              : 0;
  if (receive_size < 0 || (size_t)receive_size >= sizeof(receive_arg) || mint_size < 0 ||
      (size_t)mint_size >= sizeof(mint_arg) || scope_size < 0 || (size_t)scope_size >= sizeof(scope_arg)) {
    goto fail;
  }
  if (namespace_size < 0 || (size_t)namespace_size >= sizeof(namespace_arg) || pipe_size < 0 ||
      (size_t)pipe_size >= sizeof(pipe_arg) || console_size < 0 || (size_t)console_size >= sizeof(console_arg) ||
      (namespace_capability > 0 && scope <= 0) || (pipe_capability > 0 && namespace_capability <= 0) ||
      (console_capability > 0 && pipe_capability <= 0) || archive_arg_size < 0 ||
      (size_t)archive_arg_size >= sizeof(archive_arg) || archive_size_size < 0 ||
      (size_t)archive_size_size >= sizeof(archive_size_arg) ||
      (archive_capability > 0 && (!archive_size || scope > 0 || namespace_capability > 0 || pipe_capability > 0 ||
                                  console_capability > 0)) ||
      (archive_capability <= 0 && archive_size))
    goto fail;

  long domain = 0;
  // Slot three carries either the boot archive or the process scope. The
  // argument checks above keep those service roles mutually exclusive.
  const struct moss_fork_capability handles[] = {
      {(unsigned long)receive, MOSS_CAP_RECEIVE, 0},
      {(unsigned long)mint, MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE | MOSS_CAP_MINT, 0},
      {(unsigned long)(archive_capability > 0 ? archive_capability : scope),
       archive_capability > 0 ? MOSS_CAP_MAP_READ : MOSS_CAP_DOMAIN_SCOPE_INSPECT, 0},
      {(unsigned long)namespace_capability, MOSS_CAP_SEND, 0},
      {(unsigned long)pipe_capability, MOSS_CAP_SEND, 0},
      {(unsigned long)console_capability, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE, 0}};
  size_t handle_count = 2 + (archive_capability > 0) + (scope > 0) + (namespace_capability > 0) +
                        (pipe_capability > 0) + (console_capability > 0);
  pid_t child = fork_domain(&domain, handles, handle_count, 0);
  if (child == 0) {
    char *role_arg = NULL;
    char *next_arg = NULL;
    if (archive_capability > 0) {
      role_arg = archive_arg;
      next_arg = archive_size_arg;
    } else if (scope > 0) {
      role_arg = scope_arg;
      if (namespace_capability > 0)
        next_arg = namespace_arg;
    }
    char *const argv[] = {(char *)program,
                          receive_arg,
                          mint_arg,
                          role_arg,
                          next_arg,
                          pipe_capability > 0 ? pipe_arg : NULL,
                          console_capability > 0 ? console_arg : NULL,
                          NULL};
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
  pid_t child = fork_domain(&domain, handles, sizeof(handles) / sizeof(handles[0]), 0);
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
  // The loader receives only a selected sender for this code authority epoch.
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

static int seed_loader_file(long file, const char *name, const char *source, long *image) {
  *image = 0;
  long source_object = 0;
  if (source) {
    struct moss_ipc_message lookup = {.size = strlen(source) + 3, .payload = {MOSS_FILE_OPEN}};
    memcpy(lookup.payload + 2, source, strlen(source) + 1);
    struct moss_ipc_message found = {0};
    long opened = call_service(file, &lookup, &found);
    if (opened != 1 || found.size != 1 || found.payload[0] != MOSS_FILE_OK || !found.capability ||
        found.rights != (MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE)) {
      if (found.capability)
        (void)syscall1(SYS_CAP_CLOSE, (long)found.capability);
      return -1;
    }
    source_object = (long)found.capability;
  }
  struct moss_ipc_message request = {.size = strlen(name) + 3,
                                     .payload = {MOSS_FILE_OPEN, MOSS_FILE_OPEN_CREATE | MOSS_FILE_OPEN_UNLISTED}};
  memcpy(request.payload + 2, name, strlen(name) + 1);
  struct moss_ipc_message response = {0};
  long sent = call_service(file, &request, &response);
  if (sent != 1 || response.size != 1 || response.payload[0] != MOSS_FILE_OK || !response.capability ||
      response.rights != (MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE)) {
    if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    if (source_object > 0)
      (void)syscall1(SYS_CAP_CLOSE, source_object);
    return -1;
  }
  long object = (long)response.capability;
  long memory = syscall1(SYS_MEM_CREATE, MOSS_MEM_OBJECT_BYTES);
  long mapped = memory > 0 ? syscall2(SYS_MEM_MAP, memory, MOSS_CAP_MAP_READ | MOSS_CAP_MAP_WRITE) : -1;
  int valid = mapped > 0;
  size_t offset = 0;
  while (valid) {
    unsigned int count;
    if (source) {
      request = (struct moss_ipc_message){.size = MOSS_FILE_IO_HEADER_BYTES,
                                          .capability = (unsigned long)memory,
                                          .rights = MOSS_CAP_MAP_WRITE,
                                          .payload = {MOSS_FILE_READ}};
      moss_file_put_u64(request.payload + 1, offset);
      moss_file_put_u16(request.payload + 9, MOSS_MEM_OBJECT_BYTES);
      response = (struct moss_ipc_message){0};
      sent = call_service(source_object, &request, &response);
      count = sent == MOSS_FILE_IO_REPLY_BYTES && response.payload[0] == MOSS_FILE_OK && !response.capability &&
                      !response.rights
                  ? moss_file_get_u16(response.payload + 1)
                  : MOSS_MEM_OBJECT_BYTES + 1;
      if (response.capability)
        (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    } else {
      memcpy((void *)mapped, "BAD", 3);
      count = offset ? 0 : 3;
    }
    if (count > MOSS_MEM_OBJECT_BYTES || count > MOSS_FILE_CONTENT_BUDGET_BYTES - offset) {
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
    moss_file_put_u16(request.payload + 9, count);
    response = (struct moss_ipc_message){0};
    sent = call_service(object, &request, &response);
    if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    if (sent != MOSS_FILE_IO_REPLY_BYTES || response.payload[0] != MOSS_FILE_OK || response.capability ||
        response.rights || moss_file_get_u16(response.payload + 1) != count) {
      valid = 0;
      break;
    }
    offset += (size_t)count;
  }
  if (mapped > 0 && syscall2(SYS_MUNMAP, mapped, MOSS_MEM_OBJECT_BYTES) != 0)
    _exit(1); // Init cannot safely retry with an accumulating leaked mapping.
  if (memory > 0)
    (void)syscall1(SYS_CAP_CLOSE, memory);
  if (source_object > 0)
    (void)syscall1(SYS_CAP_CLOSE, source_object);
  if (valid && offset) {
    // A root sender can create an unlisted object, but must not be able to
    // recover its authority by a later name lookup.
    request = (struct moss_ipc_message){.size = strlen(name) + 3, .payload = {MOSS_FILE_OPEN}};
    memcpy(request.payload + 2, name, strlen(name) + 1);
    response = (struct moss_ipc_message){0};
    sent = call_service(file, &request, &response);
    valid = sent == 1 && response.size == 1 && response.payload[0] == MOSS_FILE_NO_ENTRY && !response.capability &&
            !response.rights;
    if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  }
  if (!valid || !offset) {
    (void)syscall1(SYS_CAP_CLOSE, object);
    return -1;
  }
  *image = object;
  return 0;
}

static int start_loader_service(const struct Service *code, long factory, long scope, struct Service *service) {
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
  pid_t child = fork_domain(&domain, handles, sizeof(handles) / sizeof(handles[0]), scope);
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

static int request_loader(long send, long image, unsigned char expected, long *domain) {
  struct moss_ipc_message request = {.size = MOSS_LOADER_RUN_HEADER_BYTES,
                                     .capability = (unsigned long)image,
                                     .rights = MOSS_CAP_SEND,
                                     .payload = {MOSS_LOADER_RUN, 2, 1}};
  const char *const strings[] = {"loader-probe", "from-supervisor", "MOSS_LOADER=ready"};
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
        response.rights == (MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT | MOSS_CAP_DOMAIN_SIGNAL |
                            MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE)) ||
       (expected != MOSS_LOADER_OK && !response.capability && !response.rights))) {
    *domain = (long)response.capability;
    return 0;
  }
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  return -1;
}

static int launch_loader_service(const struct Service *code, const struct LoaderImages *images, long factory,
                                 struct Service *loader) {
  if (start_loader_service(code, factory, 0, loader) != 0)
    return -1;
  struct moss_ipc_message malformed = {.size = MOSS_LOADER_RUN_HEADER_BYTES,
                                       .capability = (unsigned long)images->probe,
                                       .rights = MOSS_CAP_SEND,
                                       .payload = {MOSS_LOADER_RUN, 1, 0}};
  struct moss_ipc_message rejected = {0};
  long sent = call_service(loader->send, &malformed, &rejected);
  int valid = sent == 1 && rejected.size == 1 && rejected.payload[0] == MOSS_LOADER_BAD_REQUEST &&
              !rejected.capability && !rejected.rights;
  if (rejected.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)rejected.capability);
  long domain = 0;
  valid = valid && request_loader(loader->send, images->bad, MOSS_LOADER_BAD_IMAGE, &domain) == 0 &&
          request_loader(loader->send, images->probe, MOSS_LOADER_OK, &domain) == 0;
  struct moss_domain_exit status = {0};
  if (valid)
    valid = syscall1(SYS_DOMAIN_WAIT, domain) == 0 && syscall2(SYS_DOMAIN_STATUS, domain, (long)&status) == 0 &&
            status.code == MOSS_LOADER_PROBE_EXIT_CODE && status.signal == 0;
  if (domain > 0) {
    if (!valid) {
      (void)syscall1(SYS_DOMAIN_TERMINATE, domain);
      (void)syscall1(SYS_DOMAIN_WAIT, domain);
    }
    (void)syscall1(SYS_CAP_CLOSE, domain);
  }
  if (!valid) {
    stop_service(loader);
    return -1;
  }
  report_started("loader service", loader->pid);
  return 0;
}

static int verify_loader_process_bridge(const struct Service *code, long factory, long image, long parent_session,
                                        long scope) {
  // The compatibility registry accepts children only inside its recovery
  // scope. A scoped Loader also places the domain it spawns in that scope.
  struct Service scoped_loader = {0};
  if (start_loader_service(code, factory, scope, &scoped_loader) != 0)
    return -1;
  long domain = 0;
  long session = 0;
  uint64_t identity = 0;
  int valid = request_loader(scoped_loader.send, image, MOSS_LOADER_OK, &domain) == 0;
  struct moss_ipc_message request = {0}, response = {0};
  long received = 0;
  if (valid) {
    request =
        (struct moss_ipc_message){.size = 1,
                                  .capability = (unsigned long)domain,
                                  .rights = MOSS_CAP_DOMAIN_OBSERVE | MOSS_CAP_DOMAIN_INSPECT | MOSS_CAP_DOMAIN_SIGNAL,
                                  .payload = {MOSS_PROCESS_REGISTER_CHILD}};
    received = call_service(parent_session, &request, &response);
    identity = received == MOSS_PROCESS_REPLY_VALUE_BYTES ? moss_process_get_u64(response.payload + 1) : 0;
    valid = received == MOSS_PROCESS_REPLY_VALUE_BYTES && response.size == MOSS_PROCESS_REPLY_VALUE_BYTES &&
            response.payload[0] == MOSS_PROCESS_OK && response.capability && identity > MOSS_PROCESS_INIT_ID &&
            response.rights == (MOSS_CAP_SEND | MOSS_CAP_DUPLICATE);
    if (valid)
      session = (long)response.capability;
    else if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  }
  if (valid) {
    request = (struct moss_ipc_message){.size = 1, .payload = {MOSS_PROCESS_IDENTITY}};
    response = (struct moss_ipc_message){0};
    received = call_service(session, &request, &response);
    valid = received == MOSS_PROCESS_REPLY_IDENTITY_BYTES && response.size == MOSS_PROCESS_REPLY_IDENTITY_BYTES &&
            response.payload[0] == MOSS_PROCESS_OK && moss_process_get_u64(response.payload + 1) == identity &&
            moss_process_get_u64(response.payload + 9) == MOSS_PROCESS_INIT_ID && !response.capability &&
            !response.rights;
    if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
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
    if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  }
  if (valid) {
    request = (struct moss_ipc_message){.size = MOSS_PROCESS_REPLY_VALUE_BYTES, .payload = {MOSS_PROCESS_WAIT_CHILD}};
    moss_process_put_u64(request.payload + 1, identity);
    response = (struct moss_ipc_message){0};
    received = call_service(parent_session, &request, &response);
    valid = received == MOSS_PROCESS_REPLY_WAIT_BYTES && response.size == MOSS_PROCESS_REPLY_WAIT_BYTES &&
            response.payload[0] == MOSS_PROCESS_EXITED && moss_process_get_u64(response.payload + 1) == identity &&
            moss_process_get_u64(response.payload + 9) == MOSS_LOADER_PROBE_EXIT_CODE && !response.capability &&
            !response.rights;
    if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  }
  if (session > 0)
    (void)syscall1(SYS_CAP_CLOSE, session);
  if (domain > 0) {
    if (!valid) {
      (void)syscall1(SYS_DOMAIN_TERMINATE, domain);
      (void)syscall1(SYS_DOMAIN_WAIT, domain);
    }
    (void)syscall1(SYS_CAP_CLOSE, domain);
  }
  stop_service(&scoped_loader);
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
  pid_t child = fork_domain(&domain, handles, sizeof(handles) / sizeof(handles[0]), 0);
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

static pid_t start_shell(long namespace_capability, long process_capability, long pipe_capability,
                         long console_input_capability, long parent_session, long scope, long file_domain,
                         long namespace_domain, long process_domain, long pipe_domain, long console_domain,
                         long code_domain, long loader_domain, long supervisor_domain, long *domain,
                         unsigned long *shell_id, long *delegated_session) {
  *domain = 0;
  *shell_id = 0;
  *delegated_session = 0;
  if (namespace_capability <= 0 || process_capability <= 0 || pipe_capability <= 0 || console_input_capability <= 0 ||
      parent_session <= 0 || scope <= 0 || file_domain <= 0 || namespace_domain <= 0 || process_domain <= 0 ||
      pipe_domain <= 0 || console_domain <= 0 || code_domain <= 0 || loader_domain <= 0 || supervisor_domain <= 0)
    return -1;
  char namespace_env[64]; // Environment key plus decimal 64-bit handle.
  char process_env[64], pipe_env[64], console_input_env[64], file_domain_env[64], namespace_domain_env[64],
      process_domain_env[64], pipe_domain_env[64], console_domain_env[64], code_domain_env[64], loader_domain_env[64],
      supervisor_domain_env[64];
  int size =
      snprintf(namespace_env, sizeof(namespace_env), "MOSS_NAMESPACE_CAP=%lu", (unsigned long)namespace_capability);
  if (size < 0 || (size_t)size >= sizeof(namespace_env)) {
    return -1;
  }
  size = snprintf(process_env, sizeof(process_env), "MOSS_PROCESS_CAP=%lu", (unsigned long)process_capability);
  if (size < 0 || (size_t)size >= sizeof(process_env))
    return -1;
  size = snprintf(pipe_env, sizeof(pipe_env), "MOSS_PIPE_CAP=%lu", (unsigned long)pipe_capability);
  if (size < 0 || (size_t)size >= sizeof(pipe_env))
    return -1;
  size = snprintf(console_input_env, sizeof(console_input_env), "MOSS_CONSOLE_INPUT_CAP=%lu",
                  (unsigned long)console_input_capability);
  if (size < 0 || (size_t)size >= sizeof(console_input_env))
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
  size = snprintf(pipe_domain_env, sizeof(pipe_domain_env), "MOSS_PIPE_DOMAIN_CAP=%lu", (unsigned long)pipe_domain);
  if (size < 0 || (size_t)size >= sizeof(pipe_domain_env))
    return -1;
  size = snprintf(console_domain_env, sizeof(console_domain_env), "MOSS_CONSOLE_DOMAIN_CAP=%lu",
                  (unsigned long)console_domain);
  if (size < 0 || (size_t)size >= sizeof(console_domain_env))
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
  struct moss_ipc_message prepare = {.size = 1, .payload = {MOSS_PROCESS_PREPARE_CHILD}};
  struct moss_ipc_message reservation = {0};
  long prepared = process_call(parent_session, &prepare, &reservation);
  if (prepared != MOSS_PROCESS_REPLY_VALUE_BYTES || reservation.payload[0] != MOSS_PROCESS_OK ||
      !reservation.capability || reservation.rights != (MOSS_CAP_SEND | MOSS_CAP_DUPLICATE)) {
    if (reservation.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)reservation.capability);
    return -1;
  }
  unsigned long child_id = moss_process_get_u64(reservation.payload + 1);
  long child_session = (long)reservation.capability;
  if (!child_id) {
    (void)syscall1(SYS_CAP_CLOSE, child_session);
    return -1;
  }
  // The privileged management shell receives attenuated termination rights.
  // Commands need INHERIT because ash forks before executing them; stable
  // handle numbers let their environment refer to the same local authority.
  // The pipe root is inherited only for the privileged direct-service probe;
  // managed descriptors use the Process Service session.
  const struct moss_fork_capability handles[] = {
      {(unsigned long)namespace_capability, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)process_capability, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)pipe_capability, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)console_input_capability, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)file_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)namespace_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)process_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)pipe_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)console_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)code_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)loader_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)supervisor_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)child_session, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT}};
  pid_t child = fork_domain(domain, handles, sizeof(handles) / sizeof(handles[0]), scope);
  if (child == 0) {
    char *const argv[] = {"ash", "-i", NULL};
    char *const env[] = {"PATH=/",
                         "HOME=/",
                         "TERM=dumb",
                         "PS1=moss$ ",
                         "PS2=> ",
                         namespace_env,
                         process_env,
                         pipe_env,
                         console_input_env,
                         file_domain_env,
                         namespace_domain_env,
                         process_domain_env,
                         pipe_domain_env,
                         console_domain_env,
                         code_domain_env,
                         loader_domain_env,
                         supervisor_domain_env,
                         NULL};
    long self = syscall0(SYS_DOMAIN_SELF);
    int attached = self > 0 && process_child_request(child_session, MOSS_PROCESS_ATTACH_CHILD, child_id, self);
    if (self > 0)
      (void)syscall1(SYS_CAP_CLOSE, self);
    if (!attached)
      _exit(127);
    (void)syscall6(SYS_EXECVE_CAP, (long)"/busybox.elf", (long)argv, (long)env, child_session, 0, 0);
    _exit(127);
  }
  int attached = child > 0 && *domain > 0 &&
                 process_child_request(parent_session, MOSS_PROCESS_ATTACH_CHILD, child_id, (unsigned long)*domain);
  if (!attached) {
    stop_child(child, *domain);
    *domain = 0;
    (void)process_child_request(parent_session, MOSS_PROCESS_CANCEL_CHILD, child_id, 0);
    child = -1;
  } else {
    *shell_id = child_id;
  }
  if (child > 0) {
    // Retain the session actually delegated to the shell so recovery can
    // verify that its old endpoint cannot reach the replacement service.
    *delegated_session = child_session;
  } else {
    (void)syscall1(SYS_CAP_CLOSE, child_session);
  }
  return child;
}

static enum ServiceLoss supervise(struct Service *file, struct Service *namespace, struct Service *process,
                                  struct Service *pipe, struct Service *console, struct Service *code,
                                  struct Service *loader, long console_input, long process_session, long scope,
                                  long approver, long revoker, long *code_probe, long factory,
                                  const struct LoaderImages *images, long supervisor_domain, pid_t *shell,
                                  long *shell_domain, unsigned long *shell_id, long *shell_session) {
  for (;;) {
    const unsigned long domains[] = {(unsigned long)file->domain,    (unsigned long)namespace->domain,
                                     (unsigned long)process->domain, (unsigned long)*shell_domain,
                                     (unsigned long)pipe->domain,    (unsigned long)console->domain,
                                     (unsigned long)code->domain,    (unsigned long)loader->domain};
    long exited = syscall2(SYS_DOMAIN_WAIT_ANY, (long)domains, 8);
    if (exited == -EINTR)
      continue;
    if (exited < 0 || exited > 7) {
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
    if (exited == 4) {
      pipe->pid = 0;
      report(STDOUT_FILENO, "moss-init: pipe service died\n");
      return PIPE_LOST;
    }
    if (exited == 5) {
      console->pid = 0;
      report(STDOUT_FILENO, "moss-init: console service died\n");
      return CONSOLE_LOST;
    }
    if (exited == 6) {
      code->pid = 0;
      report(STDOUT_FILENO, "moss-init: code authority service died\n");
      // A loader's selected sender names this code-authority incarnation.
      stop_service(loader);
      // The independent revoker retires the surviving approval before a
      // replacement receives approval power.
      if (syscall2(SYS_CODE_REVOKE, revoker, *code_probe) != 0)
        return SUPERVISOR_FAILURE;
      (void)syscall1(SYS_CAP_CLOSE, *code_probe);
      *code_probe = 0;
      stop_service(code);
      if (restart_delay() != 0 || launch_code_service(code, approver, code_probe) != 0) {
        report(STDERR_FILENO, "moss-init: code authority service launch failed\n");
        return SUPERVISOR_FAILURE;
      }
      if (launch_loader_service(code, images, factory, loader) != 0) {
        report(STDERR_FILENO, "moss-init: loader service launch failed\n");
        return SUPERVISOR_FAILURE;
      }
      // Refresh the management shell's old termination handle while keeping
      // the independent process, file, namespace, pipe and console services.
      stop_child(*shell, *shell_domain);
      *shell = 0;
      *shell_domain = 0;
      if (*shell_session > 0)
        (void)syscall1(SYS_CAP_CLOSE, *shell_session);
      *shell_session = 0;
      if (!reap_shell(process_session, *shell_id))
        return PROCESS_LOST;
      *shell_id = 0;
      report(STDOUT_FILENO, "moss-init: restarting shell\n");
      *shell = start_shell(namespace->send, process->send, pipe->send, console_input, process_session, scope,
                           file->domain, namespace->domain, process->domain, pipe->domain, console->domain,
                           code->domain, loader->domain, supervisor_domain, shell_domain, shell_id, shell_session);
      if (*shell < 0)
        return SUPERVISOR_FAILURE;
      continue;
    }
    if (exited == 7) {
      loader->pid = 0;
      report(STDOUT_FILENO, "moss-init: loader service died\n");
      stop_service(loader);
      if (restart_delay() != 0 || launch_loader_service(code, images, factory, loader) != 0) {
        report(STDERR_FILENO, "moss-init: loader service launch failed\n");
        return SUPERVISOR_FAILURE;
      }
      stop_child(*shell, *shell_domain);
      *shell = 0;
      *shell_domain = 0;
      if (*shell_session > 0)
        (void)syscall1(SYS_CAP_CLOSE, *shell_session);
      *shell_session = 0;
      if (!reap_shell(process_session, *shell_id))
        return PROCESS_LOST;
      *shell_id = 0;
      report(STDOUT_FILENO, "moss-init: restarting shell\n");
      *shell = start_shell(namespace->send, process->send, pipe->send, console_input, process_session, scope,
                           file->domain, namespace->domain, process->domain, pipe->domain, console->domain,
                           code->domain, loader->domain, supervisor_domain, shell_domain, shell_id, shell_session);
      if (*shell < 0)
        return SUPERVISOR_FAILURE;
      continue;
    }
    *shell = 0;
    (void)syscall1(SYS_CAP_CLOSE, *shell_domain);
    *shell_domain = 0;
    if (*shell_session > 0)
      (void)syscall1(SYS_CAP_CLOSE, *shell_session);
    *shell_session = 0;
    if (!reap_shell(process_session, *shell_id))
      return PROCESS_LOST;
    *shell_id = 0;
    report(STDOUT_FILENO, "moss-init: restarting shell\n");
    if (restart_delay() != 0)
      return SUPERVISOR_FAILURE;
    *shell = start_shell(namespace->send, process->send, pipe->send, console_input, process_session, scope,
                         file->domain, namespace->domain, process->domain, pipe->domain, console->domain, code->domain,
                         loader->domain, supervisor_domain, shell_domain, shell_id, shell_session);
    if (*shell < 0) {
      report(STDERR_FILENO, "moss-init: shell launch failed\n");
      *shell = 0;
      return NAMESPACE_LOST;
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
  unsigned long archive_size = 0;
  long archive = syscall1(SYS_BOOT_ARCHIVE, (long)&archive_size);
  long writable = archive > 0 ? syscall2(SYS_MEM_MAP, archive, MOSS_CAP_MAP_WRITE) : 0;
  if (archive <= 0 || !archive_size || writable != -EACCES) {
    char message[128];
    int length = snprintf(message, sizeof(message), "moss-init: boot archive failed cap=%ld size=%lu write=%ld\n",
                          archive, archive_size, writable);
    if (length > 0 && (size_t)length < sizeof(message))
      report(STDERR_FILENO, message);
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
    if (start_endpoint_service(&file, "/file-service.elf", 0, 0, 0, 0, archive, archive_size) != 0) {
      report(STDERR_FILENO, "moss-init: file service launch failed\n");
      if (restart_delay() != 0) {
        return 1;
      }
      continue;
    }
    report_started("file service", file.pid);
    // The Loader receives capability-addressed private copies from the
    // read-only boot object, so its image authority survives namespace restarts.
    struct LoaderImages images = {0};
    if (seed_loader_file(file.send, "loader-probe", "loader_probe.elf", &images.probe) != 0 ||
        seed_loader_file(file.send, "loader-bad", NULL, &images.bad) != 0) {
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
      if (launch_loader_service(&code, &images, factory, &loader) != 0) {
        report(STDERR_FILENO, "moss-init: loader service launch failed\n");
        stop_service(&namespace);
        break;
      }
      enum ServiceLoss lost;
      long stale_session = 0;
      long stale_delegate = 0;
      for (;;) {
        struct Service process = {0};
        struct Service pipe = {0};
        struct Service console = {0};
        long scope = syscall0(SYS_DOMAIN_SCOPE_CREATE);
        long process_session = 0;
        long console_input = 0;
        long shell_domain = 0;
        long shell_session = 0;
        unsigned long shell_id = 0;
        pid_t shell = -1;
        if (scope <= 0) {
          report(STDERR_FILENO, "moss-init: process scope creation failed\n");
          lost = PROCESS_LOST;
        } else if (start_endpoint_service(&pipe, "/pipe-service.elf", 0, 0, 0, 0, 0, 0) != 0) {
          report(STDERR_FILENO, "moss-init: pipe service launch failed\n");
          lost = PIPE_LOST;
        } else if (start_endpoint_service(&console, "/console-service.elf", 0, 0, 0, 0, 0, 0) != 0) {
          report(STDERR_FILENO, "moss-init: console service launch failed\n");
          lost = CONSOLE_LOST;
        } else if (!console_input_sender(console.send, &console_input)) {
          report(STDERR_FILENO, "moss-init: console input sender acquisition failed\n");
          lost = CONSOLE_LOST;
        } else if (start_endpoint_service(&process, "/process-service.elf", scope, namespace.send, pipe.send,
                                          console.send, 0, 0) != 0) {
          report(STDERR_FILENO, "moss-init: process service launch failed\n");
          lost = PROCESS_LOST;
        } else {
          report_started("pipe service", pipe.pid);
          report_started("console service", console.pid);
          report_started("process service", process.pid);
          lost = PROCESS_LOST;
          process_session = register_supervisor(process.send);
          if (process_session > 0 && (stale_session > 0 || stale_delegate > 0)) {
            int stale_closed = stale_session <= 0 || stale_process_session_closed(stale_session);
            int delegated_closed = stale_delegate <= 0 || stale_process_session_closed(stale_delegate);
            if (stale_session > 0)
              (void)syscall1(SYS_CAP_CLOSE, stale_session);
            if (stale_delegate > 0)
              (void)syscall1(SYS_CAP_CLOSE, stale_delegate);
            stale_session = 0;
            stale_delegate = 0;
            if (!stale_closed || !delegated_closed) {
              report(STDERR_FILENO, "moss-init: stale process session or delegate remained usable\n");
              _exit(1);
            }
          }
          if (process_session > 0 && process_rejects_unscoped_child(process_session, supervisor_domain)) {
            if (verify_loader_process_bridge(&code, factory, images.probe, process_session, scope) != 0) {
              report(STDERR_FILENO, "moss-init: loader process bridge failed\n");
              lost = SUPERVISOR_FAILURE;
            } else {
              shell =
                  start_shell(namespace.send, process.send, pipe.send, console_input, process_session, scope,
                              file.domain, namespace.domain, process.domain, pipe.domain, console.domain, code.domain,
                              loader.domain, supervisor_domain, &shell_domain, &shell_id, &shell_session);
            }
          }
          if (lost != SUPERVISOR_FAILURE) {
            if (shell < 0) {
              report(STDERR_FILENO, "moss-init: shell launch failed\n");
              lost = PROCESS_LOST;
            } else {
              lost = supervise(&file, &namespace, &process, &pipe, &console, &code, &loader, console_input,
                               process_session, scope, approver, revoker, &code_probe, factory, &images,
                               supervisor_domain, &shell, &shell_domain, &shell_id, &shell_session);
            }
          }
        }
        if (scope > 0) {
          if (retire_scope(scope) != 0) {
            report(STDERR_FILENO, "moss-init: process scope did not drain\n");
            _exit(1);
          }
          (void)syscall1(SYS_CAP_CLOSE, scope);
        }
        stop_child(shell, shell_domain);
        if (lost == PROCESS_LOST || lost == PIPE_LOST || lost == CONSOLE_LOST) {
          if (process_session > 0)
            stale_session = process_session;
          if (shell_session > 0)
            stale_delegate = shell_session;
        } else {
          if (process_session > 0)
            (void)syscall1(SYS_CAP_CLOSE, process_session);
          if (shell_session > 0)
            (void)syscall1(SYS_CAP_CLOSE, shell_session);
        }
        stop_service(&process);
        if (console_input > 0)
          (void)syscall1(SYS_CAP_CLOSE, console_input);
        // These object authorities belong to this Process Service epoch.
        // Drain managed children before discarding the serving domains.
        stop_service(&console);
        stop_service(&pipe);
        if (lost != PROCESS_LOST && lost != PIPE_LOST && lost != CONSOLE_LOST)
          break;
        if (restart_delay() != 0) {
          stop_service(&loader);
          stop_service(&namespace);
          close_loader_images(&images);
          stop_service(&file);
          return 1;
        }
      }
      if (stale_session > 0)
        (void)syscall1(SYS_CAP_CLOSE, stale_session);
      if (stale_delegate > 0)
        (void)syscall1(SYS_CAP_CLOSE, stale_delegate);
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
