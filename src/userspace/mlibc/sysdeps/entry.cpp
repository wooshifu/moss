#include <mlibc/elf/startup.h>
#include <stdint.h>
#include <stdlib.h>

extern "C" void __dlapi_enter(uintptr_t *);
extern "C" int main(int, char **, char **);
extern char **environ;

extern "C" [[noreturn]] void __moss_libc_entry(uintptr_t, char **argv) {
  // Moss preserves its C entry registers; argc is also immediately before
  // argv in the kernel-prepared vector, followed by envp and the null auxv.
  __dlapi_enter(reinterpret_cast<uintptr_t *>(argv) - 1);
  exit(main(mlibc::entry_stack.argc, mlibc::entry_stack.argv, environ));
}
