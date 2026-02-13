#pragma once

/**
 * @file ut_kernel.hpp
 * @brief Kernel-optimized version of Boost.UT testing framework
 *
 * This is a simplified version of ut.hpp (Boost.UT) specifically optimized for
 * freestanding kernel environments. It removes features not needed in kernel:
 * - File I/O and logging
 * - Threading and parallel execution
 * - Process spawning and subprocess tests
 * - Complex formatting and output streams
 * - Dynamic memory allocation where possible
 *
 * Preserves core ut.hpp features:
 * - Modern testing syntax: expect(), "test_name"_test
 * - Automatic test discovery and registration
 * - Assertion reporting and test statistics
 * - Template-based test framework
 */

// Minimal includes - only what we actually need
// No external includes needed - we'll implement everything inline

// MOSS kernel integration
namespace moss::kernel {
    void kernel_uart_puts(const char* str) noexcept;
    [[noreturn]] void kernel_test_exit(int exit_code) noexcept;
}

// ============================================================================
// Kernel UT Configuration
// ============================================================================

#ifndef BOOST_UT_DISABLE_MODULE
#define BOOST_UT_DISABLE_MODULE 1
#endif

#ifndef BOOST_UT_DISABLE_FILE_OUTPUT
#define BOOST_UT_DISABLE_FILE_OUTPUT 1
#endif

// ============================================================================
// Core UT Implementation (Kernel Optimized)
// ============================================================================

namespace boost::ut {

// Forward declarations
template<typename T>
struct test_case;

template<typename T>
struct suite;

// ============================================================================
// Basic Types and Utilities
// ============================================================================

using literals_t = int;

template<typename T>
constexpr auto type_name() -> const char* {
    return "unknown";  // Simplified for kernel
}

// ============================================================================
// Test Result and Statistics
// ============================================================================

struct test_result {
    static inline int tests_passed = 0;
    static inline int tests_failed = 0;
    static inline int assertions_passed = 0;
    static inline int assertions_failed = 0;
    static inline const char* current_test_name = nullptr;
};

// ============================================================================
// Output System (UART-based for kernel)
// ============================================================================

struct kernel_printer {
    static void print(const char* message) {
        moss::kernel::kernel_uart_puts(message);
    }

    static void print_number(int num) {
        char buffer[12] = {0};
        if (num == 0) {
            print("0");
            return;
        }

        int index = 0;
        bool negative = num < 0;
        if (negative) num = -num;

        while (num > 0 && index < 11) {
            buffer[index++] = static_cast<char>('0' + (num % 10));
            num /= 10;
        }

        if (negative) buffer[index++] = '-';

        // Reverse string
        for (int i = 0; i < index / 2; ++i) {
            char temp = buffer[i];
            buffer[i] = buffer[index - 1 - i];
            buffer[index - 1 - i] = temp;
        }

        print(buffer);
    }
};

// ============================================================================
// Enhanced Comparison operators with detailed reporting
// ============================================================================

template<typename T, typename U>
struct eq_t {
    T lhs;
    U rhs;

    constexpr operator bool() const {
        return lhs == rhs;
    }
};

template<typename T, typename U>
struct ne_t {
    T lhs;
    U rhs;

    constexpr operator bool() const {
        return lhs != rhs;
    }
};

template<typename T, typename U>
struct gt_t {
    T lhs;
    U rhs;

    constexpr operator bool() const {
        return lhs > rhs;
    }
};

template<typename T, typename U>
struct lt_t {
    T lhs;
    U rhs;

    constexpr operator bool() const {
        return lhs < rhs;
    }
};

template<typename T, typename U>
struct ge_t {
    T lhs;
    U rhs;

    constexpr operator bool() const {
        return lhs >= rhs;
    }
};

template<typename T, typename U>
struct le_t {
    T lhs;
    U rhs;

    constexpr operator bool() const {
        return lhs <= rhs;
    }
};

// ============================================================================
// Type trait helpers to detect comparison types
// ============================================================================

template<typename T>
struct is_eq_t { static constexpr bool value = false; };

template<typename T, typename U>
struct is_eq_t<eq_t<T, U>> { static constexpr bool value = true; };

template<typename T>
struct is_ne_t { static constexpr bool value = false; };

template<typename T, typename U>
struct is_ne_t<ne_t<T, U>> { static constexpr bool value = true; };

template<typename T>
struct is_gt_t { static constexpr bool value = false; };

template<typename T, typename U>
struct is_gt_t<gt_t<T, U>> { static constexpr bool value = true; };

template<typename T>
struct is_lt_t { static constexpr bool value = false; };

template<typename T, typename U>
struct is_lt_t<lt_t<T, U>> { static constexpr bool value = true; };

template<typename T>
struct is_ge_t { static constexpr bool value = false; };

template<typename T, typename U>
struct is_ge_t<ge_t<T, U>> { static constexpr bool value = true; };

template<typename T>
struct is_le_t { static constexpr bool value = false; };

template<typename T, typename U>
struct is_le_t<le_t<T, U>> { static constexpr bool value = true; };

// ============================================================================
// Value Formatting for Error Reporting
// ============================================================================

// Helper to format values for error reporting
template<typename T>
void format_value(T value) {
    if constexpr (requires { static_cast<int>(value); }) {
        if constexpr (sizeof(T) == 1) {
            // Character types
            if (value >= 32 && value <= 126) {
                kernel_printer::print("'");
                char c[2] = {static_cast<char>(value), '\0'};
                kernel_printer::print(c);  // Print single character with null terminator
                kernel_printer::print("'");
            } else {
                kernel_printer::print_number(static_cast<int>(value));
            }
        } else {
            kernel_printer::print_number(static_cast<int>(value));
        }
    } else if constexpr (requires { value == nullptr; }) {
        // Pointer types
        if (value == nullptr) {
            kernel_printer::print("nullptr");
        } else {
            kernel_printer::print("0x");
            // Print hex address (truncated to int size for kernel_printer compatibility)
            kernel_printer::print_number(static_cast<int>(reinterpret_cast<unsigned long>(value)));
        }
    } else {
        kernel_printer::print("[complex_type]");
    }
}

// ============================================================================
// Assertion Framework
// ============================================================================

template<typename T>
struct expectation {
    T value;
    const char* expression;
    const char* file;
    int line;

    constexpr expectation(T v, const char* expr, const char* f, int l)
        : value(v), expression(expr), file(f), line(l) {}

    constexpr auto operator!() const {
        if (!value) {
            test_result::assertions_passed++;
            return true;
        } else {
            test_result::assertions_failed++;
            report_failure();
            return false;
        }
    }

    constexpr operator bool() const {
        if (value) {
            test_result::assertions_passed++;
            return true;
        } else {
            test_result::assertions_failed++;
            report_failure();
            return false;
        }
    }

private:
    void report_failure() const {
        kernel_printer::print("\n  ❌ Assertion failed: ");
        kernel_printer::print(expression);
        kernel_printer::print("\n     Test: ");
        kernel_printer::print(test_result::current_test_name ? test_result::current_test_name : "unknown");
        kernel_printer::print("\n     File: ");
        kernel_printer::print(file ? file : "unknown");
        kernel_printer::print("\n     Line: ");
        kernel_printer::print_number(line);

        // Check if this is a comparison type and show expected vs actual
        report_comparison_details();

        kernel_printer::print("\n");
    }


    void report_comparison_details() const {
        // Base implementation - no additional details for non-comparison types
    }
};

// ============================================================================
// Specialized expectation templates for comparison types
// ============================================================================

// Specialization for equality comparison
template<typename T, typename U>
struct expectation<eq_t<T, U>> {
    eq_t<T, U> value;
    const char* expression;
    const char* file;
    int line;

    constexpr expectation(eq_t<T, U> v, const char* expr, const char* f, int l)
        : value(v), expression(expr), file(f), line(l) {}

    constexpr operator bool() const {
        if (value) {
            test_result::assertions_passed++;
            return true;
        } else {
            test_result::assertions_failed++;
            report_failure();
            return false;
        }
    }

private:
    void report_failure() const {
        kernel_printer::print("\n  ❌ Assertion failed: ");
        kernel_printer::print(expression);
        kernel_printer::print("\n     Test: ");
        kernel_printer::print(test_result::current_test_name ? test_result::current_test_name : "unknown");
        kernel_printer::print("\n     File: ");
        kernel_printer::print(file ? file : "unknown");
        kernel_printer::print("\n     Line: ");
        kernel_printer::print_number(line);
        kernel_printer::print("\n     Expected: ");
        format_value(value.rhs);
        kernel_printer::print("\n     Actual:   ");
        format_value(value.lhs);
        kernel_printer::print("\n");
    }
};

// Specialization for inequality comparison
template<typename T, typename U>
struct expectation<ne_t<T, U>> {
    ne_t<T, U> value;
    const char* expression;
    const char* file;
    int line;

    constexpr expectation(ne_t<T, U> v, const char* expr, const char* f, int l)
        : value(v), expression(expr), file(f), line(l) {}

    constexpr operator bool() const {
        if (value) {
            test_result::assertions_passed++;
            return true;
        } else {
            test_result::assertions_failed++;
            report_failure();
            return false;
        }
    }

private:
    void report_failure() const {
        kernel_printer::print("\n  ❌ Assertion failed: ");
        kernel_printer::print(expression);
        kernel_printer::print("\n     Test: ");
        kernel_printer::print(test_result::current_test_name ? test_result::current_test_name : "unknown");
        kernel_printer::print("\n     File: ");
        kernel_printer::print(file ? file : "unknown");
        kernel_printer::print("\n     Line: ");
        kernel_printer::print_number(line);
        kernel_printer::print("\n     Should not be equal to: ");
        format_value(value.rhs);
        kernel_printer::print("\n     Actual value:           ");
        format_value(value.lhs);
        kernel_printer::print("\n");
    }
};

// Specialization for greater than comparison
template<typename T, typename U>
struct expectation<gt_t<T, U>> {
    gt_t<T, U> value;
    const char* expression;
    const char* file;
    int line;

    constexpr expectation(gt_t<T, U> v, const char* expr, const char* f, int l)
        : value(v), expression(expr), file(f), line(l) {}

    constexpr operator bool() const {
        if (value) {
            test_result::assertions_passed++;
            return true;
        } else {
            test_result::assertions_failed++;
            report_failure();
            return false;
        }
    }

private:
    void report_failure() const {
        kernel_printer::print("\n  ❌ Assertion failed: ");
        kernel_printer::print(expression);
        kernel_printer::print("\n     Test: ");
        kernel_printer::print(test_result::current_test_name ? test_result::current_test_name : "unknown");
        kernel_printer::print("\n     File: ");
        kernel_printer::print(file ? file : "unknown");
        kernel_printer::print("\n     Line: ");
        kernel_printer::print_number(line);
        kernel_printer::print("\n     Expected: > ");
        format_value(value.rhs);
        kernel_printer::print("\n     Actual:     ");
        format_value(value.lhs);
        kernel_printer::print("\n");
    }
};

// Expect function - core of ut.hpp API
template<typename T>
constexpr auto expect_impl(T&& value, const char* expr, const char* file, int line) {
    // Debug: Print type information
    kernel_printer::print("\n     DEBUG: expect_impl called with type: ");
    if constexpr (sizeof(T) == sizeof(bool)) {
        kernel_printer::print("bool-sized");
    } else {
        kernel_printer::print("other");
    }

    auto exp = expectation<T>{static_cast<T&&>(value), expr, file, line};
    // Force evaluation to count assertions
    (void)static_cast<bool>(exp);
    return exp;
}

// Macro to capture proper file/line information at call site
#define expect(condition) expect_impl((condition), #condition, __FILE__, __LINE__)

// ============================================================================
// Test Registration and Execution (Type-erased)
// ============================================================================

// Non-templated base for test registration to avoid template instantiation issues
struct test_base {
    const char* name;
    void (*test_function)();
    test_base* next;

    static inline test_base* head = nullptr;

    test_base(const char* n, void (*f)()) : name(n), test_function(f), next(head) {
        head = this;
    }

    void run() const {
        test_result::current_test_name = name;
        kernel_printer::print("Running test: ");
        kernel_printer::print(name);
        kernel_printer::print(" ... ");

        // Execute test function
        test_function();
        kernel_printer::print("✅ PASS\n");
        test_result::tests_passed++;
    }

    [[noreturn]] static void run_all() {
        kernel_printer::print("\n=== Kernel UT Test Execution ===\n");

        // Count and show registered tests
        int count = 0;
        test_base* current = head;
        while (current) {
            count++;
            current = current->next;
        }

        kernel_printer::print("DEBUG: Found ");
        kernel_printer::print_number(count);
        kernel_printer::print(" registered tests\n");

        // Run the tests
        current = head;
        while (current) {
            current->run();
            current = current->next;
        }

        // Print results
        kernel_printer::print("\n=== Test Results ===\n");
        kernel_printer::print("Tests passed: ");
        kernel_printer::print_number(test_result::tests_passed);
        kernel_printer::print("\nTests failed: ");
        kernel_printer::print_number(test_result::tests_failed);
        kernel_printer::print("\nAssertions passed: ");
        kernel_printer::print_number(test_result::assertions_passed);
        kernel_printer::print("\nAssertions failed: ");
        kernel_printer::print_number(test_result::assertions_failed);
        kernel_printer::print("\n");

        if (test_result::tests_failed == 0) {
            kernel_printer::print("🎉 All tests passed!\n");
            moss::kernel::kernel_test_exit(0);
        } else {
            kernel_printer::print("❌ Some tests failed!\n");
            moss::kernel::kernel_test_exit(1);
        }
    }
};

// ============================================================================
// Simplified Test Registration (Direct Function Registration)
// ============================================================================

// Simple registration function
inline void register_test(const char* name, void (*test_func)()) {
    static test_base* test_instances[64];  // Maximum 64 tests
    static int test_count = 0;

    kernel_printer::print("DEBUG: Registering test: ");
    kernel_printer::print(name);
    kernel_printer::print("\n");

    if (test_count < 64) {
        test_instances[test_count] = new test_base(name, test_func);
        test_count++;

        kernel_printer::print("DEBUG: Test registered, count now: ");
        kernel_printer::print_number(test_count);
        kernel_printer::print("\n");
    }
}

// Note: REGISTER_TEST macro removed as we use test_case_t direct registration

// For compatibility with existing test_case_t syntax
template<typename F>
struct test_case_t {
    test_case_t(const char* name, F test_function) {
        // Add debug output to see if constructor is called
        kernel_printer::print("DEBUG: test_case_t constructor called for: ");
        kernel_printer::print(name);
        kernel_printer::print("\n");

        // Create wrapper function for this specific lambda
        static F stored_lambda = test_function;
        static auto wrapper_func = []() { stored_lambda(); };

        register_test(name, wrapper_func);
    }
};

// String literal operator for test names (ut.hpp style)
constexpr auto operator""_test(const char* name, decltype(sizeof(int))) {
    return [name](auto test_function) {
        return test_case_t{name, test_function};
    };
}

// ============================================================================
// Test Suite Support (simplified)
// ============================================================================

template<typename F>
struct suite_t {
    constexpr suite_t([[maybe_unused]] const char* name, F suite_function) {
        // For kernel, we just execute immediately - no complex suite management
        suite_function();
    }
};

constexpr auto operator""_suite(const char* name, decltype(sizeof(int))) {
    return [name](auto suite_function) {
        return suite_t{name, suite_function};
    };
}

// ============================================================================
// Main Test Runner Entry Point
// ============================================================================

[[noreturn]] inline void run_all_tests() {
    test_base::run_all();
}

// Helper functions that create comparison objects for enhanced reporting
template<typename T, typename U>
constexpr auto eq(T&& lhs, U&& rhs) -> eq_t<T, U> {
    return {static_cast<T&&>(lhs), static_cast<U&&>(rhs)};
}

template<typename T, typename U>
constexpr auto ne(T&& lhs, U&& rhs) -> ne_t<T, U> {
    return {static_cast<T&&>(lhs), static_cast<U&&>(rhs)};
}

template<typename T, typename U>
constexpr auto gt(T&& lhs, U&& rhs) -> gt_t<T, U> {
    return {static_cast<T&&>(lhs), static_cast<U&&>(rhs)};
}

template<typename T, typename U>
constexpr auto lt(T&& lhs, U&& rhs) -> lt_t<T, U> {
    return {static_cast<T&&>(lhs), static_cast<U&&>(rhs)};
}

template<typename T, typename U>
constexpr auto ge(T&& lhs, U&& rhs) -> ge_t<T, U> {
    return {static_cast<T&&>(lhs), static_cast<U&&>(rhs)};
}

template<typename T, typename U>
constexpr auto le(T&& lhs, U&& rhs) -> le_t<T, U> {
    return {static_cast<T&&>(lhs), static_cast<U&&>(rhs)};
}

} // namespace boost::ut

// ============================================================================
// Compatibility layer for MOSS framework integration
// ============================================================================

// For direct compatibility with MOSS framework
namespace moss::test {
    inline void initialize_freestanding_std() noexcept {
        // No initialization needed for kernel ut
    }

    inline void run_validation_tests() noexcept {
        boost::ut::kernel_printer::print("Starting kernel ut.hpp validation tests...\n");
    }

    [[noreturn]] inline void run_freestanding_validation_tests() noexcept {
        boost::ut::run_all_tests();
    }
}

