// Moss enters _start using the native C calling convention (argc, argv, envp).
// The C entry adapter passes the kernel-prepared startup vector to mlibc.
.global _start
.type _start, %function
_start:
#if defined(__x86_64__)
  jmp __moss_libc_entry
#elif defined(__aarch64__)
  b __moss_libc_entry
#elif defined(__riscv)
  .option push
  .option norelax
  la gp, __global_pointer$
  .option pop
  tail __moss_libc_entry
#endif
.section .note.GNU-stack,"",%progbits
