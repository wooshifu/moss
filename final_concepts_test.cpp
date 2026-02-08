/**
 * @file final_concepts_test.cpp
 * @brief 最终的C++23 concepts集成测试
 * @author MOSS Kernel Team
 *
 * 这个文件验证所有concepts文件都能正确编译和工作
 */

#include "src/include/concepts/kernel_concepts.hpp"
#include "src/include/concepts/memory_concepts.hpp"
#include "src/include/concepts/container_concepts.hpp"
#include <iostream>

using namespace moss::concepts;
using namespace moss::kernel;

/**
 * @brief 使用concepts约束的模板函数示例
 */
template<KernelSafe T>
constexpr T safe_add(T a, T b) {
    return a + b;
}

template<QueueElement T, usize Cap>
    requires ValidQueueCapacity<Cap>
constexpr usize queue_size() {
    return Cap;
}

template<UniquePtrCompatible T>
class TestUniquePtr {
private:
    T* ptr_;
public:
    explicit TestUniquePtr(T* p = nullptr) : ptr_(p) {}
    ~TestUniquePtr() { delete ptr_; }
    T& operator*() const { return *ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
};

int main() {
    std::cout << "=== MOSS C++23 Concepts 最终测试 ===" << std::endl;

    // 测试所有基础concepts
    static_assert(KernelSafe<u32>);
    static_assert(SmallType<u64>);
    static_assert(AtomicCompatible<u32>);
    static_assert(IntegerType<u64>);
    static_assert(PowerOfTwo<128>);
    static_assert(ValidCapacity<256>);

    std::cout << "✅ 基础concepts验证通过" << std::endl;

    // 测试容器concepts
    static_assert(LockFreeElement<u32>);
    static_assert(SPSCQueueElement<u32>);
    static_assert(ValidQueueCapacity<512>);
    static_assert(CounterType<u64>);
    static_assert(PoolableObject<u32>);
    static_assert(MPMCQueueElement<u32>);

    std::cout << "✅ 容器concepts验证通过" << std::endl;

    // 测试内存concepts
    static_assert(UniquePtrCompatible<int>);
    static_assert(SharedPtrCompatible<int>);
    static_assert(PtrCompatible<int>);

    std::cout << "✅ 内存concepts验证通过" << std::endl;

    // 测试concepts约束的函数
    auto result = safe_add<u32>(10, 20);
    std::cout << "safe_add(10, 20) = " << result << std::endl;

    auto size = queue_size<u32, 64>();
    std::cout << "queue_size<u32, 64>() = " << size << std::endl;

    // 测试concepts约束的类模板
    TestUniquePtr<int> ptr(new int(42));
    if (ptr) {
        std::cout << "TestUniquePtr<int> 值: " << *ptr << std::endl;
    }

    std::cout << "✅ 所有MOSS C++23 Concepts测试通过！" << std::endl;
    std::cout << "🎉 编译错误已全部修复，concepts系统正常工作！" << std::endl;

    return 0;
}