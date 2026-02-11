// 内核运行时支持函数
// 提供必要的C库函数和内存管理函数的内核实现

#include "../include/arch/arch_abstraction.hpp"
#include "../include/types.hpp"
#include "../mm/runtime_heap_allocator.hpp"

// 前向声明必要的类型（避免循环依赖）
namespace moss::kernel::containers {
class SlabAllocator;
}

namespace moss::kernel::ipc {
class SharedMemoryManager;
class IpcManager;
}

// 在freestanding环境中定义必要的类型
using size_t = moss::kernel::usize;

extern "C" {

// 基本内存操作函数
void *memset(void *dst, int c, size_t n) noexcept {
  unsigned char *d = static_cast<unsigned char *>(dst);
  unsigned char value = static_cast<unsigned char>(c);

  for (size_t i = 0; i < n; ++i) {
    d[i] = value;
  }

  return dst;
}

void *memcpy(void *dst, const void *src, size_t n) noexcept {
  unsigned char *d = static_cast<unsigned char *>(dst);
  const unsigned char *s = static_cast<const unsigned char *>(src);

  for (size_t i = 0; i < n; ++i) {
    d[i] = s[i];
  }

  return dst;
}

void *memmove(void *dst, const void *src, size_t n) noexcept {
  unsigned char *d = static_cast<unsigned char *>(dst);
  const unsigned char *s = static_cast<const unsigned char *>(src);

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

int memcmp(const void *s1, const void *s2, size_t n) noexcept {
  const unsigned char *p1 = static_cast<const unsigned char *>(s1);
  const unsigned char *p2 = static_cast<const unsigned char *>(s2);

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

// 早期静态堆缓冲区 - 在RuntimeHeapAllocator初始化之前使用
static char early_heap_buffer[64 * 1024]; // 64KB早期堆
static size_t early_heap_used = 0;
static bool runtime_heap_ready = false;

// 运行时堆分配器支持 - 带fallback机制
void *kernel_malloc(size_t size) noexcept {
  using moss::kernel::mm::RuntimeHeapAllocator;

  // 如果RuntimeHeapAllocator已经初始化，使用它
  if (runtime_heap_ready) {
    auto result = RuntimeHeapAllocator::allocate(size);
    if (!result) {
      return nullptr; // 分配失败
    }

    void *ptr = result.value();
    // 清零分配的内存
    memset(ptr, 0, size);
    return ptr;
  } else {
    // 使用早期静态堆
    size_t aligned_size = (size + 15UL) & ~15UL; // 16字节对齐
    if (early_heap_used + aligned_size > sizeof(early_heap_buffer)) {
      return nullptr; // 早期堆空间不足
    }

    void *ptr = &early_heap_buffer[early_heap_used];
    early_heap_used += aligned_size;

    // 清零分配的内存
    memset(ptr, 0, size);
    return ptr;
  }
}

void kernel_free(void *ptr) noexcept {
  if (ptr == nullptr) {
    return;
  }

  using moss::kernel::mm::RuntimeHeapAllocator;

  // 如果RuntimeHeapAllocator已经初始化，使用它
  if (runtime_heap_ready) {
    // 注意：这里使用0作为大小，因为RuntimeHeapAllocator会从块头获取实际大小
    [[maybe_unused]] auto result = RuntimeHeapAllocator::deallocate(ptr, 0);

    // 在调试模式下可以检查释放结果
#ifdef DEBUG
    if (!result) {
      // 内存释放失败 - 可能是堆损坏
      moss::kernel::arch::kernel_panic();
    }
#endif
  } else {
    // 早期堆分配的内存不需要释放（静态缓冲区）
    // 在实际系统中，这些内存在RuntimeHeapAllocator初始化后就废弃了
  }
}

// 标记运行时堆已准备好
void mark_runtime_heap_ready() noexcept {
  runtime_heap_ready = true;
}

// C++ operator new/delete 实现
// 直接提供链接器需要的符号
void *_Znwm(size_t size) {
  // operator new(unsigned long) 的修饰符号
  void *ptr = kernel_malloc(size);
  if (!ptr) {
    // 内核panic - 内存耗尽是致命错误
    moss::kernel::arch::kernel_panic();
  }
  return ptr;
}

void *_Znam(size_t size) {
  // operator new[](unsigned long) 的修饰符号
  return _Znwm(size);
}

void _ZdlPvm(void *ptr, [[maybe_unused]] size_t size) {
  // operator delete(void*, unsigned long) 的修饰符号
  if (ptr) {
    kernel_free(ptr);
  }
}

void _ZdaPvm(void *ptr, [[maybe_unused]] size_t size) {
  // operator delete[](void*, unsigned long) 的修饰符号
  _ZdlPvm(ptr, size);
}

void _ZdlPv(void *ptr) {
  // operator delete(void*) 的修饰符号
  if (ptr) {
    kernel_free(ptr);
  }
}

void _ZdaPv(void *ptr) {
  // operator delete[](void*) 的修饰符号
  _ZdlPv(ptr);
}

// 对齐版本的operator new/delete
// std::align_val_t 在内核中定义为size_t
void *_ZnwmSt11align_val_t(size_t size, size_t alignment) {
  // operator new(unsigned long, std::align_val_t) 的修饰符号
  // 使用RuntimeHeapAllocator的对齐分配
  using moss::kernel::mm::RuntimeHeapAllocator;

  auto result = RuntimeHeapAllocator::allocate_aligned(size, alignment);
  if (!result) {
    // 内核panic - 内存耗尽是致命错误
    moss::kernel::arch::kernel_panic();
  }

  void *ptr = result.value();
  // 清零分配的内存
  memset(ptr, 0, size);

  return ptr;
}

void *_ZnamSt11align_val_t(size_t size, size_t alignment) {
  // operator new[](unsigned long, std::align_val_t) 的修饰符号
  return _ZnwmSt11align_val_t(size, alignment);
}

void _ZdlPvmSt11align_val_t(void *ptr, [[maybe_unused]] size_t size,
                            [[maybe_unused]] size_t alignment) {
  // operator delete(void*, unsigned long, std::align_val_t) 的修饰符号
  if (ptr) {
    kernel_free(ptr);
  }
}

void _ZdaPvmSt11align_val_t(void *ptr, [[maybe_unused]] size_t size,
                            [[maybe_unused]] size_t alignment) {
  // operator delete[](void*, unsigned long, std::align_val_t) 的修饰符号
  _ZdlPvmSt11align_val_t(ptr, size, alignment);
}

} // extern "C"

// 提供缺失的全局变量实例
namespace moss::kernel::containers {
// 全局slab分配器实例（简化实现）
SlabAllocator *g_slab_allocator = nullptr;
} // namespace moss::kernel::containers

namespace moss::kernel::ipc {
// 全局共享内存管理器实例（简化实现）
SharedMemoryManager *g_shared_memory_manager = nullptr;

// 全局IPC管理器实例
IpcManager *g_ipc_manager = nullptr;
} // namespace moss::kernel::ipc
