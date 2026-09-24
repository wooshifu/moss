#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "moss_process_protocol.h"
#include "syscall.h"

// One second between launches bounds a failing service's restart rate.
#define RESTART_DELAY_NS 1000000000UL

struct Service {
  pid_t pid;
  long send;
  long domain;
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
  static const unsigned long call_timeout_ns = 5000000000UL;
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
      moss_process_get_u64(response.payload + 1) == 1 && response.capability &&
      response.rights == (MOSS_CAP_SEND | MOSS_CAP_DUPLICATE))
    return (long)response.capability;
  if (response.capability)
    (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
  return 0;
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

static int process_child_ready(long session) {
  // ATTACH races the child's first instruction; bounded polling lets a failed
  // supervisor launch exit instead of running with an unattached identity.
  enum { READY_RETRIES = 500, READY_DELAY_NS = 10000000UL };
  for (unsigned int retry = 0; retry < READY_RETRIES; ++retry) {
    struct moss_ipc_message request = {.size = 1, .payload = {MOSS_PROCESS_READY}};
    struct moss_ipc_message response = {0};
    long result = process_call(session, &request, &response);
    if (response.capability)
      (void)syscall1(SYS_CAP_CLOSE, (long)response.capability);
    if (result != 1 || response.capability || response.rights)
      return 0;
    if (response.payload[0] == MOSS_PROCESS_OK)
      return 1;
    if (response.payload[0] != MOSS_PROCESS_RUNNING)
      return 0;
    unsigned long delay = READY_DELAY_NS;
    if (syscall1(SYS_NANOSLEEP, (long)&delay) != 0)
      return 0;
  }
  return 0;
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

static pid_t start_shell(long namespace_capability, long process_capability, long parent_session, long file_domain,
                         long namespace_domain, long process_domain, long supervisor_domain, long *domain,
                         unsigned long *shell_id) {
  *domain = 0;
  *shell_id = 0;
  if (namespace_capability <= 0 || process_capability <= 0 || parent_session <= 0 || file_domain <= 0 ||
      namespace_domain <= 0 || process_domain <= 0 || supervisor_domain <= 0)
    return -1;
  char namespace_env[64]; // Environment key plus decimal 64-bit handle.
  char process_env[64], file_domain_env[64], namespace_domain_env[64], process_domain_env[64],
      supervisor_domain_env[64];
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
  const struct moss_fork_capability handles[] = {
      {(unsigned long)namespace_capability, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)process_capability, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)file_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)namespace_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)process_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)supervisor_domain, MOSS_CAP_DOMAIN_TERMINATE | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT},
      {(unsigned long)child_session, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE, MOSS_FORK_CAP_INHERIT}};
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
                         supervisor_domain_env,
                         NULL};
    if (!process_child_ready(child_session))
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
  (void)syscall1(SYS_CAP_CLOSE, child_session);
  return child;
}

static enum ServiceLoss supervise(struct Service *file, struct Service *namespace, struct Service *process,
                                  long process_session, long supervisor_domain, pid_t *shell, long *shell_domain,
                                  unsigned long *shell_id) {
  for (;;) {
    const unsigned long domains[] = {(unsigned long)file->domain, (unsigned long)namespace->domain,
                                     (unsigned long)process->domain, (unsigned long)*shell_domain};
    long exited = syscall2(SYS_DOMAIN_WAIT_ANY, (long)domains, 4);
    if (exited == -EINTR) {
      continue;
    }
    if (exited < 0 || exited > 3) {
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
      *shell = 0;
      (void)syscall1(SYS_CAP_CLOSE, *shell_domain);
      *shell_domain = 0;
      if (!reap_shell(process_session, *shell_id))
        return PROCESS_LOST;
      *shell_id = 0;
      report(STDOUT_FILENO, "moss-init: restarting shell\n");
      if (restart_delay() != 0) {
        return SUPERVISOR_FAILURE;
      }
      *shell = start_shell(namespace->send, process->send, process_session, file->domain, namespace->domain,
                           process->domain, supervisor_domain, shell_domain, shell_id);
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
    for (;;) {
      struct Service namespace = {0};
      if (start_namespace_service(&file, &namespace) != 0) {
        report(STDERR_FILENO, "moss-init: namespace service launch failed\n");
        break;
      }
      report_started("namespace service", namespace.pid);
      enum ServiceLoss lost;
      for (;;) {
        struct Service process = {0};
        if (start_endpoint_service(&process, "/process-service.elf") != 0) {
          report(STDERR_FILENO, "moss-init: process service launch failed\n");
          lost = PROCESS_LOST;
        } else {
          report_started("process service", process.pid);
          long process_session = register_supervisor(process.send);
          long shell_domain = 0;
          unsigned long shell_id = 0;
          pid_t shell = process_session > 0
                            ? start_shell(namespace.send, process.send, process_session, file.domain, namespace.domain,
                                          process.domain, supervisor_domain, &shell_domain, &shell_id)
                            : -1;
          if (shell < 0) {
            report(STDERR_FILENO, "moss-init: shell launch failed\n");
            lost = PROCESS_LOST;
          } else {
            lost = supervise(&file, &namespace, &process, process_session, supervisor_domain, &shell, &shell_domain,
                             &shell_id);
          }
          stop_child(shell, shell_domain);
          if (process_session > 0)
            (void)syscall1(SYS_CAP_CLOSE, process_session);
          stop_service(&process);
        }
        if (lost != PROCESS_LOST)
          break;
        if (restart_delay() != 0) {
          stop_service(&namespace);
          stop_service(&file);
          return 1;
        }
      }
      stop_service(&namespace);
      if (lost == SUPERVISOR_FAILURE) {
        stop_service(&file);
        return 1;
      }
      if (lost == FILE_LOST) {
        break;
      }
      // Namespace policy can restart while the independent file object and
      // capabilities already transferred to clients remain valid.
      if (restart_delay() != 0) {
        stop_service(&file);
        return 1;
      }
    }
    stop_service(&file);
    if (restart_delay() != 0) {
      return 1;
    }
  }
}
