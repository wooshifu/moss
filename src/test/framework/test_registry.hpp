#pragma once

// MOSS内核单元测试框架 - 测试注册中心
// 管理多个测试套件的统一执行和结果汇总

#include "test_framework.hpp"

namespace moss::kernel::test {

// 全局测试结果汇总
struct GlobalTestResult {
    u32 total_suites = 0;
    u32 passed_suites = 0;
    u32 failed_suites = 0;
    u32 total_tests = 0;
    u32 passed_tests = 0;
    u32 failed_tests = 0;
    u64 total_execution_time_ns = 0;

    char first_suite_failure[MAX_ERROR_MESSAGE_LENGTH] = {0};

    [[nodiscard]] constexpr bool is_all_passed() const noexcept {
        return failed_tests == 0 && total_tests > 0;
    }

    [[nodiscard]] constexpr u32 overall_success_rate_percent() const noexcept {
        if (total_tests == 0) return 0;
        return (passed_tests * 100) / total_tests;
    }
};

// 注册的测试套件条目
struct RegisteredTestSuite {
    TestSuite* test_suite;
    bool is_enabled;
    TestResult last_result;

    RegisteredTestSuite() : test_suite(nullptr), is_enabled(true) {}
    RegisteredTestSuite(TestSuite* suite) : test_suite(suite), is_enabled(true) {}
};

// 测试注册中心 - 管理所有测试套件
class TestRegistry {
private:
    RegisteredTestSuite registered_suites_[MAX_TEST_SUITES];
    u32 suite_count_ = 0;
    GlobalTestResult last_global_result_;
    bool registry_initialized_ = false;

    // 单例实例
    static TestRegistry* instance_;

    // 私有构造函数
    TestRegistry() noexcept = default;

public:
    // 获取单例实例
    [[nodiscard]] static TestRegistry& get_instance() noexcept;

    // 注册测试套件
    [[nodiscard]] VoidResult register_suite(TestSuite* test_suite) noexcept;

    // 运行所有注册的测试套件
    [[nodiscard]] GlobalTestResult run_all_suites() noexcept;

    // 运行特定测试套件
    [[nodiscard]] TestResult run_suite(const char* suite_name) noexcept;

    // 启用/禁用测试套件
    [[nodiscard]] VoidResult set_suite_enabled(const char* suite_name, bool enabled) noexcept;

    // 获取注册信息
    [[nodiscard]] u32 get_suite_count() const noexcept { return suite_count_; }
    [[nodiscard]] const GlobalTestResult& get_last_global_result() const noexcept {
        return last_global_result_;
    }

    // 列出所有注册的测试套件
    void list_all_suites() const noexcept;

    // 打印详细的测试报告
    void print_detailed_report(const GlobalTestResult& result) const noexcept;

    // 打印简要的测试报告
    void print_summary_report(const GlobalTestResult& result) const noexcept;

    // 禁用复制和移动
    NON_COPYABLE_NON_MOVABLE(TestRegistry)
};

// 全局便利函数
[[nodiscard]] VoidResult moss_register_test_suite(TestSuite* suite) noexcept;
[[nodiscard]] GlobalTestResult moss_run_all_tests() noexcept;
void moss_print_test_report(const GlobalTestResult& result) noexcept;

// 自动注册宏 - 简化测试套件注册
#define MOSS_AUTO_REGISTER_SUITE(suite_name) \
    namespace { \
        struct AutoRegister##suite_name { \
            AutoRegister##suite_name() { \
                [[maybe_unused]] auto result = moss::kernel::test::TestRegistry::get_instance().register_suite(&g_test_suite_##suite_name); \
            } \
        }; \
        static AutoRegister##suite_name auto_register_##suite_name; \
    }

} // namespace moss::kernel::test
