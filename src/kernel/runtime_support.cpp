// 内核运行时支持函数
// 提供必要的C库函数和内存管理函数的内核实现

#include "../include/types.hpp"
#include "../include/arch/arch_abstraction.hpp"

// 前向声明必要的类型（避免循环依赖）
namespace moss::kernel::containers {
    class SlabAllocator;
}

namespace moss::kernel::ipc {
    class SharedMemoryManager;
}

// 在freestanding环境中定义必要的类型
using size_t = moss::kernel::usize;

extern "C" {

// 基本内存操作函数
void* memset(void* dst, int c, size_t n) noexcept {
    unsigned char* d = static_cast<unsigned char*>(dst);
    unsigned char value = static_cast<unsigned char>(c);

    for (size_t i = 0; i < n; ++i) {
        d[i] = value;
    }

    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) noexcept {
    unsigned char* d = static_cast<unsigned char*>(dst);
    const unsigned char* s = static_cast<const unsigned char*>(src);

    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }

    return dst;
}

void* memmove(void* dst, const void* src, size_t n) noexcept {
    unsigned char* d = static_cast<unsigned char*>(dst);
    const unsigned char* s = static_cast<const unsigned char*>(src);

    if (d < s) {
        // 向前拷贝
        for (size_t i = 0; i < n; ++i) {
            d[i] = s[i];
        }
    } else if (d > s) {
        // 向后拷贝
        for (size_t i = n; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }

    return dst;
}

int memcmp(const void* s1, const void* s2, size_t n) noexcept {
    const unsigned char* p1 = static_cast<const unsigned char*>(s1);
    const unsigned char* p2 = static_cast<const unsigned char*>(s2);

    for (size_t i = 0; i < n; ++i) {
        if (p1[i] < p2[i]) {
            return -1;
        } else if (p1[i] > p2[i]) {
            return 1;
        }
    }

    return 0;
}

// 系统调用处理器（临时实现）
void syscall_handler() noexcept {
    // 目前只是一个占位符实现
    // 实际的系统调用处理逻辑稍后实现
    asm volatile("nop");
}

// 简单的内核内存分配器
namespace {
    // 静态内存池，用于临时分配
    alignas(64) char kernel_heap[2 * 1024 * 1024];  // 2MB静态堆
    size_t heap_used = 0;
}

void* kernel_malloc(size_t size) noexcept {
    // 简单的线性分配器（仅用于调试）
    // 对齐到8字节边界
    size = (size + 7UL) & ~7UL;

    if (heap_used + size > sizeof(kernel_heap)) {
        return nullptr;  // 堆耗尽
    }

    void* result = &kernel_heap[heap_used];
    heap_used += size;

    // 清零分配的内存
    memset(result, 0, size);

    return result;
}

void kernel_free([[maybe_unused]] void* ptr) noexcept {
    // 简单实现：不实际释放内存
    // 实际内核需要真正的内存管理
}

// C++ operator new/delete 实现
// 直接提供链接器需要的符号
void* _Znwm(size_t size) {
    // operator new(unsigned long) 的修饰符号
    void* ptr = kernel_malloc(size);
    if (!ptr) {
        // 内核panic - 内存耗尽是致命错误
        moss::kernel::arch::kernel_panic();
    }
    return ptr;
}

void* _Znam(size_t size) {
    // operator new[](unsigned long) 的修饰符号
    return _Znwm(size);
}

void _ZdlPvm(void* ptr, [[maybe_unused]] size_t size) {
    // operator delete(void*, unsigned long) 的修饰符号
    if (ptr) {
        kernel_free(ptr);
    }
}

void _ZdaPvm(void* ptr, [[maybe_unused]] size_t size) {
    // operator delete[](void*, unsigned long) 的修饰符号
    _ZdlPvm(ptr, size);
}

void _ZdlPv(void* ptr) {
    // operator delete(void*) 的修饰符号
    if (ptr) {
        kernel_free(ptr);
    }
}

void _ZdaPv(void* ptr) {
    // operator delete[](void*) 的修饰符号
    _ZdlPv(ptr);
}

// 对齐版本的operator new/delete
// std::align_val_t 在内核中定义为size_t
void* _ZnwmSt11align_val_t(size_t size, size_t alignment) {
    // operator new(unsigned long, std::align_val_t) 的修饰符号
    // 在简单实现中，忽略对齐要求（实际内核应该处理对齐）
    (void)alignment;  // 忽略对齐参数
    void* ptr = kernel_malloc(size);
    if (!ptr) {
        // 内核panic - 内存耗尽是致命错误
        moss::kernel::arch::kernel_panic();
    }
    return ptr;
}

void* _ZnamSt11align_val_t(size_t size, size_t alignment) {
    // operator new[](unsigned long, std::align_val_t) 的修饰符号
    return _ZnwmSt11align_val_t(size, alignment);
}

void _ZdlPvmSt11align_val_t(void* ptr, [[maybe_unused]] size_t size, [[maybe_unused]] size_t alignment) {
    // operator delete(void*, unsigned long, std::align_val_t) 的修饰符号
    if (ptr) {
        kernel_free(ptr);
    }
}

void _ZdaPvmSt11align_val_t(void* ptr, [[maybe_unused]] size_t size, [[maybe_unused]] size_t alignment) {
    // operator delete[](void*, unsigned long, std::align_val_t) 的修饰符号
    _ZdlPvmSt11align_val_t(ptr, size, alignment);
}

} // extern "C"

// 提供缺失的全局变量实例
namespace moss::kernel::containers {
    // 全局slab分配器实例（简化实现）
    SlabAllocator* g_slab_allocator = nullptr;
}

namespace moss::kernel::ipc {
    // 全局共享内存管理器实例（简化实现）
    SharedMemoryManager* g_shared_memory_manager = nullptr;
}
