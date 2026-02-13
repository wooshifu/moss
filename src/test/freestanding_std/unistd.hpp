#pragma once

/**
 * @file unistd.hpp
 * @brief POSIX unistd.h compatibility stubs for freestanding environment
 *
 * Provides ut.hpp subprocess testing functionality stubs that are no-ops
 * in the kernel environment. ut.hpp uses these for child process testing.
 */

#include "type_traits.hpp"

// POSIX constants and types
using pid_t = int;
using uid_t = unsigned int;
using gid_t = unsigned int;
using ssize_t = std::isize;  // Use kernel's isize type

constexpr pid_t INVALID_PID = -1;

// Process creation and management stubs
namespace std {
namespace detail {

    // Process management stubs for ut.hpp compatibility
    inline pid_t fork() noexcept {
        // In kernel environment, subprocess testing is not supported
        // Return failure to indicate process creation is not available
        return INVALID_PID;
    }

    inline int execvp(const char* file, char* const argv[]) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(file);
        static_cast<void>(argv);

        // In kernel environment, exec operations are not supported
        return -1;
    }

    inline int pipe(int pipefd[2]) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(pipefd);

        // In kernel environment, pipe creation is not supported
        return -1;
    }

    inline int dup2(int oldfd, int newfd) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(oldfd);
        static_cast<void>(newfd);

        // In kernel environment, file descriptor duplication is not supported
        return -1;
    }

    inline int close(int fd) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(fd);

        // In kernel environment, file descriptor closing is no-op
        return 0;
    }

    inline ssize_t read(int fd, void* buf, size_t count) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(fd);
        static_cast<void>(buf);
        static_cast<void>(count);

        // In kernel environment, reading from file descriptors is not supported
        return -1;
    }

    inline ssize_t write(int fd, const void* buf, size_t count) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(fd);
        static_cast<void>(buf);
        static_cast<void>(count);

        // In kernel environment, writing to file descriptors is not supported
        return -1;
    }

    // Sleep and timing stubs
    inline unsigned int sleep(unsigned int seconds) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(seconds);

        // In kernel environment, user-space sleep is not applicable
        return 0;
    }

    inline int usleep(unsigned int usec) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(usec);

        // In kernel environment, user-space sleep is not applicable
        return 0;
    }

    // Process identification stubs
    inline pid_t getpid() noexcept {
        // In kernel environment, return a fixed "kernel" PID
        return 1;
    }

    inline pid_t getppid() noexcept {
        // In kernel environment, kernel has no parent process
        return 0;
    }

    inline uid_t getuid() noexcept {
        // In kernel environment, return root user ID
        return 0;
    }

    inline gid_t getgid() noexcept {
        // In kernel environment, return root group ID
        return 0;
    }

    // Working directory stubs
    inline char* getcwd(char* buf, size_t size) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(buf);
        static_cast<void>(size);

        // In kernel environment, working directory concept doesn't apply
        return nullptr;
    }

    inline int chdir(const char* path) noexcept {
        // Suppress unused parameter warnings
        static_cast<void>(path);

        // In kernel environment, working directory concept doesn't apply
        return -1;
    }

} // namespace detail
} // namespace std

// Global namespace POSIX compatibility (what ut.hpp expects to find)
using ::std::detail::fork;
using ::std::detail::execvp;
using ::std::detail::pipe;
using ::std::detail::dup2;
using ::std::detail::close;
using ::std::detail::read;
using ::std::detail::write;
using ::std::detail::sleep;
using ::std::detail::usleep;
using ::std::detail::getpid;
using ::std::detail::getppid;
using ::std::detail::getuid;
using ::std::detail::getgid;
using ::std::detail::getcwd;
using ::std::detail::chdir;

// Standard file descriptors
constexpr int STDIN_FILENO = 0;
constexpr int STDOUT_FILENO = 1;
constexpr int STDERR_FILENO = 2;
