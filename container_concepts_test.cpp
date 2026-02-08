#include "src/include/concepts/container_concepts.hpp"
#include <iostream>

using namespace moss::concepts;
using namespace moss::kernel;

/**
 * @brief 简化的SPSC队列实现，用于测试concepts
 */
template<SPSCQueueElement T, usize Capacity>
    requires ValidQueueCapacity<Capacity>
class SimpleSPSCQueue {
private:
    T buffer_[Capacity];
    usize head_{0};
    usize tail_{0};

public:
    constexpr SimpleSPSCQueue() = default;

    bool enqueue(const T& item) noexcept {
        usize next = (tail_ + 1) % Capacity;
        if (next == head_) return false;

        buffer_[tail_] = item;
        tail_ = next;
        return true;
    }

    bool dequeue(T& item) noexcept {
        if (head_ == tail_) return false;

        item = buffer_[head_];
        head_ = (head_ + 1) % Capacity;
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_ == tail_;
    }

    [[nodiscard]] static constexpr usize capacity() noexcept {
        return Capacity;
    }
};

int main() {
    std::cout << "=== 容器 Concepts 测试 ===" << std::endl;

    // 测试SPSC队列
    SimpleSPSCQueue<u32, 64> queue;
    std::cout << "创建了容量为 " << queue.capacity() << " 的队列" << std::endl;

    // 测试入队
    std::cout << "入队测试:" << std::endl;
    for (u32 i = 1; i <= 3; ++i) {
        if (queue.enqueue(i)) {
            std::cout << "  ✓ 入队: " << i << std::endl;
        }
    }

    // 测试出队
    std::cout << "出队测试:" << std::endl;
    u32 value;
    while (queue.dequeue(value)) {
        std::cout << "  ✓ 出队: " << value << std::endl;
    }

    // 验证concepts
    static_assert(SPSCQueueElement<u32>);
    static_assert(ValidQueueCapacity<64>);
    static_assert(LockFreeElement<u32>);
    static_assert(CounterType<u64>);

    std::cout << "✅ 容器concepts测试通过！" << std::endl;

    return 0;
}