#include <errno.h>
#include <stddef.h>
#include <sys/wait.h>
#include <unistd.h>

#define MOSS_SYSCALL_RAW_ONLY
#include "syscall.h"

// One second between launches bounds a failing shell's restart rate.
#define SHELL_RESTART_DELAY_NS 1000000000UL

int main(void) {
  static char *const shell_argv[] = {"ash", "-i", NULL};
  static char *const shell_env[] = {"PATH=/", "HOME=/", "TERM=dumb", "PS1=moss$ ", "PS2=> ", NULL};
  static const char ready[] = "moss-init: supervisor ready\n";
  static const char restart[] = "moss-init: restarting shell\n";
  static const char launch_failed[] = "moss-init: shell launch failed\n";
  static const char fork_failed[] = "moss-init: fork failed\n";
  static const char wait_failed[] = "moss-init: wait failed\n";
  static const char sleep_failed[] = "moss-init: restart delay failed\n";

  (void)write(STDOUT_FILENO, ready, sizeof(ready) - 1);
  for (;;) {
    pid_t shell = fork();
    if (shell == 0) {
      execve("/busybox.elf", shell_argv, shell_env);
      (void)write(STDERR_FILENO, launch_failed, sizeof(launch_failed) - 1);
      _exit(127);
    }
    if (shell > 0) {
      // Reap adopted children too: a service can leave descendants behind
      // when it dies, and PID 1 must not retain their zombies indefinitely.
      for (;;) {
        pid_t reaped = waitpid(-1, NULL, 0);
        if (reaped == shell) {
          break;
        }
        if (reaped < 0 && errno != EINTR) {
          (void)write(STDERR_FILENO, wait_failed, sizeof(wait_failed) - 1);
          return 1;
        }
      }
      (void)write(STDOUT_FILENO, restart, sizeof(restart) - 1);
    } else {
      (void)write(STDERR_FILENO, fork_failed, sizeof(fork_failed) - 1);
    }

    // The Moss native syscall takes a nanosecond count, while this mlibc
    // port has no nanosleep sysdep yet.
    unsigned long delay = SHELL_RESTART_DELAY_NS;
    unsigned long remaining = 0;
    long sleep_result;
    while ((sleep_result = syscall2(SYS_NANOSLEEP, (long)&delay, (long)&remaining)) == -EINTR) {
      delay = remaining;
    }
    if (sleep_result < 0) {
      (void)write(STDERR_FILENO, sleep_failed, sizeof(sleep_failed) - 1);
      return 1;
    }
  }
}
