// -*- C++ -*- (Tell editors this is C++26 module code)
//
// Copyright 2025 Mercedes-Benz Tech Innovation GmbH
// SPDX-License-Identifier: MIT
//
// MOSS Kernel - Intrinsics Module: Source Location
//
// This partition provides compile-time source location information using Clang intrinsics.
// Useful for logging, debugging, and diagnostics.

export module moss.intrinsics:source;

/// Source location intrinsics using Clang builtins.
/// Capture file name, line number, and function name at compile time.
export namespace moss::intrinsics::source {

// ============================================================================
// File Name
// ============================================================================

#if __has_builtin(__builtin_FILE_NAME)
/// Get base file name (without directory path).
/// Preferred over file() for cleaner log output.
/// Example: "/path/to/file.cpp" -> "file.cpp"
constexpr const char *file_name() noexcept { return __builtin_FILE_NAME(); }

/// Feature detection: true if __builtin_FILE_NAME is available.
inline constexpr bool has_file_name_builtin = true;
#else
/// Feature detection: true if __builtin_FILE_NAME is available.
inline constexpr bool has_file_name_builtin = false;
#endif

// ============================================================================
// Full File Path
// ============================================================================

/// Get full file path (with directory).
/// Example: "/path/to/file.cpp"
constexpr const char *file() noexcept { return __builtin_FILE(); }

// ============================================================================
// Line Number
// ============================================================================

/// Get current line number in source file.
/// Returns unsigned integer (1-indexed).
constexpr unsigned line() noexcept { return __builtin_LINE(); }

// ============================================================================
// Function Name
// ============================================================================

/// Get current function name.
/// Returns function signature for C++, plain name for C.
/// Example: "void moss::kernel::process::schedule()"
constexpr const char *function() noexcept { return __builtin_FUNCTION(); }

} // namespace moss::intrinsics::source
