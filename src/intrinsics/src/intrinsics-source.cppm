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
///
/// Each function uses a default parameter so the builtin is evaluated
/// at the **caller's** site, not inside this file.  This is the same
/// technique used by std::source_location::current().
export namespace moss::intrinsics::source {

// ============================================================================
// File Name
// ============================================================================

#if __has_builtin(__builtin_FILE_NAME)
/// Get base file name (without directory path).
/// Preferred over file() for cleaner log output.
/// Example: "/path/to/file.cpp" -> "file.cpp"
constexpr const char *file_name(const char *name = __builtin_FILE_NAME()) noexcept { return name; }

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
constexpr const char *file(const char *path = __builtin_FILE()) noexcept { return path; }

// ============================================================================
// Line Number
// ============================================================================

/// Get current line number in source file.
/// Returns unsigned integer (1-indexed).
constexpr unsigned line(unsigned ln = __builtin_LINE()) noexcept { return ln; }

// ============================================================================
// Function Name
// ============================================================================

/// Get current function name.
/// Returns function signature for C++, plain name for C.
/// Example: "void moss::kernel::process::schedule()"
constexpr const char *function(const char *fn = __builtin_FUNCTION()) noexcept { return fn; }

} // namespace moss::intrinsics::source
