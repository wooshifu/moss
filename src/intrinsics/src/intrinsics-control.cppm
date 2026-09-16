// -*- C++ -*- (Tell editors this is C++26 module code)
//
// Copyright 2025 Mercedes-Benz Tech Innovation GmbH
// SPDX-License-Identifier: MIT
//
// MOSS Kernel - Intrinsics Module: Control Flow Optimization
//
// This partition provides control flow hints and optimizations using Clang intrinsics.
// Includes branch prediction, unreachable markers, and constant detection.

export module moss.intrinsics:control;

/// Control flow optimization intrinsics using Clang builtins.
/// Help the compiler generate more efficient code for hot paths and error handling.
export namespace moss::intrinsics::control {

// ============================================================================
// Unreachable Code Marker
// ============================================================================

/// Mark code as unreachable (invokes undefined behavior if executed).
/// Helps optimizer eliminate dead code and generate better assembly.
/// Use carefully: only mark truly unreachable paths.
[[noreturn]] inline void unreachable() noexcept { __builtin_unreachable(); }

// ============================================================================
// Branch Prediction Hints
// ============================================================================

/// Hint that expr is expected to have value expected (true or false).
/// Optimizes instruction layout for better cache locality.
[[nodiscard]] constexpr bool expect(bool expr, bool expected) noexcept { return __builtin_expect(expr, expected); }

/// Hint that expr is likely to be true.
/// Use for common success paths.
/// Example: if (likely(ptr != nullptr)) { ... }
[[nodiscard]] constexpr bool likely(bool expr) noexcept { return __builtin_expect(expr, true); }

/// Hint that expr is unlikely to be true.
/// Use for error handling and rare edge cases.
/// Example: if (unlikely(error_occurred)) { ... }
[[nodiscard]] constexpr bool unlikely(bool expr) noexcept { return __builtin_expect(expr, false); }

// ============================================================================
// Constant Propagation Hint
// ============================================================================

/// Check if value is known at compile time.
/// Returns true if compiler can constant-fold the value.
/// Useful for conditional optimization paths.
template <typename T> [[nodiscard]] constexpr bool is_constant(const T &value) noexcept {
  return __builtin_constant_p(value);
}

// ============================================================================
// Optimization Assumptions
// ============================================================================

#if __has_builtin(__builtin_assume)
/// Tell optimizer to assume expr is true (undefined behavior if false).
/// This supplies an invariant, not a runtime check even in debug builds.
/// The wrapper's bool argument is evaluated normally before entering assume().
/// Use carefully: only for verified invariants.
inline void assume(bool expr) noexcept { __builtin_assume(expr); }

/// Feature detection: true if __builtin_assume is available.
inline constexpr bool has_assume_builtin = true;
#else
/// Feature detection: true if __builtin_assume is available.
inline constexpr bool has_assume_builtin = false;
#endif

} // namespace moss::intrinsics::control
