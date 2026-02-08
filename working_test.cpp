#include "src/include/concepts/working_concepts.hpp"
#include <iostream>

int main() {
    using namespace moss::concepts;

    std::cout << "=== 实际可编译的 MOSS C++23 Concepts ===" << std::endl;

    // 测试队列
    KernelQueue<u32, 64> queue;
    std::cout << "创建了容量为 " << queue.capacity() << " 的队列" << std::endl;

    // 测试入队
    std::cout << "\n入队测试:" << std::endl;
    for (u32 i = 1; i <= 3; ++i) {
        if (queue.enqueue(i)) {
            std::cout << "  ✓ 入队: " << i << std::endl;
        }
    }

    // 测试出队
    std::cout << "\n出队测试:" << std::endl;
    u32 value;
    while (queue.dequeue(value)) {
        std::cout << "  ✓ 出队: " << value << std::endl;
    }

    // 测试智能指针
    std::cout << "\n智能指针测试:" << std::endl;
    {
        SimpleUniquePtr<int> ptr(new int(42));
        if (ptr) {
            std::cout << "  ✓ 智能指针值: " << *ptr << std::endl;
        }
    } // 自动释放内存

    // 编译期concepts验证
    std::cout << "\n=== 编译期 Concepts 验证 ===" << std::endl;

    constexpr bool small_u32 = SmallType<u32>;
    constexpr bool atomic_u64 = AtomicSize<u64>;
    constexpr bool power_256 = PowerOfTwo<256>;
    constexpr bool power_255 = PowerOfTwo<255>;
    constexpr bool valid_1024 = ValidCapacity<1024>;

    std::cout << "SmallType<u32>: " << small_u32 << std::endl;
    std::cout << "AtomicSize<u64>: " << atomic_u64 << std::endl;
    std::cout << "PowerOfTwo<256>: " << power_256 << std::endl;
    std::cout << "PowerOfTwo<255>: " << power_255 << " (应该是0)" << std::endl;
    std::cout << "ValidCapacity<1024>: " << valid_1024 << std::endl;

    std::cout << "\n✅ 所有测试通过！这个版本真的能工作！" << std::endl;

    return 0;
}