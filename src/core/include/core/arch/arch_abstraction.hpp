#pragma once

// Thin bridge header - architecture abstractions now in moss.arch module
import moss.arch;

// Architecture detection macros (do not cross module boundaries)
#ifndef MOSS_ARCH_ARM64
#ifndef MOSS_ARCH_X86_64
#ifndef MOSS_ARCH_RISCV
#if defined(__x86_64__) || defined(__x86_64) || defined(__amd64__) ||          \
    defined(__amd64) || defined(_M_X64)
#define MOSS_ARCH_X86_64
#elif defined(__aarch64__) || defined(_M_ARM64)
#define MOSS_ARCH_ARM64
#elif defined(__riscv) && __riscv_xlen == 64
#define MOSS_ARCH_RISCV
#else
#define MOSS_ARCH_X86_64
#endif
#endif
#endif
#endif
