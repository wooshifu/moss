#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "syscall.h"

// One second between launches bounds a failing service's restart rate.
#define RESTART_DELAY_NS 1000000000UL

struct FileService {
  pid_t pid;
  long send;
};

static void report(int fd, const char *message) { (void)write(fd, message, strlen(message)); }

static int wait_for(pid_t child) {
  int status;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return 0;
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

static int start_file_service(struct FileService *service) {
  struct moss_ipc_endpoints endpoints = {0};
  if (syscall1(SYS_IPC_CREATE, (long)&endpoints) != 0) {
    return -1;
  }
  long receive = syscall2(SYS_CAP_DUPLICATE, (long)endpoints.receive, MOSS_CAP_RECEIVE | MOSS_CAP_DUPLICATE);
  if (receive <= 0 || syscall2(SYS_CAP_SET_INHERIT, receive, 1) != 0) {
    goto fail;
  }
  char receive_arg[32]; // Decimal 64-bit handle plus terminator.
  int size = snprintf(receive_arg, sizeof(receive_arg), "%lu", (unsigned long)receive);
  if (size < 0 || (size_t)size >= sizeof(receive_arg)) {
    goto fail;
  }

  pid_t child = fork();
  if (child == 0) {
    char *const argv[] = {"file-service", receive_arg, NULL};
    execve("/file-service.elf", argv, NULL);
    _exit(127);
  }
  (void)syscall1(SYS_CAP_CLOSE, receive);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.receive);
  if (child < 0) {
    (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
    return -1;
  }

  // Shell descendants may copy the sender across fork, but receive and
  // transfer authority remain confined to this service incarnation.
  long send = syscall2(SYS_CAP_DUPLICATE, (long)endpoints.send, MOSS_CAP_SEND | MOSS_CAP_DUPLICATE);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
  if (send <= 0 || syscall2(SYS_CAP_SET_INHERIT, send, 1) != 0) {
    if (send > 0) {
      (void)syscall1(SYS_CAP_CLOSE, send);
    }
    (void)kill(child, SIGKILL);
    (void)wait_for(child);
    return -1;
  }
  service->pid = child;
  service->send = send;
  return 0;

fail:
  if (receive > 0) {
    (void)syscall1(SYS_CAP_CLOSE, receive);
  }
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.receive);
  (void)syscall1(SYS_CAP_CLOSE, (long)endpoints.send);
  return -1;
}

static pid_t start_shell(long file_capability) {
  char file_env[64]; // Environment key plus decimal 64-bit handle.
  int size = snprintf(file_env, sizeof(file_env), "MOSS_FILE_CAP=%lu", (unsigned long)file_capability);
  if (size < 0 || (size_t)size >= sizeof(file_env)) {
    return -1;
  }
  pid_t child = fork();
  if (child == 0) {
    char *const argv[] = {"ash", "-i", NULL};
    char *const env[] = {"PATH=/", "HOME=/", "TERM=dumb", "PS1=moss$ ", "PS2=> ", file_env, NULL};
    execve("/busybox.elf", argv, env);
    _exit(127);
  }
  return child;
}

int main(void) {
  report(STDOUT_FILENO, "moss-init: supervisor ready\n");
  for (;;) {
    struct FileService service;
    if (start_file_service(&service) != 0) {
      report(STDERR_FILENO, "moss-init: file service launch failed\n");
      if (restart_delay() != 0) {
        return 1;
      }
      continue;
    }
    char started[64];
    int started_size =
        snprintf(started, sizeof(started), "moss-init: file service started pid=%ld\n", (long)service.pid);
    if (started_size > 0 && (size_t)started_size < sizeof(started)) {
      (void)write(STDOUT_FILENO, started, (size_t)started_size);
    }
    pid_t shell = start_shell(service.send);
    if (shell < 0) {
      report(STDERR_FILENO, "moss-init: shell launch failed\n");
      (void)kill(service.pid, SIGKILL);
      (void)wait_for(service.pid);
      (void)syscall1(SYS_CAP_CLOSE, service.send);
      if (restart_delay() != 0) {
        return 1;
      }
      continue;
    }

    for (;;) {
      int status;
      pid_t reaped = waitpid(-1, &status, 0);
      if (reaped < 0) {
        if (errno == EINTR) {
          continue;
        }
        report(STDERR_FILENO, "moss-init: wait failed\n");
        return 1;
      }
      if (reaped == service.pid) {
        // An old sender stays bound to the dead endpoint. Replace the shell
        // too, so its new children discover only the new incarnation.
        report(STDOUT_FILENO, "moss-init: file service died\n");
        (void)kill(shell, SIGKILL);
        (void)wait_for(shell);
        (void)syscall1(SYS_CAP_CLOSE, service.send);
        if (restart_delay() != 0) {
          return 1;
        }
        break;
      }
      if (reaped == shell) {
        report(STDOUT_FILENO, "moss-init: restarting shell\n");
        if (restart_delay() != 0) {
          return 1;
        }
        shell = start_shell(service.send);
        if (shell < 0) {
          report(STDERR_FILENO, "moss-init: shell launch failed\n");
          (void)kill(service.pid, SIGKILL);
          (void)wait_for(service.pid);
          (void)syscall1(SYS_CAP_CLOSE, service.send);
          break;
        }
      }
    }
  }
}
