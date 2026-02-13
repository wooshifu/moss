#pragma once

// 架构特定的汇编指令抽象层
// 为不同架构提供统一的接口

#include "../types.hpp"

// 默认架构检测
#ifndef MOSS_ARCH_ARM64
#ifndef MOSS_ARCH_X86_64
#ifndef MOSS_ARCH_RISCV
// 如果没有定义架构，默认使用x86_64
#if defined(__x86_64__) || defined(__x86_64) || defined(__amd64__) ||          \
    defined(__amd64) || defined(_M_X64)
#define MOSS_ARCH_X86_64
#elif defined(__aarch64__) || defined(_M_ARM64)
#define MOSS_ARCH_ARM64
#elif defined(__riscv) && __riscv_xlen == 64
#define MOSS_ARCH_RISCV
#else
// 默认fallback到x86_64
#define MOSS_ARCH_X86_64
#endif
#endif
#endif
#endif

namespace moss::kernel::arch {

// 架构特定的panic/breakpoint操作
[[noreturn]] inline void kernel_panic() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("brk #0"); // ARM64 breakpoint
  __builtin_unreachable();
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("int3"); // X86_64 breakpoint
  __builtin_unreachable();
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("ebreak"); // RISC-V breakpoint
  __builtin_unreachable();
#else
#error "Unsupported architecture for kernel_panic"
#endif
}

// 内存屏障操作
inline void memory_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dsb sy" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("mfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence" ::: "memory");
#else
#error "Unsupported architecture for memory_barrier"
#endif
}

// 读取内存屏障
inline void read_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dsb ld" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("lfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence r,r" ::: "memory");
#else
#error "Unsupported architecture for read_barrier"
#endif
}

// 写入内存屏障
inline void write_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dsb st" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("sfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence w,w" ::: "memory");
#else
#error "Unsupported architecture for write_barrier"
#endif
}

// 指令同步屏障
inline void instruction_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("isb" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  // X86_64 has strong ordering, no explicit instruction barrier needed
  asm volatile("" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence.i" ::: "memory");
#else
#error "Unsupported architecture for instruction_barrier"
#endif
}

// CPU yield hint
inline void cpu_yield() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("yield");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("pause");
#elif defined(MOSS_ARCH_RISCV)
  // RISC-V doesn't have a standard yield instruction, use nop
  asm volatile("nop");
#else
#error "Unsupported architecture for cpu_yield"
#endif
}

// CPU halt/wait for interrupt (低功耗等待)
inline void cpu_halt() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("wfi");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("hlt");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("wfi");
#else
#error "Unsupported architecture for cpu_halt"
#endif
}

// 获取当前时间戳计数器
inline u64 get_timestamp_counter() noexcept {
#if defined(MOSS_ARCH_ARM64)
  u64 count;
  asm volatile("mrs %0, cntvct_el0" : "=r"(count));
  return count;
#elif defined(MOSS_ARCH_X86_64)
  u64 low, high;
  asm volatile("rdtsc" : "=a"(low), "=d"(high));
  return (high << 32) | low;
#elif defined(MOSS_ARCH_RISCV)
  u64 cycles;
  asm volatile("rdcycle %0" : "=r"(cycles));
  return cycles;
#else
#error "Unsupported architecture for get_timestamp_counter"
#endif
}

// 获取当前CPU ID
inline u32 get_current_cpu_id() noexcept {
#if defined(MOSS_ARCH_ARM64)
  u64 mpidr;
  asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  return static_cast<u32>(mpidr & 0xFF) % MAX_CPUS;
#elif defined(MOSS_ARCH_X86_64)
  // X86_64 uses LAPIC ID from CPUID
  u32 eax, ebx, ecx, edx;
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
  return (ebx >> 24) & 0xFF; // LAPIC ID is in bits 31:24 of EBX
#elif defined(MOSS_ARCH_RISCV)
  // RISC-V uses hart ID from mhartid CSR
  u64 hartid;
  asm volatile("csrr %0, mhartid" : "=r"(hartid));
  return static_cast<u32>(hartid) % MAX_CPUS;
#else
#error "Unsupported architecture for get_current_cpu_id"
#endif
}

// 刷新缓存行
inline void flush_cache_line(VirtAddr addr) noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dc civac, %0" ::"r"(addr) : "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("clflush (%0)" ::"r"(addr) : "memory");
#elif defined(MOSS_ARCH_RISCV)
  // RISC-V doesn't have standard cache management instructions
  // This is a no-op on many RISC-V implementations
  (void)addr;
  asm volatile("" ::: "memory");
#else
#error "Unsupported architecture for flush_cache_line"
#endif
}

// MMU (内存管理单元) 抽象
namespace mmu {

// 设置页表基址和MMU配置
inline void setup_kernel_mmu(PhysAddr kernel_pgd_pa) noexcept {
#if defined(MOSS_ARCH_ARM64)
  // ARM64 MMU 配置 - 与page_table.hpp的AddressSpaceConfig保持完全一致
  // 使用双TTBR方案（EPD1=0）：TTBR0用于身份映射，TTBR1用于内核虚拟映射
  // 此配置与page_table.cpp中的实现兼容，确保MMU配置一致性
  constexpr u64 MAIR_VALUE =
      (0x00ULL << (0 * 8)) | // 属性索引0：设备内存 (Device-nGnRnE)
      (0xFFULL << (1 * 8)) | // 属性索引1：普通缓存内存 (Normal WB/WA/RA)
      (0x44ULL << (2 * 8));  // 属性索引2：非缓存内存 (Normal NC)

  constexpr u64 TCR_VALUE =
      (16ULL << 0) |  // T0SZ=16 (48位虚拟地址空间，身份映射)
      (16ULL << 16) | // T1SZ=16 (48位内核虚拟地址空间)
      (0ULL << 6) |   // EPD0=0 (启用TTBR0_EL1用于身份映射)
      (0ULL << 23) |  // EPD1=0 (启用TTBR1_EL1，双TTBR方案)
      (0ULL << 14) |  // TG0=00 (4KB页面大小，身份映射)
      (0ULL << 30) |  // TG1=00 (4KB页面大小，内核空间)
      (1ULL << 8) |   // IRGN0=01 (身份映射内部Write-Back/Write-Allocate)
      (1ULL << 10) |  // ORGN0=01 (身份映射外部Write-Back/Write-Allocate)
      (3ULL << 12) |  // SH0=11 (身份映射内部共享)
      (1ULL << 24) |  // IRGN1=01 (内核内部Write-Back/Write-Allocate)
      (1ULL << 26) |  // ORGN1=01 (内核外部Write-Back/Write-Allocate)
      (3ULL << 28) |  // SH1=11 (内核内部共享)
      (5ULL << 32);   // IPS=101 (48位物理地址空间)

  // 完整的ARM64 MMU启用序列

  // 1. 完全禁用MMU和缓存，确保干净状态
  u64 sctlr;
  asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
  sctlr &= ~(1ULL << 0 | 1ULL << 2 | 1ULL << 12); // 清除M,C,I位
  asm volatile("msr sctlr_el1, %0" ::"r"(sctlr));
  asm volatile("isb");

  // 2. 无效化所有TLB条目
  asm volatile("tlbi vmalle1");
  asm volatile("dsb sy");
  asm volatile("isb");

  // 3. 设置内存属性寄存器
  asm volatile("msr mair_el1, %0" ::"r"(MAIR_VALUE));

  // 4. 设置翻译控制寄存器
  asm volatile("msr tcr_el1, %0" ::"r"(TCR_VALUE));

  // 5. 设置页表基址寄存器 (双TTBR方案，与page_table.cpp一致)
  asm volatile("msr ttbr0_el1, %0" ::"r"(kernel_pgd_pa)); // TTBR0用于身份映射
  asm volatile(
      "msr ttbr1_el1, %0" ::"r"(kernel_pgd_pa)); // TTBR1用于内核虚拟映射

  // 6. 内存和指令同步屏障
  asm volatile("dsb sy");
  asm volatile("isb");

  // 7. 启用MMU和缓存
  sctlr |= (1ULL << 0 | 1ULL << 2 | 1ULL << 12); // 设置M,C,I位
  asm volatile("msr sctlr_el1, %0" ::"r"(sctlr));

  // 8. 最终同步确保MMU生效
  asm volatile("dsb sy");
  asm volatile("isb");

#elif defined(MOSS_ARCH_X86_64)
  // X86_64 MMU 配置（简化实现）
  // 在X86_64中，MMU在启动时已经由引导加载程序设置
  // 这里只需要设置页表基址
  (void)kernel_pgd_pa;

  // 设置CR3寄存器（页目录基址）
  asm volatile("mov %0, %%cr3" ::"r"(kernel_pgd_pa) : "memory");

#elif defined(MOSS_ARCH_RISCV)
  // RISC-V MMU 配置
  (void)kernel_pgd_pa;

  // 设置satp寄存器（Sv48模式 + 页表物理地址）
  u64 satp = (8ULL << 60) | (kernel_pgd_pa >> 12); // MODE=Sv48, PPN=pgd_pa>>12
  asm volatile("csrw satp, %0" ::"r"(satp));

  // 刷新TLB
  asm volatile("sfence.vma");

#else
#error "Unsupported architecture for MMU setup"
#endif
}

// 刷新TLB (Translation Lookaside Buffer)
inline void flush_tlb() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("tlbi vmalle1is");
  asm volatile("dsb sy");
  asm volatile("isb");
#elif defined(MOSS_ARCH_X86_64)
  // 刷新所有TLB条目
  asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
#elif defined(MOSS_ARCH_RISCV)
  // 刷新所有TLB条目
  asm volatile("sfence.vma");
#else
#error "Unsupported architecture for TLB flush"
#endif
}

// 刷新特定虚拟地址的TLB条目
inline void flush_tlb_addr(VirtAddr addr) noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("tlbi vae1is, %0" ::"r"(addr >> 12));
  asm volatile("dsb sy");
  asm volatile("isb");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("invlpg (%0)" ::"r"(addr) : "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("sfence.vma %0" ::"r"(addr));
#else
#error "Unsupported architecture for single TLB flush"
#endif
}

} // namespace mmu

// 系统调用抽象
namespace syscall {

// 系统调用上下文结构体 - 保存用户空间寄存器状态
struct SyscallContext {
    // 通用寄存器
    u64 regs[32];        // 通用寄存器 (架构特定数量)
    u64 sp;              // 栈指针
    u64 pc;              // 程序计数器/返回地址
    u64 pstate;          // 处理器状态寄存器

    // 系统调用参数和返回值
    u64 syscall_nr;      // 系统调用号
    u64 args[6];         // 系统调用参数 (arg0-arg5)
    u64 ret_value;       // 返回值

    // 错误信息
    u64 error_code;      // 错误代码
};

// 系统调用入口点函数类型
extern "C" {
    // 系统调用处理函数 - 由kernel_main.cpp中的system_call_handler实现
    long system_call_handler(long syscall_number, long arg0, long arg1,
                            long arg2, long arg3, long arg4, long arg5) noexcept;

    // 架构特定的系统调用入口点 - 由汇编实现
    void syscall_entry_point() noexcept;

    // 系统调用返回处理 - 从内核空间返回用户空间
    void syscall_return(SyscallContext* context) noexcept;
}

// 初始化系统调用支持
inline void initialize_syscall_support() noexcept {
#if defined(MOSS_ARCH_ARM64)
    // ARM64: 设置异常向量表中的SVC处理程序
    // TODO: 实现异常向量表设置
    // 目前暂时使用简化实现
#elif defined(MOSS_ARCH_X86_64)
    // X86_64: 设置SYSCALL指令的MSR寄存器
    // IA32_LSTAR: 系统调用入口点地址
    // IA32_FMASK: RFLAGS掩码
    // IA32_STAR: 段选择器配置
    u64 syscall_entry = reinterpret_cast<u64>(&syscall_entry_point);

    // 设置SYSCALL入口点
    asm volatile("wrmsr" :: "c"(0xC0000082), "a"(syscall_entry), "d"(syscall_entry >> 32));
    // 设置RFLAGS掩码 (禁用中断等)
    asm volatile("wrmsr" :: "c"(0xC0000084), "a"(0x200), "d"(0));
    // 设置段选择器
    asm volatile("wrmsr" :: "c"(0xC0000081), "a"(0x00180008), "d"(0));

#elif defined(MOSS_ARCH_RISCV)
    // RISC-V: 设置trap向量表中的ECALL处理程序
    // TODO: 实现trap向量表设置
    // 目前暂时使用简化实现
#else
#error "Unsupported architecture for syscall initialization"
#endif
}

// 获取当前系统调用上下文 (从架构特定寄存器中提取)
inline void extract_syscall_args(SyscallContext* context,
                                 u64 syscall_nr, u64 arg0, u64 arg1, u64 arg2,
                                 u64 arg3, u64 arg4, u64 arg5) noexcept {
    if (!context) return;

    context->syscall_nr = syscall_nr;
    context->args[0] = arg0;
    context->args[1] = arg1;
    context->args[2] = arg2;
    context->args[3] = arg3;
    context->args[4] = arg4;
    context->args[5] = arg5;
    context->ret_value = 0;
    context->error_code = 0;
}

// 设置系统调用返回值
inline void set_syscall_return(SyscallContext* context, long ret_value) noexcept {
    if (!context) return;

    if (ret_value < 0) {
        // 负数表示错误
        context->error_code = static_cast<u64>(-ret_value);
        context->ret_value = static_cast<u64>(-1); // 错误时返回-1
    } else {
        // 正数或0表示成功
        context->error_code = 0;
        context->ret_value = static_cast<u64>(ret_value);
    }
}

// 架构特定的系统调用参数提取
inline void get_syscall_args_from_registers(u64* syscall_nr, u64* arg0, u64* arg1,
                                           u64* arg2, u64* arg3, u64* arg4, u64* arg5) noexcept {
#if defined(MOSS_ARCH_ARM64)
    // ARM64系统调用约定：
    // x8 = 系统调用号
    // x0-x5 = 参数
    asm volatile(
        "str x8, %0\n"   // 系统调用号
        "str x0, %1\n"   // arg0
        "str x1, %2\n"   // arg1
        "str x2, %3\n"   // arg2
        "str x3, %4\n"   // arg3
        "str x4, %5\n"   // arg4
        "str x5, %6\n"   // arg5
        : "=m"(*syscall_nr), "=m"(*arg0), "=m"(*arg1),
          "=m"(*arg2), "=m"(*arg3), "=m"(*arg4), "=m"(*arg5)
        :
        : "memory"
    );
#elif defined(MOSS_ARCH_X86_64)
    // X86_64系统调用约定：
    // rax = 系统调用号
    // rdi, rsi, rdx, r10, r8, r9 = 参数
    asm volatile(
        "movq %%rax, %0\n"   // 系统调用号
        "movq %%rdi, %1\n"   // arg0
        "movq %%rsi, %2\n"   // arg1
        "movq %%rdx, %3\n"   // arg2
        "movq %%r10, %4\n"   // arg3
        "movq %%r8, %5\n"    // arg4
        "movq %%r9, %6\n"    // arg5
        : "=m"(*syscall_nr), "=m"(*arg0), "=m"(*arg1),
          "=m"(*arg2), "=m"(*arg3), "=m"(*arg4), "=m"(*arg5)
        :
        : "memory"
    );
#elif defined(MOSS_ARCH_RISCV)
    // RISC-V系统调用约定：
    // a7 = 系统调用号
    // a0-a5 = 参数
    asm volatile(
        "sd a7, %0\n"   // 系统调用号
        "sd a0, %1\n"   // arg0
        "sd a1, %2\n"   // arg1
        "sd a2, %3\n"   // arg2
        "sd a3, %4\n"   // arg3
        "sd a4, %5\n"   // arg4
        "sd a5, %6\n"   // arg5
        : "=m"(*syscall_nr), "=m"(*arg0), "=m"(*arg1),
          "=m"(*arg2), "=m"(*arg3), "=m"(*arg4), "=m"(*arg5)
        :
        : "memory"
    );
#else
#error "Unsupported architecture for syscall register extraction"
#endif
}

// 设置系统调用返回值到寄存器
inline void set_syscall_return_to_registers(u64 ret_value) noexcept {
#if defined(MOSS_ARCH_ARM64)
    // ARM64: 返回值在x0寄存器中
    asm volatile("mov x0, %0" :: "r"(ret_value) : "x0");
#elif defined(MOSS_ARCH_X86_64)
    // X86_64: 返回值在rax寄存器中
    asm volatile("movq %0, %%rax" :: "r"(ret_value) : "rax");
#elif defined(MOSS_ARCH_RISCV)
    // RISC-V: 返回值在a0寄存器中
    asm volatile("mv a0, %0" :: "r"(ret_value) : "a0");
#else
#error "Unsupported architecture for syscall return value setting"
#endif
}

} // namespace syscall

} // namespace moss::kernel::arch
