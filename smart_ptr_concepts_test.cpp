#include "src/include/concepts/memory_concepts.hpp"
#include <iostream>

using namespace moss::concepts;
using namespace moss::kernel;

/**
 * @brief 简化的UniquePtr实现，用于测试concepts
 */
template<UniquePtrCompatible T>
class SimpleUniquePtr {
private:
    T* ptr_;

public:
    explicit SimpleUniquePtr(T* p = nullptr) noexcept : ptr_(p) {}
    ~SimpleUniquePtr() { delete ptr_; }

    // 移动语义
    SimpleUniquePtr(SimpleUniquePtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    SimpleUniquePtr& operator=(SimpleUniquePtr&& other) noexcept {
        if (this != &other) {
            delete ptr_;
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    // 禁用复制
    SimpleUniquePtr(const SimpleUniquePtr&) = delete;
    SimpleUniquePtr& operator=(const SimpleUniquePtr&) = delete;

    T* get() const noexcept { return ptr_; }
    T& operator*() const { return *ptr_; }
    T* operator->() const noexcept { return ptr_; }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }
};

int main() {
    std::cout << "=== 智能指针 Concepts 测试 ===" << std::endl;

    // 测试UniquePtr concepts约束
    SimpleUniquePtr<int> ptr(new int(42));
    if (ptr) {
        std::cout << "智能指针值: " << *ptr << std::endl;
    }

    // 移动测试
    SimpleUniquePtr<int> ptr2 = std::move(ptr);
    if (ptr2) {
        std::cout << "移动后的值: " << *ptr2 << std::endl;
    }

    // 验证concepts
    static_assert(UniquePtrCompatible<int>);
    static_assert(SharedPtrCompatible<int>);
    static_assert(KernelSafe<int>);

    std::cout << "✅ 智能指针concepts测试通过！" << std::endl;

    return 0;
}