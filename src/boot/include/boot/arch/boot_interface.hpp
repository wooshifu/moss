/*
 * 多架构统一启动接口定义
 * 为不同架构提供标准化的启动流程接口
 */

#pragma once

#include "core/types.hpp"
#include "core/result.hpp"

namespace moss::boot {

/**
 * 启动上下文结构
 * 包含启动过程中需要在各阶段间传递的关键信息
 */
struct BootContext {
    void* device_tree_ptr;      // 设备树指针（ARM64）或启动信息结构指针
    PhysAddr memory_start;      // 可用物理内存起始地址
    moss::kernel::usize memory_size;          // 可用物理内存大小
    u32 cpu_id;                 // 当前CPU ID
    u32 total_cpus;             // 系统总CPU核心数
    PhysAddr kernel_phys_base;  // 内核物理地址基址
    VirtAddr kernel_virt_base;  // 内核虚拟地址基址
};

/**
 * 架构特定启动接口抽象基类
 * 所有架构必须实现这些标准化的启动阶段
 */
class ArchBootInterface {
public:
    /**
     * 阶段1：硬件层早期初始化
     * - ARM64: 异常级别转换、基础系统寄存器设置
     * - x86_64: GDT/IDT设置、保护模式确认
     * - RISC-V: 机器模式初始化、基础CSR设置
     */
    static moss::kernel::VoidResult hardware_early_init(BootContext& ctx) noexcept;

    /**
     * 阶段2：内存管理系统设置
     * - ARM64: 页表创建、MMU启用
     * - x86_64: 页目录设置、分页启用
     * - RISC-V: 页表设置、虚拟内存启用
     */
    static moss::kernel::VoidResult setup_memory_management(BootContext& ctx) noexcept;

    /**
     * 阶段3：中断和异常系统设置
     * - ARM64: 异常向量表、GIC初始化
     * - x86_64: IDT完善、PIC/APIC配置
     * - RISC-V: 中断控制器、异常处理设置
     */
    static moss::kernel::VoidResult setup_interrupts_and_exceptions(BootContext& ctx) noexcept;

    /**
     * 阶段4：多核启动支持设置
     * - ARM64: SMP启动协议、CPU停泊区设置
     * - x86_64: AP唤醒、SMP初始化
     * - RISC-V: 多核协调、hart管理
     */
    static moss::kernel::VoidResult setup_smp_support(BootContext& ctx) noexcept;

    /**
     * 架构特定的最终清理和准备工作
     * 在转交给架构无关代码前的最后准备
     */
    static moss::kernel::VoidResult finalize_arch_init(BootContext& ctx) noexcept;

protected:
    // 通用辅助函数，各架构可以重载

    /**
     * 检测可用物理内存范围
     */
    static moss::kernel::VoidResult detect_memory_layout(BootContext& ctx) noexcept;

    /**
     * 获取当前CPU ID
     */
    static u32 get_current_cpu_id() noexcept;

    /**
     * 内核panic处理
     */
    [[noreturn]] static void arch_panic(const char* message) noexcept;
};

/**
 * 启动阶段枚举
 * 用于调试和状态跟踪
 */
enum class BootStage : u32 {
    PreInit = 0,
    HardwareInit = 1,
    MemoryManagement = 2,
    InterruptsExceptions = 3,
    SmpSupport = 4,
    ArchFinalize = 5,
    SystemInit = 6,
    Complete = 7
};

/**
 * 启动状态结构
 * 用于跟踪启动进度和调试
 */
struct BootStatus {
    BootStage current_stage;
    u32 completed_stages_mask;
    u64 stage_timestamps[8];  // 每个阶段的时间戳
    moss::kernel::ErrorCode last_error;
};

// 全局启动状态（每个架构实现中定义）
extern BootStatus g_boot_status;

/**
 * 获取当前启动状态
 */
inline const BootStatus& get_boot_status() noexcept {
    return g_boot_status;
}

/**
 * 更新启动阶段状态
 */
void update_boot_stage(BootStage stage, moss::kernel::ErrorCode error = moss::kernel::ErrorCode::Success) noexcept;

/**
 * Linux风格SMP延迟激活函数
 * 在调度器初始化完成后激活所有停放的从CPU
 */
void activate_secondary_cpus() noexcept;

/**
 * 等待所有CPU完成激活
 * @param timeout_ms 最大等待时间（毫秒）
 * @return 成功激活的CPU数量
 */
u32 wait_for_all_cpus_active(u32 timeout_ms = 5000) noexcept;

} // namespace moss::boot
