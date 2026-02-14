// MOSS FDT Module - Flattened Device Tree 解析
// 封装 libfdt 库，提供类型安全的 C++26 接口
// 从 DTB 中提取硬件拓扑信息，替代硬编码的平台参数

module;

// libfdt C 头文件在全局模块片段中引入
// libfdt include 目录在 CMake 中标记为 SYSTEM，抑制 vendored C 代码的所有警告
extern "C" {
#include "libfdt.h"
}

export module moss.fdt;

import moss.std;
import moss.types;

export namespace moss::fdt {

using moss::u8;
using moss::u32;
using moss::u64;
using moss::kernel::PhysAddr;
using moss::kernel::VirtAddr;
using moss::kernel::usize;

/// DTB 中最大支持的内存区域数
constexpr u32 MAX_MEMORY_REGIONS = 8;

/// 单个物理内存区域（来自 /memory 节点的 reg 属性）
struct MemoryRegion {
  PhysAddr base;
  u64 size;
};

/// UART 设备信息（来自 compatible = "arm,pl011" / "ns16550a" 节点）
struct UartInfo {
  PhysAddr base_addr;
  u64 size;
  u32 clock_freq;
  u32 irq;
  bool valid;
};

/// 中断控制器信息（GIC / PLIC）
struct InterruptControllerInfo {
  PhysAddr dist_base; // GIC distributor 或 PLIC 基地址
  PhysAddr cpu_base;  // GIC CPU interface（仅 ARM64）
  u64 dist_size;
  u64 cpu_size;
  bool valid;
};

/// 完整的平台硬件信息，从 DTB 解析填充
struct PlatformInfo {
  // DTB 有效性
  bool dtb_valid; // DTB 存在且解析成功

  // CPU 拓扑（来自 /cpus 节点）
  u32 cpu_count;
  u32 boot_cpu_id;

  // 物理内存区域（来自 /memory 节点）
  MemoryRegion memory_regions[MAX_MEMORY_REGIONS];
  u32 memory_region_count;
  PhysAddr total_memory_start; // 第一个区域的基地址
  u64 total_memory_size;       // 所有区域大小之和

  // 设备信息
  UartInfo uart;
  InterruptControllerInfo intc;

  // 启动参数（来自 /chosen 节点，指针指向 DTB blob 内部）
  const char *bootargs;
  const char *stdout_path;
};

/// 全局平台信息实例（早期启动阶段填充）
extern PlatformInfo g_platform_info;

/// 解析 DTB blob 并填充 PlatformInfo
/// @param dtb_ptr 指向内存中 DTB blob 的指针
/// @return 成功返回 true，失败返回 false
bool parse_dtb(const void *dtb_ptr) noexcept;

/// 获取全局平台信息（只读引用）
inline auto get_platform_info() noexcept -> const PlatformInfo & {
  return g_platform_info;
}

} // namespace moss::fdt
