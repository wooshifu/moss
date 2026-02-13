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

// ut.hpp configuration is handled by CMakeLists.txt through compile definitions
// No need to redefine macros here to avoid redefinition warnings

// Configure for single-threaded freestanding environment
#define BOOST_UT_THREAD_SAFE(x)          // Disable thread safety features
#define BOOST_UT_HAS_THREADING 0         // Explicitly disable threading

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
// Phase 3: Simplified ut.hpp-style testing framework for freestanding
// ========================================================================

// Forward declarations to avoid namespace issues
namespace moss::kernel {
    void kernel_uart_puts(const char* str) noexcept;
}

// Create our own simplified testing framework with ut.hpp-like syntax
namespace boost::ut {

    // Basic test result tracking
    struct test_state {
        const char* current_test_name = nullptr;
        int tests_run = 0;
        int tests_failed = 0;
    };

    static test_state global_test_state;

    // Helper function to avoid std::cout macro issues
    inline void test_output(const char* message) {
        // Use kernel UART output to avoid iostream macro conflicts
        moss::kernel::kernel_uart_puts(message);
    }

    template<typename T>
    inline void test_output_value(const T& value) {
        // For now, just output a placeholder - will implement proper formatting later
        (void)value;  // Suppress unused parameter warning
        test_output("[value]");
    }

    // Test registration and execution
    struct test_case {
        template<typename F>
        test_case(const char* name, F&& func) {
            global_test_state.current_test_name = name;
            global_test_state.tests_run++;

            test_output("Running test: ");
            test_output(name);
            test_output(" ... ");

            // Execute test function directly (no exceptions in freestanding)
            func();
            test_output("✅ PASS\n");
        }
    };

    // Expect function for assertions
    template<typename T>
    void expect_impl(T condition, const char* expr, const char* file, int line) {
        if (!condition) {
            test_output("\n  ❌ Assertion failed: ");
            test_output(expr);
            test_output(" at ");
            test_output(file);
            test_output(":");
            // TODO: Add proper line number formatting
            (void)line; // Suppress unused parameter warning
            test_output("[line]\n");
            global_test_state.tests_failed++;
        }
    }

    // Literals namespace
    namespace literals {
        struct test_string {
            const char* name;

            template<typename F>
            auto operator=(F&& func) {
                return test_case(name, std::forward<F>(func));
            }
        };

        test_string operator""_test(const char* name, std::size_t) {
            return {name};
        }

        constexpr std::usize operator""_u(unsigned long long value) {
            return static_cast<std::usize>(value);
        }
    }

    // Operators namespace
    namespace operators {
        template<typename T>
        bool operator==(T lhs, T rhs) {
            return lhs == rhs;
        }
    }
}

#define expect(condition) boost::ut::expect_impl((condition), #condition, __FILE__, __LINE__)

// Note: Avoid global using namespace directives for header hygiene
// Users can add using declarations in their implementation files if needed

// ========================================================================
// Phase 4: MOSS kernel test utilities
// ========================================================================

namespace moss::test {

    /**
     * @brief Initialize the kernel test environment
     */
    inline void initialize_kernel_test_environment() noexcept {
        // TODO: Initialize UART and semihosting when kernel integration is ready
    }

    /**
     * @brief Initialize the freestanding standard library
     */
    void initialize_freestanding_std() noexcept;

    /**
     * @brief Run validation tests
     */
    void run_validation_tests() noexcept;

    /**
     * @brief Run freestanding library validation tests
     */
    void run_freestanding_validation_tests() noexcept;

    /**
     * @brief Simple test result reporter using std::cout
     */
    class KernelReporter {
    public:
        [[noreturn]] void print_summary(int total_tests, int passed_tests, int failed_tests) {
            boost::ut::test_output("\n");
            boost::ut::test_output("========================================\n");
            boost::ut::test_output("MOSS Kernel Test Results Summary\n");
            boost::ut::test_output("========================================\n");
            boost::ut::test_output("Tests run: ");
            boost::ut::test_output_value(total_tests);
            boost::ut::test_output("\n");
            boost::ut::test_output("Passed:    ");
            boost::ut::test_output_value(passed_tests);
            boost::ut::test_output("\n");
            boost::ut::test_output("Failed:    ");
            boost::ut::test_output_value(failed_tests);
            boost::ut::test_output("\n");
            boost::ut::test_output("Success:   ");
            boost::ut::test_output(failed_tests == 0 ? "YES" : "NO");
            boost::ut::test_output("\n");
            boost::ut::test_output("========================================\n");

            if (failed_tests == 0) {
                kernel_test_exit_success();
            } else {
                kernel_test_exit_failure();
            }
        }

    private:
        [[noreturn]] void kernel_test_exit_success() noexcept {
            // No need to flush since we're using direct UART output
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

        [[noreturn]] void kernel_test_exit_failure() noexcept {
            // No need to flush since we're using direct UART output
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
            boost::ut::test_output("[TEST] ");
            boost::ut::test_output(name);
            boost::ut::test_output(" ... ");

            // No try/catch in freestanding environment
            test_func();
            ++passed_tests_;
            boost::ut::test_output("PASS\n");
        }

        [[noreturn]] void print_results() {
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
