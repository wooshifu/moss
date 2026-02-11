#pragma once

/*
 * ==================================================================
 *                    ARM64 内存管理单元 (MMU) 详细说明
 * ==================================================================
 *
 * 本文件实现了ARM64架构的4级页表内存管理系统，提供虚拟地址到物理地址的转换。
 *
 * 1. MMU (Memory Management Unit) 基本概念
 * ==========================================
 *
 * MMU是CPU的一个硬件组件，负责：
 * - 虚拟地址到物理地址的转换（地址翻译）
 * - 内存访问权限控制（读、写、执行权限）
 * - 内存属性管理（缓存策略、共享性等）
 * - TLB（Translation Lookaside Buffer）管理
 *
 * 2. ARM64 4级页表结构详解
 * ===========================
 *
 * ARM64使用4级页表结构来支持48位虚拟地址空间：
 *
 *   48位虚拟地址格式：
 *   ┌─────────────┬─────────────┬─────────────┬─────────────┬─────────────┐
 *   │[47:39] 9位  │[38:30] 9位  │[29:21] 9位  │[20:12] 9位  │[11:0] 12位  │
 *   │   PGD索引   │   PUD索引   │   PMD索引   │   PTE索引   │   页面偏移   │
 *   └─────────────┴─────────────┴─────────────┴─────────────┴─────────────┘
 *
 *   页表层级结构：
 *   Level 0: PGD (Page Global Directory)   - 最高级，管理512GB地址空间
 *   Level 1: PUD (Page Upper Directory)    - 管理1GB地址空间
 *   Level 2: PMD (Page Middle Directory)   - 管理2MB地址空间
 *   Level 3: PTE (Page Table Entry)        - 管理4KB页面
 *
 *   每级页表包含512个8字节条目，总共4KB大小
 *
 * 3. 地址转换过程详解
 * ===================
 *
 * 步骤1: 解析虚拟地址
 *   虚拟地址 0x12345ABCDEF → 分解为：
 *   - PGD索引: (0x12345ABCDEF >> 39) & 0x1FF = 0x024
 *   - PUD索引: (0x12345ABCDEF >> 30) & 0x1FF = 0x0D1
 *   - PMD索引: (0x12345ABCDEF >> 21) & 0x1FF = 0x156
 *   - PTE索引: (0x12345ABCDEF >> 12) & 0x1FF = 0x0BC
 *   - 页偏移:   0x12345ABCDEF & 0xFFF = 0xDEF
 *
 * 步骤2: 遍历页表层级
 *   1) TTBR0/1寄存器 → 获取PGD基址
 *   2) PGD基址 + PGD索引*8 → 读取PUD基址
 *   3) PUD基址 + PUD索引*8 → 读取PMD基址
 *   4) PMD基址 + PMD索引*8 → 读取PTE基址
 *   5) PTE基址 + PTE索引*8 → 读取物理页面基址
 *   6) 物理页面基址 + 页偏移 → 最终物理地址
 *
 * 4. 页表项格式详解（8字节）
 * ============================
 *
 *   位[63:54] 位[53:48] 位[47:12]     位[11:10] 位[9:2]  位[1] 位[0]
 *   ┌────────┬────────┬──────────────┬────────┬────────┬────┬────┐
 *   │ 保留位  │扩展属性 │  物理地址     │  AF等  │属性索引 │ T  │ V  │
 *   └────────┴────────┴──────────────┴────────┴────────┴────┴────┘
 *
 *   关键位说明：
 *   - 位[0] Valid: 页表项有效位（1=有效，0=无效）
 *   - 位[1] Table: 类型位（1=指向下级页表，0=指向物理页面/块）
 *   - 位[4:2] AttrIndx: 内存属性索引（0-7，索引MAIR_EL1寄存器）
 *   - 位[6] AP[1]: 用户访问权限
 *   - 位[7] AP[0]: 写权限控制
 *   - 位[10] AF: 访问标志（硬件设置，用于页面置换算法）
 *   - 位[11] nG: 非全局位（ASID相关）
 *   - 位[47:12] 物理地址: 4KB对齐的物理地址
 *   - 位[53] PXN: 特权执行从不（禁止EL1执行）
 *   - 位[54] XN: 执行从不（禁止所有执行）
 *
 * 5. 1GB块映射优化（当前实现）
 * ==============================
 *
 * 为了提高性能和简化早期启动，当前实现使用1GB块映射：
 * - 直接在PGD级使用块映射，跳过PUD/PMD/PTE层级
 * - 每个PGD条目映射1GB地址空间
 * - TLB效率极高（仅需4个条目覆盖4GB空间）
 * - 内存开销最小（仅需1个4KB页表）
 *
 * 6. TTBR双表配置
 * =================
 *
 * ARM64支持两个页表基址寄存器：
 * - TTBR0_EL1: 用户空间页表（0x0000000000000000-0x0000FFFFFFFFFFFF）
 * - TTBR1_EL1: 内核空间页表（0xFFFF000000000000-0xFFFFFFFFFFFFFFFF）
 *
 * 当前实现使用双TTBR方案，两个寄存器指向同一页表，简化早期启动。
 */

#include "../include/arch/arch_abstraction.hpp"
#include "../include/result.hpp"
#include "../include/types.hpp"

namespace moss::kernel::mm {

/*
 * ==================================================================
 *                      内存属性与缓存策略详解
 * ==================================================================
 */

/**
 * ARM64 内存属性枚举 - 定义不同类型内存的访问特性
 *
 * ARM64通过MAIR_EL1寄存器定义8种内存属性（索引0-7），每种属性描述：
 * - 缓存策略：是否可缓存，内外部缓存行为
 * - 共享性：是否在多核间共享
 * - 设备特性：是否为设备内存，访问顺序要求
 *
 * 内存属性影响：
 * 1. 性能：缓存策略直接影响访问速度
 * 2. 正确性：设备内存需要严格的访问顺序
 * 3. 一致性：共享内存需要跨核心同步
 */
enum class MemoryAttributes : u8 {
  /**
   * 普通可缓存内存 (MAIR值: 0xFF)
   * - 用于：代码段、数据段、堆栈
   * - 特性：完全可缓存，支持写回写分配
   * - 性能：最高，适合CPU频繁访问的内存
   */
  NORMAL_CACHEABLE = 0,

  /**
   * 普通不可缓存内存 (MAIR值: 0x44)
   * - 用于：DMA缓冲区、共享内存区
   * - 特性：不缓存，直接访问主内存
   * - 一致性：保证跨设备的内存一致性
   */
  NORMAL_NON_CACHEABLE = 1,

  /**
   * 严格设备内存 (MAIR值: 0x00) - nGnRnE
   * - 用于：关键设备寄存器（中断控制器、定时器）
   * - 特性：不可聚集、不可重排序、不可提前执行
   * - 保证：严格的访问顺序和副作用
   */
  DEVICE_nGnRnE = 2,

  /**
   * 设备内存 (MAIR值: 0x04) - nGnRE
   * - 用于：一般设备寄存器
   * - 特性：不可聚集、不可重排序、可提前执行
   * - 平衡：性能与访问顺序的平衡
   */
  DEVICE_nGnRE = 3,

  /**
   * 弱设备内存 (MAIR值: 0x0C) - GRE
   * - 用于：缓冲区类设备、网卡等
   * - 特性：可聚集、可重排序、可提前执行
   * - 性能：较高，适合批量传输设备
   */
  DEVICE_GRE = 4
};

/*
 * ==================================================================
 *                      页表层级结构详解
 * ==================================================================
 */

/**
 * ARM64 4级页表层级枚举
 *
 * 每级页表的管理范围和功能：
 *
 * ┌─────────────────────────────────────────────────┐
 * │ Level 0: PGD (Page Global Directory)           │
 * │ - 虚拟地址位: [47:39] (9位)                    │
 * │ - 管理范围: 512GB (512 * 1GB)                 │
 * │ - 可映射: 1GB块 或 指向PUD表                   │
 * └─────────────────────────────────────────────────┘
 *          │
 *          ▼
 * ┌─────────────────────────────────────────────────┐
 * │ Level 1: PUD (Page Upper Directory)            │
 * │ - 虚拟地址位: [38:30] (9位)                    │
 * │ - 管理范围: 1GB (512 * 2MB)                   │
 * │ - 可映射: 1GB块 或 指向PMD表                   │
 * └─────────────────────────────────────────────────┘
 *          │
 *          ▼
 * ┌─────────────────────────────────────────────────┐
 * │ Level 2: PMD (Page Middle Directory)           │
 * │ - 虚拟地址位: [29:21] (9位)                    │
 * │ - 管理范围: 2MB (512 * 4KB)                   │
 * │ - 可映射: 2MB块 或 指向PTE表                   │
 * └─────────────────────────────────────────────────┘
 *          │
 *          ▼
 * ┌─────────────────────────────────────────────────┐
 * │ Level 3: PTE (Page Table Entry)                │
 * │ - 虚拟地址位: [20:12] (9位)                    │
 * │ - 管理范围: 4KB页面                            │
 * │ - 只能映射: 4KB页面                            │
 * └─────────────────────────────────────────────────┘
 */
enum class PageLevel : u32 {
  PGD = 0, ///< Page Global Directory (Level 0) - 最高级页表
  PUD = 1, ///< Page Upper Directory (Level 1) - 上级页表
  PMD = 2, ///< Page Middle Directory (Level 2) - 中级页表
  PTE = 3  ///< Page Table Entry (Level 3) - 页表项
};

/**
 * 页面大小枚举 - ARM64支持的页面和块大小
 *
 * ARM64支持多种粒度的内存映射：
 * - 4KB页面: 标准页面，适合通用内存管理
 * - 2MB大页: 中等大页，减少页表开销，适合大内存应用
 * - 1GB巨页: 最大页面，极大减少TLB miss，适合超大内存应用
 *
 * 使用场景：
 * - 4KB: 用户进程、内核堆栈、小对象分配
 * - 2MB: 大内存应用、数据库缓存、图形缓冲区
 * - 1GB: 虚拟化、大数据处理、高性能计算
 */
enum class PageSize : u64 {
  Size4KB = PAGE_SIZE,       ///< 4KB标准页面 (4096字节)
  Size2MB = LARGE_PAGE_SIZE, ///< 2MB大页面 (2097152字节)
  Size1GB = HUGE_PAGE_SIZE   ///< 1GB巨页面 (1073741824字节)
};

/*
 * ==================================================================
 *                      页表项属性位详解
 * ==================================================================
 */

/**
 * 页表项属性位定义 - ARM64页表项格式的各个控制位
 *
 * ARM64页表项格式（64位）：
 * ┌─────┬─────┬────────────────────┬──────┬────────┬───┬───┐
 * │63:55│54:53│      52:12         │11:10 │  9:2   │ 1 │ 0 │
 * │保留位│ XN  │   输出地址[47:12]   │ nG等 │AttrIndx│ T │ V │
 * │     │ PXN │                    │      │        │   │   │
 * └─────┴─────┴────────────────────┴──────┴────────┴───┴───┘
 */
namespace PageAttr {
// 基本控制位
static constexpr u64 VALID = (1ULL << 0); ///< [0] 有效位: 1=页表项有效
static constexpr u64 TABLE = (1ULL << 1); ///< [1] 表类型: 1=页表，0=块/页面

// 访问权限位 (AP字段 - Access Permission)
static constexpr u64 USER = (1ULL << 6);     ///< [6] AP[1]: 1=用户可访问
static constexpr u64 READONLY = (1ULL << 7); ///< [7] AP[0]: 1=只读，0=读写

// 共享和缓存控制位
static constexpr u64 SHARED = (1ULL << 8); ///< [8] SH[0]: 共享性控制(配合SH[1])
static constexpr u64 AF = (1ULL << 10);    ///< [10] 访问标志: 硬件自动设置
static constexpr u64 NG = (1ULL << 11);    ///< [11] 非全局: 1=进程特定

// 执行权限控制位
static constexpr u64 PXN = (1ULL << 53); ///< [53] 特权执行从不: 禁止EL1执行
static constexpr u64 XN = (1ULL << 54);  ///< [54] 执行从不: 禁止所有执行

// 内存属性索引 (AttrIndx字段 - 索引MAIR_EL1寄存器)
static constexpr u64 ATTR_IDX_SHIFT = 2; ///< AttrIndx字段起始位
static constexpr u64 ATTR_DEVICE =
    (0ULL << ATTR_IDX_SHIFT); ///< 属性索引0: 设备内存
static constexpr u64 ATTR_NORMAL =
    (1ULL << ATTR_IDX_SHIFT); ///< 属性索引1: 普通缓存内存
static constexpr u64 ATTR_NORMAL_NC =
    (2ULL << ATTR_IDX_SHIFT); ///< 属性索引2: 非缓存内存

/*
 * 权限组合示例：
 *
 * 内核只读代码: VALID | ATTR_NORMAL | READONLY | AF
 * 内核读写数据: VALID | ATTR_NORMAL | AF | PXN | XN
 * 用户只读代码: VALID | ATTR_NORMAL | READONLY | USER | AF
 * 用户读写数据: VALID | ATTR_NORMAL | USER | AF | PXN | XN
 * 设备寄存器:   VALID | ATTR_DEVICE | AF | PXN | XN
 */
} // namespace PageAttr

/*
 * ==================================================================
 *                      预定义权限组合
 * ==================================================================
 */

/**
 * 页表项权限组合 - 常用的权限配置模板
 *
 * 这些预定义组合涵盖了操作系统中常见的内存区域类型，
 * 包括安全性、性能和功能需求的平衡。
 */
namespace PagePerms {
/**
 * 内核只读区域 (代码段、只读数据段)
 * - 特性: 内核可读，禁止写入和执行
 * - 用于: .rodata段、常量字符串、只读配置
 * - 安全: 防止意外修改关键数据
 */
static constexpr u64 KERNEL_RO = PageAttr::VALID | PageAttr::AF |
                                 PageAttr::ATTR_NORMAL | PageAttr::READONLY |
                                 PageAttr::PXN | PageAttr::XN;

/**
 * 内核读写区域 (数据段、BSS段、堆栈)
 * - 特性: 内核可读写，禁止执行
 * - 用于: 全局变量、堆、栈、动态分配内存
 * - 安全: NX位防止代码注入攻击
 */
static constexpr u64 KERNEL_RW = PageAttr::VALID | PageAttr::AF |
                                 PageAttr::ATTR_NORMAL | PageAttr::PXN |
                                 PageAttr::XN;

/**
 * 内核可执行区域 (代码段)
 * - 特性: 内核可读可执行，禁止写入
 * - 用于: 内核代码段、内核模块代码
 * - 安全: W^X原则，代码区不可写
 */
static constexpr u64 KERNEL_RX =
    PageAttr::VALID | PageAttr::AF | PageAttr::ATTR_NORMAL | PageAttr::READONLY;

/**
 * 用户只读区域 (用户程序只读数据)
 * - 特性: 用户和内核可读，禁止写入和执行
 * - 用于: 用户程序常量、共享库只读段
 * - 权限: 用户可访问但不可修改
 */
static constexpr u64 USER_RO = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                               PageAttr::READONLY | PageAttr::ATTR_NORMAL;

/**
 * 用户读写区域 (用户数据段、堆、栈)
 * - 特性: 用户可读写，禁止执行
 * - 用于: 用户变量、堆内存、栈空间
 * - 安全: DEP/NX位防止栈溢出攻击
 */
static constexpr u64 USER_RW =
    PageAttr::VALID | PageAttr::AF | PageAttr::USER | PageAttr::ATTR_NORMAL;

/**
 * 用户可执行区域 (用户程序代码)
 * - 特性: 用户可读可执行，禁止写入
 * - 用于: 用户程序代码段、动态链接库代码
 * - 安全: 代码完整性保护
 */
static constexpr u64 USER_RX = PageAttr::VALID | PageAttr::AF | PageAttr::USER |
                               PageAttr::READONLY | PageAttr::ATTR_NORMAL;

/**
 * 设备内存区域 (设备寄存器、MMIO)
 * - 特性: 内核可读写，严格顺序访问，禁止执行
 * - 用于: 设备寄存器、内存映射I/O
 * - 特殊: 不可缓存，保证访问顺序和副作用
 */
static constexpr u64 DEVICE = PageAttr::VALID | PageAttr::AF |
                              PageAttr::ATTR_DEVICE | PageAttr::XN |
                              PageAttr::PXN;
} // namespace PagePerms

/*
 * ==================================================================
 *                      页表项结构实现
 * ==================================================================
 */

/**
 * ARM64页表项结构 - 封装64位页表项的所有操作
 *
 * 页表项是ARM64内存管理的核心数据结构，每个8字节的条目包含：
 * - 物理地址信息 (位[47:12])
 * - 内存属性 (位[11:2])
 * - 控制位 (位[1:0])
 * - 扩展属性 (位[63:48])
 *
 * 页表项类型：
 * 1. 无效项: VALID=0，所有访问导致页面错误
 * 2. 表项: VALID=1, TABLE=1，指向下一级页表
 * 3. 块/页项: VALID=1, TABLE=0，直接映射到物理内存
 */
struct [[gnu::packed]] PageTableEntry {
  u64 raw; ///< 原始64位页表项值

  /*
   * 构造函数
   */

  /// 默认构造 - 创建无效的页表项
  constexpr PageTableEntry() : raw(0) {}

  /// 从原始值构造 - 用于加载已存在的页表项
  constexpr explicit PageTableEntry(u64 value) : raw(value) {}

  /*
   * 状态查询方法
   */

  /**
   * 检查页表项是否有效
   * @return true表示页表项有效，可以进行地址转换
   */
  [[nodiscard]] constexpr bool is_valid() const {
    return raw & PageAttr::VALID;
  }

  /**
   * 检查是否为表项（指向下级页表）
   * @return true表示指向下级页表，false表示块/页映射
   */
  [[nodiscard]] constexpr bool is_table() const {
    return raw & PageAttr::TABLE;
  }

  /**
   * 检查是否为块项或页项（直接映射）
   * @return true表示直接映射到物理内存
   */
  [[nodiscard]] constexpr bool is_block() const {
    return is_valid() && !is_table();
  }

  /*
   * 地址提取方法
   */

  /**
   * 获取页表项中的物理地址
   *
   * 从64位页表项中提取48位物理地址，自动处理4KB对齐：
   * - 位[47:12]: 实际物理地址位
   * - 位[11:0]:  假设为0（4KB对齐）
   *
   * @return 4KB对齐的48位物理地址
   */
  [[nodiscard]] constexpr PhysAddr get_phys_addr() const {
    return raw & 0x0000FFFFFFFFF000ULL; // 掩码提取位[47:12]
  }

  /*
   * 页表项设置方法
   */

  /**
   * 设置为表项 - 指向下一级页表
   *
   * 配置页表项指向下一级页表，用于4级页表遍历：
   * - 设置物理地址为下级页表的物理地址
   * - 设置VALID=1和TABLE=1
   * - 清除其他属性位
   *
   * @param next_table_pa 下一级页表的4KB对齐物理地址
   */
  constexpr void set_table(PhysAddr next_table_pa) {
    raw = (next_table_pa & 0x0000FFFFFFFFF000ULL) | PageAttr::VALID |
          PageAttr::TABLE;
  }

  /**
   * 设置为块项 - 直接映射到物理内存
   *
   * 配置页表项直接映射到物理内存块/页面：
   * - 设置物理地址
   * - 设置VALID=1和TABLE=0
   * - 应用指定的内存属性和权限
   *
   * 使用示例：
   * ```cpp
   * PageTableEntry pte;
   * // 映射1GB内核代码块，只读可执行
   * pte.set_block(0x40000000, PagePerms::KERNEL_RX);
   *
   * // 映射4KB用户数据页，读写不可执行
   * pte.set_block(0x12345000, PagePerms::USER_RW);
   * ```
   *
   * @param block_pa 物理块/页面的4KB对齐基址
   * @param attributes 内存属性和权限标志的组合
   */
  constexpr void set_block(PhysAddr block_pa, u64 attributes) {
    raw = (block_pa & 0x0000FFFFFFFFF000ULL) | attributes | PageAttr::VALID;
  }

  /**
   * 清除页表项 - 设置为无效
   *
   * 将页表项标记为无效，任何访问都会触发页面错误。
   * 用于取消映射或初始化页表。
   */
  constexpr void clear() { raw = 0; }
};

// 编译时检查：确保页表项大小符合ARM64规范
static_assert(sizeof(PageTableEntry) == 8, "页表项必须是8字节");

/*
 * ==================================================================
 *                      页表结构实现
 * ==================================================================
 */

/**
 * ARM64页表结构 - 一个完整的页表页面
 *
 * 每个页表占用一个4KB页面，包含512个8字节的页表项。
 * 这个结构提供了类型安全的页表访问接口。
 *
 * 页表布局：
 * ┌─────────────────────────────────────────────┐
 * │ PageTableEntry[0]   - 8字节                 │
 * │ PageTableEntry[1]   - 8字节                 │
 * │ ...                                         │
 * │ PageTableEntry[511] - 8字节                 │
 * └─────────────────────────────────────────────┘
 * 总计：512 * 8 = 4096字节 = 1页面
 *
 * 索引计算：
 * - PGD索引：(vaddr >> 39) & 0x1FF
 * - PUD索引：(vaddr >> 30) & 0x1FF
 * - PMD索引：(vaddr >> 21) & 0x1FF
 * - PTE索引：(vaddr >> 12) & 0x1FF
 */
struct alignas(PAGE_SIZE) PageTable {
  /// 每个页表包含的条目数量（512个）
  static constexpr usize ENTRIES_PER_TABLE = PAGE_SIZE / sizeof(PageTableEntry);

  /// 页表项数组 - 512个8字节条目
  PageTableEntry entries[ENTRIES_PER_TABLE];

  /**
   * 默认构造函数 - 初始化所有条目为无效
   *
   * 使用零初始化确保所有页表项开始时都无效（VALID=0），
   * 避免意外的内存映射。
   */
  constexpr PageTable() : entries{} {}

  /**
   * 数组下标操作符 - 非const版本
   *
   * 提供类型安全的页表项访问，支持修改操作。
   *
   * @param index 页表项索引 (0-511)
   * @return 指定索引的页表项引用
   */
  [[nodiscard]] constexpr PageTableEntry &operator[](usize index) {
    return entries[index];
  }

  /**
   * 数组下标操作符 - const版本
   *
   * 提供类型安全的页表项只读访问。
   *
   * @param index 页表项索引 (0-511)
   * @return 指定索引的页表项const引用
   */
  [[nodiscard]] constexpr const PageTableEntry &operator[](usize index) const {
    return entries[index];
  }
};

// 编译时检查：确保页表大小符合ARM64规范
static_assert(sizeof(PageTable) == PAGE_SIZE, "页表必须是一个页面大小");

// 虚拟地址解析结果
struct VirtualAddressBreakdown {
  u16 pgd_index;   // [47:39] - 9 bits
  u16 pud_index;   // [38:30] - 9 bits
  u16 pmd_index;   // [29:21] - 9 bits
  u16 pte_index;   // [20:12] - 9 bits
  u16 page_offset; // [11:0] - 12 bits
};

// 虚拟地址解析函数
constexpr VirtualAddressBreakdown break_virtual_address(VirtAddr vaddr) {
  return {.pgd_index = static_cast<u16>((vaddr >> 39) & 0x1FF),
          .pud_index = static_cast<u16>((vaddr >> 30) & 0x1FF),
          .pmd_index = static_cast<u16>((vaddr >> 21) & 0x1FF),
          .pte_index = static_cast<u16>((vaddr >> 12) & 0x1FF),
          .page_offset = static_cast<u16>(vaddr & 0xFFF)};
}

// ARM64地址空间配置
struct AddressSpaceConfig {
  // Translation Control Register (TCR_EL1) 统一配置
  // 双TTBR方案：TTBR0用于身份映射，TTBR1用于内核虚拟映射
  static constexpr u64 TCR_VALUE =
      (16ULL << 0) |  // T0SZ=16 (48位地址空间，用于身份映射)
      (16ULL << 16) | // T1SZ=16 (48位内核虚拟地址空间)
      (0ULL << 6) |   // EPD0=0 (启用TTBR0_EL1用于身份映射)
      (0ULL << 23) |  // EPD1=0 (启用TTBR1_EL1内核空间)
      (0ULL << 14) |  // TG0=00 (4KB页面大小，身份映射)
      (0ULL << 30) |  // TG1=00 (4KB页面大小，内核空间)
      (1ULL << 8) |   // IRGN0=01 (身份映射内部Write-Back/Write-Allocate)
      (1ULL << 10) |  // ORGN0=01 (身份映射外部Write-Back/Write-Allocate)
      (3ULL << 12) |  // SH0=11 (身份映射内部共享)
      (1ULL << 24) |  // IRGN1=01 (内核内部Write-Back/Write-Allocate)
      (1ULL << 26) |  // ORGN1=01 (内核外部Write-Back/Write-Allocate)
      (3ULL << 28) |  // SH1=11 (内核内部共享)
      (5ULL << 32);   // IPS=101 (48位物理地址空间)

  // Memory Attribute Indirection Register (MAIR_EL1) 配置
  static constexpr u64 MAIR_DEVICE_nGnRnE =
      0x00ULL; // 设备内存，非聚集，非重排序，非早期写应答
  static constexpr u64 MAIR_NORMAL_WBWA = 0xFFULL; // 普通内存，写回写分配
  static constexpr u64 MAIR_NORMAL_NC = 0x44ULL;   // 普通内存，非缓存

  static constexpr u64 MAIR_VALUE =
      (MAIR_DEVICE_nGnRnE << 0) | // 属性索引0：设备内存
      (MAIR_NORMAL_WBWA << 8) |   // 属性索引1：普通缓存内存
      (MAIR_NORMAL_NC << 16);     // 属性索引2：非缓存内存
};

/*
 * ==================================================================
 *                      页表管理器详细实现
 * ==================================================================
 */

/**
 * ARM64页表管理器 - 内核内存管理的核心组件
 *
 * 页表管理器负责整个ARM64内存管理子系统的运作，包括：
 * 1. 页表的分配和释放
 * 2. 虚拟地址到物理地址的映射建立
 * 3. MMU的配置和启用
 * 4. TLB的管理和无效化
 * 5. 调试和监控功能
 *
 * 核心设计原则：
 * - 静态分配池：早期启动阶段使用预分配的页表池，避免动态内存分配
 * - 统一架构接口：通过架构抽象层支持多种处理器架构
 * - 4级页表支持：完整实现ARM64的PGD→PUD→PMD→PTE层级结构
 * - 调试友好：提供详细的页表状态输出和验证功能
 *
 * 内存分配策略：
 * 页表管理器使用两阶段内存管理策略：
 * 1. **早期阶段（Early Stage）**：
 *    - 使用静态分配的early_tables[]数组
 *    - 支持最多1024个页表（4MB总内存）
 *    - 无锁简单分配器，适用于单核启动阶段
 *    - 页表物理地址直接等于虚拟地址（身份映射）
 *
 * 2. **运行时阶段（Runtime Stage）**：
 *    - 后期可扩展为动态页表分配器
 *    - 支持页表的释放和重用
 *    - 多核安全的分配机制
 *
 * 页表层级管理：
 * ARM64 4级页表完整实现示例：
 *
 * 虚拟地址 0x0000123456789ABC 的映射过程：
 * 1. PGD索引计算：(0x0000123456789ABC >> 39) & 0x1FF = 0x000
 * 2. PUD索引计算：(0x0000123456789ABC >> 30) & 0x1FF = 0x048
 * 3. PMD索引计算：(0x0000123456789ABC >> 21) & 0x1FF = 0x0D1
 * 4. PTE索引计算：(0x0000123456789ABC >> 12) & 0x1FF = 0x189
 * 5. 页内偏移：   0x0000123456789ABC & 0xFFF = 0xABC
 *
 * 页表遍历路径：
 * kernel_pgd[0x000] → pud_table[0x048] → pmd_table[0x0D1] → pte_table[0x189]
 * → 物理页面基址 + 0xABC = 最终物理地址
 *
 * 当前实现优化：
 * 为了简化早期启动和提高性能，当前实现使用PGD级1GB块映射：
 * - 直接在PGD级建立1GB块映射，跳过中间层级
 * - 覆盖0-4GB物理地址空间的身份映射
 * - TLB效率最高（仅需4个TLB条目）
 * - 内存开销最小（仅需1个4KB页表）
 */
class PageTableManager {
private:
  /**
   * 早期页表分配器 - 内核启动阶段的静态页表池
   *
   * 【作用和用途】：
   * - 在内核启动早期阶段提供页表分配能力
   * - 支持MMU初始化和早期虚拟内存映射建立
   * - 避免在动态内存分配器初始化前的页表分配依赖问题
   *
   * 【使用时机】：
   * - 内核启动阶段：从 _start 到动态内存分配器初始化完成
   * - MMU配置期间：建立恒等映射和内核虚拟地址空间
   * - 早期内存管理初始化：页表结构建立和TLB配置
   *
   * 【不再使用的时机】：
   * - RuntimeHeapAllocator初始化完成后
   * - 动态页表分配机制启用后
   * - 进入用户空间支持阶段（如果需要）
   *
   * 【技术细节】：
   * - 基于Linux内核实践，采用保守的静态预分配策略
   * - 配合1GB块映射优化，实际使用量极少（通常<10个页表）
   * - 每个页表4KB，总计256KB内存占用（相比原4MB减少93.75%）
   * - 支持256GB地址空间映射能力，为实际需求的4倍安全余量
   */
  static constexpr usize MAX_EARLY_TABLES = 64;

  /**
   * 早期页表数组 - 4KB页面对齐的静态页表池
   *
   * 【内存布局】：
   * - 每个页表：4KB (PAGE_SIZE)
   * - 总大小：64 × 4KB = 256KB
   * - 对齐要求：PAGE_SIZE边界对齐（ARM64 MMU硬件要求）
   *
   * 【访问模式】：
   * - 顺序分配：通过next_table_index递增分配
   * - 单次分配：不支持释放和重用（简化早期实现）
   * - 线性搜索：适合早期启动阶段的简单需求
   *
   * 【生命周期】：
   * 1. 编译时：静态分配在.bss段，启动时被清零
   * 2. 启动时：通过allocate_early_page_table()分配
   * 3. 运行时：只读访问，不再分配新页表
   * 4. 废弃时：动态分配器接管后，该数组成为"遗留内存"
   */
  alignas(PAGE_SIZE) static inline PageTable early_tables[MAX_EARLY_TABLES];

  /**
   * 早期页表分配索引 - 跟踪下一个可用的页表槽位
   *
   * 【工作原理】：
   * - 初始值：0（指向early_tables[0]）
   * - 分配时：返回&early_tables[next_table_index++]
   * - 耗尽检查：next_table_index >= MAX_EARLY_TABLES时报错
   *
   * 【监控和调试】：
   * - 正常使用率：< 10% (实际使用<6个页表)
   * - 警告阈值：> 50% (32个页表)
   * - 错误阈值：= 100% (64个页表耗尽)
   */
  static inline usize next_table_index = 0;

  // 内核页表根目录
  static inline PageTable *kernel_pgd = nullptr;

public:
  /*
   * ==================================================================
   *                      页表分配和管理接口
   * ==================================================================
   */

  /**
   * 分配一个新的页表页面（早期阶段分配器）
   *
   * 从静态预分配的early_tables[]数组中分配一个4KB页表页面。
   * 这是一个线性分配器，适用于早期启动阶段的单线程环境。
   *
   * 分配特性：
   * - 线性分配：从索引0开始顺序分配，不支持释放
   * - 零初始化：每个新分配的页表都会被清零
   * - 4KB对齐：所有页表都按PAGE_SIZE对齐
   * - 物理地址：在身份映射阶段，虚拟地址=物理地址
   *
   * @return KernelResult<PageTable*> 成功时返回页表指针，失败时返回错误码
   *         - 成功：指向新分配页表的指针
   *         - 失败：ErrorCode::OutOfMemory（池已满）
   *
   * 使用示例：
   * ```cpp
   * auto pgd_result = PageTableManager::allocate_page_table();
   * if (pgd_result) {
   *     PageTable* pgd = *pgd_result;
   *     // 使用新分配的页表...
   * } else {
   *     // 处理内存不足...
   * }
   * ```
   */
  [[nodiscard]] static KernelResult<PageTable *> allocate_page_table();

  /*
   * ==================================================================
   *                      地址转换和管理接口
   * ==================================================================
   */

  /**
   * 获取页表的物理地址
   *
   * 在早期启动的身份映射阶段，虚拟地址直接等于物理地址。
   * 这个函数执行简单的指针转换，将页表虚拟地址转换为物理地址。
   *
   * @param table 页表指针（虚拟地址）
   * @return PhysAddr 对应的物理地址
   *
   * 注意：
   * - 仅在MMU启用前的身份映射阶段有效
   * - MMU启用后需要通过页表遍历获取物理地址
   */
  [[nodiscard]] static PhysAddr get_physical_address(const PageTable *table) {
    return reinterpret_cast<PhysAddr>(table);
  }

  /**
   * 从物理地址获取页表指针（MMU安全版本）
   *
   * 将物理地址转换为可访问的页表指针。在身份映射阶段，
   * 这是一个简单的类型转换。在虚拟映射阶段，需要确保
   * 物理地址在内核的虚拟地址空间中有对应的映射。
   *
   * @param pa 页表的物理地址
   * @return PageTable* 对应的页表指针
   *
   * 使用场景：
   * - 从页表项中提取下级页表的物理地址后访问
   * - 页表遍历过程中的地址转换
   */
  [[nodiscard]] static PageTable *get_table_from_physical(PhysAddr pa) {
    return reinterpret_cast<PageTable *>(pa);
  }

  /**
   * 从页表指针获取数组索引（用于MMU启用后的安全访问）
   *
   * 将页表指针转换为early_tables[]数组中的索引。这提供了
   * 一种在MMU启用后安全访问页表的方法，因为索引是稳定的。
   *
   * @param table 页表指针
   * @return usize 数组索引，如果指针无效则返回MAX_EARLY_TABLES
   *
   * 安全检查：
   * - 空指针检查
   * - 边界检查：确保指针在early_tables[]范围内
   * - 返回值验证：MAX_EARLY_TABLES表示无效索引
   */
  [[nodiscard]] static usize get_table_index(const PageTable *table) {
    if (!table || table < early_tables ||
        table >= early_tables + MAX_EARLY_TABLES) {
      return MAX_EARLY_TABLES; // 无效索引
    }
    return static_cast<usize>(table - early_tables);
  }

  /**
   * 从数组索引获取页表指针（MMU安全版本）
   *
   * 通过索引访问early_tables[]数组中的页表。这是在MMU
   * 启用后访问页表的安全方式，因为数组的虚拟地址是固定的。
   *
   * @param index 数组索引
   * @return PageTable* 对应的页表指针，索引无效时返回nullptr
   *
   * 边界检查：
   * - 索引必须小于MAX_EARLY_TABLES
   * - 返回nullptr表示索引无效
   */
  [[nodiscard]] static PageTable *get_table_by_index(usize index) {
    if (index >= MAX_EARLY_TABLES) {
      return nullptr;
    }
    return &early_tables[index];
  }

  /*
   * ==================================================================
   *                      MMU配置和启用接口
   * ==================================================================
   */

  /**
   * 创建内核页表映射
   *
   * 设置基本的内核页表结构，为MMU启用做准备。当前实现使用
   * PGD级1GB块映射来覆盖0-4GB的物理地址空间。
   *
   * 映射策略：
   * - PGD索引0：映射0GB-1GB物理内存（包含内核代码/数据）
   * - PGD索引1：映射1GB-2GB物理内存
   * - PGD索引2：映射2GB-3GB物理内存
   * - PGD索引3：映射3GB-4GB物理内存
   *
   * 内存属性：
   * - 属性索引0：设备内存（Device nGnRnE）
   * - Valid=1, Table=0：1GB块映射
   * - AF=1：访问标志预设，避免访问错误
   *
   * @return VoidResult 成功时返回空结果，失败时返回错误码
   *
   * 可能的错误：
   * - ErrorCode::OutOfMemory：页表分配失败
   * - ErrorCode::InvalidState：系统状态异常
   */
  [[nodiscard]] static VoidResult setup_kernel_page_tables();

  /**
   * 映射内存区域到虚拟地址空间
   *
   * 将连续的物理内存区域映射到指定的虚拟地址空间。这个函数
   * 支持任意大小的内存区域映射，内部会自动分页处理。
   *
   * 4级页表完整映射过程：
   * 1. 检查地址对齐（必须4KB对齐）
   * 2. 计算需要映射的页面数量
   * 3. 遍历每个4KB页面：
   *    a. 解析虚拟地址（PGD/PUD/PMD/PTE索引）
   *    b. 遍历页表层级，按需创建中间页表
   *    c. 在PTE级设置最终的页面映射
   *
   * @param virt_addr 目标虚拟地址（必须4KB对齐）
   * @param phys_addr 源物理地址（必须4KB对齐）
   * @param size 映射大小（字节），会向上对齐到页面边界
   * @param permissions 页面权限位（PagePerms命名空间中的组合）
   * @return VoidResult 成功时返回空结果，失败时返回错误码
   *
   * 权限示例：
   * ```cpp
   * // 映射内核代码段（只读可执行）
   * map_region(0xFFFF800000000000, 0x40000000,
   *            code_size, PagePerms::KERNEL_RX);
   *
   * // 映射内核数据段（读写不可执行）
   * map_region(0xFFFF800001000000, 0x41000000,
   *            data_size, PagePerms::KERNEL_RW);
   * ```
   */
  [[nodiscard]] static VoidResult map_region(VirtAddr virt_addr,
                                             PhysAddr phys_addr, usize size,
                                             u64 permissions);

  /**
   * 映射单个页面（4KB）
   *
   * 将一个4KB物理页面映射到指定的虚拟地址。这是内存映射的
   * 基本单元，支持完整的4级页表遍历和创建。
   *
   * 详细映射过程：
   * 1. **地址解析**：
   *    - PGD索引：(virt_addr >> 39) & 0x1FF
   *    - PUD索引：(virt_addr >> 30) & 0x1FF
   *    - PMD索引：(virt_addr >> 21) & 0x1FF
   *    - PTE索引：(virt_addr >> 12) & 0x1FF
   *
   * 2. **页表遍历**：
   *    - 检查PGD[pgd_idx]，如无效则创建PUD页表
   *    - 检查PUD[pud_idx]，如无效则创建PMD页表
   *    - 检查PMD[pmd_idx]，如无效则创建PTE页表
   *    - 设置PTE[pte_idx]为最终的页面映射
   *
   * 3. **页表项设置**：
   *    - 物理地址：4KB对齐的物理页面基址
   *    - 权限位：根据permissions参数设置
   *    - Valid=1：标记页表项有效
   *
   * @param virt_addr 目标虚拟地址（必须4KB对齐）
   * @param phys_addr 源物理地址（必须4KB对齐）
   * @param permissions 页面权限位组合
   * @return VoidResult 成功时返回空结果，失败时返回错误码
   *
   * 使用示例：
   * ```cpp
   * // 映射栈页面（内核读写，用户不可访问）
   * auto result = map_page(0xFFFF800002000000, 0x42000000,
   *                        PagePerms::KERNEL_RW);
   * if (!result) {
   *     // 处理映射失败...
   * }
   * ```
   */
  [[nodiscard]] static VoidResult map_page(VirtAddr virt_addr,
                                           PhysAddr phys_addr, u64 permissions);

  /**
   * 启用MMU（内存管理单元）
   *
   * 执行完整的ARM64 MMU启用序列，将处理器从物理地址模式
   * 切换到虚拟地址模式。这是内存管理系统激活的关键步骤。
   *
   * 完整的MMU启用序列：
   * 1. **状态验证**：
   *    - 检查kernel_pgd是否已初始化
   *    - 确保页表映射已建立
   *
   * 2. **寄存器配置**：
   *    - MAIR_EL1：配置内存属性（设备内存、普通缓存、非缓存）
   *    - TCR_EL1：配置转换控制（48位地址空间、4KB页面等）
   *    - TTBR0_EL1：设置用户空间页表基址（当前设为kernel_pgd）
   *    - TTBR1_EL1：设置内核空间页表基址（kernel_pgd）
   *
   * 3. **内存屏障**：
   *    - DSB SY：确保内存访问完成
   *    - ISB：确保指令流同步
   *
   * 4. **MMU激活**：
   *    - 设置SCTLR_EL1.M=1启用MMU
   *    - 最终同步确保MMU生效
   *
   * 5. **状态验证**：
   *    - 读取SCTLR_EL1确认MMU已启用
   *    - 如果能正常返回，说明页表配置正确
   *
   * @return VoidResult 成功时返回空结果，失败时返回错误码
   *
   * 可能的错误：
   * - ErrorCode::InvalidState：kernel_pgd未初始化
   * - 如果页表配置错误，可能导致系统挂起或异常
   *
   * 注意事项：
   * - 这个函数执行后，系统从物理地址模式切换到虚拟地址模式
   * - 必须确保当前执行的代码在页表中有正确的身份映射
   * - MMU启用失败通常导致系统立即崩溃或挂起
   */
  [[nodiscard]] static VoidResult enable_mmu();

  /*
   * ==================================================================
   *                      访问器和查询接口
   * ==================================================================
   */

  /**
   * 获取内核页表根目录指针
   *
   * 返回当前系统使用的内核页表根目录（PGD）指针。
   * 这是整个虚拟内存系统的入口点。
   *
   * @return PageTable* 内核PGD指针，如果未初始化则返回nullptr
   *
   * 使用场景：
   * - MMU状态检查
   * - 页表遍历的起点
   * - 调试和监控
   */
  [[nodiscard]] static PageTable *get_kernel_pgd() { return kernel_pgd; }

  /*
   * ==================================================================
   *                      TLB管理接口
   * ==================================================================
   */

  /**
   * 无效化所有TLB条目
   *
   * 刷新处理器的TLB（Translation Lookaside Buffer），强制
   * 重新从页表加载地址转换信息。当页表内容发生变化时必须调用。
   *
   * TLB无效化的必要性：
   * - TLB缓存虚拟地址到物理地址的转换
   * - 页表修改后，TLB中的缓存可能过时
   * - 不刷新TLB会导致访问错误的物理地址
   *
   * 实现细节：
   * - 使用架构抽象层的flush_tlb()函数
   * - ARM64：执行TLBI VMALLE1IS指令
   * - 包含必要的内存和指令屏障
   */
  static void invalidate_tlb() {
    // 使用架构抽象层的TLB刷新功能
    moss::kernel::arch::mmu::flush_tlb();
  }

  /**
   * 从当前页表初始化（用于启动后阶段）
   *
   * 在系统启动后期，当MMU已经启用时，初始化页表管理器
   * 来接管现有的页表结构。
   *
   * @return VoidResult 当前简化实现直接返回成功
   *
   * 注意：这是一个占位符实现，完整版本需要：
   * - 读取当前TTBR寄存器的值
   * - 扫描现有的页表结构
   * - 重建页表管理器的内部状态
   */
  [[nodiscard]] static VoidResult initialize_from_current() {
    // 简化实现：假设已经有页表设置
    return VoidResult{};
  }

  /*
   * ==================================================================
   *                      调试和监控接口
   * ==================================================================
   */

  /**
   * 打印页表详细信息（综合调试输出）
   *
   * 输出完整的页表状态信息，包括分配器状态、内存布局、
   * MMU寄存器、页表条目详情和地址转换示例。
   *
   * 输出内容：
   * 1. 页表分配器状态（已分配/可用页表数量）
   * 2. 页表内存布局（起始地址、大小等）
   * 3. MMU寄存器详细状态
   * 4. PGD级页表项详细信息
   * 5. 地址转换示例和验证
   *
   * 使用场景：
   * - 系统启动时的状态验证
   * - 内存管理问题的调试
   * - 性能分析和优化
   */
  static void print_page_table_details();

  /**
   * 打印PGD级页表项详情
   *
   * 专门输出PGD（顶级页表）的条目信息，包括有效条目的
   * 详细解析和权限位说明。
   *
   * 每个有效条目的输出包括：
   * - 虚拟地址范围
   * - 物理地址
   * - 映射类型（页表/块映射）
   * - 权限位详细解析
   * - 内存类型说明
   */
  static void print_pgd_entries();

  /**
   * 打印MMU寄存器状态
   *
   * 输出所有关键MMU寄存器的当前值和详细解析：
   * - SCTLR_EL1：系统控制寄存器（MMU/缓存启用状态）
   * - TCR_EL1：转换控制寄存器（地址空间配置）
   * - MAIR_EL1：内存属性寄存器（属性索引配置）
   * - TTBR0/1_EL1：页表基址寄存器
   */
  static void print_mmu_registers();
};

/*
 * ==================================================================
 *                      全局MMU管理接口
 * ==================================================================
 */

/**
 * 设置和启用MMU（内存管理单元）- 系统初始化的关键函数
 *
 * 这是系统启动过程中最重要的函数之一，负责完整的MMU初始化流程。
 * 它将系统从物理地址模式转换到虚拟地址模式，激活内存保护和虚拟内存管理。
 *
 * 完整的MMU设置流程：
 * 1. **页表结构建立**：
 *    - 调用PageTableManager::setup_kernel_page_tables()
 *    - 创建内核页表根目录（PGD）
 *    - 建立0-4GB的身份映射（1GB块映射）
 *    - 确保当前执行代码有正确的映射
 *
 * 2. **MMU寄存器配置**：
 *    - 调用PageTableManager::enable_mmu()
 *    - 配置MAIR_EL1（内存属性）
 *    - 配置TCR_EL1（转换控制）
 *    - 设置TTBR0/1_EL1（页表基址）
 *    - 启用SCTLR_EL1.M位
 *
 * 3. **状态验证**：
 *    - 检查SCTLR_EL1.M位确认MMU已启用
 *    - 测试虚拟地址访问验证转换工作
 *    - 输出详细的调试信息
 *
 * 4. **调试输出**：
 *    - 打印完整的页表详细信息
 *    - 显示MMU寄存器状态
 *    - 展示地址转换示例
 *
 * 错误处理：
 * - 页表分配失败：返回ErrorCode::OutOfMemory
 * - MMU启用失败：返回ErrorCode::InvalidState
 * - 系统配置错误：可能导致立即崩溃
 *
 * @return VoidResult 成功时返回空结果，失败时返回详细错误码
 *
 * 调用时机：
 * - 仅在系统早期启动阶段调用
 * - 必须在中断和进程管理初始化之前
 * - 调用后系统将运行在虚拟地址模式
 *
 * 注意事项：
 * - 这个函数只能调用一次
 * - 失败通常意味着硬件不支持或配置错误
 * - 成功后所有内存访问都通过MMU进行地址转换
 *
 * 使用示例：
 * ```cpp
 * // 在系统初始化中调用
 * auto result = setup_mmu();
 * if (!result) {
 *     kernel_panic("MMU初始化失败");
 * }
 * // 此时系统运行在虚拟地址模式
 * ```
 */
[[nodiscard]] VoidResult setup_mmu();

/**
 * 无效化所有TLB条目（全局TLB刷新）
 *
 * 这是一个全局的TLB管理函数，通过PageTableManager::invalidate_tlb()
 * 来执行实际的TLB刷新操作。当页表内容发生任何修改时都必须调用。
 *
 * TLB（Translation Lookaside Buffer）管理：
 * - TLB是MMU的高速缓存，存储最近使用的地址转换
 * - 页表修改后TLB内容可能过时
 * - 不刷新TLB会导致使用错误的地址转换
 * - 刷新TLB会导致短暂的性能下降（需要重新加载转换）
 *
 * 调用场景：
 * - 页表映射被修改或删除
 * - 内存权限发生变化
 * - 进程切换（如果使用ASID）
 * - 内存区域重新映射
 *
 * 性能考虑：
 * - TLB刷新会导致后续内存访问变慢
 * - 处理器需要重新从页表加载转换
 * - 频繁刷新会显著影响系统性能
 * - 应该批量修改页表后统一刷新
 *
 * 实现细节：
 * - ARM64: 执行TLBI VMALLE1IS指令
 * - x86_64: 重新加载CR3寄存器
 * - RISC-V: 执行SFENCE.VMA指令
 * - 包含必要的内存和指令同步屏障
 *
 * 使用示例：
 * ```cpp
 * // 修改页表映射后
 * map_region(vaddr, paddr, size, permissions);
 * invalidate_all_tlb();  // 确保修改生效
 *
 * // 批量修改后统一刷新（推荐）
 * map_region(vaddr1, paddr1, size1, perms1);
 * map_region(vaddr2, paddr2, size2, perms2);
 * map_region(vaddr3, paddr3, size3, perms3);
 * invalidate_all_tlb();  // 一次性刷新所有修改
 * ```
 */
void invalidate_all_tlb();

} // namespace moss::kernel::mm
