// -*- C++ -*- (Tell editors this is C++26 module code)
//
// SPDX-License-Identifier: MIT
//
// MOSS Kernel - Intrinsics Module: Memory Operations
//
// This partition provides constexpr memory operations using Clang intrinsics.
// These functions support compile-time evaluation and optimize to efficient
// instruction sequences at runtime.

export module moss.intrinsics:memory;

/// Memory operations using Clang intrinsics.
/// All functions are constexpr and can be evaluated at compile time.
export namespace moss::intrinsics::memory {

/// Size type for memory operations (platform-dependent).
using size_t = __SIZE_TYPE__;

/// Copy n bytes from src to dest (non-overlapping regions).
/// Undefined behavior if regions overlap. Use memmove for overlapping regions.
/// Can be evaluated at compile time.
constexpr void *memcpy(void *dest, const void *src, size_t n) noexcept { return __builtin_memcpy(dest, src, n); }

/// Fill n bytes at dest with byte value ch.
/// Can be evaluated at compile time.
constexpr void *memset(void *dest, int ch, size_t n) noexcept { return __builtin_memset(dest, ch, n); }

/// Copy n bytes from src to dest (handles overlapping regions correctly).
/// Slower than memcpy but safe for overlapping memory.
/// Can be evaluated at compile time.
constexpr void *memmove(void *dest, const void *src, size_t n) noexcept { return __builtin_memmove(dest, src, n); }

/// Compare first n bytes of lhs and rhs.
/// Returns 0 if equal, <0 if lhs < rhs, >0 if lhs > rhs.
/// Can be evaluated at compile time.
constexpr int memcmp(const void *lhs, const void *rhs, size_t n) noexcept { return __builtin_memcmp(lhs, rhs, n); }

} // namespace moss::intrinsics::memory
