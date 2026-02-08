#pragma once

// Moss 微内核基础类型定义（无标准库环境）

namespace moss::kernel {

// 基础整数类型（原生定义，无需标准库）
using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;

using i8 = signed char;
using i16 = signed short;
using i32 = signed int;
using i64 = signed long long;

// 平台相关的 size_t 和 ptrdiff_t 定义
#ifdef MOSS_ARCH_X86_64
using usize = unsigned long;
using isize = signed long;
#elif defined(MOSS_ARCH_ARM64)
using usize = unsigned long;
using isize = signed long;
#elif defined(MOSS_ARCH_RISCV)
using usize = unsigned long;
using isize = signed long;
#else
using usize = unsigned long long;
using isize = signed long long;
#endif

// 物理和虚拟地址类型
using PhysAddr = u64;
using VirtAddr = u64;

// 页面相关常量
static constexpr usize PAGE_SIZE = 4096;
static constexpr usize PAGE_SHIFT = 12;
static constexpr usize LARGE_PAGE_SIZE = 2 * 1024 * 1024;  // 2MB
static constexpr usize HUGE_PAGE_SIZE = 1024 * 1024 * 1024; // 1GB

// 内存布局常量
static constexpr VirtAddr KERNEL_BASE = 0xFFFF800000000000ULL;
static constexpr VirtAddr USER_BASE = 0x0000000000000000ULL;
static constexpr VirtAddr USER_MAX = 0x0000800000000000ULL;

// 进程和线程标识符
using ProcessId = u32;
using ThreadId = u64;
using EndpointId = u32;
using DeviceId = u32;
using InterruptId = u32;

// IPC相关类型
using MessageId = u64;
using ChannelId = u32;
using ShmId = u32;
using ServiceId = u32;

// 特殊ID值
static constexpr ProcessId INVALID_PROCESS_ID = 0;
static constexpr ThreadId INVALID_THREAD_ID = 0;
static constexpr EndpointId INVALID_ENDPOINT_ID = 0;

// ARM64特定常量
static constexpr usize CACHE_LINE_SIZE = 64;
static constexpr usize MAX_CPUS = 8;

// 编译时对齐宏
#define ALIGNED(x) __attribute__((aligned(x)))
#define CACHE_ALIGNED ALIGNED(CACHE_LINE_SIZE)
#define PAGE_ALIGNED ALIGNED(PAGE_SIZE)

// 禁用拷贝和移动的宏
#define NON_COPYABLE(ClassName) \
    ClassName(const ClassName&) = delete; \
    ClassName& operator=(const ClassName&) = delete;

#define NON_MOVABLE(ClassName) \
    ClassName(ClassName&&) = delete; \
    ClassName& operator=(ClassName&&) = delete;

#define NON_COPYABLE_NON_MOVABLE(ClassName) \
    NON_COPYABLE(ClassName) \
    NON_MOVABLE(ClassName)

} // namespace moss::kernel

// 全局操作符重载（placement new）
void* operator new(unsigned long, void* ptr) noexcept;
void* operator new[](unsigned long, void* ptr) noexcept;
void operator delete(void*, void*) noexcept;
void operator delete[](void*, void*) noexcept;