// MOSS内核单元测试框架 - 测试内核主入口点
// 这是一个独立的测试模式内核，专门用于运行单元测试

#include "framework/test_framework.hpp"
#include "framework/test_registry.hpp"

// 内核基础设施
#include "types.hpp"
#include "result.hpp"

// 必要的内核组件 - 仅包含测试所需的最小集合
extern "C" {
    // 启动相关的外部函数声明
    void early_debug_print(const char* message) noexcept;
    void kernel_panic(const char* message) noexcept;
}

using namespace moss::kernel;
using namespace moss::kernel::test;

// ============================================================================
// 测试内核全局状态
// ============================================================================

// 测试结果退出码
enum class TestExitCode : u32 {
    AllPassed = 0,      // 所有测试通过
    TestFailed = 1,     // 有测试失败
    SystemError = 2,    // 系统错误
    NoTests = 3         // 没有找到测试
};

// 全局测试统计
struct TestKernelStats {
    u32 total_assertions = 0;
    u32 failed_assertions = 0;
    u32 total_suites = 0;
    u32 executed_suites = 0;
    bool has_critical_error = false;
};

static TestKernelStats g_test_stats;

// ============================================================================
// 测试内核基础设施
// ============================================================================

// 简化的内核初始化 - 仅测试所需
void test_kernel_early_init() noexcept {
    // 🔍 Layer 4 诊断: 测试框架初始化开始
    early_debug_print("🔍 [LAYER 4] test_kernel_early_init() entry\n");

    early_debug_print("🧪 MOSS内核测试模式启动\n");
    early_debug_print("🧪 初始化测试内核基础设施...\n");

    // 重置全局测试状态
    early_debug_print("🔍 [LAYER 4] Resetting global test state\n");
    g_test_state.current_test_failed = false;
    g_test_state.current_failure_message[0] = '\0';
    g_test_state.current_test_name = nullptr;
    g_test_state.current_suite_name = nullptr;
    g_test_state.total_assertions = 0;
    g_test_state.failed_assertions = 0;

    // 重置内核统计
    early_debug_print("🔍 [LAYER 4] Resetting kernel stats\n");
    g_test_stats.total_assertions = 0;
    g_test_stats.failed_assertions = 0;
    g_test_stats.total_suites = 0;
    g_test_stats.executed_suites = 0;
    g_test_stats.has_critical_error = false;

    early_debug_print("✅ 测试内核基础设施初始化完成\n");
    early_debug_print("🔍 [LAYER 4] test_kernel_early_init() completed\n");
}

// 测试内核关闭处理
[[noreturn]] void test_kernel_shutdown(TestExitCode exit_code) noexcept {
    const char* exit_message = nullptr;

    switch (exit_code) {
        case TestExitCode::AllPassed:
            exit_message = "🎉 所有测试通过！内核质量验证成功！\n";
            break;
        case TestExitCode::TestFailed:
            exit_message = "❌ 测试失败！发现问题需要修复\n";
            break;
        case TestExitCode::SystemError:
            exit_message = "💥 系统错误！测试框架出现问题\n";
            break;
        case TestExitCode::NoTests:
            exit_message = "⚠️ 警告：没有找到任何测试\n";
            break;
        default:
            exit_message = "❓ 未知退出状态\n";
            break;
    }

    early_debug_print("\n");
    early_debug_print("================================\n");
    early_debug_print("=== 测试内核关闭 ===\n");
    early_debug_print(exit_message);
    early_debug_print("================================\n");

    // 在QEMU中，可以使用特殊的退出机制
    // 对于真实硬件，这里会是halt指令
    #if defined(MOSS_ARCH_ARM64)
    // ARM64: WFI (等待中断) 指令
    while (true) {
        asm volatile("wfi");
    }
    #elif defined(MOSS_ARCH_X86_64)
    // x86_64: HLT 指令
    while (true) {
        asm volatile("hlt");
    }
    #elif defined(MOSS_ARCH_RISCV)
    // RISC-V: WFI 指令
    while (true) {
        asm volatile("wfi");
    }
    #else
    // 通用回退 - 死循环
    while (true) {
        asm volatile("nop");
    }
    #endif
}

// ============================================================================
// 示例测试套件 - 验证测试框架本身的工作
// ============================================================================

// 测试框架自身的基础功能验证
namespace framework_self_test {

MOSS_DEFINE_TEST_SUITE(framework_basic);

MOSS_TEST_FUNCTION(test_assertions_work) {
    // 测试基础断言功能
    MOSS_ASSERT_TRUE(true);
    MOSS_ASSERT_FALSE(false);
    MOSS_ASSERT_EQ_U32(42, 42);
    MOSS_ASSERT_EQ_U64(1000000ULL, 1000000ULL);
}

MOSS_TEST_FUNCTION(test_null_pointer_checks) {
    void* null_ptr = nullptr;
    char valid_data = 'A';
    void* valid_ptr = &valid_data;

    MOSS_ASSERT_NULL(null_ptr);
    MOSS_ASSERT_NOT_NULL(valid_ptr);
}

MOSS_TEST_FUNCTION(test_timing_measurement) {
    u64 start_time = get_test_timestamp_ns();

    // 执行一些简单操作
    volatile u32 counter = 0;
    for (u32 i = 0; i < 100; i++) {
        counter += i;
    }

    u64 end_time = get_test_timestamp_ns();

    // 验证时间测量功能
    MOSS_ASSERT_TRUE(end_time >= start_time);
}

MOSS_TEST_FUNCTION(test_error_handling) {
    // 测试测试框架的错误处理
    // 这个测试会故意"通过"，用来验证正常流程
    MOSS_ASSERT_TRUE(g_test_state.total_assertions > 0);
}

// 注册测试到套件
static bool register_framework_tests() {
    VoidResult result1 = g_test_suite_framework_basic.add_test("test_assertions_work", test_assertions_work);
    VoidResult result2 = g_test_suite_framework_basic.add_test("test_null_pointer_checks", test_null_pointer_checks);
    VoidResult result3 = g_test_suite_framework_basic.add_test("test_timing_measurement", test_timing_measurement);
    VoidResult result4 = g_test_suite_framework_basic.add_test("test_error_handling", test_error_handling);

    // 检查注册结果
    if (result1.is_error() || result2.is_error() || result3.is_error() || result4.is_error()) {
        early_debug_print("❌ 警告：部分框架测试注册失败\n");
        return false;
    }

    return true;
}

[[maybe_unused]] static bool framework_tests_registered = register_framework_tests();

// 自动注册套件到全局注册表
MOSS_AUTO_REGISTER_SUITE(framework_basic);

} // namespace framework_self_test

// ============================================================================
// 测试内核主函数
// ============================================================================

extern "C" [[noreturn]] void test_kernel_main() noexcept {
    // 🔍 Layer 3 诊断: test_kernel_main函数开始执行
    early_debug_print("🔍 [LAYER 3] test_kernel_main() entry\n");

    // === 阶段1: 测试内核初始化 ===
    early_debug_print("🔍 [LAYER 3] Calling test_kernel_early_init()\n");
    test_kernel_early_init();
    early_debug_print("🔍 [LAYER 3] test_kernel_early_init() completed\n");

    early_debug_print("\n");
    early_debug_print("🧪 开始执行MOSS内核单元测试...\n");

    // === 阶段2: 运行所有注册的测试套件 ===
    early_debug_print("🔍 [LAYER 3] Getting TestRegistry instance\n");
    TestRegistry& registry = TestRegistry::get_instance();
    early_debug_print("🔍 [LAYER 3] TestRegistry instance obtained\n");

    // 列出所有注册的测试套件
    early_debug_print("🔍 [LAYER 3] Listing all test suites\n");
    registry.list_all_suites();
    early_debug_print("🔍 [LAYER 3] Test suites listed\n");

    if (registry.get_suite_count() == 0) {
        early_debug_print("❌ 错误：没有找到任何注册的测试套件\n");
        test_kernel_shutdown(TestExitCode::NoTests);
    }

    early_debug_print("🚀 开始执行测试...\n");

    // 运行所有测试
    GlobalTestResult global_result = registry.run_all_suites();

    // === 阶段3: 处理测试结果 ===
    g_test_stats.total_assertions = g_test_state.total_assertions;
    g_test_stats.failed_assertions = g_test_state.failed_assertions;
    g_test_stats.total_suites = global_result.total_suites;
    g_test_stats.executed_suites = global_result.passed_suites + global_result.failed_suites;

    // 确定退出状态
    TestExitCode exit_code = TestExitCode::AllPassed;

    if (global_result.failed_tests > 0) {
        exit_code = TestExitCode::TestFailed;
    } else if (global_result.total_tests == 0) {
        exit_code = TestExitCode::NoTests;
    } else if (g_test_stats.has_critical_error) {
        exit_code = TestExitCode::SystemError;
    }

    // === 阶段4: 测试内核关闭 ===
    early_debug_print("\n");
    early_debug_print("📊 测试执行统计:\n");
    early_debug_print("   断言总数: ");
    // 简单输出数字 (避免复杂的格式化)
    UartWriter::write_u32(g_test_stats.total_assertions);
    early_debug_print("\n   失败断言: ");
    UartWriter::write_u32(g_test_stats.failed_assertions);
    early_debug_print("\n   执行套件: ");
    UartWriter::write_u32(g_test_stats.executed_suites);
    early_debug_print("/");
    UartWriter::write_u32(g_test_stats.total_suites);
    early_debug_print("\n");

    test_kernel_shutdown(exit_code);
}

// ============================================================================
// 架构特定的入口点适配
// ============================================================================

// ARM64入口点 - 由boot loader调用
#if defined(MOSS_ARCH_ARM64)
extern "C" [[noreturn]] __attribute__((section(".text.boot"))) void _start() noexcept {
    // 🔧 极简ARM64启动测试

    // 直接UART输出测试
    volatile char* uart_base = reinterpret_cast<volatile char*>(0x09000000);

    // Test message 1
    const char msg1[] = "MINIMAL START TEST\n";
    for (int i = 0; msg1[i] != 0; i++) {
        *uart_base = msg1[i];
    }

    // Test message 2
    const char msg2[] = "CALLING TEST MAIN\n";
    for (int i = 0; msg2[i] != 0; i++) {
        *uart_base = msg2[i];
    }

    // 6. 直接调用测试内核主函数
    test_kernel_main();
}
#endif

// x86_64入口点
#if defined(MOSS_ARCH_X86_64)
extern "C" [[noreturn]] __attribute__((section(".text.boot"))) void _start() noexcept {
    test_kernel_main();
}
#endif

// RISC-V入口点
#if defined(MOSS_ARCH_RISCV)
extern "C" [[noreturn]] __attribute__((section(".text.boot"))) void _start() noexcept {
    test_kernel_main();
}
#endif

