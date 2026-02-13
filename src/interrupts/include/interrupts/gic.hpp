#pragma once

// ARM64 Generic Interrupt Controller (GIC) 驱动
// 支持 GICv2/GICv3 中断控制器

#include "core/arch/arch_abstraction.hpp"
#include "containers/containers.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

namespace moss::kernel::interrupts {

// GIC版本
enum class GicVersion : u8 { GICv2 = 2, GICv3 = 3, Unknown = 0 };

// 中断类型
enum class InterruptType : u8 {
  SGI = 0, // Software Generated Interrupt (0-15)
  PPI = 1, // Private Peripheral Interrupt (16-31)
  SPI = 2, // Shared Peripheral Interrupt (32+)
  LPI = 3  // Locality-specific Peripheral Interrupt (GICv3)
};

// 中断触发类型
enum class TriggerType : u8 {
  EdgeRising = 0,
  LevelHigh = 1,
  EdgeFalling = 2,
  LevelLow = 3
};

// 中断优先级
using InterruptPriority = u8; // 0-255, 0是最高优先级

// 中断处理函数类型
using InterruptHandler = void (*)(InterruptId irq, void *context);

// 中断描述符
struct InterruptDescriptor {
  InterruptId irq;            // 中断号
  InterruptType type;         // 中断类型
  TriggerType trigger;        // 触发类型
  InterruptPriority priority; // 优先级
  u32 target_cpu_mask;        // 目标CPU掩码
  InterruptHandler handler;   // 处理函数
  void *context;              // 上下文指针
  u64 count;                  // 中断计数
  bool enabled;               // 是否启用
  const char *name;           // 中断名称

  InterruptDescriptor() noexcept
      : irq(0), type(InterruptType::SPI), trigger(TriggerType::LevelHigh),
        priority(128), target_cpu_mask(1), handler(nullptr), context(nullptr),
        count(0), enabled(false), name(nullptr) {}

  InterruptDescriptor(InterruptId id, InterruptHandler h, void *ctx,
                      const char *n) noexcept
      : irq(id), type(determine_type(id)), trigger(TriggerType::LevelHigh),
        priority(128), target_cpu_mask(1), handler(h), context(ctx), count(0),
        enabled(false), name(n) {}

private:
  static constexpr InterruptType determine_type(InterruptId id) noexcept {
    if (id < 16)
      return InterruptType::SGI;
    if (id < 32)
      return InterruptType::PPI;
    return InterruptType::SPI;
  }
};

// GIC寄存器偏移 (GICv2)
namespace GicRegs {
// Distributor寄存器
static constexpr u32 GICD_CTLR = 0x000;       // 分发器控制寄存器
static constexpr u32 GICD_TYPER = 0x004;      // 中断控制器类型寄存器
static constexpr u32 GICD_IIDR = 0x008;       // 分发器实现标识寄存器
static constexpr u32 GICD_IGROUPR = 0x080;    // 中断组寄存器
static constexpr u32 GICD_ISENABLER = 0x100;  // 中断使能设置寄存器
static constexpr u32 GICD_ICENABLER = 0x180;  // 中断使能清除寄存器
static constexpr u32 GICD_ISPENDR = 0x200;    // 中断挂起设置寄存器
static constexpr u32 GICD_ICPENDR = 0x280;    // 中断挂起清除寄存器
static constexpr u32 GICD_ISACTIVER = 0x300;  // 中断活跃设置寄存器
static constexpr u32 GICD_ICACTIVER = 0x380;  // 中断活跃清除寄存器
static constexpr u32 GICD_IPRIORITYR = 0x400; // 中断优先级寄存器
static constexpr u32 GICD_ITARGETSR = 0x800;  // 中断目标寄存器
static constexpr u32 GICD_ICFGR = 0xC00;      // 中断配置寄存器
static constexpr u32 GICD_SGIR = 0xF00;       // 软件生成中断寄存器

// CPU Interface寄存器
static constexpr u32 GICC_CTLR = 0x000;  // CPU接口控制寄存器
static constexpr u32 GICC_PMR = 0x004;   // 中断优先级掩码寄存器
static constexpr u32 GICC_BPR = 0x008;   // 二进制点寄存器
static constexpr u32 GICC_IAR = 0x00C;   // 中断确认寄存器
static constexpr u32 GICC_EOIR = 0x010;  // 中断结束寄存器
static constexpr u32 GICC_RPR = 0x014;   // 运行优先级寄存器
static constexpr u32 GICC_HPPIR = 0x018; // 最高优先级挂起中断寄存器
} // namespace GicRegs

// GIC驱动主类
class GenericInterruptController {
private:
  // GIC配置
  GicVersion version_;
  VirtAddr distributor_base_;   // 分发器基址
  VirtAddr cpu_interface_base_; // CPU接口基址
  u32 max_interrupts_;          // 最大中断数
  u32 max_cpus_;                // 最大CPU数

  // 中断描述符表
  containers::RcuHashMap<InterruptId, InterruptDescriptor *> interrupt_table_;

  // 每个CPU的中断统计
  containers::PerCpuData<u64> interrupt_counts_;

  // 全局中断统计
  containers::AtomicCounter<u64> total_interrupts_;
  containers::AtomicCounter<u64> spurious_interrupts_;

public:
  GenericInterruptController() noexcept
      : version_(GicVersion::Unknown), distributor_base_(0),
        cpu_interface_base_(0), max_interrupts_(0), max_cpus_(0),
        total_interrupts_(0), spurious_interrupts_(0) {}

  ~GenericInterruptController() noexcept { cleanup(); }

  // 禁用拷贝和移动
  NON_COPYABLE_NON_MOVABLE(GenericInterruptController)

  // GIC统计信息
  struct GicStats {
    u64 total_interrupts;
    u64 spurious_interrupts;
    u32 registered_interrupts;
    u32 enabled_interrupts;
  };

  // 初始化GIC
  [[nodiscard]] VoidResult initialize(VirtAddr dist_base,
                                      VirtAddr cpu_base) noexcept {
    distributor_base_ = dist_base;
    cpu_interface_base_ = cpu_base;

    // 检测GIC版本和配置
    auto detect_result = detect_gic_config();
    if (!detect_result) {
      return detect_result;
    }

    // 初始化分发器
    auto dist_result = initialize_distributor();
    if (!dist_result) {
      return dist_result;
    }

    // 初始化CPU接口
    auto cpu_result = initialize_cpu_interface();
    if (!cpu_result) {
      return cpu_result;
    }

    return VoidResult{};
  }

  // 注册中断处理函数
  [[nodiscard]] VoidResult
  register_interrupt(InterruptId irq, InterruptHandler handler, void *context,
                     const char *name = nullptr) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    if (handler == nullptr) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    // 检查是否已经注册
    if (interrupt_table_.find(irq) != nullptr) {
      return VoidResult{ErrorCode::AlreadyExists};
    }

    // 创建中断描述符
    InterruptDescriptor *desc =
        new InterruptDescriptor(irq, handler, context, name);
    if (desc == nullptr) {
      return VoidResult{ErrorCode::OutOfMemory};
    }

    // 注册中断
    interrupt_table_.insert_or_update(irq, desc);

    return VoidResult{};
  }

  // 取消注册中断处理函数
  [[nodiscard]] VoidResult unregister_interrupt(InterruptId irq) noexcept {
    auto desc_ptr = interrupt_table_.find(irq);
    if (desc_ptr == nullptr) {
      return VoidResult{ErrorCode::NotFound};
    }
    InterruptDescriptor *desc = *desc_ptr;

    // 先禁用中断
    (void)disable_interrupt(irq);

    // 移除注册
    interrupt_table_.remove(irq);
    delete desc;

    return VoidResult{};
  }

  // 启用中断
  [[nodiscard]] VoidResult enable_interrupt(InterruptId irq) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    // 设置使能位
    u32 reg_offset = GicRegs::GICD_ISENABLER + (irq / 32) * 4;
    u32 bit_pos = irq % 32;
    u32 reg_value = 1U << bit_pos;

    write_distributor_reg(reg_offset, reg_value);

    // 更新描述符状态
    auto desc_ptr = interrupt_table_.find(irq);
    if (desc_ptr != nullptr) {
      InterruptDescriptor *desc = *desc_ptr;
      desc->enabled = true;
    }

    return VoidResult{};
  }

  // 禁用中断
  [[nodiscard]] VoidResult disable_interrupt(InterruptId irq) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    // 清除使能位
    u32 reg_offset = GicRegs::GICD_ICENABLER + (irq / 32) * 4;
    u32 bit_pos = irq % 32;
    u32 reg_value = 1U << bit_pos;

    write_distributor_reg(reg_offset, reg_value);

    // 更新描述符状态
    auto desc_ptr = interrupt_table_.find(irq);
    if (desc_ptr != nullptr) {
      InterruptDescriptor *desc = *desc_ptr;
      desc->enabled = false;
    }

    return VoidResult{};
  }

  // 设置中断优先级
  [[nodiscard]] VoidResult
  set_interrupt_priority(InterruptId irq, InterruptPriority priority) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    // 计算寄存器偏移
    u32 reg_offset = GicRegs::GICD_IPRIORITYR + irq;
    write_distributor_reg(reg_offset, priority);

    // 更新描述符
    auto desc_ptr = interrupt_table_.find(irq);
    if (desc_ptr != nullptr) {
      InterruptDescriptor *desc = *desc_ptr;
      desc->priority = priority;
    }

    return VoidResult{};
  }

  // 设置中断目标CPU
  [[nodiscard]] VoidResult set_interrupt_target(InterruptId irq,
                                                u32 cpu_mask) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    // SPI中断才能设置目标CPU
    if (irq < 32) {
      return VoidResult{ErrorCode::NotSupported};
    }

    // 计算寄存器偏移
    u32 reg_offset = GicRegs::GICD_ITARGETSR + irq;
    write_distributor_reg(reg_offset, cpu_mask);

    // 更新描述符
    auto desc_ptr = interrupt_table_.find(irq);
    if (desc_ptr != nullptr) {
      InterruptDescriptor *desc = *desc_ptr;
      desc->target_cpu_mask = cpu_mask;
    }

    return VoidResult{};
  }

  // 发送软件中断
  [[nodiscard]] VoidResult send_sgi(InterruptId sgi,
                                    u32 target_cpu_mask) noexcept {
    if (sgi >= 16) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    // 构造SGIR寄存器值
    u32 sgir_value = sgi | (target_cpu_mask << 16);
    write_distributor_reg(GicRegs::GICD_SGIR, sgir_value);

    return VoidResult{};
  }

  // 主中断处理入口
  void handle_interrupt() noexcept {
    u32 cpu = get_current_cpu_id();

    // 读取中断确认寄存器
    u32 iar = read_cpu_interface_reg(GicRegs::GICC_IAR);
    InterruptId irq = iar & 0x3FF; // 中断号在低10位

    // 检查是否为伪中断
    if (irq >= 1020) {
      (void)spurious_interrupts_.fetch_add(1, containers::MemoryOrder::Relaxed);
      return;
    }

    // 更新统计
    (void)total_interrupts_.fetch_add(1, containers::MemoryOrder::Relaxed);
    interrupt_counts_.get_cpu(cpu)++;

    // 查找中断处理函数
    auto desc_ptr = interrupt_table_.find(irq);
    if (desc_ptr != nullptr) {
      InterruptDescriptor *desc = *desc_ptr;
      if (desc->handler != nullptr) {
        // 调用中断处理函数
        desc->handler(irq, desc->context);

        // 更新中断计数
        desc->count++;
      }
    }

    // 写入中断结束寄存器
    write_cpu_interface_reg(GicRegs::GICC_EOIR, iar);
  }

  // 获取中断统计信息
  [[nodiscard]] GicStats get_statistics() const noexcept {
    u32 registered = 0;
    u32 enabled = 0;

    interrupt_table_.for_each([&registered, &enabled](const auto &entry) {
      registered++;
      if (entry.value->enabled) {
        enabled++;
      }
    });

    return {total_interrupts_.load(containers::MemoryOrder::Relaxed),
            spurious_interrupts_.load(containers::MemoryOrder::Relaxed),
            registered, enabled};
  }

  // 获取中断信息
  [[nodiscard]] const InterruptDescriptor *
  get_interrupt_info(InterruptId irq) const noexcept {
    auto desc_ptr = interrupt_table_.find(irq);
    return desc_ptr ? *desc_ptr : nullptr;
  }

  // 设置CPU接口优先级掩码
  void set_priority_mask(InterruptPriority mask) noexcept {
    write_cpu_interface_reg(GicRegs::GICC_PMR, mask);
  }

  // 获取当前CPU ID (多架构支持)
  [[nodiscard]] static u32 get_current_cpu_id() noexcept {
    return arch::get_current_cpu_id();
  }

private:
  // 检测GIC配置
  [[nodiscard]] VoidResult detect_gic_config() noexcept {
    // 读取类型寄存器
    u32 typer = read_distributor_reg(GicRegs::GICD_TYPER);

    max_interrupts_ = ((typer & 0x1F) + 1) * 32;
    max_cpus_ = ((typer >> 5) & 0x7) + 1;

    // 简化检测，假设GICv2
    version_ = GicVersion::GICv2;

    return VoidResult{};
  }

  // 初始化分发器
  [[nodiscard]] VoidResult initialize_distributor() noexcept {
    // 禁用分发器
    write_distributor_reg(GicRegs::GICD_CTLR, 0);

    // 禁用所有中断
    for (u32 i = 0; i < max_interrupts_; i += 32) {
      write_distributor_reg(GicRegs::GICD_ICENABLER + i / 8, 0xFFFFFFFF);
    }

    // 清除所有挂起中断
    for (u32 i = 0; i < max_interrupts_; i += 32) {
      write_distributor_reg(GicRegs::GICD_ICPENDR + i / 8, 0xFFFFFFFF);
    }

    // 设置默认优先级
    for (u32 i = 0; i < max_interrupts_; i += 4) {
      write_distributor_reg(GicRegs::GICD_IPRIORITYR + i, 0x80808080);
    }

    // 设置SPI默认目标为CPU0
    for (u32 i = 32; i < max_interrupts_; i += 4) {
      write_distributor_reg(GicRegs::GICD_ITARGETSR + i, 0x01010101);
    }

    // 启用分发器
    write_distributor_reg(GicRegs::GICD_CTLR, 1);

    return VoidResult{};
  }

  // 初始化CPU接口
  [[nodiscard]] VoidResult initialize_cpu_interface() noexcept {
    // 设置优先级掩码（允许所有中断）
    write_cpu_interface_reg(GicRegs::GICC_PMR, 0xFF);

    // 启用CPU接口
    write_cpu_interface_reg(GicRegs::GICC_CTLR, 1);

    return VoidResult{};
  }

  // 读写寄存器辅助函数
  [[nodiscard]] u32 read_distributor_reg(u32 offset) const noexcept {
    return *reinterpret_cast<volatile u32 *>(distributor_base_ + offset);
  }

  void write_distributor_reg(u32 offset, u32 value) const noexcept {
    *reinterpret_cast<volatile u32 *>(distributor_base_ + offset) = value;
  }

  [[nodiscard]] u32 read_cpu_interface_reg(u32 offset) const noexcept {
    return *reinterpret_cast<volatile u32 *>(cpu_interface_base_ + offset);
  }

  void write_cpu_interface_reg(u32 offset, u32 value) const noexcept {
    *reinterpret_cast<volatile u32 *>(cpu_interface_base_ + offset) = value;
  }

  // 清理资源
  void cleanup() noexcept {
    interrupt_table_.for_each([](const auto &entry) { delete entry.value; });
    // RcuHashMap doesn't support assignment, entries are automatically cleaned
    // up
  }
};

// 全局GIC实例
extern GenericInterruptController *g_gic;

// 便利的C风格接口
extern "C" {
void interrupt_handler_entry() noexcept;
ErrorCode register_irq_handler(InterruptId irq, InterruptHandler handler,
                               void *context, const char *name);
ErrorCode enable_irq(InterruptId irq);
ErrorCode disable_irq(InterruptId irq);
}

} // namespace moss::kernel::interrupts
