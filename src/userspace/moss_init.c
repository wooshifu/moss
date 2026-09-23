#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "syscall.h"

// One second between launches bounds a failing service's restart rate.
#define RESTART_DELAY_NS 1000000000UL

struct Service {
  pid_t pid;
  long send;
  long domain;
};

enum ServiceLoss { FILE_LOST, NAMESPACE_LOST, SUPERVISOR_FAILURE };

static void report(int fd, const char *message) { (void)write(fd, message, strlen(message)); }

static void report_started(const char *name, pid_t pid) {
  char message[80];
  int size = snprintf(message, sizeof(message), "moss-init: %s started pid=%ld\n", name, (long)pid);
  if (size > 0 && (size_t)size < sizeof(message)) {
    (void)write(STDOUT_FILENO, message, (size_t)size);
  }
}

static void wait_for(pid_t child) {
  int status;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
}

static pid_t fork_domain(long *domain) {
  *domain = 0;
  return (pid_t)syscall1(SYS_FORK_DOMAIN, (long)domain);
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
    wait_for(child);
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

static int start_file_service(struct Service *service) {
  struct moss_ipc_endpoints endpoints = {0};
  if (syscall1(SYS_IPC_CREATE, (long)&endpoints) != 0) {
    return -1;
  }
  long mint = 0;
  long receive = syscall2(SYS_CAP_DUPLICATE, (long)endpoints.receive, MOSS_CAP_RECEIVE | MOSS_CAP_DUPLICATE);
  if (receive <= 0 || syscall2(SYS_CAP_SET_INHERIT, receive, 1) != 0) {
    goto fail;
  }
  // The file service owns mint authority. The supervisor retains only an
  // attenuated sender for service lookup and recovery.
  mint = syscall2(SYS_CAP_DUPLICATE, (long)endpoints.send,
                  MOSS_CAP_SEND | MOSS_CAP_TRANSFER | MOSS_CAP_DUPLICATE | MOSS_CAP_MINT);
  if (mint <= 0 || syscall2(SYS_CAP_SET_INHERIT, mint, 1) != 0) {
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
  pid_t child = fork_domain(&domain);
  if (child == 0) {
    char *const argv[] = {"file-service", receive_arg, mint_arg, NULL};
    execve("/file-service.elf", argv, NULL);
    _exit(127);
  }
  (void)syscall1(SYS_CAP_CLOSE, mint);
  (void)syscall1(SYS_CAP_CLOSE, receive);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.receive);
  if (child < 0) {
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
  // Inheriting through fork needs DUPLICATE. The namespace can ask the file
  // service to open an object, but cannot mint or transfer this root sender.
  long file_cap = syscall2(SYS_CAP_DUPLICATE, file->send, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE);
  if (file_cap <= 0) {
    goto fail;
  }
  receive = syscall2(SYS_CAP_DUPLICATE, (long)endpoints.receive, MOSS_CAP_RECEIVE | MOSS_CAP_DUPLICATE);
  if (receive <= 0 || syscall2(SYS_CAP_SET_INHERIT, receive, 1) != 0) {
    goto fail;
  }
  char receive_arg[32], file_arg[32]; // Each holds a decimal 64-bit handle.
  int receive_size = snprintf(receive_arg, sizeof(receive_arg), "%lu", (unsigned long)receive);
  int file_size = snprintf(file_arg, sizeof(file_arg), "%lu", (unsigned long)file_cap);
  if (receive_size < 0 || (size_t)receive_size >= sizeof(receive_arg) || file_size < 0 ||
      (size_t)file_size >= sizeof(file_arg) || syscall2(SYS_CAP_SET_INHERIT, file_cap, 1) != 0) {
    goto fail;
  }

  // Fork copies opted-in handles with their numbers unchanged. The parent
  // closes its temporary minted handle before it creates a shell.
  long domain = 0;
  pid_t child = fork_domain(&domain);
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
  if (send <= 0 || syscall2(SYS_CAP_SET_INHERIT, send, 1) != 0) {
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

static pid_t start_shell(long namespace_capability, long *domain) {
  *domain = 0;
  char namespace_env[64]; // Environment key plus decimal 64-bit handle.
  int size =
      snprintf(namespace_env, sizeof(namespace_env), "MOSS_NAMESPACE_CAP=%lu", (unsigned long)namespace_capability);
  if (size < 0 || (size_t)size >= sizeof(namespace_env)) {
    return -1;
  }
  pid_t child = fork_domain(domain);
  if (child == 0) {
    char *const argv[] = {"ash", "-i", NULL};
    char *const env[] = {"PATH=/", "HOME=/", "TERM=dumb", "PS1=moss$ ", "PS2=> ", namespace_env, NULL};
    execve("/busybox.elf", argv, env);
    _exit(127);
  }
  return child;
}

static enum ServiceLoss supervise(struct Service *file, struct Service *namespace, pid_t *shell, long *shell_domain) {
  for (;;) {
    int status;
    pid_t reaped = waitpid(-1, &status, 0);
    if (reaped < 0) {
      if (errno == EINTR) {
        continue;
      }
      report(STDERR_FILENO, "moss-init: wait failed\n");
      return SUPERVISOR_FAILURE;
    }
    if (reaped == file->pid) {
      file->pid = 0;
      report(STDOUT_FILENO, "moss-init: file service died\n");
      return FILE_LOST;
    }
    if (reaped == namespace->pid) {
      namespace->pid = 0;
      report(STDOUT_FILENO, "moss-init: namespace service died\n");
      return NAMESPACE_LOST;
    }
    if (reaped == *shell) {
      *shell = 0;
      (void)syscall1(SYS_CAP_CLOSE, *shell_domain);
      *shell_domain = 0;
      report(STDOUT_FILENO, "moss-init: restarting shell\n");
      if (restart_delay() != 0) {
        return SUPERVISOR_FAILURE;
      }
      *shell = start_shell(namespace->send, shell_domain);
      if (*shell < 0) {
        report(STDERR_FILENO, "moss-init: shell launch failed\n");
        *shell = 0;
        return NAMESPACE_LOST;
      }
    }
  }
}

int main(void) {
  report(STDOUT_FILENO, "moss-init: supervisor ready\n");
  for (;;) {
    struct Service file = {0};
    if (start_file_service(&file) != 0) {
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
      long shell_domain = 0;
      pid_t shell = start_shell(namespace.send, &shell_domain);
      if (shell < 0) {
        report(STDERR_FILENO, "moss-init: shell launch failed\n");
        stop_service(&namespace);
        if (restart_delay() != 0) {
          stop_service(&file);
          return 1;
        }
        continue;
      }

      enum ServiceLoss lost = supervise(&file, &namespace, &shell, &shell_domain);
      stop_child(shell, shell_domain);
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
