#pragma once

/**
 * @file sys/wait.hpp
 * @brief POSIX sys/wait.h compatibility stubs for freestanding environment
 *
 * Provides ut.hpp subprocess waiting functionality stubs that are no-ops
 * in the kernel environment. ut.hpp uses these for child process testing.
 */

#include "../type_traits.hpp"
#include "../unistd.hpp"  // For pid_t

// Wait status macros and constants
namespace std {
namespace detail {

    // Process wait functionality stubs for ut.hpp compatibility
    inline pid_t wait(int* wstatus) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(wstatus);

        // In kernel environment, process waiting is not supported
        return INVALID_PID;
    }

    inline pid_t waitpid(pid_t pid, int* wstatus, int options) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(pid);
        static_cast<void>(wstatus);
        static_cast<void>(options);

        // In kernel environment, process waiting is not supported
        return INVALID_PID;
    }

    // Wait status analysis functions - always return safe defaults
    inline bool WIFEXITED(int wstatus) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(wstatus);

        // In kernel environment, always indicate normal exit
        return true;
    }

    inline int WEXITSTATUS(int wstatus) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(wstatus);

        // In kernel environment, always indicate success exit
        return 0;
    }

    inline bool WIFSIGNALED(int wstatus) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(wstatus);

        // In kernel environment, never indicate signal termination
        return false;
    }

    inline int WTERMSIG(int wstatus) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(wstatus);

        // In kernel environment, no signal termination
        return 0;
    }

    inline bool WIFSTOPPED(int wstatus) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(wstatus);

        // In kernel environment, process stopping not applicable
        return false;
    }

    inline int WSTOPSIG(int wstatus) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(wstatus);

        // In kernel environment, no stop signals
        return 0;
    }

    inline bool WIFCONTINUED(int wstatus) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(wstatus);

        // In kernel environment, process continuation not applicable
        return false;
    }

} // namespace detail
} // namespace std

// Global namespace POSIX compatibility (what ut.hpp expects to find)
using ::std::detail::wait;
using ::std::detail::waitpid;
using ::std::detail::WIFEXITED;
using ::std::detail::WEXITSTATUS;
using ::std::detail::WIFSIGNALED;
using ::std::detail::WTERMSIG;
using ::std::detail::WIFSTOPPED;
using ::std::detail::WSTOPSIG;
using ::std::detail::WIFCONTINUED;

// Wait options constants that ut.hpp might use
constexpr int WNOHANG = 1;      // Don't hang if no child has exited
constexpr int WUNTRACED = 2;    // Report status of stopped children
constexpr int WCONTINUED = 8;   // Report continued children
constexpr int WSTOPPED = WUNTRACED;  // Compatibility alias

// Exit status constants
constexpr int WEXITED = 4;      // Wait for processes that have exited
constexpr int WNOWAIT = 0x01000000;  // Don't remove child from system

// Additional process group wait support
inline pid_t wait3(int* wstatus, int options, void* rusage) noexcept {
    // Suppress unused parameter warnings
    static_cast<void>(wstatus);
    static_cast<void>(options);
    static_cast<void>(rusage);

    // In kernel environment, extended wait is not supported
    return INVALID_PID;
}

inline pid_t wait4(pid_t pid, int* wstatus, int options, void* rusage) noexcept {
    // Suppress unused parameter warnings
    static_cast<void>(pid);
    static_cast<void>(wstatus);
    static_cast<void>(options);
    static_cast<void>(rusage);

    // In kernel environment, extended wait is not supported
    return INVALID_PID;
}
