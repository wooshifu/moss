#pragma once

/**
 * @file kernel_ut.hpp
 * @brief Lightweight kernel-specific unit testing framework
 *
 * A minimalist testing framework designed specifically for freestanding kernel environments.
 * Provides ut.hpp-like syntax without the complexity of a full standard library implementation.
 */

// Forward declarations to avoid namespace issues
namespace moss::kernel {
    void kernel_uart_puts(const char* str) noexcept;
}

// ========================================================================
// Core Testing Framework
// ========================================================================

namespace kernel_ut {

    // Test state tracking (simple global state)
    struct test_state {
        const char* current_test_name = nullptr;
        int tests_run = 0;
        int tests_failed = 0;
    };

    static test_state global_state;

    // Simple UART output wrapper
    inline void output(const char* message) {
        moss::kernel::kernel_uart_puts(message);
    }

    // Integer to string conversion (simple, no formatting)
    inline void output_number(int num) {
        if (num == 0) {
            output("0");
            return;
        }

        char buffer[16] = {0};
        int index = 0;
        bool negative = num < 0;
        if (negative) num = -num;

        while (num > 0 && index < 15) {
            buffer[index++] = static_cast<char>('0' + (num % 10));
            num /= 10;
        }

        if (negative) buffer[index++] = '-';

        // Reverse the string
        for (int i = 0; i < index / 2; ++i) {
            char temp = buffer[i];
            buffer[i] = buffer[index - 1 - i];
            buffer[index - 1 - i] = temp;
        }

        output(buffer);
    }

    // Test case registration and execution
    struct test_case {
        template<typename F>
        test_case(const char* name, F&& test_func) {
            global_state.current_test_name = name;
            global_state.tests_run++;

            output("Running test: ");
            output(name);
            output(" ... ");

            // Execute test function
            test_func();
            output("✅ PASS\n");
        }
    };

    // Expectation checking
    template<typename T>
    void expect_impl(T condition, const char* expr, const char* file, int line) {
        if (!condition) {
            output("\n  ❌ Assertion failed: ");
            output(expr);
            output(" at ");
            output(file);
            output(":");
            output_number(line);
            output("\n");
            global_state.tests_failed++;
        }
    }

    // Test results reporting
    [[noreturn]] void report_results() {
        output("\n");
        output("========================================\n");
        output("MOSS Kernel Test Results\n");
        output("========================================\n");
        output("Tests run: ");
        output_number(global_state.tests_run);
        output("\nTests failed: ");
        output_number(global_state.tests_failed);
        output("\n");

        if (global_state.tests_failed == 0) {
            output("✅ All tests passed!\n");
        } else {
            output("❌ Some tests failed!\n");
        }
        output("========================================\n");

        // Architecture-specific halt
        while (true) {
#if defined(MOSS_ARCH_ARM64)
            asm volatile("wfi");
#elif defined(MOSS_ARCH_X86_64)
            asm volatile("hlt");
#else
            for (volatile int i = 0; i < 1000000; ++i) {}
#endif
        }
    }

} // namespace kernel_ut

// ========================================================================
// User-friendly macros and aliases
// ========================================================================

#define expect(condition) kernel_ut::expect_impl((condition), #condition, __FILE__, __LINE__)

#define KERNEL_TEST(name) \
    static kernel_ut::test_case test_##name(#name, []()

#define KERNEL_TEST_END );

// For compatibility with existing code
namespace moss::test {
    inline void initialize_freestanding_std() noexcept {
        // No initialization needed for kernel testing
    }

    // Function declarations - implementations provided by test files
    void run_validation_tests() noexcept;
    [[noreturn]] void run_freestanding_validation_tests() noexcept;
}
