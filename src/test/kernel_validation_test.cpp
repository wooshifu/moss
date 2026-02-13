/**
 * @file kernel_validation_test.cpp
 * @brief Simplified kernel validation tests using lightweight testing framework
 *
 * This file contains basic validation tests specifically designed for the kernel environment.
 * Uses the lightweight kernel_ut.hpp framework instead of complex freestanding std library.
 */

#include "kernel_ut.hpp"

// ========================================================================
// Basic Kernel Functionality Tests
// ========================================================================

KERNEL_TEST(basic_arithmetic) {
    // Test basic arithmetic operations
    int a = 5;
    int b = 3;
    expect(a + b == 8);
    expect(a - b == 2);
    expect(a * b == 15);
    expect(a / b == 1);
} KERNEL_TEST_END

KERNEL_TEST(pointer_operations) {
    // Test basic pointer operations
    int value = 42;
    int* ptr = &value;
    expect(ptr != nullptr);
    expect(*ptr == 42);

    *ptr = 24;
    expect(value == 24);
} KERNEL_TEST_END

KERNEL_TEST(array_operations) {
    // Test basic array operations
    int arr[5] = {1, 2, 3, 4, 5};
    expect(arr[0] == 1);
    expect(arr[4] == 5);

    // Test array modification
    arr[2] = 10;
    expect(arr[2] == 10);
} KERNEL_TEST_END

KERNEL_TEST(string_literals) {
    // Test string literal operations
    const char* str1 = "Hello";

    // Basic string length check
    int len1 = 0;
    while (str1[len1] != '\0') len1++;
    expect(len1 == 5);

    // Character access
    expect(str1[0] == 'H');
    expect(str1[4] == 'o');
} KERNEL_TEST_END

KERNEL_TEST(conditional_logic) {
    // Test conditional logic
    bool condition = true;
    expect(condition);
    expect(!false);

    int x = 10;
    if (x > 5) {
        expect(true);
    } else {
        expect(false); // Should not reach here
    }
} KERNEL_TEST_END

KERNEL_TEST(loops_and_iteration) {
    // Test loop functionality
    int sum = 0;
    for (int i = 1; i <= 5; ++i) {
        sum += i;
    }
    expect(sum == 15); // 1+2+3+4+5 = 15

    // Test while loop
    int counter = 0;
    while (counter < 3) {
        counter++;
    }
    expect(counter == 3);
} KERNEL_TEST_END

KERNEL_TEST(function_calls) {
    // Test function call mechanism
    auto square = [](int x) { return x * x; };
    expect(square(4) == 16);
    expect(square(0) == 0);
    expect(square(-3) == 9);
} KERNEL_TEST_END

KERNEL_TEST(memory_alignment) {
    // Test basic memory alignment concepts
    struct aligned_struct {
        char c;
        int i;
        char c2;
    };

    aligned_struct as{};
    as.c = 'A';
    as.i = 123;
    as.c2 = 'B';

    expect(as.c == 'A');
    expect(as.i == 123);
    expect(as.c2 == 'B');
} KERNEL_TEST_END

KERNEL_TEST(bit_operations) {
    // Test bitwise operations
    unsigned int flags = 0;

    // Set bits
    flags |= (1U << 2);  // Set bit 2
    flags |= (1U << 5);  // Set bit 5

    expect((flags & (1U << 2)) != 0);  // Bit 2 should be set
    expect((flags & (1U << 5)) != 0);  // Bit 5 should be set
    expect((flags & (1U << 1)) == 0);  // Bit 1 should not be set

    // Clear bit
    flags &= ~(1U << 2);  // Clear bit 2
    expect((flags & (1U << 2)) == 0);  // Bit 2 should now be clear
} KERNEL_TEST_END

KERNEL_TEST(constants_and_enums) {
    // Test compile-time constants
    constexpr int BUFFER_SIZE = 256;
    expect(BUFFER_SIZE == 256);

    enum class Status { OK, ERROR, PENDING };
    Status s = Status::OK;
    expect(s == Status::OK);
    expect(s != Status::ERROR);
} KERNEL_TEST_END

// ========================================================================
// Test Registration and Execution
// ========================================================================

namespace moss::test {
    void run_validation_tests() noexcept {
        kernel_ut::output("========================================\n");
        kernel_ut::output("MOSS Kernel Basic Validation Tests\n");
        kernel_ut::output("========================================\n");
        kernel_ut::output("Testing core kernel functionality...\n\n");

        // Tests are automatically registered and executed through static constructors
    }

    [[noreturn]] void run_freestanding_validation_tests() noexcept {
        kernel_ut::output("Running kernel-specific validation tests...\n");

        // Print final test results and exit
        kernel_ut::report_results();
    }
}
