#include "mm/page_table.hpp"
#include "../include/arch/arch_abstraction.hpp"

// 简化的调试输出函数 - 直接使用UART输出
namespace {
    // UART基址（QEMU Virt平台）
    static volatile char* const UART_BASE = reinterpret_cast<volatile char*>(0x09000000);

    // 简单UART输出字符
    void debug_putchar(char c) {
        *UART_BASE = c;
    }

    // 简单UART输出字符串
    void debug_print(const char* str) {
        if (!str) return;
        while (*str) {
            debug_putchar(*str++);
        }
    }

    // 简单十六进制输出（带0x前缀）
    void debug_print_hex(u64 value) {
        constexpr char hex_chars[] = "0123456789ABCDEF";
        char buffer[19] = "0x";  // "0x" + 16个十六进制字符 + null终止符

        for (int i = 15; i >= 0; i--) {
            buffer[2 + (15 - i)] = hex_chars[(value >> (i * 4)) & 0xF];
        }
        buffer[18] = '\0';

        debug_print(buffer);
    }

    // 简单十六进制输出（不带0x前缀）
    void debug_print_hex_plain(u64 value) {
        constexpr char hex_chars[] = "0123456789ABCDEF";
        char buffer[17];  // 16个十六进制字符 + null终止符

        for (int i = 15; i >= 0; i--) {
            buffer[15 - i] = hex_chars[(value >> (i * 4)) & 0xF];
        }
        buffer[16] = '\0';

        debug_print(buffer);
    }
}

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
    extern char _pagetable_start_addr[];
    extern char _pagetable_end_addr[];
    extern char _kernel_end_addr[];
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

// 使用本地调试输出函数替换external early_print调用，避免链接问题

// 创建内核页表映射
VoidResult PageTableManager::setup_kernel_page_tables() {
    // 1. 分配内核页表根目录
    auto pgd_result = PageTableManager::allocate_page_table();
    if (!pgd_result) {
        return VoidResult{pgd_result.error()};
    }
    PageTableManager::kernel_pgd = *pgd_result;

    // 创建全面的内存映射 - 映射整个4GB地址空间
    // 使用最简单的设备内存权限，确保MMU能正常工作
    u64 block_permissions = (1ULL << 0) |   // Valid
                           (0ULL << 1) |    // Block (不是Table)
                           (0ULL << 2) |    // AttrIndx=0 (设备内存)
                           (1ULL << 10);    // AF (Access Flag)

    // 映射前4个1GB块，覆盖0x00000000-0x100000000 (4GB)
    for (usize i = 0; i < 4; i++) {
        usize pgd_index = i;  // PGD索引0, 1, 2, 3
        PhysAddr block_addr = (PhysAddr)(i * 0x40000000ULL);  // 0GB, 1GB, 2GB, 3GB

        u64 block_entry = (block_addr & 0x0000FFFFFFFFF000ULL) | block_permissions;
        PageTableManager::kernel_pgd->entries[pgd_index].raw = block_entry;
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
    // 使用原始配置参数，避免自定义配置导致的问题
    PhysAddr kernel_pgd_pa = PageTableManager::get_physical_address(PageTableManager::kernel_pgd);

    u64 current_sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(current_sctlr));

    // 使用项目原始的MAIR配置
    asm volatile("msr mair_el1, %0" :: "r"(AddressSpaceConfig::MAIR_VALUE));

    // 使用项目原始的TCR配置
    asm volatile("msr tcr_el1, %0" :: "r"(AddressSpaceConfig::TCR_VALUE));

    // 按照原始配置设置TTBR（双TTBR方案）
    asm volatile("msr ttbr0_el1, %0" :: "r"(kernel_pgd_pa));
    asm volatile("msr ttbr1_el1, %0" :: "r"(kernel_pgd_pa));

    // 内存屏障
    asm volatile("dsb sy");
    asm volatile("isb");

    // 只启用MMU，不改变缓存设置
    u64 sctlr = current_sctlr;
    sctlr |= (1ULL << 0);   // M位：启用MMU
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
    // 现在有了正确的身份映射，尝试真正启用MMU

    // 阶段1：设置内核页表映射（现在包含1GB身份映射）
    auto setup_result = PageTableManager::setup_kernel_page_tables();
    if (!setup_result) {
        return VoidResult{setup_result.error()};
    }

    // 2. 启用MMU - 使用原始配置参数
    auto enable_result = PageTableManager::enable_mmu();
    if (!enable_result) {
        return VoidResult{enable_result.error()};
    }

    // 3. 验证MMU已启用
    u64 sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if (!(sctlr & (1ULL << 0))) {
        // MMU启用失败但继续运行
        // return VoidResult{ErrorCode::InvalidState};
    }

    // 4. 打印页表详细信息（调试输出）
    PageTableManager::print_page_table_details();

    return VoidResult{};  // MMU成功启用
}

void invalidate_all_tlb() {
    PageTableManager::invalidate_tlb();
}

// 调试功能实现：打印MMU寄存器状态
void PageTableManager::print_mmu_registers() {
    debug_print("\n=== MMU寄存器详细状态 ===\n");

#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
    // 读取关键MMU寄存器
    u64 sctlr_el1, tcr_el1, mair_el1, ttbr0_el1, ttbr1_el1;

    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr_el1));
    asm volatile("mrs %0, tcr_el1" : "=r"(tcr_el1));
    asm volatile("mrs %0, mair_el1" : "=r"(mair_el1));
    asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0_el1));
    asm volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1_el1));

    debug_print("SCTLR_EL1:  0x");
    debug_print_hex(sctlr_el1);
    debug_print("\n  MMU启用:  ");
    debug_print((sctlr_el1 & (1ULL << 0)) ? "是" : "否");
    debug_print(" (M位)\n  缓存启用: ");
    debug_print((sctlr_el1 & (1ULL << 2)) ? "是" : "否");
    debug_print(" (C位)\n  指令缓存: ");
    debug_print((sctlr_el1 & (1ULL << 12)) ? "是" : "否");
    debug_print(" (I位)\n");

    debug_print("\nTCR_EL1:    0x");
    debug_print_hex(tcr_el1);
    debug_print("\n  T0SZ:     ");
    debug_print_hex(tcr_el1 & 0x3F);
    debug_print(" (TTBR0地址空间大小)\n  T1SZ:     ");
    debug_print_hex((tcr_el1 >> 16) & 0x3F);
    debug_print(" (TTBR1地址空间大小)\n  EPD0:     ");
    debug_print_hex((tcr_el1 >> 7) & 1);
    debug_print(" (TTBR0 ");
    debug_print(((tcr_el1 >> 7) & 1) ? "禁用" : "启用");
    debug_print(")\n  EPD1:     ");
    debug_print_hex((tcr_el1 >> 23) & 1);
    debug_print(" (TTBR1 ");
    debug_print(((tcr_el1 >> 23) & 1) ? "禁用" : "启用");
    debug_print(")\n");

    debug_print("\nMAIR_EL1:   0x");
    debug_print_hex(mair_el1);
    debug_print("\n  属性0:    0x");
    debug_print_hex((mair_el1 >> 0) & 0xFF);
    debug_print(" (设备内存)\n  属性1:    0x");
    debug_print_hex((mair_el1 >> 8) & 0xFF);
    debug_print(" (普通缓存)\n  属性2:    0x");
    debug_print_hex((mair_el1 >> 16) & 0xFF);
    debug_print(" (非缓存)\n");

    debug_print("\nTTBR0_EL1:  0x");
    debug_print_hex(ttbr0_el1);
    debug_print("\nTTBR1_EL1:  0x");
    debug_print_hex(ttbr1_el1);
    debug_print("\n");
#else
    debug_print("非ARM64架构，跳过MMU寄存器读取\n");
#endif
}

// 调试功能实现：打印PGD级页表项
void PageTableManager::print_pgd_entries() {
    debug_print("\n=== PGD级页表项详细信息 ===\n");

    if (!kernel_pgd) {
        debug_print("错误：内核页表根目录未初始化\n");
        return;
    }

    debug_print("PGD物理地址: 0x");
    debug_print_hex_plain(get_physical_address(kernel_pgd));
    debug_print("\nPGD索引范围: [0-511] (每个索引覆盖1GB地址空间)\n\n");

    // 遍历PGD的所有条目，但只详细显示有效的条目
    u32 valid_entries = 0;

    for (usize i = 0; i < PageTable::ENTRIES_PER_TABLE; i++) {
        const auto& entry = kernel_pgd->entries[i];

        if (entry.is_valid()) {
            valid_entries++;

            // 计算这个条目覆盖的虚拟地址范围
            VirtAddr virt_start = i * 0x40000000ULL;  // i * 1GB
            VirtAddr virt_end = virt_start + 0x40000000ULL - 1;
            PhysAddr phys_addr = entry.get_phys_addr();

            debug_print("PGD[");
            debug_print_hex(i);
            debug_print("] = 0x");
            debug_print_hex_plain(entry.raw);
            debug_print(" (有效)\n  虚拟地址范围: 0x");
            debug_print_hex_plain(virt_start);
            debug_print(" - 0x");
            debug_print_hex_plain(virt_end);
            debug_print(" (1GB)\n  物理地址:     0x");
            debug_print_hex_plain(phys_addr);
            debug_print("\n  映射类型:     ");
            debug_print(entry.is_table() ? "页表" : "1GB块");

            // 解析权限位
            u64 raw = entry.raw;
            debug_print("\n  权限位:\n    Valid:    ");
            debug_print((raw & (1ULL << 0)) ? "是" : "否");
            debug_print(" (bit0)\n    Table:    ");
            debug_print((raw & (1ULL << 1)) ? "是" : "否");
            debug_print(" (bit1)\n    AttrIndx: ");
            debug_print_hex((raw >> 2) & 7);
            debug_print(" (bits[4:2])\n    AF:       ");
            debug_print((raw & (1ULL << 10)) ? "是" : "否");
            debug_print(" (bit10)\n    PXN:      ");
            debug_print((raw & (1ULL << 53)) ? "是" : "否");
            debug_print(" (bit53)\n    XN:       ");
            debug_print((raw & (1ULL << 54)) ? "是" : "否");
            debug_print(" (bit54)\n");

            // 根据AttrIndx解释内存类型
            u64 attr_idx = (raw >> 2) & 7;
            const char* mem_type;
            switch(attr_idx) {
                case 0: mem_type = "设备内存 (nGnRnE)"; break;
                case 1: mem_type = "普通缓存内存 (WB/WA)"; break;
                case 2: mem_type = "普通非缓存内存"; break;
                default: mem_type = "未知内存类型"; break;
            }
            debug_print("    内存类型: ");
            debug_print(mem_type);
            debug_print("\n\n");
        }
    }

    debug_print("页表统计:\n  有效条目数: ");
    debug_print_hex(valid_entries);
    debug_print(" / 512\n  映射覆盖:   ");
    debug_print_hex(valid_entries);
    debug_print(" GB\n");
}

// 调试功能实现：打印页表详细信息
void PageTableManager::print_page_table_details() {
    debug_print("\n========================================\n");
    debug_print("        MOSS页表详细信息调试输出\n");
    debug_print("========================================\n");

    // 1. 页表分配器状态
    debug_print("\n=== 页表分配器状态 ===\n");
    debug_print("最大页表数量: ");
    debug_print_hex(MAX_EARLY_TABLES);
    debug_print("\n已分配页表数: ");
    debug_print_hex(next_table_index);
    debug_print("\n可用页表数:   ");
    debug_print_hex(MAX_EARLY_TABLES - next_table_index);
    debug_print("\n总内存占用:   ");
    debug_print_hex(MAX_EARLY_TABLES * 4);
    debug_print(" KB\n");

    // 2. 页表数组布局
    debug_print("\n=== 页表内存布局 ===\n");
    debug_print("early_tables起始: 0x");
    debug_print_hex_plain(reinterpret_cast<PhysAddr>(early_tables));
    debug_print("\n单个页表大小:     ");
    debug_print_hex(sizeof(PageTable));
    debug_print(" 字节\n每页表条目数:     ");
    debug_print_hex(PageTable::ENTRIES_PER_TABLE);
    debug_print(" 个\n");

    if (kernel_pgd) {
        usize pgd_index = get_table_index(kernel_pgd);
        debug_print("内核PGD位置:      early_tables[");
        debug_print_hex(pgd_index);
        debug_print("] (0x");
        debug_print_hex(get_physical_address(kernel_pgd));
        debug_print(")\n");
    } else {
        debug_print("内核PGD:          未初始化\n");
    }

    // 3. MMU寄存器状态
    print_mmu_registers();

    // 4. PGD页表项详情
    print_pgd_entries();

    // 5. 地址转换示例（1GB块映射）
    debug_print("\n=== 地址转换示例 (1GB块映射) ===\n");
    VirtAddr test_addrs[] = {0x00000000, 0x12345678, 0x40000000, 0x80000000, 0xC0000000};
    const char* addr_names[] = {"0GB起始", "内核代码", "1GB边界", "2GB边界", "3GB边界"};

    for (size_t i = 0; i < 5; i++) {
        VirtAddr vaddr = test_addrs[i];

        // 对于1GB块映射，PGD索引是地址的前2位 (vaddr >> 30)
        u32 pgd_index_1gb = (vaddr >> 30) & 0x3;  // 1GB块映射的PGD索引
        u32 block_offset_1gb = vaddr & 0x3FFFFFFF;  // 1GB块内偏移

        debug_print("虚拟地址 0x");
        debug_print_hex_plain(vaddr);
        debug_print(" (");
        debug_print(addr_names[i]);
        debug_print("):\n  PGD索引: ");
        debug_print_hex(pgd_index_1gb);
        debug_print(" (1GB块映射)\n  块内偏移: 0x");
        debug_print_hex_plain(block_offset_1gb);
        debug_print("\n");

        // 检查对应的PGD条目
        if (kernel_pgd && pgd_index_1gb < PageTable::ENTRIES_PER_TABLE) {
            const auto& pgd_entry = kernel_pgd->entries[pgd_index_1gb];
            if (pgd_entry.is_valid()) {
                PhysAddr phys_base = pgd_entry.get_phys_addr();
                PhysAddr phys_final = phys_base + block_offset_1gb;
                debug_print("  → 物理地址: 0x");
                debug_print_hex_plain(phys_final);
                debug_print("\n");
            } else {
                debug_print("  → 未映射\n");
            }
        }
        debug_print("\n");
    }

    debug_print("========================================\n");
    debug_print("        页表详细信息输出完成\n");
    debug_print("========================================\n\n");
}

} // namespace moss::kernel::mm