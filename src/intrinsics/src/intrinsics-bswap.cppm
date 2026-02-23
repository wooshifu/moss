// -*- C++ -*- (Tell editors this is C++26 module code)
//
// SPDX-License-Identifier: MIT
//
// MOSS Kernel - Intrinsics Module: Byte Swapping
//
// This partition provides byte order reversal operations using Clang intrinsics.
// Useful for endianness conversion (e.g., big-endian to little-endian).

export module moss.intrinsics:bswap;

/// Byte swapping operations using Clang intrinsics.
/// Efficiently reverse byte order for 16/32/64-bit values.
export namespace moss::intrinsics::bswap {

/// Reverse byte order of a 16-bit value.
/// Example: 0x1234 -> 0x3412
[[nodiscard]] constexpr unsigned short bswap16(unsigned short x) noexcept { return __builtin_bswap16(x); }

/// Reverse byte order of a 32-bit value.
/// Example: 0x12345678 -> 0x78563412
[[nodiscard]] constexpr unsigned int bswap32(unsigned int x) noexcept { return __builtin_bswap32(x); }

/// Reverse byte order of a 64-bit value.
/// Example: 0x123456789ABCDEF0 -> 0xF0DEBC9A78563412
[[nodiscard]] constexpr unsigned long long bswap64(unsigned long long x) noexcept { return __builtin_bswap64(x); }

} // namespace moss::intrinsics::bswap
