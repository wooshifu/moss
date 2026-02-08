/**
 * @brief 直接路径包含测试
 */
#include "/home/wu/mercedes-benz/moss/src/include/types.hpp"
#include "/home/wu/mercedes-benz/moss/src/include/result.hpp"

using namespace moss::kernel;

int main() {
    u32 test = 42;
    return test == 42 ? 0 : 1;
}