#pragma once

// MOSS内核单元测试框架 - 核心测试框架
// 设计目标: 非侵入性、全面覆盖、详细报告、继续执行

#include "types.hpp"
#include "result.hpp"

namespace moss::kernel::test {

// 使用内核类型命名空间
using namespace moss::kernel;

// 最大常量定义 - 适应内核环境的静态内存分配
static constexpr u32 MAX_TESTS_PER_SUITE = 64;
static constexpr u32 MAX_ERROR_MESSAGE_LENGTH = 256;
static constexpr u32 MAX_TEST_SUITES = 32;
static constexpr u32 MAX_TEST_NAME_LENGTH = 64;

// ============================================================================
// 测试退出代码和内核关闭
// ============================================================================

// 测试退出状态码
enum class TestExitCode : u32 {
    AllPassed = 0,      // 所有测试通过
    TestFailed = 1,     // 有测试失败
    SystemError = 2,    // 系统错误
    NoTests = 3         // 没有找到测试
};

// 测试内核关闭函数（使用QEMU semihosting正确退出）
[[noreturn]] void test_kernel_shutdown(TestExitCode exit_code) noexcept;

// 测试结果结构
struct TestResult {
    u32 total_tests = 0;
    u32 passed_tests = 0;
    u32 failed_tests = 0;
    u64 execution_time_ns = 0;
    char first_failure[MAX_ERROR_MESSAGE_LENGTH] = {0};

    // 计算成功率（百分比）
    [[nodiscard]] constexpr u32 success_rate_percent() const noexcept {
        if (total_tests == 0) return 0;
        return (passed_tests * 100) / total_tests;
    }

    [[nodiscard]] constexpr bool is_all_passed() const noexcept {
        return failed_tests == 0 && total_tests > 0;
    }
};

// 单个测试用例结构
struct TestCase {
    const char* test_name;
    void (*test_function)();
    bool is_enabled = true;

    TestCase() : test_name(nullptr), test_function(nullptr) {}
    TestCase(const char* name, void (*func)()) : test_name(name), test_function(func) {}
};

// UART输出类 - 用于测试结果直接输出到硬件
class UartWriter {
public:
    // 写入单个字符
    static void write_char(char c) noexcept;

    // 写入字符串
    static void write_string(const char* str) noexcept;

    // 写入数字 (十进制)
    static void write_u32(u32 value) noexcept;
    static void write_u64(u64 value) noexcept;

    // 写入数字 (十六进制)
    static void write_hex_u32(u32 value) noexcept;
    static void write_hex_u64(u64 value) noexcept;

    // 换行
    static void write_newline() noexcept;
};

// 测试套件类 - 管理单个模块的所有测试
class TestSuite {
private:
    const char* suite_name_;
    TestCase test_cases_[MAX_TESTS_PER_SUITE];
    u32 test_count_ = 0;
    bool suite_enabled_ = true;

public:
    explicit TestSuite(const char* name) noexcept : suite_name_(name) {}

    // 注册测试用例
    [[nodiscard]] VoidResult add_test(const char* test_name, void (*test_func)()) noexcept;

    // 运行所有测试用例
    [[nodiscard]] TestResult run_all_tests() noexcept;

    // 获取套件信息
    [[nodiscard]] const char* get_name() const noexcept { return suite_name_; }
    [[nodiscard]] u32 get_test_count() const noexcept { return test_count_; }
    [[nodiscard]] bool is_enabled() const noexcept { return suite_enabled_; }

    // 启用/禁用整个套件
    void set_enabled(bool enabled) noexcept { suite_enabled_ = enabled; }

    // 启用/禁用特定测试
    [[nodiscard]] VoidResult set_test_enabled(const char* test_name, bool enabled) noexcept;
};

// 全局测试状态 - 用于断言处理
struct GlobalTestState {
    bool current_test_failed = false;
    char current_failure_message[MAX_ERROR_MESSAGE_LENGTH] = {0};
    const char* current_test_name = nullptr;
    const char* current_suite_name = nullptr;
    u32 total_assertions = 0;
    u32 failed_assertions = 0;

    void reset_for_new_test(const char* suite_name, const char* test_name) noexcept;
    void record_assertion_failure(const char* file, u32 line, const char* expression, const char* message = nullptr) noexcept;
};

// 全局测试状态访问
extern GlobalTestState g_test_state;

// 断言实现函数
void assert_true_impl(const char* file, u32 line, bool condition, const char* expression) noexcept;
void assert_false_impl(const char* file, u32 line, bool condition, const char* expression) noexcept;
void assert_eq_u32_impl(const char* file, u32 line, u32 expected, u32 actual, const char* expr) noexcept;
void assert_eq_u64_impl(const char* file, u32 line, u64 expected, u64 actual, const char* expr) noexcept;
void assert_eq_ptr_impl(const char* file, u32 line, const void* expected, const void* actual, const char* expr) noexcept;
void assert_null_impl(const char* file, u32 line, const void* ptr, const char* expression) noexcept;
void assert_not_null_impl(const char* file, u32 line, const void* ptr, const char* expression) noexcept;

// 核心断言宏 - 适用于freestanding环境
#define MOSS_ASSERT_TRUE(condition) \
    moss::kernel::test::assert_true_impl(__FILE__, __LINE__, (condition), #condition)

#define MOSS_ASSERT_FALSE(condition) \
    moss::kernel::test::assert_false_impl(__FILE__, __LINE__, (condition), #condition)

#define MOSS_ASSERT_EQ_U32(expected, actual) \
    moss::kernel::test::assert_eq_u32_impl(__FILE__, __LINE__, (expected), (actual), #expected " == " #actual)

#define MOSS_ASSERT_EQ_U64(expected, actual) \
    moss::kernel::test::assert_eq_u64_impl(__FILE__, __LINE__, (expected), (actual), #expected " == " #actual)

#define MOSS_ASSERT_EQ_PTR(expected, actual) \
    moss::kernel::test::assert_eq_ptr_impl(__FILE__, __LINE__, (expected), (actual), #expected " == " #actual)

#define MOSS_ASSERT_NULL(ptr) \
    moss::kernel::test::assert_null_impl(__FILE__, __LINE__, (ptr), #ptr)

#define MOSS_ASSERT_NOT_NULL(ptr) \
    moss::kernel::test::assert_not_null_impl(__FILE__, __LINE__, (ptr), #ptr)

// 便利断言宏 - 适应不同数据类型
#define MOSS_ASSERT_EQ(expected, actual) \
    static_assert(false, "Use MOSS_ASSERT_EQ_U32, MOSS_ASSERT_EQ_U64, or MOSS_ASSERT_EQ_PTR for type safety")

// 测试套件注册宏
#define MOSS_DECLARE_TEST_SUITE(suite_name) \
    extern moss::kernel::test::TestSuite g_test_suite_##suite_name

#define MOSS_DEFINE_TEST_SUITE(suite_name) \
    moss::kernel::test::TestSuite g_test_suite_##suite_name(#suite_name)

#define MOSS_REGISTER_TEST(suite_name, test_func) \
    static bool __register_##suite_name##_##test_func = []() { \
        g_test_suite_##suite_name.add_test(#test_func, test_func); \
        return true; \
    }()

// 测试函数定义宏
#define MOSS_TEST_FUNCTION(test_func_name) \
    static void test_func_name()

// 内核专用的高精度时间测量
u64 get_test_timestamp_ns() noexcept;

} // namespace moss::kernel::test
