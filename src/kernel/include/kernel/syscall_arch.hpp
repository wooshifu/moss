#pragma once

// 多架构系统调用入口点选择
// 根据目标架构包含对应的汇编实现

#include "core/arch/arch_abstraction.hpp"

namespace moss::kernel::arch::syscall {

// 架构特定的汇编函数声明
// 这些函数在对应的汇编文件中实现
extern "C" {
    // 系统调用入口点 - 架构特定汇编实现
    void syscall_entry_point() noexcept;

    // 系统调用返回处理 - 架构特定汇编实现
    void syscall_return(SyscallContext* context) noexcept;

    // 系统调用分发器 - C++实现 (kernel_main.cpp)
    long system_call_handler(long syscall_number, long arg0, long arg1,
                            long arg2, long arg3, long arg4, long arg5) noexcept;
}

// 统一的系统调用初始化接口
inline bool initialize_architecture_syscalls() noexcept {
#if defined(MOSS_ARCH_ARM64)
    // ARM64: 需要设置异常向量表来处理SVC指令
    // TODO: 设置EL1异常向量表，将SVC异常指向syscall_entry_point
    // 目前暂时返回true，表示初始化成功
    return true;

#elif defined(MOSS_ARCH_X86_64)
    // X86_64: 设置SYSCALL指令的MSR寄存器
    initialize_syscall_support();
    return true;

#elif defined(MOSS_ARCH_RISCV)
    // RISC-V: 需要设置trap向量表来处理ECALL指令
    // TODO: 设置stvec寄存器指向syscall_entry_point
    // 目前暂时返回true，表示初始化成功
    return true;

#else
    // 不支持的架构
    return false;
#endif
}

// 获取当前架构的系统调用约定信息
struct SyscallConvention {
    const char* arch_name;              // 架构名称
    const char* syscall_instruction;    // 系统调用指令
    const char* syscall_nr_register;    // 系统调用号寄存器
    const char* return_register;        // 返回值寄存器
    const char* arg_registers[6];       // 参数寄存器列表
};

inline const SyscallConvention& get_syscall_convention() noexcept {
#if defined(MOSS_ARCH_ARM64)
    static const SyscallConvention conv = {
        .arch_name = "ARM64",
        .syscall_instruction = "SVC",
        .syscall_nr_register = "x8",
        .return_register = "x0",
        .arg_registers = {"x0", "x1", "x2", "x3", "x4", "x5"}
    };
    return conv;

#elif defined(MOSS_ARCH_X86_64)
    static const SyscallConvention conv = {
        .arch_name = "x86_64",
        .syscall_instruction = "SYSCALL",
        .syscall_nr_register = "rax",
        .return_register = "rax",
        .arg_registers = {"rdi", "rsi", "rdx", "r10", "r8", "r9"}
    };
    return conv;

#elif defined(MOSS_ARCH_RISCV)
    static const SyscallConvention conv = {
        .arch_name = "RISC-V",
        .syscall_instruction = "ECALL",
        .syscall_nr_register = "a7",
        .return_register = "a0",
        .arg_registers = {"a0", "a1", "a2", "a3", "a4", "a5"}
    };
    return conv;

#else
    static const SyscallConvention conv = {
        .arch_name = "Unknown",
        .syscall_instruction = "Unknown",
        .syscall_nr_register = "Unknown",
        .return_register = "Unknown",
        .arg_registers = {"Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown"}
    };
    return conv;
#endif
}

// 调试：打印当前架构的系统调用约定
void print_syscall_convention() noexcept;

} // namespace moss::kernel::arch::syscall
