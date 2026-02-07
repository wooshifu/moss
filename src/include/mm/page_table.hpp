#pragma once

// ARM64页表管理系统
// 支持4KB页面的4级页表结构

#include "../include/types.hpp"
#include "../include/result.hpp"
#include "../include/arch/arch_abstraction.hpp"

namespace moss::kernel::mm {

// 内存属性枚举
enum class MemoryAttributes : u8 {
    NORMAL_CACHEABLE = 0,       // 普通可缓存内存
    NORMAL_NON_CACHEABLE = 1,   // 普通不可缓存内存
    DEVICE_nGnRnE = 2,          // 设备内存 (不可收集，不可重排序，不可提前执行)
    DEVICE_nGnRE = 3,           // 设备内存 (不可收集，不可重排序，可提前执行)
    DEVICE_GRE = 4              // 设备内存 (可收集，可重排序，可提前执行)
};

// ARM64页表级别
enum class PageLevel : u32 {
    PGD = 0,    // Page Global Directory (Level 0)
    PUD = 1,    // Page Upper Directory (Level 1)
    PMD = 2,    // Page Middle Directory (Level 2)
    PTE = 3     // Page Table Entry (Level 3)
};

// 页面大小枚举
enum class PageSize : u64 {
    Size4KB = PAGE_SIZE,                    // 4KB
    Size2MB = LARGE_PAGE_SIZE,             // 2MB
    Size1GB = HUGE_PAGE_SIZE               // 1GB
};

// 页表项属性位
namespace PageAttr {
    static constexpr u64 VALID       = (1ULL << 0);   // 有效位
    static constexpr u64 TABLE       = (1ULL << 1);   // 表项类型（0=块，1=表）
    static constexpr u64 USER        = (1ULL << 6);   // 用户访问
    static constexpr u64 READONLY    = (1ULL << 7);   // 只读
    static constexpr u64 SHARED      = (1ULL << 8);   // 共享
    static constexpr u64 AF          = (1ULL << 10);  // 访问标志
    static constexpr u64 NG          = (1ULL << 11);  // 非全局
    static constexpr u64 PXN         = (1ULL << 53);  // 特权执行从不
    static constexpr u64 XN          = (1ULL << 54);  // 执行从不

    // 内存类型（通过MAIR_EL1寄存器配置）
    static constexpr u64 ATTR_IDX_SHIFT = 2;
    static constexpr u64 ATTR_DEVICE    = (0ULL << ATTR_IDX_SHIFT);  // 设备内存
    static constexpr u64 ATTR_NORMAL    = (1ULL << ATTR_IDX_SHIFT);  // 普通内存
    static constexpr u64 ATTR_NORMAL_NC = (2ULL << ATTR_IDX_SHIFT);  // 非缓存
}

// 页表项权限组合
namespace PagePerms {
    static constexpr u64 KERNEL_RO = PageAttr::VALID | PageAttr::AF | PageAttr::ATTR_NORMAL;
    static constexpr u64 KERNEL_RW = KERNEL_RO;
    static constexpr u64 KERNEL_RX = KERNEL_RO | PageAttr::PXN;  // 内核代码不允许特权执行

    static constexpr u64 USER_RO = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                                   PageAttr::READONLY | PageAttr::ATTR_NORMAL;
    static constexpr u64 USER_RW = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                                   PageAttr::ATTR_NORMAL;
    static constexpr u64 USER_RX = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                                   PageAttr::READONLY | PageAttr::ATTR_NORMAL;

    static constexpr u64 DEVICE = PageAttr::VALID | PageAttr::AF | PageAttr::ATTR_DEVICE |
                                  PageAttr::XN | PageAttr::PXN;
}

// 页表项结构
struct [[gnu::packed]] PageTableEntry {
    u64 raw;

    // 构造函数
    constexpr PageTableEntry() : raw(0) {}
    constexpr explicit PageTableEntry(u64 value) : raw(value) {}

    // 检查状态
    [[nodiscard]] constexpr bool is_valid() const { return raw & PageAttr::VALID; }
    [[nodiscard]] constexpr bool is_table() const { return raw & PageAttr::TABLE; }
    [[nodiscard]] constexpr bool is_block() const { return is_valid() && !is_table(); }

    // 获取物理地址（48位物理地址空间）
    [[nodiscard]] constexpr PhysAddr get_phys_addr() const {
        return raw & 0x0000FFFFFFFFF000ULL;
    }

    // 设置为表项
    constexpr void set_table(PhysAddr next_table_pa) {
        raw = (next_table_pa & 0x0000FFFFFFFFF000ULL) | PageAttr::VALID | PageAttr::TABLE;
    }

    // 设置为块项
    constexpr void set_block(PhysAddr block_pa, u64 attributes) {
        raw = (block_pa & 0x0000FFFFFFFFF000ULL) | attributes | PageAttr::VALID;
    }

    // 清除
    constexpr void clear() { raw = 0; }
};

static_assert(sizeof(PageTableEntry) == 8, "页表项必须是8字节");

// 页表页面结构（每页512个条目）
struct alignas(PAGE_SIZE) PageTable {
    static constexpr usize ENTRIES_PER_TABLE = PAGE_SIZE / sizeof(PageTableEntry);
    PageTableEntry entries[ENTRIES_PER_TABLE];

    constexpr PageTable() : entries{} {}

    [[nodiscard]] constexpr PageTableEntry& operator[](usize index) {
        return entries[index];
    }

    [[nodiscard]] constexpr const PageTableEntry& operator[](usize index) const {
        return entries[index];
    }
};

static_assert(sizeof(PageTable) == PAGE_SIZE, "页表必须是一个页面大小");

// 虚拟地址解析结果
struct VirtualAddressBreakdown {
    u16 pgd_index;  // [47:39] - 9 bits
    u16 pud_index;  // [38:30] - 9 bits
    u16 pmd_index;  // [29:21] - 9 bits
    u16 pte_index;  // [20:12] - 9 bits
    u16 page_offset; // [11:0] - 12 bits
};

// 虚拟地址解析函数
constexpr VirtualAddressBreakdown break_virtual_address(VirtAddr vaddr) {
    return {
        .pgd_index = static_cast<u16>((vaddr >> 39) & 0x1FF),
        .pud_index = static_cast<u16>((vaddr >> 30) & 0x1FF),
        .pmd_index = static_cast<u16>((vaddr >> 21) & 0x1FF),
        .pte_index = static_cast<u16>((vaddr >> 12) & 0x1FF),
        .page_offset = static_cast<u16>(vaddr & 0xFFF)
    };
}

// ARM64地址空间配置
struct AddressSpaceConfig {
    // Translation Control Register (TCR_EL1) 配置
    static constexpr u64 TCR_T0SZ = 16;     // 48位虚拟地址空间 (64-16=48)
    static constexpr u64 TCR_T1SZ = 16;     // 48位虚拟地址空间
    static constexpr u64 TCR_TG0_4KB = 0ULL << 14;  // 4KB页面大小
    static constexpr u64 TCR_TG1_4KB = 2ULL << 30;  // 4KB页面大小
    static constexpr u64 TCR_SH0_INNER = 3ULL << 12; // 内部共享
    static constexpr u64 TCR_SH1_INNER = 3ULL << 28; // 内部共享
    static constexpr u64 TCR_ORGN0_WBWA = 1ULL << 10; // 外部写回写分配
    static constexpr u64 TCR_ORGN1_WBWA = 1ULL << 26; // 外部写回写分配
    static constexpr u64 TCR_IRGN0_WBWA = 1ULL << 8;  // 内部写回写分配
    static constexpr u64 TCR_IRGN1_WBWA = 1ULL << 24; // 内部写回写分配
    static constexpr u64 TCR_EPD1 = 0ULL << 23;       // 启用TTBR1_EL1
    static constexpr u64 TCR_A1 = 0ULL << 22;         // ASID在TTBR0中定义
    static constexpr u64 TCR_IPS_48BIT = 5ULL << 32;  // 48位物理地址

    static constexpr u64 TCR_VALUE =
        TCR_T0SZ | TCR_T1SZ | TCR_TG0_4KB | TCR_TG1_4KB |
        TCR_SH0_INNER | TCR_SH1_INNER | TCR_ORGN0_WBWA | TCR_ORGN1_WBWA |
        TCR_IRGN0_WBWA | TCR_IRGN1_WBWA | TCR_EPD1 | TCR_A1 | TCR_IPS_48BIT;

    // Memory Attribute Indirection Register (MAIR_EL1) 配置
    static constexpr u64 MAIR_DEVICE_nGnRnE = 0x00ULL; // 设备内存，非聚集，非重排序，非早期写应答
    static constexpr u64 MAIR_NORMAL_WBWA = 0xFFULL;   // 普通内存，写回写分配
    static constexpr u64 MAIR_NORMAL_NC = 0x44ULL;     // 普通内存，非缓存

    static constexpr u64 MAIR_VALUE =
        (MAIR_DEVICE_nGnRnE << 0) |   // 属性索引0：设备内存
        (MAIR_NORMAL_WBWA << 8) |     // 属性索引1：普通缓存内存
        (MAIR_NORMAL_NC << 16);       // 属性索引2：非缓存内存
};

// 页表管理器前置声明
class PageTableManager {
private:
    // 早期页表分配器（简单的静态分配）
    static constexpr usize MAX_EARLY_TABLES = 128;
    alignas(PAGE_SIZE) static inline PageTable early_tables[MAX_EARLY_TABLES];
    static inline usize next_table_index = 0;

    // 内核页表根目录
    static inline PageTable* kernel_pgd = nullptr;

public:
    // 分配一个新的页表（早期阶段）
    [[nodiscard]] static KernelResult<PageTable*> allocate_page_table();

    // 获取页表的物理地址
    [[nodiscard]] static PhysAddr get_physical_address(const PageTable* table) {
        return reinterpret_cast<PhysAddr>(table);
    }

    // 创建内核页表映射
    [[nodiscard]] static VoidResult setup_kernel_page_tables();

    // 映射内存区域
    [[nodiscard]] static VoidResult map_region(VirtAddr virt_addr, PhysAddr phys_addr,
                                               usize size, u64 permissions);

    // 映射单个页面
    [[nodiscard]] static VoidResult map_page(VirtAddr virt_addr, PhysAddr phys_addr,
                                             u64 permissions);

    // 启用MMU
    [[nodiscard]] static VoidResult enable_mmu();

    // 获取内核页表根目录
    [[nodiscard]] static PageTable* get_kernel_pgd() {
        return kernel_pgd;
    }

    // 无效化TLB（使用架构抽象层）
    static void invalidate_tlb() {
        // 使用架构抽象层的TLB刷新功能
        moss::kernel::arch::mmu::flush_tlb();
    }

    // 从当前页表初始化（用于启动后）
    [[nodiscard]] static VoidResult initialize_from_current() {
        // 简化实现：假设已经有页表设置
        return VoidResult{};
    }
};

// 全局函数声明
VoidResult setup_mmu();
void invalidate_all_tlb();

} // namespace moss::kernel::mm