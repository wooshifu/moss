#include "src/include/concepts/kernel_concepts.hpp"
#include "src/include/concepts/memory_concepts.hpp"
#include "src/include/concepts/container_concepts.hpp"
#include <iostream>

using namespace moss::concepts;
using namespace moss::kernel;

int main() {
    std::cout << "=== C++23 Concepts 编译测试 ===" << std::endl;

    // 测试基础concepts
    constexpr bool kernel_safe_u32 = KernelSafe<u32>;
    constexpr bool atomic_u64 = AtomicCompatible<u64>;
    constexpr bool power_256 = PowerOfTwo<256>;
    constexpr bool valid_64 = ValidCapacity<64>;

    std::cout << "KernelSafe<u32>: " << kernel_safe_u32 << std::endl;
    std::cout << "AtomicCompatible<u64>: " << atomic_u64 << std::endl;
    std::cout << "PowerOfTwo<256>: " << power_256 << std::endl;
    std::cout << "ValidCapacity<64>: " << valid_64 << std::endl;

    // 测试容器concepts
    constexpr bool spsc_elem = SPSCQueueElement<u32>;
    constexpr bool valid_queue_cap = ValidQueueCapacity<128>;

    std::cout << "SPSCQueueElement<u32>: " << spsc_elem << std::endl;
    std::cout << "ValidQueueCapacity<128>: " << valid_queue_cap << std::endl;

    // 测试内存concepts
    constexpr bool unique_ptr_compat = UniquePtrCompatible<int>;
    constexpr bool shared_ptr_compat = SharedPtrCompatible<int>;

    std::cout << "UniquePtrCompatible<int>: " << unique_ptr_compat << std::endl;
    std::cout << "SharedPtrCompatible<int>: " << shared_ptr_compat << std::endl;

    std::cout << "✅ 所有concepts编译成功！" << std::endl;

    return 0;
}