/**
 * @file boost_ut_demo.cpp
 * @brief Boost.UT compatibility demo with sum example
 */

#include "../framework/moss_ut.hpp"

// Import ut.hpp symbols for this test file
using namespace boost::ut;

// Force test registration from this file
extern "C" void force_boost_ut_demo_registration();

// ============================================================================
// Boost.UT Compatible Syntax Demo
// ============================================================================

static test_case_t boost_ut_demo_test{"boost_ut_demo_test", [] {
    kernel_printer::print("\n=== Boost.UT Compatible Demo ===\n");
    kernel_printer::print("Running test \"sum\"...\n");

    // Simple sum function for testing (variadic template)
    auto sum = [](auto... values) { return (values + ...); };

    // Test cases that should pass
    kernel_printer::print("Testing: sum(0) == 0_i\n");
    expect(sum(0) == 0_i);

    kernel_printer::print("Testing: sum(1, 2) == 3_i\n");
    expect(sum(1, 2) == 3_i);

    kernel_printer::print("✅ First two assertions passed\n");

    // Test case that should fail - demonstrates boost::ut style output
    kernel_printer::print("Testing complex expression: sum(1, 2) > 0_i and 41_i == sum(40, 2)\n");
    kernel_printer::print("Expected to fail with condition display...\n");

    expect(sum(1, 2) > 0_i and 41_i == sum(40, 2));  // This will fail: 41 == 42

    kernel_printer::print("✅ Demo test completed\n");
}};

extern "C" void force_boost_ut_demo_registration() {
    using namespace boost::ut;

    kernel_printer::print("DEBUG: Registering boost::ut demo test\n");

    register_test("boost_ut_syntax_demo", []() {
        kernel_printer::print("\n=== Boost.UT Compatible Syntax Demo ===\n");

        // Test with _i literals and simple comparisons
        kernel_printer::print("Running test \"sum\"...\n");

        // Simple sum function for testing
        auto sum = [](auto... values) { return (values + ...); };

        // Test cases that should pass
        expect(sum(0) == 0_i);
        expect(sum(1, 2) == 3_i);

        kernel_printer::print("✅ First two assertions passed\n");

        // Test case that should fail - demonstrates boost::ut style output
        kernel_printer::print("🧪 Now testing complex expression that will fail...\n");
        expect(sum(1, 2) > 0_i and 41_i == sum(40, 2));  // This will fail: 41 == 42

        kernel_printer::print("🔍 Boost.UT syntax demonstration complete\n");
    });

    kernel_printer::print("DEBUG: Boost.UT demo test registered\n");
}
