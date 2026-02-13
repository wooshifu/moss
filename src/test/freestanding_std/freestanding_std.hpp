#pragma once

/**
 * @file freestanding_std.hpp
 * @brief Complete freestanding standard library compatibility layer for ut.hpp
 *
 * This header provides all standard library functionality required by Boost.UT (ut.hpp)
 * using kernel-compatible, static allocation implementations that work in the MOSS
 * freestanding environment.
 *
 * Design Principles:
 * - Static allocation only (no heap usage)
 * - Compile-time bounds checking
 * - Direct UART integration for I/O
 * - ARM64 timestamp-based timing
 * - Complete API compatibility with standard library
 *
 * Usage:
 * Include this header BEFORE including ut.hpp to provide all required dependencies.
 */

// Prevent inclusion of system standard library headers
#ifndef MOSS_FREESTANDING_STD_HPP
#define MOSS_FREESTANDING_STD_HPP

// Forward declarations for kernel test infrastructure
namespace moss::kernel::test {
    class UartWriter;
    unsigned long long get_test_timestamp_ns() noexcept;
}

// ============================================================================
// Core Standard Library Components
// ============================================================================

// Basic type traits and utilities (build on kernel_std.hpp)
#include "type_traits.hpp"

// Container implementations (existing files only)
#include "vector.hpp"       // std::vector with static storage
#include "string.hpp"       // std::string with fixed buffers
#include "string_view.hpp"  // std::string_view implementation
#include "array.hpp"        // std::array wrapper
#include "unordered_map.hpp" // std::unordered_map with static buckets

// Memory management
#include "memory.hpp"       // std::shared_ptr, std::unique_ptr stubs

// I/O and formatting
#include "iostream.hpp"     // std::cout, std::ostream (UART-based)
#include "sstream.hpp"      // std::ostringstream with static buffers

// Time and chrono
#include "chrono.hpp"       // std::chrono with ARM64 timestamps

// Platform compatibility stubs
#include "unistd.hpp"       // POSIX stubs for subprocess testing
#include "sys/wait.hpp"     // Process wait stubs

// ============================================================================
// Configuration and Integration
// ============================================================================

namespace moss::freestanding_std {

    /**
     * @brief Initialize the freestanding standard library environment
     *
     * This should be called once during test framework initialization to set up
     * the UART output redirection and any required static data structures.
     */
    void initialize() noexcept;

    /**
     * @brief Get memory usage statistics for debugging
     */
    struct MemoryStats {
        std::usize total_static_bytes;
        std::usize used_bytes;
        std::usize peak_used_bytes;
        std::usize num_allocations;
    };

    MemoryStats get_memory_stats() noexcept;

    /**
     * @brief Configuration constants
     */
    namespace config {
        constexpr std::usize MAX_VECTOR_SIZE = 256;      // Maximum vector elements
        constexpr std::usize MAX_STRING_SIZE = 512;      // Maximum string length
        constexpr std::usize MAX_MAP_BUCKETS = 128;      // Hash map bucket count
        constexpr std::usize MAX_STACK_DEPTH = 64;       // Stack container depth
        constexpr std::usize MEMORY_POOL_SIZE = 16384;   // 16KB static memory pool
    }
}

// ============================================================================
// Global Initialization
// ============================================================================

// Automatic initialization when included
namespace {
    [[maybe_unused]] static const bool freestanding_std_initialized =
        (moss::freestanding_std::initialize(), true);
}

#endif // MOSS_FREESTANDING_STD_HPP