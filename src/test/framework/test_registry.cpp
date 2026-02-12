#include "test_registry.hpp"

namespace moss::kernel::test {

// 静态实例指针初始化
TestRegistry* TestRegistry::instance_ = nullptr;

// ============================================================================
// TestRegistry 单例实现
// ============================================================================

TestRegistry& TestRegistry::get_instance() noexcept {
    if (!instance_) {
        // 使用placement new在静态内存上构造对象
        alignas(TestRegistry) static char instance_buffer[sizeof(TestRegistry)];
        instance_ = new (instance_buffer) TestRegistry();
        instance_->registry_initialized_ = true;
    }
    return *instance_;
}

VoidResult TestRegistry::register_suite(TestSuite* test_suite) noexcept {
    if (!test_suite) {
        return Error(ErrorCode::InvalidParameter);
    }

    if (suite_count_ >= MAX_TEST_SUITES) {
        return Error(ErrorCode::ResourceExhausted);
    }

    // 检查是否已经注册
    for (u32 i = 0; i < suite_count_; i++) {
        if (registered_suites_[i].test_suite == test_suite) {
            return Error(ErrorCode::AlreadyExists);
        }
    }

    registered_suites_[suite_count_] = RegisteredTestSuite(test_suite);
    suite_count_++;

    return Ok();
}

GlobalTestResult TestRegistry::run_all_suites() noexcept {
    GlobalTestResult global_result{};

    UartWriter::write_string("\n");
    UartWriter::write_string("================================\n");
    UartWriter::write_string("=== MOSS内核单元测试执行 ===\n");
    UartWriter::write_string("================================\n");

    u64 global_start_time = get_test_timestamp_ns();

    UartWriter::write_string("发现 ");
    UartWriter::write_u32(suite_count_);
    UartWriter::write_string(" 个测试套件\n\n");

    for (u32 i = 0; i < suite_count_; i++) {
        if (!registered_suites_[i].is_enabled || !registered_suites_[i].test_suite) {
            continue;
        }

        global_result.total_suites++;

        TestResult suite_result = registered_suites_[i].test_suite->run_all_tests();
        registered_suites_[i].last_result = suite_result;

        // 累计统计
        global_result.total_tests += suite_result.total_tests;
        global_result.passed_tests += suite_result.passed_tests;
        global_result.failed_tests += suite_result.failed_tests;

        if (suite_result.is_all_passed()) {
            global_result.passed_suites++;
        } else {
            global_result.failed_suites++;

            // 记录第一个失败套件的信息
            if (global_result.first_suite_failure[0] == '\0') {
                u32 pos = 0;
                const char* suite_name = registered_suites_[i].test_suite->get_name();

                // 复制套件名称
                while (*suite_name && pos < MAX_ERROR_MESSAGE_LENGTH - 50) {
                    global_result.first_suite_failure[pos++] = *suite_name++;
                }

                const char* separator = ": ";
                const char* sep = separator;
                while (*sep && pos < MAX_ERROR_MESSAGE_LENGTH - 30) {
                    global_result.first_suite_failure[pos++] = *sep++;
                }

                // 复制第一个失败信息
                const char* failure_msg = suite_result.first_failure;
                while (*failure_msg && pos < MAX_ERROR_MESSAGE_LENGTH - 1) {
                    global_result.first_suite_failure[pos++] = *failure_msg++;
                }

                global_result.first_suite_failure[pos] = '\0';
            }
        }
    }

    u64 global_end_time = get_test_timestamp_ns();
    global_result.total_execution_time_ns = global_end_time - global_start_time;

    last_global_result_ = global_result;

    // 打印详细报告
    print_detailed_report(global_result);

    return global_result;
}

TestResult TestRegistry::run_suite(const char* suite_name) noexcept {
    TestResult empty_result{};

    if (!suite_name) {
        return empty_result;
    }

    for (u32 i = 0; i < suite_count_; i++) {
        if (!registered_suites_[i].test_suite || !registered_suites_[i].is_enabled) {
            continue;
        }

        // 简单字符串比较
        const char* registered_name = registered_suites_[i].test_suite->get_name();
        const char* a = registered_name;
        const char* b = suite_name;
        bool match = true;

        while (*a && *b) {
            if (*a != *b) {
                match = false;
                break;
            }
            a++;
            b++;
        }

        if (match && *a == *b) { // 两个字符串都到达末尾
            TestResult result = registered_suites_[i].test_suite->run_all_tests();
            registered_suites_[i].last_result = result;
            return result;
        }
    }

    return empty_result;
}

VoidResult TestRegistry::set_suite_enabled(const char* suite_name, bool enabled) noexcept {
    if (!suite_name) {
        return Error(ErrorCode::InvalidParameter);
    }

    for (u32 i = 0; i < suite_count_; i++) {
        if (!registered_suites_[i].test_suite) {
            continue;
        }

        // 字符串比较
        const char* registered_name = registered_suites_[i].test_suite->get_name();
        const char* a = registered_name;
        const char* b = suite_name;
        bool match = true;

        while (*a && *b) {
            if (*a != *b) {
                match = false;
                break;
            }
            a++;
            b++;
        }

        if (match && *a == *b) {
            registered_suites_[i].is_enabled = enabled;
            return Ok();
        }
    }

    return Error(ErrorCode::NotFound);
}

void TestRegistry::list_all_suites() const noexcept {
    UartWriter::write_string("\n=== 已注册的测试套件 ===\n");

    if (suite_count_ == 0) {
        UartWriter::write_string("无已注册的测试套件\n");
        return;
    }

    for (u32 i = 0; i < suite_count_; i++) {
        if (!registered_suites_[i].test_suite) {
            continue;
        }

        UartWriter::write_string("  ");
        UartWriter::write_u32(i + 1);
        UartWriter::write_string(". ");
        UartWriter::write_string(registered_suites_[i].test_suite->get_name());
        UartWriter::write_string(" (");
        UartWriter::write_u32(registered_suites_[i].test_suite->get_test_count());
        UartWriter::write_string(" 个测试");

        if (!registered_suites_[i].is_enabled) {
            UartWriter::write_string(", 已禁用");
        }

        UartWriter::write_string(")\n");
    }

    UartWriter::write_string("\n");
}

void TestRegistry::print_detailed_report(const GlobalTestResult& result) const noexcept {
    UartWriter::write_string("\n");
    UartWriter::write_string("================================\n");
    UartWriter::write_string("===     测试执行报告        ===\n");
    UartWriter::write_string("================================\n");

    // 总体统计
    UartWriter::write_string("测试套件: ");
    UartWriter::write_u32(result.passed_suites);
    UartWriter::write_string("/");
    UartWriter::write_u32(result.total_suites);
    UartWriter::write_string(" 通过\n");

    UartWriter::write_string("测试用例: ");
    UartWriter::write_u32(result.passed_tests);
    UartWriter::write_string("/");
    UartWriter::write_u32(result.total_tests);
    UartWriter::write_string(" 通过\n");

    UartWriter::write_string("总执行时间: ");
    UartWriter::write_u64(result.total_execution_time_ns / 1000000); // 毫秒
    UartWriter::write_string("ms\n");

    UartWriter::write_string("成功率: ");
    u32 success_rate_int = result.overall_success_rate_percent();
    UartWriter::write_u32(success_rate_int);
    UartWriter::write_string("%\n");

    // 各套件详细结果
    UartWriter::write_string("\n=== 各套件详细结果 ===\n");
    for (u32 i = 0; i < suite_count_; i++) {
        if (!registered_suites_[i].test_suite || !registered_suites_[i].is_enabled) {
            continue;
        }

        const TestResult& suite_result = registered_suites_[i].last_result;
        UartWriter::write_string("  ");

        if (suite_result.is_all_passed()) {
            UartWriter::write_string("✅ ");
        } else {
            UartWriter::write_string("❌ ");
        }

        UartWriter::write_string(registered_suites_[i].test_suite->get_name());
        UartWriter::write_string(": ");
        UartWriter::write_u32(suite_result.passed_tests);
        UartWriter::write_string("/");
        UartWriter::write_u32(suite_result.total_tests);
        UartWriter::write_string(" (");
        UartWriter::write_u64(suite_result.execution_time_ns / 1000000); // 毫秒
        UartWriter::write_string("ms)\n");
    }

    // 失败信息
    if (result.failed_tests > 0) {
        UartWriter::write_string("\n=== 失败详情 ===\n");
        UartWriter::write_string("第一个失败: ");
        UartWriter::write_string(result.first_suite_failure);
        UartWriter::write_string("\n");
    }

    // 最终结论
    UartWriter::write_string("\n=== 测试结论 ===\n");
    if (result.is_all_passed()) {
        UartWriter::write_string("🎉 所有测试通过！内核质量验证成功！\n");
    } else {
        UartWriter::write_string("⚠️  发现 ");
        UartWriter::write_u32(result.failed_tests);
        UartWriter::write_string(" 个测试失败，需要修复\n");
    }

    UartWriter::write_string("================================\n\n");
}

void TestRegistry::print_summary_report(const GlobalTestResult& result) const noexcept {
    UartWriter::write_string("测试汇总: ");
    UartWriter::write_u32(result.passed_tests);
    UartWriter::write_string("/");
    UartWriter::write_u32(result.total_tests);
    UartWriter::write_string(" 通过 (");
    u32 success_rate_int = result.overall_success_rate_percent();
    UartWriter::write_u32(success_rate_int);
    UartWriter::write_string("%) - ");
    UartWriter::write_u64(result.total_execution_time_ns / 1000000);
    UartWriter::write_string("ms\n");
}

// ============================================================================
// 全局便利函数实现
// ============================================================================

VoidResult moss_register_test_suite(TestSuite* suite) noexcept {
    return TestRegistry::get_instance().register_suite(suite);
}

GlobalTestResult moss_run_all_tests() noexcept {
    return TestRegistry::get_instance().run_all_suites();
}

void moss_print_test_report(const GlobalTestResult& result) noexcept {
    TestRegistry::get_instance().print_detailed_report(result);
}

} // namespace moss::kernel::test
