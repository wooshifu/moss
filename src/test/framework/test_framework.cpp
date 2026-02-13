#include "test_framework.hpp"

// 架构特定的包含文件
#include "arch/arch_abstraction.hpp"

namespace moss::kernel::test {

// 全局测试状态实例
GlobalTestState g_test_state;

// ============================================================================
// GlobalTestState 实现
// ============================================================================

void GlobalTestState::reset_for_new_test(const char* suite_name, const char* test_name) noexcept {
    current_test_failed = false;
    current_failure_message[0] = '\0';
    current_test_name = test_name;
    current_suite_name = suite_name;
}

void GlobalTestState::record_assertion_failure(const char* file, u32 line, const char* expression, const char* message) noexcept {
    current_test_failed = true;
    failed_assertions++;

    // 组装错误消息 - 手工字符串处理，避免标准库
    u32 pos = 0;

    // 添加文件名 (只取最后部分)
    const char* filename = file;
    const char* last_slash = nullptr;
    while (*filename) {
        if (*filename == '/' || *filename == '\\') {
            last_slash = filename + 1;
        }
        filename++;
    }
    if (last_slash) {
        filename = last_slash;
    } else {
        filename = file;
    }

    // 复制文件名
    while (*filename && pos < MAX_ERROR_MESSAGE_LENGTH - 50) {
        current_failure_message[pos++] = *filename++;
    }

    // 添加行号
    current_failure_message[pos++] = ':';

    // 简单的数字转字符串
    char line_str[16];
    u32 line_pos = 0;
    u32 temp_line = line;
    if (temp_line == 0) {
        line_str[line_pos++] = '0';
    } else {
        while (temp_line > 0 && line_pos < 15) {
            line_str[line_pos++] = '0' + (temp_line % 10);
            temp_line /= 10;
        }
        // 反转数字字符串
        for (u32 i = 0; i < line_pos / 2; i++) {
            char temp = line_str[i];
            line_str[i] = line_str[line_pos - 1 - i];
            line_str[line_pos - 1 - i] = temp;
        }
    }
    line_str[line_pos] = '\0';

    // 复制行号
    u32 line_str_idx = 0;
    while (line_str[line_str_idx] && pos < MAX_ERROR_MESSAGE_LENGTH - 20) {
        current_failure_message[pos++] = line_str[line_str_idx++];
    }

    // 添加表达式
    const char* separator = " - ";
    const char* sep_ptr = separator;
    while (*sep_ptr && pos < MAX_ERROR_MESSAGE_LENGTH - 10) {
        current_failure_message[pos++] = *sep_ptr++;
    }

    // 复制表达式
    while (*expression && pos < MAX_ERROR_MESSAGE_LENGTH - 5) {
        current_failure_message[pos++] = *expression++;
    }

    // 如果有自定义消息，添加它
    if (message) {
        const char* msg_separator = " (";
        const char* msg_sep_ptr = msg_separator;
        while (*msg_sep_ptr && pos < MAX_ERROR_MESSAGE_LENGTH - 3) {
            current_failure_message[pos++] = *msg_sep_ptr++;
        }

        while (*message && pos < MAX_ERROR_MESSAGE_LENGTH - 2) {
            current_failure_message[pos++] = *message++;
        }

        if (pos < MAX_ERROR_MESSAGE_LENGTH - 1) {
            current_failure_message[pos++] = ')';
        }
    }

    current_failure_message[pos] = '\0';
}

// ============================================================================
// UartWriter 实现 - 架构特定的UART输出
// ============================================================================

void UartWriter::write_char(char c) noexcept {
    // 使用架构抽象层输出单个字符
    #if defined(MOSS_ARCH_ARM64) || defined(MOSS_ARCH_X86_64) || defined(MOSS_ARCH_RISCV)
    // 这里应该调用具体的UART驱动，暂时使用简化实现
    // 在真实内核中，这会调用uart_driver输出字符
    volatile char* uart_base = reinterpret_cast<volatile char*>(0x09000000); // QEMU UART地址
    *uart_base = c;
    #endif
}

void UartWriter::write_string(const char* str) noexcept {
    if (!str) return;

    while (*str) {
        write_char(*str);
        str++;
    }
}

void UartWriter::write_u32(u32 value) noexcept {
    if (value == 0) {
        write_char('0');
        return;
    }

    char buffer[16]; // u32最多10位
    u32 pos = 0;

    while (value > 0 && pos < 15) {
        buffer[pos++] = '0' + (value % 10);
        value /= 10;
    }

    // 反向输出
    for (u32 i = pos; i > 0; i--) {
        write_char(buffer[i - 1]);
    }
}

void UartWriter::write_u64(u64 value) noexcept {
    if (value == 0) {
        write_char('0');
        return;
    }

    char buffer[24]; // u64最多20位
    u32 pos = 0;

    while (value > 0 && pos < 23) {
        buffer[pos++] = '0' + (value % 10);
        value /= 10;
    }

    // 反向输出
    for (u32 i = pos; i > 0; i--) {
        write_char(buffer[i - 1]);
    }
}

void UartWriter::write_hex_u32(u32 value) noexcept {
    write_string("0x");

    const char hex_chars[] = "0123456789ABCDEF";
    bool started = false;

    for (i32 i = 7; i >= 0; i--) {
        u8 nibble = (value >> (i * 4)) & 0xF;
        if (nibble != 0 || started || i == 0) {
            write_char(hex_chars[nibble]);
            started = true;
        }
    }
}

void UartWriter::write_hex_u64(u64 value) noexcept {
    write_string("0x");

    const char hex_chars[] = "0123456789ABCDEF";
    bool started = false;

    for (i32 i = 15; i >= 0; i--) {
        u8 nibble = (value >> (i * 4)) & 0xF;
        if (nibble != 0 || started || i == 0) {
            write_char(hex_chars[nibble]);
            started = true;
        }
    }
}

void UartWriter::write_newline() noexcept {
    write_char('\n');
}

// ============================================================================
// TestSuite 实现
// ============================================================================

VoidResult TestSuite::add_test(const char* test_name, void (*test_func)()) noexcept {
    if (!test_name || !test_func) {
        return Error(ErrorCode::InvalidParameter);
    }

    if (test_count_ >= MAX_TESTS_PER_SUITE) {
        return Error(ErrorCode::ResourceExhausted);
    }

    test_cases_[test_count_] = TestCase(test_name, test_func);
    test_count_++;

    return Ok();
}

TestResult TestSuite::run_all_tests() noexcept {
    TestResult result{};

    if (!suite_enabled_) {
        return result;
    }

    UartWriter::write_string("=== 运行测试套件: ");
    UartWriter::write_string(suite_name_);
    UartWriter::write_string(" ===\n");

    u64 suite_start_time = get_test_timestamp_ns();

    for (u32 i = 0; i < test_count_; i++) {
        if (!test_cases_[i].is_enabled) {
            continue;
        }

        result.total_tests++;

        // 重置测试状态
        g_test_state.reset_for_new_test(suite_name_, test_cases_[i].test_name);

        UartWriter::write_string("  运行测试: ");
        UartWriter::write_string(test_cases_[i].test_name);
        UartWriter::write_string(" ... ");

        u64 test_start_time = get_test_timestamp_ns();

        // 执行测试函数
        test_cases_[i].test_function();

        u64 test_end_time = get_test_timestamp_ns();
        u64 test_duration = test_end_time - test_start_time;

        if (g_test_state.current_test_failed) {
            result.failed_tests++;
            UartWriter::write_string("❌ 失败 (");
            UartWriter::write_u64(test_duration / 1000); // 微秒
            UartWriter::write_string("μs)\n");
            UartWriter::write_string("    错误: ");
            UartWriter::write_string(g_test_state.current_failure_message);
            UartWriter::write_string("\n");

            // 记录第一个失败信息
            if (result.first_failure[0] == '\0') {
                u32 pos = 0;
                const char* src = g_test_state.current_failure_message;
                while (*src && pos < MAX_ERROR_MESSAGE_LENGTH - 1) {
                    result.first_failure[pos++] = *src++;
                }
                result.first_failure[pos] = '\0';
            }
        } else {
            result.passed_tests++;
            UartWriter::write_string("✅ 通过 (");
            UartWriter::write_u64(test_duration / 1000); // 微秒
            UartWriter::write_string("μs)\n");
        }
    }

    u64 suite_end_time = get_test_timestamp_ns();
    result.execution_time_ns = suite_end_time - suite_start_time;

    // 输出套件汇总
    UartWriter::write_string("套件 '");
    UartWriter::write_string(suite_name_);
    UartWriter::write_string("' 完成: ");
    UartWriter::write_u32(result.passed_tests);
    UartWriter::write_string("/");
    UartWriter::write_u32(result.total_tests);
    UartWriter::write_string(" 通过 (");
    UartWriter::write_u64(result.execution_time_ns / 1000000); // 毫秒
    UartWriter::write_string("ms)\n\n");

    return result;
}

VoidResult TestSuite::set_test_enabled(const char* test_name, bool enabled) noexcept {
    if (!test_name) {
        return Error(ErrorCode::InvalidParameter);
    }

    for (u32 i = 0; i < test_count_; i++) {
        // 简单字符串比较
        const char* a = test_cases_[i].test_name;
        const char* b = test_name;
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
            test_cases_[i].is_enabled = enabled;
            return Ok();
        }
    }

    return Error(ErrorCode::NotFound);
}

// ============================================================================
// 断言实现函数
// ============================================================================

void assert_true_impl(const char* file, u32 line, bool condition, const char* expression) noexcept {
    g_test_state.total_assertions++;
    if (!condition) {
        g_test_state.record_assertion_failure(file, line, expression);
    }
}

void assert_false_impl(const char* file, u32 line, bool condition, const char* expression) noexcept {
    g_test_state.total_assertions++;
    if (condition) {
        g_test_state.record_assertion_failure(file, line, expression);
    }
}

void assert_eq_u32_impl(const char* file, u32 line, u32 expected, u32 actual, const char* expr) noexcept {
    g_test_state.total_assertions++;
    if (expected != actual) {
        // 构建详细错误消息
        char detailed_msg[128];
        u32 pos = 0;

        const char* prefix = "expected ";
        while (*prefix && pos < 100) {
            detailed_msg[pos++] = *prefix++;
        }

        // 简化的数字转换 (期望值)
        char exp_str[16];
        u32 exp_pos = 0;
        u32 temp_exp = expected;
        if (temp_exp == 0) {
            exp_str[exp_pos++] = '0';
        } else {
            while (temp_exp > 0 && exp_pos < 15) {
                exp_str[exp_pos++] = '0' + (temp_exp % 10);
                temp_exp /= 10;
            }
            // 反转
            for (u32 i = 0; i < exp_pos / 2; i++) {
                char temp = exp_str[i];
                exp_str[i] = exp_str[exp_pos - 1 - i];
                exp_str[exp_pos - 1 - i] = temp;
            }
        }
        exp_str[exp_pos] = '\0';

        // 复制期望值
        u32 exp_idx = 0;
        while (exp_str[exp_idx] && pos < 110) {
            detailed_msg[pos++] = exp_str[exp_idx++];
        }

        const char* middle = ", got ";
        while (*middle && pos < 120) {
            detailed_msg[pos++] = *middle++;
        }

        // 实际值
        char act_str[16];
        u32 act_pos = 0;
        u32 temp_act = actual;
        if (temp_act == 0) {
            act_str[act_pos++] = '0';
        } else {
            while (temp_act > 0 && act_pos < 15) {
                act_str[act_pos++] = '0' + (temp_act % 10);
                temp_act /= 10;
            }
            // 反转
            for (u32 i = 0; i < act_pos / 2; i++) {
                char temp = act_str[i];
                act_str[i] = act_str[act_pos - 1 - i];
                act_str[act_pos - 1 - i] = temp;
            }
        }
        act_str[act_pos] = '\0';

        // 复制实际值
        u32 act_idx = 0;
        while (act_str[act_idx] && pos < 127) {
            detailed_msg[pos++] = act_str[act_idx++];
        }

        detailed_msg[pos] = '\0';

        g_test_state.record_assertion_failure(file, line, expr, detailed_msg);
    }
}

void assert_eq_u64_impl(const char* file, u32 line, u64 expected, u64 actual, const char* expr) noexcept {
    g_test_state.total_assertions++;
    if (expected != actual) {
        g_test_state.record_assertion_failure(file, line, expr, "u64 values not equal");
    }
}

void assert_eq_ptr_impl(const char* file, u32 line, const void* expected, const void* actual, const char* expr) noexcept {
    g_test_state.total_assertions++;
    if (expected != actual) {
        g_test_state.record_assertion_failure(file, line, expr, "pointers not equal");
    }
}

void assert_null_impl(const char* file, u32 line, const void* ptr, const char* expression) noexcept {
    g_test_state.total_assertions++;
    if (ptr != nullptr) {
        g_test_state.record_assertion_failure(file, line, expression, "pointer is not null");
    }
}

void assert_not_null_impl(const char* file, u32 line, const void* ptr, const char* expression) noexcept {
    g_test_state.total_assertions++;
    if (ptr == nullptr) {
        g_test_state.record_assertion_failure(file, line, expression, "pointer is null");
    }
}

// ============================================================================
// 时间测量实现
// ============================================================================

u64 get_test_timestamp_ns() noexcept {
    // 使用架构特定的高精度时间戳
    #if defined(MOSS_ARCH_ARM64)
    // ARM64: 使用系统计数器
    u64 ticks;
    asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
    // 假设系统计数器频率为25MHz (QEMU默认)
    return (ticks * 1000000000ULL) / 25000000ULL;

    #elif defined(MOSS_ARCH_X86_64)
    // x86_64: 使用RDTSC
    u32 low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    u64 ticks = (static_cast<u64>(high) << 32) | low;
    // 假设TSC频率为1GHz
    return ticks;

    #elif defined(MOSS_ARCH_RISCV)
    // RISC-V: 使用cycle计数器
    u64 ticks;
    asm volatile("rdcycle %0" : "=r"(ticks));
    // 假设计数器频率为100MHz
    return (ticks * 1000000000ULL) / 100000000ULL;

    #else
    // 回退实现
    static u64 counter = 0;
    return ++counter * 1000; // 简单递增，每次1微秒
    #endif
}

// ============================================================================
// QEMU Semihosting Exit Implementation
// ============================================================================

// 外部函数声明
extern "C" void early_debug_print(const char* message) noexcept;

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

    // 使用QEMU semihosting机制正确退出QEMU
    // 这样测试完成后QEMU会立即退出，而不需要手动终止
    #if defined(MOSS_ARCH_ARM64)
    // ARM64 QEMU semihosting exit
    u32 exit_status = (exit_code == TestExitCode::AllPassed) ? 0 : 1;

    // 使用 ADP_Stopped_ApplicationExit semihosting调用
    // 这是标准的ARM semihosting退出机制
    asm volatile(
        "mov x0, #0x18\n"      // ADP_Stopped_ApplicationExit
        "mov x1, %0\n"         // exit status
        "hlt #0xf000\n"        // semihosting breakpoint
        :
        : "r"(static_cast<u64>(exit_status))
        : "x0", "x1"
    );

    #elif defined(MOSS_ARCH_X86_64)
    // x86_64: 使用QEMU调试端口退出
    u32 exit_status = (exit_code == TestExitCode::AllPassed) ? 0 : 1;
    asm volatile("outl %0, $0xf4" : : "a"(exit_status));

    #elif defined(MOSS_ARCH_RISCV)
    // RISC-V: 使用SBI系统重置调用
    u32 exit_status = (exit_code == TestExitCode::AllPassed) ? 0 : 1;
    asm volatile(
        "li a0, 0\n"           // shutdown type
        "li a1, 0\n"           // reason
        "li a7, 8\n"           // SBI system reset
        "ecall\n"
        :
        :
        : "a0", "a1", "a7"
    );

    #else
    // 通用回退 - 死循环（真实硬件）
    while (true) {
        asm volatile("nop");
    }
    #endif

    // 这行代码永远不会执行到，但需要确保[[noreturn]]函数不返回
    __builtin_unreachable();
}

} // namespace moss::kernel::test
