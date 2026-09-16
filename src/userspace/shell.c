// The architecture boot trampolines enter /shell.elf in both image types.
// Production replaces this small launcher with the BusyBox interactive ash;
// the validation initramfs supplies its own program at /shell.elf.

#include "syscall.h"

void _start(void) {
  char *argv[] = {"ash", "-i", (char *)0};
  char *envp[] = {"PATH=/", "HOME=/", "TERM=dumb", "PS1=moss$ ", "PS2=> ", (char *)0};

  execve("/busybox.elf", argv, envp);
  eprint("shell: cannot start /busybox.elf ash\n");
  _exit(127); // Shell command-launch failure convention; exec succeeded only if it never returned.
}
