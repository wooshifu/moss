#pragma once

// 架构特定的汇编指令抽象层
// 为不同架构提供统一的接口

#include "../types.hpp"

namespace moss::kernel::arch {

// 架构特定的panic/breakpoint操作
inline void kernel_panic() noexcept {
#if defined(MOSS_ARCH_ARM64)
    asm volatile("brk #0");  // ARM64 breakpoint
    __builtin_unreachable();
#elif defined(MOSS_ARCH_X86_64)
    asm volatile("int3");    // X86_64 breakpoint
    __builtin_unreachable();
#elif defined(MOSS_ARCH_RISCV)
    asm volatile("ebreak");  // RISC-V breakpoint
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
    return (ebx >> 24) & 0xFF;  // LAPIC ID is in bits 31:24 of EBX
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
    asm volatile("dc civac, %0" :: "r"(addr) : "memory");
#elif defined(MOSS_ARCH_X86_64)
    asm volatile("clflush (%0)" :: "r"(addr) : "memory");
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
    // ARM64 MMU 配置
    constexpr u64 MAIR_VALUE =
        (0x00ULL << (0 * 8)) |   // Device-nGnRnE memory
        (0x04ULL << (1 * 8)) |   // Normal memory, Outer Non-cacheable
        (0xFFULL << (2 * 8)) |   // Normal memory, Inner/Outer WB/WA/RA
        (0x44ULL << (3 * 8));    // Normal memory, Inner/Outer NC

    constexpr u64 TCR_VALUE =
        (16ULL << 0) |      // T0SZ = 16 (48-bit VA space)
        (0x0ULL << 6) |     // EPD0 = 0 (enable TTBR0)
        (0x1ULL << 7) |     // EPD1 = 1 (disable TTBR1)
        (0x2ULL << 8) |     // IRGN0 = 0b10 (Inner WB RA WA)
        (0x2ULL << 10) |    // ORGN0 = 0b10 (Outer WB RA WA)
        (0x3ULL << 12) |    // SH0 = 0b11 (Inner Shareable)
        (0x0ULL << 14) |    // TG0 = 0b00 (4KB granule)
        (16ULL << 16) |     // T1SZ = 16
        (0x2ULL << 24) |    // IRGN1 = 0b10
        (0x2ULL << 26) |    // ORGN1 = 0b10
        (0x3ULL << 28) |    // SH1 = 0b11
        (0x0ULL << 30) |    // TG1 = 0b00
        (0x0ULL << 32);     // IPS = 0b000 (32-bit IPA)

    // 设置内存属性寄存器
    asm volatile("msr mair_el1, %0" :: "r"(MAIR_VALUE));

    // 设置翻译控制寄存器
    asm volatile("msr tcr_el1, %0" :: "r"(TCR_VALUE));

    // 设置页表基址寄存器（TTBR1_EL1用于内核空间）
    asm volatile("msr ttbr1_el1, %0" :: "r"(kernel_pgd_pa));

    // 设置TTBR0_EL1为0（暂时不使用用户空间）
    asm volatile("msr ttbr0_el1, %0" :: "r"(0ULL));

    // 内存屏障
    asm volatile("dsb sy");
    asm volatile("isb");

    // 启用MMU
    u64 sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0);  // M位：启用MMU
    sctlr |= (1ULL << 2);  // C位：启用数据缓存
    sctlr |= (1ULL << 12); // I位：启用指令缓存
    asm volatile("msr sctlr_el1, %0" :: "r"(sctlr));

    // 确保MMU启用生效
    asm volatile("dsb sy");
    asm volatile("isb");

#elif defined(MOSS_ARCH_X86_64)
    // X86_64 MMU 配置（简化实现）
    // 在X86_64中，MMU在启动时已经由引导加载程序设置
    // 这里只需要设置页表基址
    (void)kernel_pgd_pa;

    // 设置CR3寄存器（页目录基址）
    asm volatile("mov %0, %%cr3" :: "r"(kernel_pgd_pa) : "memory");

#elif defined(MOSS_ARCH_RISCV)
    // RISC-V MMU 配置
    (void)kernel_pgd_pa;

    // 设置satp寄存器（Sv48模式 + 页表物理地址）
    u64 satp = (8ULL << 60) | (kernel_pgd_pa >> 12);  // MODE=Sv48, PPN=pgd_pa>>12
    asm volatile("csrw satp, %0" :: "r"(satp));

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
    asm volatile("tlbi vae1is, %0" :: "r"(addr >> 12));
    asm volatile("dsb sy");
    asm volatile("isb");
#elif defined(MOSS_ARCH_X86_64)
    asm volatile("invlpg (%0)" :: "r"(addr) : "memory");
#elif defined(MOSS_ARCH_RISCV)
    asm volatile("sfence.vma %0" :: "r"(addr));
#else
    #error "Unsupported architecture for single TLB flush"
#endif
}

} // namespace mmu

} // namespace moss::kernel::arch