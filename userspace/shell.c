// MOSS mini shell — interactive command interpreter
//
// Built-in commands: help, echo, exit, pid
// External commands: fork + execve + waitpid (e.g., "hello.elf")

#include "syscall.h"

// ============================================================================
// String helpers
// ============================================================================

static int starts_with(const char *str, const char *prefix) {
  while (*prefix) {
    if (*str != *prefix)
      return 0;
    str++;
    prefix++;
  }
  return 1;
}

// Skip leading whitespace, return pointer to first non-space
static const char *skip_spaces(const char *s) {
  while (*s == ' ' || *s == '\t')
    s++;
  return s;
}

// Print a decimal number to stdout
static void print_num(long n) {
  if (n < 0) {
    write(1, "-", 1);
    n = -n;
  }
  char buf[20];
  int pos = 0;
  if (n == 0) {
    write(1, "0", 1);
    return;
  }
  while (n > 0) {
    buf[pos++] = (char)('0' + n % 10);
    n /= 10;
  }
  // Reverse
  for (int i = pos - 1; i >= 0; i--) {
    write(1, &buf[i], 1);
  }
}

// ============================================================================
// Built-in commands
// ============================================================================

static void cmd_help(void) {
  print("MOSS shell built-in commands:\n");
  print("  help       - show this help\n");
  print("  echo TEXT  - print TEXT to stdout\n");
  print("  pid        - show shell PID\n");
  print("  exit       - exit the shell\n");
  print("\n");
  print("External commands (loaded from initramfs):\n");
  print("  hello.elf  - hello world program\n");
}

static void cmd_echo(const char *args) {
  args = skip_spaces(args);
  if (*args) {
    print(args);
  }
  print("\n");
}

static void cmd_pid(void) {
  print("PID: ");
  print_num(getpid());
  print("\n");
}

// ============================================================================
// External command execution
// ============================================================================

// Maximum number of arguments (including program name)
#define MAX_ARGS 16

static void run_external(const char *cmd) {
  // Parse command line into argv[] (split on whitespace).
  // We copy into a local buffer so we can null-terminate each token.
  static char argbuf[256];
  static char *argv[MAX_ARGS + 1];
  int argc = 0;

  // Copy command string
  int len = 0;
  while (cmd[len] && len < 254) {
    argbuf[len] = cmd[len];
    len++;
  }
  argbuf[len] = '\0';

  // Tokenize: split on spaces/tabs
  char *p = argbuf;
  while (*p && argc < MAX_ARGS) {
    // Skip whitespace
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '\0')
      break;
    argv[argc++] = p;
    // Find end of token
    while (*p && *p != ' ' && *p != '\t')
      p++;
    if (*p)
      *p++ = '\0';
  }
  argv[argc] = (char *)0; // NULL-terminate argv

  if (argc == 0)
    return;

  // Build absolute path from argv[0]: prepend '/' if needed
  char path[64];
  int pi = 0;
  if (argv[0][0] != '/') {
    path[pi++] = '/';
  }
  for (int i = 0; argv[0][i] && pi < 62; i++) {
    path[pi++] = argv[0][i];
  }
  path[pi] = '\0';

  long pid = fork();
  if (pid < 0) {
    eprint("shell: fork failed\n");
    return;
  }

  if (pid == 0) {
    // Child process: replace with new program
    long ret = execve(path, argv, (char *const *)0);
    // If execve returns, it failed
    eprint("shell: exec failed: ");
    eprint(path);
    eprint(" (error ");
    print_num(ret);
    eprint(")\n");
    _exit(127);
  }

  // Parent: wait for child to finish
  int status = 0;
  waitpid(pid, &status, 0);
}

// ============================================================================
// Read-eval-print loop
// ============================================================================

void _start(void) {
  char buf[256];

  print("\n");
  print("================================\n");
  print("  MOSS shell v0.1\n");
  print("  Type 'help' for commands\n");
  print("================================\n");
  print("\n");

  while (1) {
    print("moss$ ");

    // Read one line from stdin (line-buffered by console_read)
    long n = read(0, buf, sizeof(buf) - 1);
    if (n <= 0)
      continue;

    // Strip trailing newline
    if (n > 0 && buf[n - 1] == '\n')
      n--;
    buf[n] = '\0';

    // Skip empty lines
    const char *cmd = skip_spaces(buf);
    if (*cmd == '\0')
      continue;

    // Dispatch built-in commands
    if (streq(cmd, "exit")) {
      print("Goodbye!\n");
      _exit(0);
    }
    if (streq(cmd, "help")) {
      cmd_help();
      continue;
    }
    if (starts_with(cmd, "echo ")) {
      cmd_echo(cmd + 5);
      continue;
    }
    if (streq(cmd, "echo")) {
      print("\n");
      continue;
    }
    if (streq(cmd, "pid")) {
      cmd_pid();
      continue;
    }

    // External command
    run_external(cmd);
  }
}
