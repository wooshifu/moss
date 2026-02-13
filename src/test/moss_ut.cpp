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

    void run_freestanding_validation_tests() {
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
            std::cout << "iostream test output" << std::endl;
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
            // Small delay
            for (volatile int i = 0; i < 1000; ++i) {}
            auto end = high_resolution_clock::now();
            auto duration = end - start;
            // Just verify it compiles and runs
            static_cast<void>(duration);
        });

        // Test memory functionality
        test_runner.run_test("memory_basic", []() {
            auto ptr = std::make_unique<int>(42);
            // Basic functionality test - just ensure it compiles and runs
        });

        // Print final results
        test_runner.print_results();
    }

} // namespace moss::test