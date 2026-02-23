// -*- C++ -*- (Tell editors this is C++26 module code)
//
// SPDX-License-Identifier: MIT
//
// MOSS Kernel - Intrinsics Module (Hub)
//
// This module provides type-safe wrappers around Clang compiler intrinsics.
// All operations are zero-cost abstractions that compile to efficient hardware instructions.
//
// Partitions:
// - bitops:  Bit manipulation (clz, ctz, popcount, ffs, parity)
// - bswap:   Byte swapping for endianness conversion
// - memory:  Memory operations (memcpy, memset, memmove, memcmp)
// - atomic:  Lock-free atomic operations with memory ordering
// - source:  Source location capture (file, line, function)
// - traits:  Compile-time type introspection
// - control: Control flow optimization (unreachable, likely, unlikely)

export module moss.intrinsics;

// Re-export all partitions
export import :bitops;
export import :bswap;
export import :memory;
export import :atomic;
export import :source;
export import :traits;
export import :control;

// The moss::intrinsics namespace and all sub-namespaces are already exported
// via the partition imports above. No additional declarations needed.
