/**
 * @brief 直接路径包含测试
 */
#include "./src/include/types.hpp"
#include "./src/include/result.hpp"

using namespace moss::kernel;

int main() {
    u32 test = 42;
    return test == 42 ? 0 : 1;
}