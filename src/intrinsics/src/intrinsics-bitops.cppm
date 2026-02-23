// -*- C++ -*- (Tell editors this is C++26 module code)
//
// Copyright 2025 Mercedes-Benz Tech Innovation GmbH
// SPDX-License-Identifier: MIT
//
// MOSS Kernel - Intrinsics Module: Bit Operations
//
// This partition provides efficient bit manipulation operations using Clang intrinsics.
// These map directly to hardware instructions (e.g., CLZ, CTZ, POPCNT).

export module moss.intrinsics:bitops;

/// Bit manipulation operations using Clang intrinsics.
/// All operations compile to single hardware instructions on modern CPUs.
export namespace moss::intrinsics::bitops {

// ============================================================================
// Count Leading Zeros (CLZ)
// ============================================================================

/// Count number of leading zero bits (starting from MSB).
/// Undefined behavior if x == 0. Returns 0-31 for 32-bit values.
[[nodiscard]] constexpr int clz(unsigned int x) noexcept { return __builtin_clz(x); }

/// Count number of leading zero bits (long variant).
/// Undefined behavior if x == 0.
[[nodiscard]] constexpr int clzl(unsigned long x) noexcept { return __builtin_clzl(x); }

/// Count number of leading zero bits (long long variant).
/// Undefined behavior if x == 0. Returns 0-63 for 64-bit values.
[[nodiscard]] constexpr int clzll(unsigned long long x) noexcept { return __builtin_clzll(x); }

// ============================================================================
// Count Trailing Zeros (CTZ)
// ============================================================================

/// Count number of trailing zero bits (starting from LSB).
/// Undefined behavior if x == 0. Returns 0-31 for 32-bit values.
[[nodiscard]] constexpr int ctz(unsigned int x) noexcept { return __builtin_ctz(x); }

/// Count number of trailing zero bits (long variant).
/// Undefined behavior if x == 0.
[[nodiscard]] constexpr int ctzl(unsigned long x) noexcept { return __builtin_ctzl(x); }

/// Count number of trailing zero bits (long long variant).
/// Undefined behavior if x == 0. Returns 0-63 for 64-bit values.
[[nodiscard]] constexpr int ctzll(unsigned long long x) noexcept { return __builtin_ctzll(x); }

// ============================================================================
// Population Count (POPCNT)
// ============================================================================

/// Count number of set bits (1s) in value.
/// Returns 0-32 for 32-bit values.
[[nodiscard]] constexpr int popcount(unsigned int x) noexcept { return __builtin_popcount(x); }

/// Count number of set bits (long variant).
[[nodiscard]] constexpr int popcountl(unsigned long x) noexcept { return __builtin_popcountl(x); }

/// Count number of set bits (long long variant).
/// Returns 0-64 for 64-bit values.
[[nodiscard]] constexpr int popcountll(unsigned long long x) noexcept { return __builtin_popcountll(x); }

// ============================================================================
// Find First Set (FFS)
// ============================================================================

/// Find first set bit (1-indexed from LSB).
/// Returns 0 if x == 0, otherwise returns 1-32.
[[nodiscard]] constexpr int ffs(int x) noexcept { return __builtin_ffs(x); }

/// Find first set bit (long variant).
/// Returns 0 if x == 0.
[[nodiscard]] constexpr int ffsl(long x) noexcept { return __builtin_ffsl(x); }

/// Find first set bit (long long variant).
/// Returns 0 if x == 0, otherwise returns 1-64.
[[nodiscard]] constexpr int ffsll(long long x) noexcept { return __builtin_ffsll(x); }

// ============================================================================
// Parity
// ============================================================================

/// Calculate parity (XOR of all bits).
/// Returns 0 if even number of 1s, 1 if odd number of 1s.
[[nodiscard]] constexpr int parity(unsigned int x) noexcept { return __builtin_parity(x); }

/// Calculate parity (long variant).
[[nodiscard]] constexpr int parityl(unsigned long x) noexcept { return __builtin_parityl(x); }

/// Calculate parity (long long variant).
[[nodiscard]] constexpr int parityll(unsigned long long x) noexcept { return __builtin_parityll(x); }

} // namespace moss::intrinsics::bitops
