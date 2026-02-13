#pragma once

/**
 * @file moss_ut.hpp
 * @brief MOSS kernel ut.hpp integration wrapper
 *
 * Provides ut.hpp integration for MOSS kernel using freestanding implementations.
 * This is a minimal working version that focuses on core functionality.
 */

// ========================================================================
// Phase 1: Configure ut.hpp before including it
// ========================================================================

// Disable ut.hpp features that don't work in kernel environment
#define BOOST_UT_DISABLE_MODULE          // Disable C++20 modules
#define BOOST_UT_DISABLE_FILE_OUTPUT     // Disable file I/O operations

// ========================================================================
// Phase 2: Include freestanding standard library components individually
// ========================================================================

// Include only the core components we need, in correct order
#include "freestanding_std/type_traits.hpp"
#include "freestanding_std/vector.hpp"
#include "freestanding_std/string.hpp"
#include "freestanding_std/string_view.hpp"
#include "freestanding_std/unordered_map.hpp"
#include "freestanding_std/memory.hpp"
#include "freestanding_std/iostream.hpp"
#include "freestanding_std/sstream.hpp"
#include "freestanding_std/chrono.hpp"
#include "freestanding_std/exception.hpp"
#include "freestanding_std/unistd.hpp"
#include "freestanding_std/sys/wait.hpp"

// ========================================================================
// Phase 3: MOSS kernel test utilities
// ========================================================================

namespace moss::test {

    /**
     * @brief Initialize the kernel test environment
     */
    inline void initialize_kernel_test_environment() noexcept {
        // TODO: Initialize UART and semihosting when kernel integration is ready
    }

    /**
     * @brief Simple test result reporter using std::cout
     */
    class KernelReporter {
    public:
        void print_summary(int total_tests, int passed_tests, int failed_tests) {
            std::cout << std::endl;
            std::cout << "========================================" << std::endl;
            std::cout << "MOSS Kernel Test Results Summary" << std::endl;
            std::cout << "========================================" << std::endl;
            std::cout << "Tests run: " << total_tests << std::endl;
            std::cout << "Passed:    " << passed_tests << std::endl;
            std::cout << "Failed:    " << failed_tests << std::endl;
            std::cout << "Success:   " << (failed_tests == 0 ? "YES" : "NO") << std::endl;
            std::cout << "========================================" << std::endl;

            if (failed_tests == 0) {
                kernel_test_exit_success();
            } else {
                kernel_test_exit_failure();
            }
        }

    private:
        void kernel_test_exit_success() noexcept {
            std::cout.flush();
            // TODO: Add QEMU semihosting exit when ready
            // For now, just halt
            while (true) {
                #if defined(__aarch64__)
                asm volatile("wfi");
                #elif defined(__x86_64__)
                asm volatile("hlt");
                #else
                // Generic busy wait
                for (volatile int i = 0; i < 1000000; ++i) {}
                #endif
            }
        }

        void kernel_test_exit_failure() noexcept {
            std::cout.flush();
            // TODO: Add QEMU semihosting exit when ready
            while (true) {
                #if defined(__aarch64__)
                asm volatile("wfi");
                #elif defined(__x86_64__)
                asm volatile("hlt");
                #else
                for (volatile int i = 0; i < 1000000; ++i) {}
                #endif
            }
        }
    };

    /**
     * @brief Initialize the complete kernel testing environment
     */
    inline void initialize() noexcept {
        initialize_kernel_test_environment();
    }

} // namespace moss::test

// ========================================================================
// Phase 4: Include ut.hpp (commented out until dependencies are resolved)
// ========================================================================

// TODO: Uncomment when all dependencies are working
// #include "ut.hpp"

// ========================================================================
// Phase 5: Temporary manual testing framework for validation
// ========================================================================

namespace moss::test {

    /**
     * @brief Simple test runner for validating our freestanding implementations
     */
    class SimpleTestRunner {
    private:
        int total_tests_ = 0;
        int passed_tests_ = 0;
        int failed_tests_ = 0;

    public:
        template<typename F>
        void run_test(const char* name, F&& test_func) {
            ++total_tests_;
            std::cout << "[TEST] " << name << " ... ";

            try {
                test_func();
                ++passed_tests_;
                std::cout << "PASS" << std::endl;
            } catch (...) {
                ++failed_tests_;
                std::cout << "FAIL" << std::endl;
            }
        }

        void print_results() {
            KernelReporter reporter;
            reporter.print_summary(total_tests_, passed_tests_, failed_tests_);
        }
    };

    // Global test runner instance
    extern SimpleTestRunner test_runner;

    // Test registration macro
    #define MOSS_TEST_CASE(name, test_func) \
        namespace { \
            struct test_##name { \
                test_##name() { \
                    moss::test::test_runner.run_test(#name, test_func); \
                } \
            }; \
            static test_##name test_instance_##name; \
        }

} // namespace moss::test