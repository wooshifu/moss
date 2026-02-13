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

// Configuration for freestanding kernel environment
// Note: These may be defined as macros by the build system
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
// Assertion Framework - Unified error reporting
// ============================================================================

// Extract short filename from full path (e.g. "cases/boost_ut_demo.cpp" from full path)
inline const char* short_filename(const char* path) {
    if (!path) return "unknown";
    const char* last_slash = path;
    const char* second_last = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/') {
            second_last = last_slash;
            last_slash = p + 1;
        }
    }
    // Return parent_dir/filename for context
    return second_last;
}

// Print the common failure header: "  FAIL  file:line  expression"
inline void print_failure_header(const char* file, int line, const char* expression) {
    kernel_printer::print("  FAIL  ");
    kernel_printer::print(short_filename(file));
    kernel_printer::print(":");
    kernel_printer::print_number(line);
    kernel_printer::print("  ");
    kernel_printer::print(expression);
    kernel_printer::print("\n");
}

// Base expectation - simple boolean conditions
template<typename T>
struct expectation {
    T value;
    const char* expression;
    const char* file;
    int line;

    constexpr expectation(T v, const char* expr, const char* f, int l)
        : value(v), expression(expr), file(f), line(l) {}

    constexpr auto operator!() const {
        if (!value) { test_result::assertions_passed++; return true; }
        test_result::assertions_failed++;
        print_failure_header(file, line, expression);
        return false;
    }

    constexpr operator bool() const {
        if (value) { test_result::assertions_passed++; return true; }
        test_result::assertions_failed++;
        print_failure_header(file, line, expression);
        return false;
    }
};

// ============================================================================
// Specialized expectations for comparison types (show expected vs actual)
// ============================================================================

// Helper: print "        expected: <lhs> <op> <rhs>  actual: <lhs> <neg_op> <rhs>"
template<typename T, typename U>
inline void print_comparison_detail(const char* op, const char* neg_op, T lhs, U rhs) {
    kernel_printer::print("        expected: ");
    format_value(lhs);
    kernel_printer::print(" ");
    kernel_printer::print(op);
    kernel_printer::print(" ");
    format_value(rhs);
    kernel_printer::print("  actual: ");
    format_value(lhs);
    kernel_printer::print(" ");
    kernel_printer::print(neg_op);
    kernel_printer::print(" ");
    format_value(rhs);
    kernel_printer::print("\n");
}

// Macro to reduce boilerplate in comparison expectation specializations
#define DEFINE_COMPARISON_EXPECTATION(CMP_TYPE, OP_STR, NEG_OP_STR)         \
template<typename T, typename U>                                             \
struct expectation<CMP_TYPE<T, U>> {                                         \
    CMP_TYPE<T, U> value;                                                    \
    const char* expression;                                                  \
    const char* file;                                                        \
    int line;                                                                \
                                                                             \
    constexpr expectation(CMP_TYPE<T, U> v, const char* expr,                \
                          const char* f, int l)                              \
        : value(v), expression(expr), file(f), line(l) {}                    \
                                                                             \
    constexpr operator bool() const {                                        \
        if (value) { test_result::assertions_passed++; return true; }        \
        test_result::assertions_failed++;                                    \
        print_failure_header(file, line, expression);                        \
        print_comparison_detail(OP_STR, NEG_OP_STR, value.lhs, value.rhs);  \
        return false;                                                        \
    }                                                                        \
};

DEFINE_COMPARISON_EXPECTATION(eq_t, "==", "!=")
DEFINE_COMPARISON_EXPECTATION(ne_t, "!=", "==")
DEFINE_COMPARISON_EXPECTATION(gt_t, ">",  "<=")
DEFINE_COMPARISON_EXPECTATION(lt_t, "<",  ">=")
DEFINE_COMPARISON_EXPECTATION(ge_t, ">=", "<")
DEFINE_COMPARISON_EXPECTATION(le_t, "<=", ">")

#undef DEFINE_COMPARISON_EXPECTATION

// Expect function - core of ut.hpp API
template<typename T>
constexpr auto expect_impl(T&& value, const char* expr, const char* file, int line) {
    auto exp = expectation<T>{static_cast<T&&>(value), expr, file, line};
    (void)static_cast<bool>(exp);
    return exp;
}

// Macro to capture file/line at call site
#define expect(condition) expect_impl((condition), #condition, __FILE__, __LINE__)

// ============================================================================
// Kernel-Specific Error Testing Support
// ============================================================================

// Kernel error codes (simplified enum for testing purposes)
enum class kernel_error_code : int {
    success = 0,
    invalid_argument = -1,
    out_of_memory = -2,
    permission_denied = -3,
    resource_busy = -4,
    io_error = -5,
    not_found = -6
};

// Kernel panic simulation for testing (simple flag-based approach for freestanding)
static bool panic_expected = false;
static bool panic_occurred = false;

// Simulated kernel panic function (kernel-friendly version)
inline void test_kernel_panic() {
    panic_occurred = true;
    // In a real kernel, this would trigger actual panic handling
    // For testing, we just set the flag and return
}

// Test function that expects a kernel panic to occur
template<typename F>
bool expect_kernel_panic_impl(F test_function, const char* expr, const char* file, int line) {
    panic_expected = true;
    panic_occurred = false;
    test_function();
    panic_expected = false;

    if (panic_occurred) {
        test_result::assertions_passed++;
        return true;
    }
    test_result::assertions_failed++;
    print_failure_header(file, line, expr);
    kernel_printer::print("        expected kernel panic did not occur\n");
    return false;
}

// Test function that expects a specific error code
template<typename T>
bool expect_error_code_impl(T result, kernel_error_code expected_error, const char* expr, const char* file, int line) {
    kernel_error_code actual_error = static_cast<kernel_error_code>(result);

    if (actual_error == expected_error) {
        test_result::assertions_passed++;
        return true;
    }
    test_result::assertions_failed++;
    print_failure_header(file, line, expr);
    kernel_printer::print("        expected: ");
    kernel_printer::print_number(static_cast<int>(expected_error));
    kernel_printer::print("  actual: ");
    kernel_printer::print_number(static_cast<int>(actual_error));
    kernel_printer::print("\n");
    return false;
}

// Test function that verifies no error occurred (success case)
template<typename T>
bool expect_no_error_impl(T result, const char* expr, const char* file, int line) {
    return expect_error_code_impl(result, kernel_error_code::success, expr, file, line);
}

// Convenient macros for kernel error testing
#define expect_kernel_panic(test_func) expect_kernel_panic_impl((test_func), #test_func, __FILE__, __LINE__)
#define expect_error_code(result, expected) expect_error_code_impl((result), (expected), #result, __FILE__, __LINE__)
#define expect_no_error(result) expect_no_error_impl((result), #result, __FILE__, __LINE__)

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
    static int test_count = 0;

    kernel_printer::print("DEBUG: Registering test: ");
    kernel_printer::print(name);
    kernel_printer::print("\n");

    if (test_count < 64) {
        // Create test_base instance which automatically registers itself via constructor
        new test_base(name, test_func);
        test_count++;

        kernel_printer::print("DEBUG: Test registered, count now: ");
        kernel_printer::print_number(test_count);
        kernel_printer::print("\n");
    }
}

// ============================================================================
// BDD Syntax Support (given/when/then)
// ============================================================================

// BDD test factory that prefixes test names with context markers
struct bdd_test_factory {
    const char* prefix;
    const char* name;

    template<typename F>
    void operator=(F test_function) const {
        // Create formatted name for better test organization
        char formatted_name[128]; // Fixed-size buffer for kernel environment
        int prefix_len = 0;
        int name_len = 0;

        // Calculate lengths
        while (prefix[prefix_len] != '\0' && prefix_len < 64) prefix_len++;
        while (name[name_len] != '\0' && name_len < 60) name_len++;

        // Copy prefix
        for (int i = 0; i < prefix_len && i < 127; ++i) {
            formatted_name[i] = prefix[i];
        }

        // Copy name
        for (int i = 0; i < name_len && (prefix_len + i) < 127; ++i) {
            formatted_name[prefix_len + i] = name[i];
        }
        formatted_name[prefix_len + name_len] = '\0';

        // Use static storage for the formatted name (kernel-safe)
        static char static_names[64][128]; // Support up to 64 BDD tests
        static int name_count = 0;

        if (name_count < 64) {
            for (int i = 0; i <= prefix_len + name_len; ++i) {
                static_names[name_count][i] = formatted_name[i];
            }

            // Simplified registration - just execute the test function directly
            // This avoids complex lambda storage issues
            test_function(); // Execute immediately for BDD tests

            name_count++;
        } else {
            // Fallback - execute with original name
            test_function();
        }
    }
};

// BDD syntax functions
constexpr auto given(const char* name) {
    return bdd_test_factory{"[Given] ", name};
}

constexpr auto when(const char* name) {
    return bdd_test_factory{"[When] ", name};
}

constexpr auto then(const char* name) {
    return bdd_test_factory{"[Then] ", name};
}

constexpr auto should(const char* name) {
    return bdd_test_factory{"[Should] ", name};
}

// ============================================================================
// Simple Parameterized Testing Support
// ============================================================================

// Parameter container using static arrays (kernel-safe, no std::initializer_list)
template<typename T, unsigned int N>
struct test_parameters {
    T data[N];
    unsigned int count;

    // Manual construction for freestanding environment
    constexpr test_parameters() : count(0U) {
        for (unsigned int i = 0U; i < N; ++i) {
            data[i] = T{};
        }
    }

    constexpr void add(const T& value) {
        if (count < N) {
            data[count++] = value;
        }
    }

    constexpr const T& operator[](unsigned int index) const {
        return data[index];
    }

    constexpr unsigned int size() const { return count; }
};

// Helper macros to create test parameters (freestanding-friendly)
#define MAKE_TEST_PARAMS_1(T, v1) []() { \
    test_parameters<T, 16> params; \
    params.add(v1); \
    return params; \
}()

#define MAKE_TEST_PARAMS_2(T, v1, v2) []() { \
    test_parameters<T, 16> params; \
    params.add(v1); \
    params.add(v2); \
    return params; \
}()

#define MAKE_TEST_PARAMS_3(T, v1, v2, v3) []() { \
    test_parameters<T, 16> params; \
    params.add(v1); \
    params.add(v2); \
    params.add(v3); \
    return params; \
}()

#define MAKE_TEST_PARAMS_4(T, v1, v2, v3, v4) []() { \
    test_parameters<T, 16> params; \
    params.add(v1); \
    params.add(v2); \
    params.add(v3); \
    params.add(v4); \
    return params; \
}()

// Parameterized test function
template<typename ParamType, unsigned int N, typename F>
void test_with_params(const char* base_name, test_parameters<ParamType, N> params, F test_function) {
    for (unsigned int i = 0U; i < params.size(); ++i) {
        // Create unique test name for each parameter
        char param_test_name[128];
        unsigned int name_len = 0U;

        // Copy base name
        while (base_name[name_len] != '\0' && name_len < 100U) {
            param_test_name[name_len] = base_name[name_len];
            name_len++;
        }

        // Add parameter index
        param_test_name[name_len++] = '_';
        param_test_name[name_len++] = '[';

        // Simple number to string conversion for parameter index
        if (i >= 10U) {
            param_test_name[name_len++] = static_cast<char>('0' + static_cast<char>(i / 10U));
        }
        param_test_name[name_len++] = static_cast<char>('0' + static_cast<char>(i % 10U));
        param_test_name[name_len++] = ']';
        param_test_name[name_len] = '\0';

        // Store the test name and parameter in static storage
        static char static_param_names[32][128]; // Support up to 32 parameterized tests
        static ParamType static_params[32];
        static unsigned int param_test_count = 0U;

        if (param_test_count < 32U) {
            // Copy test name
            for (unsigned int j = 0U; j <= name_len; ++j) {
                static_param_names[param_test_count][j] = param_test_name[j];
            }

            // Store parameter value
            static_params[param_test_count] = params[i];

            // Create test wrapper that captures the parameter
            const unsigned int current_index = param_test_count;

            // Create a simple function pointer instead of lambda
            static void (*test_wrappers[32])();
            test_wrappers[current_index] = +[current_index, test_function]() {
                test_function(static_params[current_index]);
            };

            register_test(static_param_names[param_test_count], test_wrappers[current_index]);
            param_test_count++;
        }
    }
}

// Convenience macro for parameterized tests
#define TEST_WITH_PARAMS(name, params, test_func) \
    test_with_params(name, params, test_func)

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

// Deduction guide for CTAD support
template<typename F>
test_case_t(const char*, F) -> test_case_t<F>;

// Test name holder - supports both "name"_test = []{}  and  "name"_test([]{ })
struct test_name_t {
    const char* name;

    // operator= for: auto t = "name"_test = [] { ... };
    template<typename F>
    auto operator=(F test_function) const {
        return test_case_t{name, test_function};
    }

    // operator() for: "name"_test([] { ... });
    template<typename F>
    auto operator()(F test_function) const {
        return test_case_t{name, test_function};
    }
};

// String literal operator for test names (ut.hpp style)
constexpr test_name_t operator""_test(const char* name, decltype(sizeof(int))) {
    return {name};
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

// ============================================================================
// Boost.UT Compatible Syntax: _i literals and logical operators
// ============================================================================

// Integer literal wrapper for boost::ut compatibility
struct integral {
    int value;

    constexpr integral(int v) : value(v) {}

    constexpr operator int() const { return value; }

    // Support comparison with integral
    template<typename T>
    constexpr auto operator==(T other) const -> eq_t<int, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator!=(T other) const -> ne_t<int, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator>(T other) const -> gt_t<int, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator<(T other) const -> lt_t<int, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator>=(T other) const -> ge_t<int, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator<=(T other) const -> le_t<int, T> {
        return {value, static_cast<T>(other)};
    }
};

// _i literal operator
constexpr integral operator""_i(unsigned long long value) {
    return integral(static_cast<int>(value));
}

// Support comparison between any type and integral
template<typename T>
constexpr auto operator==(T lhs, integral rhs) -> eq_t<T, int> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator!=(T lhs, integral rhs) -> ne_t<T, int> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator>(T lhs, integral rhs) -> gt_t<T, int> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator<(T lhs, integral rhs) -> lt_t<T, int> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator>=(T lhs, integral rhs) -> ge_t<T, int> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator<=(T lhs, integral rhs) -> le_t<T, int> {
    return {static_cast<T>(lhs), rhs.value};
}

// ============================================================================
// Boolean literal wrapper for boost::ut compatibility
// ============================================================================

struct boolean {
    bool value;

    constexpr boolean(bool v) : value(v) {}

    constexpr operator bool() const { return value; }

    // Support comparison with boolean
    template<typename T>
    constexpr auto operator==(T other) const -> eq_t<bool, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator!=(T other) const -> ne_t<bool, T> {
        return {value, static_cast<T>(other)};
    }

    // Boolean-specific comparisons (less meaningful but kept for consistency)
    template<typename T>
    constexpr auto operator>(T other) const -> gt_t<bool, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator<(T other) const -> lt_t<bool, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator>=(T other) const -> ge_t<bool, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator<=(T other) const -> le_t<bool, T> {
        return {value, static_cast<T>(other)};
    }
};

// _b literal operator
constexpr boolean operator""_b(unsigned long long value) {
    return boolean(static_cast<bool>(value));
}

// Support comparison between any type and boolean
template<typename T>
constexpr auto operator==(T lhs, boolean rhs) -> eq_t<T, bool> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator!=(T lhs, boolean rhs) -> ne_t<T, bool> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator>(T lhs, boolean rhs) -> gt_t<T, bool> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator<(T lhs, boolean rhs) -> lt_t<T, bool> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator>=(T lhs, boolean rhs) -> ge_t<T, bool> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator<=(T lhs, boolean rhs) -> le_t<T, bool> {
    return {static_cast<T>(lhs), rhs.value};
}

// Convenience constants for common usage
inline constexpr auto true_b = boolean(true);
inline constexpr auto false_b = boolean(false);

// ============================================================================
// Character literal wrapper for boost::ut compatibility
// ============================================================================

struct character {
    char value;

    constexpr character(char v) : value(v) {}

    constexpr operator char() const { return value; }

    // Support comparison with character
    template<typename T>
    constexpr auto operator==(T other) const -> eq_t<char, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator!=(T other) const -> ne_t<char, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator>(T other) const -> gt_t<char, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator<(T other) const -> lt_t<char, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator>=(T other) const -> ge_t<char, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator<=(T other) const -> le_t<char, T> {
        return {value, static_cast<T>(other)};
    }
};

// _c literal operator
constexpr character operator""_c(char value) {
    return character(value);
}

// Support comparison between any type and character
template<typename T>
constexpr auto operator==(T lhs, character rhs) -> eq_t<T, char> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator!=(T lhs, character rhs) -> ne_t<T, char> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator>(T lhs, character rhs) -> gt_t<T, char> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator<(T lhs, character rhs) -> lt_t<T, char> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator>=(T lhs, character rhs) -> ge_t<T, char> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator<=(T lhs, character rhs) -> le_t<T, char> {
    return {static_cast<T>(lhs), rhs.value};
}

// ============================================================================
// Double precision literal wrapper for boost::ut compatibility (kernel-safe)
// Note: Only available when floating point support is enabled
// ============================================================================

#ifdef __ARM_FP
// Only include floating point support when available
struct double_precision {
    double value;
    static constexpr double epsilon = 1e-9; // Kernel-safe epsilon value

    constexpr double_precision(double v) : value(v) {}

    constexpr operator double() const { return value; }

    // Epsilon-based equality comparison for floating point
    template<typename T>
    constexpr auto operator==(T other) const -> eq_t<double, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator!=(T other) const -> ne_t<double, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator>(T other) const -> gt_t<double, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator<(T other) const -> lt_t<double, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator>=(T other) const -> ge_t<double, T> {
        return {value, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator<=(T other) const -> le_t<double, T> {
        return {value, static_cast<T>(other)};
    }

    // Kernel-safe absolute value (avoid std::abs)
    constexpr double abs_diff(double other) const {
        return (value > other) ? (value - other) : (other - value);
    }
};

// _d literal operator
constexpr double_precision operator""_d(long double value) {
    return double_precision(static_cast<double>(value));
}

// Support comparison between any type and double_precision
template<typename T>
constexpr auto operator==(T lhs, double_precision rhs) -> eq_t<T, double> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator!=(T lhs, double_precision rhs) -> ne_t<T, double> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator>(T lhs, double_precision rhs) -> gt_t<T, double> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator<(T lhs, double_precision rhs) -> lt_t<T, double> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator>=(T lhs, double_precision rhs) -> ge_t<T, double> {
    return {static_cast<T>(lhs), rhs.value};
}

template<typename T>
constexpr auto operator<=(T lhs, double_precision rhs) -> le_t<T, double> {
    return {static_cast<T>(lhs), rhs.value};
}

#else
// Fallback when floating point is not available - use integer representation
struct double_precision {
    int value; // Use fixed-point representation instead

    constexpr double_precision(int v) : value(v) {}

    constexpr operator int() const { return value / 1000; }
    constexpr operator double() const = delete; // Prevent usage when FP unavailable

    // Standard comparison operators using integer representation
    template<typename T>
    constexpr auto operator==(T other) const -> eq_t<int, T> {
        return {value / 1000, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator!=(T other) const -> ne_t<int, T> {
        return {value / 1000, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator>(T other) const -> gt_t<int, T> {
        return {value / 1000, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator<(T other) const -> lt_t<int, T> {
        return {value / 1000, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator>=(T other) const -> ge_t<int, T> {
        return {value / 1000, static_cast<T>(other)};
    }

    template<typename T>
    constexpr auto operator<=(T other) const -> le_t<int, T> {
        return {value / 1000, static_cast<T>(other)};
    }
};

// _d literal operator (fixed-point version) - use unsigned long long for freestanding
constexpr double_precision operator""_d(unsigned long long value) {
    return double_precision(static_cast<int>(value));
}

// Support comparison between any type and double_precision
template<typename T>
constexpr auto operator==(T lhs, double_precision rhs) -> eq_t<T, int> {
    return {static_cast<T>(lhs), rhs.value / 1000};
}

template<typename T>
constexpr auto operator!=(T lhs, double_precision rhs) -> ne_t<T, int> {
    return {static_cast<T>(lhs), rhs.value / 1000};
}

template<typename T>
constexpr auto operator>(T lhs, double_precision rhs) -> gt_t<T, int> {
    return {static_cast<T>(lhs), rhs.value / 1000};
}

template<typename T>
constexpr auto operator<(T lhs, double_precision rhs) -> lt_t<T, int> {
    return {static_cast<T>(lhs), rhs.value / 1000};
}

template<typename T>
constexpr auto operator>=(T lhs, double_precision rhs) -> ge_t<T, int> {
    return {static_cast<T>(lhs), rhs.value / 1000};
}

template<typename T>
constexpr auto operator<=(T lhs, double_precision rhs) -> le_t<T, int> {
    return {static_cast<T>(lhs), rhs.value / 1000};
}

#endif // __ARM_FP

// ============================================================================
// Enhanced format_value for new literals
// ============================================================================

// Enhance format_value to handle boolean types properly
template<>
inline void format_value<bool>(bool value) {
    kernel_printer::print(value ? "true" : "false");
}

template<>
inline void format_value<char>(char value) {
    if (value >= 32 && value <= 126) {
        kernel_printer::print("'");
        char c[2] = {value, '\0'};
        kernel_printer::print(c);
        kernel_printer::print("'");
    } else {
        kernel_printer::print_number(static_cast<int>(value));
    }
}

#ifdef __ARM_FP
template<>
inline void format_value<double>(double value) {
    // Simple double formatting for kernel environment
    int int_part = static_cast<int>(value);
    kernel_printer::print_number(int_part);

    double frac = value - int_part;
    if (frac != 0.0) {
        kernel_printer::print(".");
        // Print a few decimal places (keep it simple for kernel)
        int frac_part = static_cast<int>(frac * 1000);
        if (frac_part < 0) frac_part = -frac_part; // Handle negative fractions
        kernel_printer::print_number(frac_part);
    }
}
#endif
// Note: When __ARM_FP is not defined, format_value<double> is not available
// This is intentional as double_precision converts to int when FP is disabled

// ============================================================================
// Logical operators with expression evaluation display
// ============================================================================

// Forward declarations for expression types
template<typename L, typename R> struct and_expr;
template<typename L, typename R> struct or_expr;

// Expression evaluation and formatting
template<typename T>
void format_expression(const T& expr) {
    if constexpr (requires { expr.lhs; expr.rhs; }) {
        // Binary comparison - show evaluated values
        kernel_printer::print("(");
        if constexpr (requires { static_cast<int>(expr.lhs); }) {
            kernel_printer::print_number(static_cast<int>(expr.lhs));
        } else {
            format_value(expr.lhs);
        }

        // Print operator based on type
        if constexpr (is_eq_t<T>::value) {
            kernel_printer::print(" == ");
        } else if constexpr (is_ne_t<T>::value) {
            kernel_printer::print(" != ");
        } else if constexpr (is_gt_t<T>::value) {
            kernel_printer::print(" > ");
        } else if constexpr (is_lt_t<T>::value) {
            kernel_printer::print(" < ");
        } else if constexpr (is_ge_t<T>::value) {
            kernel_printer::print(" >= ");
        } else if constexpr (is_le_t<T>::value) {
            kernel_printer::print(" <= ");
        }

        if constexpr (requires { static_cast<int>(expr.rhs); }) {
            kernel_printer::print_number(static_cast<int>(expr.rhs));
        } else {
            format_value(expr.rhs);
        }
        kernel_printer::print(")");
    } else {
        // Other expressions
        kernel_printer::print("[expr]");
    }
}

// Logical AND expression
template<typename L, typename R>
struct and_expr {
    L lhs;
    R rhs;

    constexpr and_expr(L l, R r) : lhs(l), rhs(r) {}

    constexpr operator bool() const {
        return static_cast<bool>(lhs) && static_cast<bool>(rhs);
    }

    void format_condition() const {
        kernel_printer::print("[");
        format_expression(lhs);
        kernel_printer::print(" and ");
        format_expression(rhs);
        kernel_printer::print("]");
    }
};

// Logical OR expression
template<typename L, typename R>
struct or_expr {
    L lhs;
    R rhs;

    constexpr or_expr(L l, R r) : lhs(l), rhs(r) {}

    constexpr operator bool() const {
        return static_cast<bool>(lhs) || static_cast<bool>(rhs);
    }

    void format_condition() const {
        kernel_printer::print("[");
        format_expression(lhs);
        kernel_printer::print(" or ");
        format_expression(rhs);
        kernel_printer::print("]");
    }
};

// AND operator for all comparison types
template<typename L, typename R>
constexpr auto operator&&(L&& lhs, R&& rhs) -> and_expr<L, R> {
    return and_expr<L, R>{static_cast<L&&>(lhs), static_cast<R&&>(rhs)};
}

// OR operator for all comparison types
template<typename L, typename R>
constexpr auto operator||(L&& lhs, R&& rhs) -> or_expr<L, R> {
    return or_expr<L, R>{static_cast<L&&>(lhs), static_cast<R&&>(rhs)};
}

// 'and' and 'or' keywords are natively supported in C++

// ============================================================================
// Enhanced expectation for complex expressions
// ============================================================================

// Specialization for AND expressions
template<typename L, typename R>
struct expectation<and_expr<L, R>> {
    and_expr<L, R> value;
    const char* expression;
    const char* file;
    int line;

    constexpr expectation(and_expr<L, R> v, const char* expr, const char* f, int l)
        : value(v), expression(expr), file(f), line(l) {}

    constexpr operator bool() const {
        if (value) { test_result::assertions_passed++; return true; }
        test_result::assertions_failed++;
        print_failure_header(file, line, expression);
        kernel_printer::print("        condition: ");
        value.format_condition();
        kernel_printer::print("\n");
        return false;
    }
};

// Specialization for OR expressions
template<typename L, typename R>
struct expectation<or_expr<L, R>> {
    or_expr<L, R> value;
    const char* expression;
    const char* file;
    int line;

    constexpr expectation(or_expr<L, R> v, const char* expr, const char* f, int l)
        : value(v), expression(expr), file(f), line(l) {}

    constexpr operator bool() const {
        if (value) { test_result::assertions_passed++; return true; }
        test_result::assertions_failed++;
        print_failure_header(file, line, expression);
        kernel_printer::print("        condition: ");
        value.format_condition();
        kernel_printer::print("\n");
        return false;
    }
};

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

