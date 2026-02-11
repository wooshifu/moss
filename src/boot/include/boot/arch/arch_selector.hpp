/*
 * 架构选择器 - 编译时选择正确的架构实现
 * 根据编译器定义的宏自动选择对应的启动实现
 */

#pragma once

#include "boot_interface.hpp"

namespace moss::boot {

// 架构实现类声明和选择
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
// ARM64实现类声明
class ARM64BootImpl : public ArchBootInterface {
public:
    static moss::kernel::VoidResult hardware_early_init(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult setup_memory_management(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult setup_interrupts_and_exceptions(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult setup_smp_support(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult finalize_arch_init(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult detect_memory_layout(BootContext& ctx) noexcept;
    static u32 get_current_cpu_id() noexcept;
    [[noreturn]] static void arch_panic(const char* message) noexcept;
};
using ArchBoot = ARM64BootImpl;
#define MOSS_CURRENT_ARCH "ARM64"
#define MOSS_CURRENT_ARCH_ID 1

#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X86_64)
// x86_64实现类声明
class X86_64BootImpl : public ArchBootInterface {
public:
    static moss::kernel::VoidResult hardware_early_init(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult setup_memory_management(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult setup_interrupts_and_exceptions(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult setup_smp_support(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult finalize_arch_init(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult detect_memory_layout(BootContext& ctx) noexcept;
    static u32 get_current_cpu_id() noexcept;
    [[noreturn]] static void arch_panic(const char* message) noexcept;
};
using ArchBoot = X86_64BootImpl;
#define MOSS_CURRENT_ARCH "x86_64"
#define MOSS_CURRENT_ARCH_ID 2

#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV)
// RISC-V实现类声明
class RISCVBootImpl : public ArchBootInterface {
public:
    static moss::kernel::VoidResult hardware_early_init(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult setup_memory_management(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult setup_interrupts_and_exceptions(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult setup_smp_support(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult finalize_arch_init(BootContext& ctx) noexcept;
    static moss::kernel::VoidResult detect_memory_layout(BootContext& ctx) noexcept;
    static u32 get_current_cpu_id() noexcept;
    [[noreturn]] static void arch_panic(const char* message) noexcept;
};
using ArchBoot = RISCVBootImpl;
#define MOSS_CURRENT_ARCH "RISC-V"
#define MOSS_CURRENT_ARCH_ID 3

#else
#error "不支持的目标架构：请确保在ARM64、x86_64或RISC-V平台上编译"
#endif

/**
 * 编译时架构信息结构
 */
struct ArchInfo {
    const char* name;
    u32 id;
    const char* description;
};

/**
 * 获取当前架构信息
 */
constexpr ArchInfo get_current_arch_info() noexcept {
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
    return ArchInfo{
        .name = "ARM64",
        .id = 1,
        .description = "ARM 64-bit (AArch64) Architecture"
    };
#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X86_64)
    return ArchInfo{
        .name = "x86_64",
        .id = 2,
        .description = "x86-64 (AMD64) Architecture"
    };
#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV)
    return ArchInfo{
        .name = "RISC-V",
        .id = 3,
        .description = "RISC-V 64-bit Architecture"
    };
#endif
}

/**
 * 架构特定的编译时检查
 */
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
static_assert(sizeof(void*) == 8, "ARM64 must be 64-bit");
#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X86_64)
static_assert(sizeof(void*) == 8, "x86_64 must be 64-bit");
#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV)
static_assert(sizeof(void*) == 8, "RISC-V must be 64-bit (RV64)");
#endif

/**
 * 架构特定常量定义
 */
namespace arch_constants {
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
    constexpr u32 PAGE_SIZE = 4096;           // ARM64标准页面大小
    constexpr u32 CACHE_LINE_SIZE = 64;       // 典型缓存行大小
    constexpr u32 STACK_ALIGNMENT = 16;       // AAPCS64要求16字节对齐
    constexpr VirtAddr KERNEL_VIRT_BASE = 0xFFFF000000000000ULL; // 内核虚拟基址

#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X86_64)
    constexpr u32 PAGE_SIZE = 4096;           // x86_64标准页面大小
    constexpr u32 CACHE_LINE_SIZE = 64;       // 典型缓存行大小
    constexpr u32 STACK_ALIGNMENT = 16;       // System V AMD64 ABI要求16字节对齐
    constexpr VirtAddr KERNEL_VIRT_BASE = 0xFFFF800000000000ULL; // 内核虚拟基址

#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV)
    constexpr u32 PAGE_SIZE = 4096;           // RISC-V标准页面大小
    constexpr u32 CACHE_LINE_SIZE = 64;       // 典型缓存行大小
    constexpr u32 STACK_ALIGNMENT = 16;       // RISC-V calling convention要求16字节对齐
    constexpr VirtAddr KERNEL_VIRT_BASE = 0xFFFFFFFF80000000ULL; // 内核虚拟基址 (Sv39)
#endif
} // namespace arch_constants

/**
 * 静态断言验证架构一致性
 */
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
static_assert(arch_constants::STACK_ALIGNMENT >= 16, "ARM64需要至少16字节栈对齐");
#elif defined(__x86_64__) || defined(__x86_64) || defined(MOSS_ARCH_X86_64)
static_assert(arch_constants::STACK_ALIGNMENT >= 16, "x86_64需要至少16字节栈对齐");
#elif defined(__riscv) || defined(__riscv__) || defined(MOSS_ARCH_RISCV)
static_assert(arch_constants::STACK_ALIGNMENT >= 16, "RISC-V需要至少16字节栈对齐");
#endif

} // namespace moss::boot
