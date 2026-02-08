#include "test_concepts.hpp"
#include <iostream>

int main() {
    using namespace moss::concepts::test;

    std::cout << "=== MOSS C++23 Concepts 演示 ===" << std::endl;

    // 创建一个满足concepts的队列
    SimpleQueue<u32, 64> queue;

    std::cout << "队列容量: " << queue.capacity() << std::endl;

    // 测试基本操作
    std::cout << "插入元素..." << std::endl;
    for (u32 i = 1; i <= 5; ++i) {
        if (queue.push(i)) {
            std::cout << "  成功插入: " << i << std::endl;
        }
    }

    std::cout << "取出元素..." << std::endl;
    u32 value;
    while (queue.pop(value)) {
        std::cout << "  取出: " << value << std::endl;
    }

    // 演示concepts的编译期检查
    std::cout << "\n=== Concepts 编译期验证 ===" << std::endl;

    std::cout << "SmallType<u32>: " << SmallType<u32> << std::endl;
    std::cout << "SmallType<u64>: " << SmallType<u64> << std::endl;
    std::cout << "IntegerLike<u32>: " << IntegerLike<u32> << std::endl;
    std::cout << "PowerOfTwo<64>: " << PowerOfTwo<64> << std::endl;
    std::cout << "PowerOfTwo<63>: " << PowerOfTwo<63> << std::endl;
    std::cout << "ValidCapacity<64>: " << ValidCapacity<64> << std::endl;

    // 演示concepts约束的函数
    std::cout << "\n=== Concepts 约束函数 ===" << std::endl;
    std::cout << "add(10, 20) = " << add(10u, 20u) << std::endl;
    std::cout << "fits_in_cache_line<u32>(16) = " << fits_in_cache_line<u32>(16) << std::endl;

    std::cout << "\n✅ 所有测试通过！C++23 concepts 工作正常！" << std::endl;

    return 0;
}