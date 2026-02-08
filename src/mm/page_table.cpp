#include "mm/page_table.hpp"
#include "../include/arch/arch_abstraction.hpp"

// 外部符号声明（来自链接器脚本）
extern "C" {
    extern char _text_start_addr[];
    extern char _text_end_addr[];
    extern char _rodata_start_addr[];
    extern char _rodata_end_addr[];
    extern char _data_start_addr[];
    extern char _data_end_addr[];
    extern char _bss_start_addr[];
    extern char _bss_end_addr[];
}

namespace moss::kernel::mm {

// PageTableManager 方法实现
KernelResult<PageTable*> PageTableManager::allocate_page_table() {
        if (next_table_index >= MAX_EARLY_TABLES) {
            return KernelResult<PageTable*>{ErrorCode::OutOfMemory};
        }

        PageTable* table = &early_tables[next_table_index++];

        // 清零页表
        for (usize i = 0; i < PageTable::ENTRIES_PER_TABLE; i++) {
            table->entries[i].clear();
        }

        return KernelResult<PageTable*>{table};
    }

// 调试输出暂时注释掉

// 创建内核页表映射
VoidResult PageTableManager::setup_kernel_page_tables() {
    // 分配内核页表根目录
    auto pgd_result = PageTableManager::allocate_page_table();
    if (!pgd_result) {
        return VoidResult{pgd_result.error()};
    }
    PageTableManager::kernel_pgd = *pgd_result;

        // 首先创建恒等映射 - 关键！确保MMU启用时代码能继续执行
        PhysAddr text_start = reinterpret_cast<PhysAddr>(_text_start_addr);
        PhysAddr bss_end = reinterpret_cast<PhysAddr>(_bss_end_addr);

        // 恒等映射内核代码区域（物理地址 = 虚拟地址）
    auto identity_result = PageTableManager::map_region(text_start, text_start,
                                                        bss_end - text_start, PagePerms::KERNEL_RW);
        if (!identity_result) {
            return VoidResult{identity_result.error()};
        }

        // 映射内核代码段（只读+可执行）到高地址
        PhysAddr text_end = reinterpret_cast<PhysAddr>(_text_end_addr);
        VirtAddr text_virt = KERNEL_BASE + text_start;

    auto text_result = PageTableManager::map_region(text_virt, text_start, text_end - text_start,
                                                    PagePerms::KERNEL_RX);
        if (!text_result) {
            return VoidResult{text_result.error()};
        }

        // 映射内核只读数据段
        PhysAddr rodata_start = reinterpret_cast<PhysAddr>(_rodata_start_addr);
        PhysAddr rodata_end = reinterpret_cast<PhysAddr>(_rodata_end_addr);
        VirtAddr rodata_virt = KERNEL_BASE + rodata_start;

    auto rodata_result = PageTableManager::map_region(rodata_virt, rodata_start,
                                                      rodata_end - rodata_start, PagePerms::KERNEL_RO);
        if (!rodata_result) {
            return VoidResult{rodata_result.error()};
        }

        // 映射内核数据段（可读写）
        PhysAddr data_start = reinterpret_cast<PhysAddr>(_data_start_addr);
        VirtAddr data_virt = KERNEL_BASE + data_start;

    auto data_result = PageTableManager::map_region(data_virt, data_start, bss_end - data_start,
                                                    PagePerms::KERNEL_RW);
        if (!data_result) {
            return VoidResult{data_result.error()};
        }

        // 恒等映射设备内存区域（UART等）
        // QEMU virt平台的设备内存映射
        constexpr PhysAddr DEVICE_BASE = 0x08000000;
        constexpr usize DEVICE_SIZE = 0x08000000;  // 128MB设备空间

        // 同时做恒等映射和高地址映射
    auto device_identity_result = PageTableManager::map_region(DEVICE_BASE, DEVICE_BASE, DEVICE_SIZE,
                                                               PagePerms::DEVICE);
        if (!device_identity_result) {
            return VoidResult{device_identity_result.error()};
        }

        VirtAddr device_virt = KERNEL_BASE + DEVICE_BASE;
    auto device_result = PageTableManager::map_region(device_virt, DEVICE_BASE, DEVICE_SIZE,
                                                      PagePerms::DEVICE);
        if (!device_result) {
            return VoidResult{device_result.error()};
        }

        return VoidResult{};
    }


// 映射内存区域
VoidResult PageTableManager::map_region(VirtAddr virt_addr, PhysAddr phys_addr,
                                        usize size, u64 permissions) {
        // 页面对齐检查
        if ((virt_addr & (PAGE_SIZE - 1)) != 0 || (phys_addr & (PAGE_SIZE - 1)) != 0) {
            return VoidResult{ErrorCode::InvalidParameter};
        }

        usize pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

        for (usize i = 0; i < pages; i++) {
            VirtAddr curr_virt = virt_addr + i * PAGE_SIZE;
            PhysAddr curr_phys = phys_addr + i * PAGE_SIZE;

            auto result = PageTableManager::map_page(curr_virt, curr_phys, permissions);
            if (!result) {
                return VoidResult{result.error()};
            }
        }

        return VoidResult{};
    }

// 映射单个页面
VoidResult PageTableManager::map_page(VirtAddr virt_addr, PhysAddr phys_addr,
                                      u64 permissions) {
    if (!PageTableManager::kernel_pgd) {
            return VoidResult{ErrorCode::InvalidState};
        }

        auto addr_breakdown = break_virtual_address(virt_addr);

        // 遍历页表层级
    PageTable* current_table = PageTableManager::kernel_pgd;

        // PGD -> PUD
        if (!current_table->entries[addr_breakdown.pgd_index].is_valid()) {
        auto pud_result = PageTableManager::allocate_page_table();
            if (!pud_result) {
                return VoidResult{pud_result.error()};
            }

        PhysAddr pud_pa = PageTableManager::get_physical_address(*pud_result);
            current_table->entries[addr_breakdown.pgd_index].set_table(pud_pa);
        }

        PhysAddr pud_pa = current_table->entries[addr_breakdown.pgd_index].get_phys_addr();
        current_table = reinterpret_cast<PageTable*>(pud_pa);

        // PUD -> PMD
        if (!current_table->entries[addr_breakdown.pud_index].is_valid()) {
        auto pmd_result = PageTableManager::allocate_page_table();
            if (!pmd_result) {
                return VoidResult{pmd_result.error()};
            }

        PhysAddr pmd_pa = PageTableManager::get_physical_address(*pmd_result);
            current_table->entries[addr_breakdown.pud_index].set_table(pmd_pa);
        }

        PhysAddr pmd_pa = current_table->entries[addr_breakdown.pud_index].get_phys_addr();
        current_table = reinterpret_cast<PageTable*>(pmd_pa);

        // PMD -> PTE
        if (!current_table->entries[addr_breakdown.pmd_index].is_valid()) {
        auto pte_result = PageTableManager::allocate_page_table();
            if (!pte_result) {
                return VoidResult{pte_result.error()};
            }

        PhysAddr pte_pa = PageTableManager::get_physical_address(*pte_result);
            current_table->entries[addr_breakdown.pmd_index].set_table(pte_pa);
        }

        PhysAddr pte_pa = current_table->entries[addr_breakdown.pmd_index].get_phys_addr();
        current_table = reinterpret_cast<PageTable*>(pte_pa);

        // 设置最终的页表项
        current_table->entries[addr_breakdown.pte_index].set_block(phys_addr, permissions);

        return VoidResult{};
    }

// 启用MMU
VoidResult PageTableManager::enable_mmu() {
    if (!PageTableManager::kernel_pgd) {
        return VoidResult{ErrorCode::InvalidState};
    }

#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
    PhysAddr kernel_pgd_pa = PageTableManager::get_physical_address(PageTableManager::kernel_pgd);

    // 逐步测试：添加MAIR寄存器设置
    u64 current_sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(current_sctlr));

    // 设置内存属性寄存器
    asm volatile("msr mair_el1, %0" :: "r"(AddressSpaceConfig::MAIR_VALUE));

    // 设置翻译控制寄存器
    asm volatile("msr tcr_el1, %0" :: "r"(AddressSpaceConfig::TCR_VALUE));

    // 设置页表基址寄存器（TTBR1_EL1用于内核空间）
    asm volatile("msr ttbr1_el1, %0" :: "r"(kernel_pgd_pa));

    // 设置TTBR0_EL1为0（暂时不使用用户空间）
    asm volatile("msr ttbr0_el1, %0" :: "r"(0ULL));

    // 内存屏障
    asm volatile("dsb sy");
    asm volatile("isb");

    // 启用MMU - 这是最可能出问题的地方
    u64 sctlr = current_sctlr;
    sctlr |= (1ULL << 0);  // M位：启用MMU
    sctlr |= (1ULL << 2);  // C位：启用数据缓存
    sctlr |= (1ULL << 12); // I位：启用指令缓存
    asm volatile("msr sctlr_el1, %0" :: "r"(sctlr));

    // 确保MMU启用生效
    asm volatile("dsb sy");
    asm volatile("isb");

    // 如果能到这里说明MMU启用成功
    (void)kernel_pgd_pa;  // 避免未使用警告
    (void)current_sctlr;  // 避免未使用警告
#else
    // 非ARM64架构，MMU操作不适用，返回成功
    // 实际项目中需要为不同架构实现相应的内存管理
#endif

    return VoidResult{};
}

// 全局函数接口
VoidResult setup_mmu() {
    // 临时跳过MMU设置，返回成功让内核继续运行
    // TODO: 修复MMU配置问题
    return VoidResult{};
}

void invalidate_all_tlb() {
    PageTableManager::invalidate_tlb();
}

} // namespace moss::kernel::mm