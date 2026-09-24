#include <mlibc/elf/startup.h>
#include <stdint.h>
#include <stdlib.h>

#include <moss-syscall.h>

extern "C" void __dlapi_enter(uintptr_t *);
extern "C" int __moss_process_after_exec();
extern "C" int main(int, char **, char **);
extern char **environ;

static void prepare_process_descriptors() {
	if (!__moss_process_after_exec()) {
		syscall1(SYS_EXIT, 127);
		__builtin_trap();
	}
}

// The loader runs preinit after relocation but before application constructors.
// Cleanup after __dlapi_enter would expose close-on-exec descriptors to them.
[[gnu::used,
  gnu::section(".preinit_array")]] static void (*const prepare_descriptors_before_constructors)() =
    prepare_process_descriptors;

extern "C" [[noreturn]] void __moss_libc_entry(uintptr_t, char **argv) {
  // Moss preserves its C entry registers; argc is also immediately before
  // argv in the kernel-prepared vector, followed by envp and the null auxv.
  __dlapi_enter(reinterpret_cast<uintptr_t *>(argv) - 1);
  exit(main(mlibc::entry_stack.argc, mlibc::entry_stack.argv, environ));
}
