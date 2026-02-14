// Architecture detection macros — single source of truth.
//
// This header replaces the ~14 copies of the detection block scattered across
// module files.  It is designed for inclusion in the *global module fragment*
// (between `module;` and `export module ...;`), because preprocessor macros
// do not cross C++26 module boundaries.
//
// Usage (in any .cppm / .cpp):
//
//   module;
//   #include "arch_detect.h"
//   export module moss.whatever;

#ifndef MOSS_ARCH_DETECT_H
#define MOSS_ARCH_DETECT_H

// --- Primary detection from compiler built-ins ---
#if !defined(MOSS_ARCH_ARM64) && !defined(MOSS_ARCH_X86_64) && !defined(MOSS_ARCH_RISCV)

#if defined(__aarch64__) || defined(_M_ARM64)
  #define MOSS_ARCH_ARM64
#elif defined(__x86_64__) || defined(__x86_64) || defined(__amd64__) || \
      defined(__amd64) || defined(_M_X64)
  #define MOSS_ARCH_X86_64
#elif defined(__riscv) && (__riscv_xlen == 64)
  #define MOSS_ARCH_RISCV
#else
  #error "Unsupported architecture: expected ARM64, x86_64, or RISC-V 64"
#endif

#endif // guard: no prior definition

#endif // MOSS_ARCH_DETECT_H
