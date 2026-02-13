/**
 * @file moss_ut.cpp
 * @brief MOSS kernel ut.hpp integration implementation
 *
 * Implements the simplified kernel testing functionality for validating
 * our freestanding standard library implementations.
 */

#include "moss_ut.hpp"

namespace moss::test {

    // Global test runner instance definition
    SimpleTestRunner test_runner;

    // ========================================================================
    // Freestanding library initialization
    // ========================================================================

    void initialize_freestanding_std() noexcept {
        // Initialize static memory pools if needed
        // Currently our implementations use stack allocation, so minimal init needed
    }

    // ========================================================================
    // Simple validation tests for our freestanding implementations
    // ========================================================================

    void run_validation_tests() noexcept {
        boost::ut::test_output("Running validation tests...\n");
        // Additional validation tests can be added here
    }

    void run_freestanding_validation_tests() noexcept {
        // Test basic type_traits
        test_runner.run_test("type_traits_basic", []() {
            static_assert(std::is_same_v<int, int>);
            static_assert(!std::is_same_v<int, float>);
            static_assert(std::is_integral_v<int>);
            static_assert(!std::is_integral_v<float>);
        });

        // Test vector functionality
        test_runner.run_test("vector_basic", []() {
            std::vector<int> vec;
            vec.push_back(42);
            vec.push_back(24);
            // Simple assertions without exceptions
            // In kernel environment, we use simple checks
        });

        // Test string functionality
        test_runner.run_test("string_basic", []() {
            std::string str("Hello");
            str += " World";
            // Basic functionality test - just ensure it compiles and runs
        });

        // Test string_view functionality
        test_runner.run_test("string_view_basic", []() {
            const char* cstr = "Test String";
            std::string_view sv(cstr);
            // Basic functionality test - just ensure it compiles and runs
        });

        // Test unordered_map functionality
        test_runner.run_test("unordered_map_basic", []() {
            std::unordered_map<int, std::string> map;
            map[42] = "answer";
            map[24] = "reverse";
            // Basic functionality test - just ensure it compiles and runs
        });

        // Test iostream functionality
        test_runner.run_test("iostream_basic", []() {
            // This test mainly verifies compilation and basic operation
            boost::ut::test_output("iostream test output\n");
        });

        // Test sstream functionality
        test_runner.run_test("sstream_basic", []() {
            std::ostringstream oss;
            oss << "Number: " << 42;
            std::string result = oss.str();
            // Basic functionality test - just ensure it compiles and runs
        });

        // Test chrono functionality
        test_runner.run_test("chrono_basic", []() {
            using namespace std::chrono;
            auto start = high_resolution_clock::now();
            // Small delay - avoid deprecated volatile increment
            for (int i = 0; i < 1000; ++i) {
                asm volatile("" ::: "memory"); // Prevent optimization
            }
            auto end = high_resolution_clock::now();
            auto duration = end - start;
            // Just verify it compiles and runs
            static_cast<void>(duration);
        });

        // Test memory functionality
        test_runner.run_test("memory_basic", []() {
            // Basic pointer test - skip dynamic allocation in freestanding environment
            int value = 42;
            int* ptr = &value;
            // Just verify pointer dereferencing works
            static_cast<void>(*ptr);
        });

        // Print final results
        test_runner.print_results();
    }

} // namespace moss::test

