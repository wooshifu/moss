/**
 * @brief 最小化内核编译测试
 */
#include "types.hpp"
#include "result.hpp"

using namespace moss::kernel;

int main() {
    // 测试基础类型
    u32 test_val = 42;

    // 测试Result类型
    using TestResult = Result<u32, ErrorCode>;
    TestResult result{test_val};

    if (result.has_value()) {
        return 0;
    }
    return 1;
}