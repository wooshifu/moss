#pragma once

/**
 * @file moss_compat.hpp
 * @brief MOSS kernel test framework compatibility bridge
 *
 * Provides backward compatibility layer for migrating from existing MOSS test
 * macros to modern ut.hpp syntax. This allows incremental migration while
 * maintaining support for legacy test code.
 */

#include "moss_ut.hpp"

namespace moss::test {

    // ========================================================================
    // Legacy MOSS Test Framework Compatibility
    // ========================================================================

    /**
     * @brief Legacy test result tracking for MOSS compatibility
     */
    class LegacyTestTracker {
    private:
        static int total_tests_;
        static int passed_tests_;
        static int failed_tests_;
        static const char* current_suite_;

    public:
        static void start_suite(const char* suite_name) {
            current_suite_ = suite_name;
            std::cout << std::endl << "[SUITE] Starting " << suite_name << std::endl;
        }

        static void end_suite() {
            std::cout << "[SUITE] Completed " << current_suite_ << std::endl;
        }

        static void register_test_result(bool passed, const char* test_name) {
            ++total_tests_;
            if (passed) {
                ++passed_tests_;
                std::cout << "[PASS] " << test_name << std::endl;
            } else {
                ++failed_tests_;
                std::cout << "[FAIL] " << test_name << std::endl;
            }
        }

        static void print_summary() {
            std::cout << std::endl;
            std::cout << "========================================" << std::endl;
            std::cout << "MOSS Legacy Test Results Summary" << std::endl;
            std::cout << "========================================" << std::endl;
            std::cout << "Tests run: " << total_tests_ << std::endl;
            std::cout << "Passed:    " << passed_tests_ << std::endl;
            std::cout << "Failed:    " << failed_tests_ << std::endl;
            std::cout << "Success:   " << (failed_tests_ == 0 ? "YES" : "NO") << std::endl;
            std::cout << "========================================" << std::endl;

            // Exit with appropriate code
            if (failed_tests_ == 0) {
                kernel_test_exit_success();
            } else {
                kernel_test_exit_failure();
            }
        }

    private:
        static void kernel_test_exit_success() noexcept {
            std::cout.flush();
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

        static void kernel_test_exit_failure() noexcept {
            std::cout.flush();
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

    // Static member definitions
    int LegacyTestTracker::total_tests_ = 0;
    int LegacyTestTracker::passed_tests_ = 0;
    int LegacyTestTracker::failed_tests_ = 0;
    const char* LegacyTestTracker::current_suite_ = "Unknown";

} // namespace moss::test

// ========================================================================
// Legacy MOSS Test Framework Macros
// ========================================================================

/**
 * @brief Assertion macros compatible with existing MOSS tests
 *
 * These macros provide the same interface as the original MOSS test framework
 * but are implemented using our freestanding standard library.
 */

// Test assertion macros
#define MOSS_ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            std::cout << "[ASSERT_FAIL] " << __FILE__ << ":" << __LINE__ \
                      << " - Expected true, got false: " << #condition << std::endl; \
            moss::test::LegacyTestTracker::register_test_result(false, __func__); \
            return; \
        } \
    } while(0)

#define MOSS_ASSERT_FALSE(condition) \
    do { \
        if (condition) { \
            std::cout << "[ASSERT_FAIL] " << __FILE__ << ":" << __LINE__ \
                      << " - Expected false, got true: " << #condition << std::endl; \
            moss::test::LegacyTestTracker::register_test_result(false, __func__); \
            return; \
        } \
    } while(0)

#define MOSS_ASSERT_EQ_U32(expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            std::cout << "[ASSERT_FAIL] " << __FILE__ << ":" << __LINE__ \
                      << " - Expected " << (expected) << ", got " << (actual) << std::endl; \
            moss::test::LegacyTestTracker::register_test_result(false, __func__); \
            return; \
        } \
    } while(0)

#define MOSS_ASSERT_EQ_U64(expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            std::cout << "[ASSERT_FAIL] " << __FILE__ << ":" << __LINE__ \
                      << " - Expected " << (expected) << ", got " << (actual) << std::endl; \
            moss::test::LegacyTestTracker::register_test_result(false, __func__); \
            return; \
        } \
    } while(0)

#define MOSS_ASSERT_NE_U32(not_expected, actual) \
    do { \
        if ((not_expected) == (actual)) { \
            std::cout << "[ASSERT_FAIL] " << __FILE__ << ":" << __LINE__ \
                      << " - Expected NOT " << (not_expected) << ", but got " << (actual) << std::endl; \
            moss::test::LegacyTestTracker::register_test_result(false, __func__); \
            return; \
        } \
    } while(0)

#define MOSS_ASSERT_NOT_NULL(ptr) \
    do { \
        if ((ptr) == nullptr) { \
            std::cout << "[ASSERT_FAIL] " << __FILE__ << ":" << __LINE__ \
                      << " - Expected non-null pointer, got null: " << #ptr << std::endl; \
            moss::test::LegacyTestTracker::register_test_result(false, __func__); \
            return; \
        } \
    } while(0)

#define MOSS_ASSERT_NULL(ptr) \
    do { \
        if ((ptr) != nullptr) { \
            std::cout << "[ASSERT_FAIL] " << __FILE__ << ":" << __LINE__ \
                      << " - Expected null pointer, got non-null: " << #ptr << std::endl; \
            moss::test::LegacyTestTracker::register_test_result(false, __func__); \
            return; \
        } \
    } while(0)

// Test function definition macros
#define MOSS_TEST_FUNCTION(func_name) \
    void func_name() { \
        std::cout << "[TEST] " << #func_name << " ... "; \
        func_name##_impl(); \
        moss::test::LegacyTestTracker::register_test_result(true, #func_name); \
    } \
    void func_name##_impl()

// Test registration and suite management
#define MOSS_TEST_SUITE_BEGIN(suite_name) \
    namespace moss_test_##suite_name { \
        struct SuiteManager { \
            SuiteManager() { \
                moss::test::LegacyTestTracker::start_suite(#suite_name); \
            } \
            ~SuiteManager() { \
                moss::test::LegacyTestTracker::end_suite(); \
            } \
        }; \
        static SuiteManager suite_manager_;

#define MOSS_TEST_SUITE_END() \
    } /* end namespace */

#define MOSS_REGISTER_TEST(suite_name, test_func) \
    namespace moss_test_##suite_name { \
        struct RegisterTest_##test_func { \
            RegisterTest_##test_func() { \
                test_func(); \
            } \
        }; \
        static RegisterTest_##test_func register_##test_func##_; \
    }

// Main test runner
#define MOSS_RUN_ALL_TESTS() \
    do { \
        moss::test::LegacyTestTracker::print_summary(); \
    } while(0)

// ========================================================================
// Modern ut.hpp Migration Helpers
// ========================================================================

/**
 * @brief Macros to help migrate from MOSS to ut.hpp syntax
 *
 * These provide a stepping stone toward modern test syntax while maintaining
 * MOSS compatibility during the transition period.
 */

// Modern assertion style (ut.hpp compatible)
#define MOSS_EXPECT(condition) \
    do { \
        if (!(condition)) { \
            std::cout << "[EXPECT_FAIL] " << __FILE__ << ":" << __LINE__ \
                      << " - " << #condition << std::endl; \
        } \
    } while(0)

#define MOSS_EXPECT_EQ(lhs, rhs) \
    do { \
        if ((lhs) != (rhs)) { \
            std::cout << "[EXPECT_FAIL] " << __FILE__ << ":" << __LINE__ \
                      << " - Expected " << (rhs) << ", got " << (lhs) << std::endl; \
        } \
    } while(0)

#define MOSS_EXPECT_NE(lhs, rhs) \
    do { \
        if ((lhs) == (rhs)) { \
            std::cout << "[EXPECT_FAIL] " << __FILE__ << ":" << __LINE__ \
                      << " - Expected NOT " << (rhs) << ", but got " << (lhs) << std::endl; \
        } \
    } while(0)

// Test case definition (modern style)
#define MOSS_TEST_CASE(name) \
    void test_case_##name(); \
    namespace { \
        struct TestCase_##name { \
            TestCase_##name() { \
                moss::test::test_runner.run_test(#name, test_case_##name); \
            } \
        }; \
        static TestCase_##name test_case_instance_##name; \
    } \
    void test_case_##name()

// ========================================================================
// Migration Guide Comments
// ========================================================================

/*
 * MIGRATION GUIDE: From MOSS to ut.hpp
 *
 * Phase 1: Use compatibility macros (this file)
 *   - Keep existing MOSS_ASSERT_* macros
 *   - Tests continue to work without changes
 *
 * Phase 2: Migrate to MOSS_EXPECT_* macros
 *   - MOSS_ASSERT_TRUE(x) -> MOSS_EXPECT(x)
 *   - MOSS_ASSERT_EQ_U32(a, b) -> MOSS_EXPECT_EQ(a, b)
 *   - MOSS_ASSERT_FALSE(x) -> MOSS_EXPECT(!x)
 *
 * Phase 3: Migrate to ut.hpp syntax (when available)
 *   - MOSS_EXPECT(x) -> expect(x)
 *   - MOSS_EXPECT_EQ(a, b) -> expect(a == b)
 *   - MOSS_TEST_CASE(name) -> "name"_test = []() { ... }
 *
 * Phase 4: Pure ut.hpp (final goal)
 *   - Remove all MOSS compatibility macros
 *   - Use pure Boost.UT syntax throughout
 */