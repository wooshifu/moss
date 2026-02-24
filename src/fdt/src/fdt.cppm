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

using moss::u32;
using moss::u64;
using moss::u8;
using moss::kernel::PhysAddr;
using moss::kernel::usize;
using moss::kernel::VirtAddr;

/// Read a big-endian 64-bit value from a potentially unaligned DTB pointer.
/// DTB property data is only guaranteed 4-byte aligned, so a direct
/// *(fdt64_t*)ptr can fault with strict alignment (QEMU 10 / SCTLR.A=1).
/// We read two aligned 32-bit halves and combine them.
inline u64 read_fdt64_unaligned(const void *ptr) noexcept {
  const auto *p = static_cast<const u8 *>(ptr);
  // DTB is big-endian: first 4 bytes = high word, next 4 = low word
  auto hi = static_cast<u64>(fdt32_to_cpu(*reinterpret_cast<const fdt32_t *>(p)));
  auto lo = static_cast<u64>(fdt32_to_cpu(*reinterpret_cast<const fdt32_t *>(p + 4)));
  return (hi << 32) | lo;
}

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
  PhysAddr dist_base;   // GIC distributor 或 PLIC 基地址
  PhysAddr cpu_base;    // GICv2: GICC CPU interface; GICv3: unused (0)
  PhysAddr redist_base; // GICv3: GICR redistributor base (0 for GICv2/PLIC)
  u64 dist_size;
  u64 cpu_size;
  u64 redist_size; // GICv3: GICR region size
  u8 gic_version;  // 0=unknown/PLIC, 2=GICv2, 3=GICv3/v4
  bool valid;
};

/// 完整的平台硬件信息，从 DTB 解析填充
struct PlatformInfo {
  // DTB 有效性
  bool dtb_valid; // DTB 存在且解析成功

  // CPU 拓扑（来自 /cpus 节点）
  u32 cpu_count;
  u32 boot_cpu_id;

  // RISC-V MMU type from DTB (3 = Sv39, 4 = Sv48, 5 = Sv57; 0 = unknown)
  u8 mmu_levels;

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

  // initramfs 地址（来自 /chosen 节点）
  PhysAddr initrd_start; // linux,initrd-start
  PhysAddr initrd_end;   // linux,initrd-end
};

/// 全局平台信息实例（早期启动阶段填充）
extern PlatformInfo g_platform_info;

/// 解析 DTB blob 并填充 PlatformInfo
/// @param dtb_ptr 指向内存中 DTB blob 的指针
/// @return 成功返回 true，失败返回 false
bool parse_dtb(const void *dtb_ptr) noexcept;

/// 获取全局平台信息（只读引用）
inline auto get_platform_info() noexcept -> const PlatformInfo & { return g_platform_info; }

} // namespace moss::fdt

// ============================================================================
// Implementation (merged from fdt_parser.cpp)
// ============================================================================

namespace moss::fdt {

// 全局平台信息实例
PlatformInfo g_platform_info = {};

// ============================================================================
// 内部辅助函数
// ============================================================================

/// 读取节点的 #address-cells 和 #size-cells 属性
static void read_cells(const void *fdt, int node, u32 &addr_cells, u32 &size_cells) noexcept {
  int len = 0;
  const void *prop = fdt_getprop(fdt, node, "#address-cells", &len);
  if (prop && len >= 4) {
    addr_cells = fdt32_to_cpu(*static_cast<const fdt32_t *>(prop));
  }

  prop = fdt_getprop(fdt, node, "#size-cells", &len);
  if (prop && len >= 4) {
    size_cells = fdt32_to_cpu(*static_cast<const fdt32_t *>(prop));
  }
}

/// 从 cells 数组中读取一个多 cell 值（支持 1-cell 和 2-cell 地址）
static auto read_cells_value(const u8 *&ptr, u32 num_cells) noexcept -> u64 {
  u64 value = 0;
  for (u32 i = 0; i < num_cells; i++) {
    value = (value << 32) | fdt32_to_cpu(*reinterpret_cast<const fdt32_t *>(ptr));
    ptr += 4;
  }
  return value;
}

/// 检查 compatible 属性中是否包含指定的字符串
/// DTB 的 compatible 是 null-terminated 字符串列表
static auto compatible_match(const void *fdt, int node, const char *match) noexcept -> bool {
  int len = 0;
  const char *compat = static_cast<const char *>(fdt_getprop(fdt, node, "compatible", &len));
  if (!compat || len <= 0) {
    return false;
  }

  const char *p = compat;
  const char *end = compat + len;
  while (p < end) {
    if (fdt_stringlist_contains(compat, len, match)) {
      return true;
    }
    // 遍历到下一个 null-terminated 字符串
    while (p < end && *p) {
      p++;
    }
    p++; // 跳过 null 终结符
  }
  return false;
}

// ============================================================================
// 各节点的解析函数
// ============================================================================

/// 解析 /cpus 节点，获取 CPU 数量和 MMU 类型
static void parse_cpus(const void *fdt) noexcept {
  int cpus_node = fdt_path_offset(fdt, "/cpus");
  if (cpus_node < 0) {
    g_platform_info.cpu_count = 1; // 默认单核
    return;
  }

  u32 count = 0;
  bool mmu_detected = false;
  int node = 0;
  fdt_for_each_subnode(node, fdt, cpus_node) {
    // 检查节点类型是否为 "cpu"
    int len = 0;
    const char *device_type = static_cast<const char *>(fdt_getprop(fdt, node, "device_type", &len));
    if (device_type && len > 0) {
      if (strncmp(device_type, "cpu", 3) == 0) {
        count++;

        // Read mmu-type from first CPU node (e.g. "riscv,sv39", "riscv,sv48")
        if (!mmu_detected) {
          int mmu_len = 0;
          const char *mmu_type = static_cast<const char *>(fdt_getprop(fdt, node, "mmu-type", &mmu_len));
          if (mmu_type && mmu_len > 0) {
            // Parse "riscv,svNN" — look for the digit after "sv"
            // Valid values: "riscv,sv39" → 3, "riscv,sv48" → 4, "riscv,sv57" → 5
            for (int i = 0; i + 1 < mmu_len; i++) {
              if (mmu_type[i] == 's' && mmu_type[i + 1] == 'v') {
                // Parse the number: sv39→39, sv48→48, sv57→57
                u32 bits = 0;
                for (int j = i + 2; j < mmu_len && mmu_type[j] >= '0' && mmu_type[j] <= '9'; j++) {
                  bits = bits * 10 + static_cast<u32>(mmu_type[j] - '0');
                }
                if (bits == 39) {
                  g_platform_info.mmu_levels = 3;
                } else if (bits == 48) {
                  g_platform_info.mmu_levels = 4;
                } else if (bits == 57) {
                  g_platform_info.mmu_levels = 5;
                }
                mmu_detected = true;
                break;
              }
            }
          }
        }
      }
    }
  }

  g_platform_info.cpu_count = (count > 0) ? count : 1;
}

/// 解析 /memory 节点，获取物理内存布局
static void parse_memory(const void *fdt) noexcept {
  // 读取根节点的 address/size cells
  int root = fdt_path_offset(fdt, "/");
  if (root < 0) {
    return;
  }

  u32 addr_cells = 2;
  u32 size_cells = 2;
  read_cells(fdt, root, addr_cells, size_cells);

  // 查找 /memory 或 /memory@xxx 节点
  int mem_node = -1;
  int node = 0;
  fdt_for_each_subnode(node, fdt, root) {
    int len = 0;
    const char *device_type = static_cast<const char *>(fdt_getprop(fdt, node, "device_type", &len));
    if (device_type && len > 0) {
      if (strncmp(device_type, "memory", 6) == 0) {
        mem_node = node;
        break;
      }
    }
  }

  if (mem_node < 0) {
    return;
  }

  // 读取 reg 属性
  int len = 0;
  const void *reg = fdt_getprop(fdt, mem_node, "reg", &len);
  if (!reg || len <= 0) {
    return;
  }

  u32 entry_size = (addr_cells + size_cells) * 4;
  u32 region_count = static_cast<u32>(len) / entry_size;
  if (region_count > MAX_MEMORY_REGIONS) {
    region_count = MAX_MEMORY_REGIONS;
  }

  const u8 *ptr = static_cast<const u8 *>(reg);
  u64 total_size = 0;

  for (u32 i = 0; i < region_count; i++) {
    u64 base = read_cells_value(ptr, addr_cells);
    u64 size = read_cells_value(ptr, size_cells);
    g_platform_info.memory_regions[i] = {.base = static_cast<PhysAddr>(base), .size = size};
    total_size += size;
  }

  g_platform_info.memory_region_count = region_count;
  if (region_count > 0) {
    g_platform_info.total_memory_start = g_platform_info.memory_regions[0].base;
  }
  g_platform_info.total_memory_size = total_size;
}

/// 解析 UART 设备节点
/// 支持 ARM PL011 (arm,pl011) 和 NS16550 (ns16550a) 兼容设备
static void parse_uart(const void *fdt) noexcept {
  // 遍历所有节点，查找 UART compatible
  int uart_node = -1;
  int offset = -1;

  while (true) {
    offset = fdt_next_node(fdt, offset, nullptr);
    if (offset < 0) {
      break;
    }

    if (compatible_match(fdt, offset, "arm,pl011") || compatible_match(fdt, offset, "ns16550a") ||
        compatible_match(fdt, offset, "ns16550")) {
      uart_node = offset;
      break;
    }
  }

  if (uart_node < 0) {
    return;
  }

  // 获取父节点的 cells 信息
  int parent = fdt_parent_offset(fdt, uart_node);
  u32 addr_cells = 2;
  u32 size_cells = 2;
  if (parent >= 0) {
    read_cells(fdt, parent, addr_cells, size_cells);
  } else {
    int root = fdt_path_offset(fdt, "/");
    if (root >= 0) {
      read_cells(fdt, root, addr_cells, size_cells);
    }
  }

  // 读取 reg 属性
  int len = 0;
  const void *reg = fdt_getprop(fdt, uart_node, "reg", &len);
  if (!reg || len <= 0) {
    return;
  }

  const u8 *ptr = static_cast<const u8 *>(reg);
  u64 base = read_cells_value(ptr, addr_cells);
  u64 size = read_cells_value(ptr, size_cells);

  g_platform_info.uart.base_addr = static_cast<PhysAddr>(base);
  g_platform_info.uart.size = size;

  // 尝试读取 clock-frequency 属性
  const void *clk = fdt_getprop(fdt, uart_node, "clock-frequency", &len);
  if (clk && len >= 4) {
    g_platform_info.uart.clock_freq = fdt32_to_cpu(*static_cast<const fdt32_t *>(clk));
  }

  g_platform_info.uart.valid = true;
}

/// 解析中断控制器节点（ARM GIC / RISC-V PLIC）
static void parse_intc(const void *fdt) noexcept {
  int intc_node = -1;
  int offset = -1;
  u8 detected_version = 0; // 0=unknown/PLIC, 2=GICv2, 3=GICv3/v4

  // 查找已知的中断控制器 compatible 字符串
  // Check GICv3 first — a GICv3 node must not be misidentified as v2
  while (true) {
    offset = fdt_next_node(fdt, offset, nullptr);
    if (offset < 0) {
      break;
    }

    if (compatible_match(fdt, offset, "arm,gic-v3")) {
      intc_node = offset;
      detected_version = 3;
      break;
    }
    if (compatible_match(fdt, offset, "arm,cortex-a15-gic") || compatible_match(fdt, offset, "arm,gic-400")) {
      intc_node = offset;
      detected_version = 2;
      break;
    }
    if (compatible_match(fdt, offset, "riscv,plic0") || compatible_match(fdt, offset, "sifive,plic-1.0.0")) {
      intc_node = offset;
      detected_version = 0;
      break;
    }
  }

  if (intc_node < 0) {
    return;
  }

  // 获取父节点的 cells 信息
  int parent = fdt_parent_offset(fdt, intc_node);
  u32 addr_cells = 2;
  u32 size_cells = 2;
  if (parent >= 0) {
    read_cells(fdt, parent, addr_cells, size_cells);
  } else {
    int root = fdt_path_offset(fdt, "/");
    if (root >= 0) {
      read_cells(fdt, root, addr_cells, size_cells);
    }
  }

  // 读取 reg 属性
  int len = 0;
  const void *reg = fdt_getprop(fdt, intc_node, "reg", &len);
  if (!reg || len <= 0) {
    return;
  }

  u32 entry_size = (addr_cells + size_cells) * 4;
  const u8 *ptr = static_cast<const u8 *>(reg);

  // 第一个 reg 条目 = distributor (same for GICv2, GICv3, and PLIC)
  u64 dist_base = read_cells_value(ptr, addr_cells);
  u64 dist_size = read_cells_value(ptr, size_cells);
  g_platform_info.intc.dist_base = static_cast<PhysAddr>(dist_base);
  g_platform_info.intc.dist_size = dist_size;

  // 第二个 reg 条目: interpretation depends on GIC version
  //   GICv2: GICC (CPU interface) — MMIO mapped
  //   GICv3: GICR (redistributor) — per-CPU register frames
  if (static_cast<u32>(len) >= entry_size * 2) {
    u64 second_base = read_cells_value(ptr, addr_cells);
    u64 second_size = read_cells_value(ptr, size_cells);

    if (detected_version == 3) {
      g_platform_info.intc.redist_base = static_cast<PhysAddr>(second_base);
      g_platform_info.intc.redist_size = second_size;
      g_platform_info.intc.cpu_base = 0;
      g_platform_info.intc.cpu_size = 0;
    } else {
      g_platform_info.intc.cpu_base = static_cast<PhysAddr>(second_base);
      g_platform_info.intc.cpu_size = second_size;
      g_platform_info.intc.redist_base = 0;
      g_platform_info.intc.redist_size = 0;
    }
  }

  g_platform_info.intc.gic_version = detected_version;
  g_platform_info.intc.valid = true;
}

/// 解析 /chosen 节点（启动参数、stdout 路径）
static void parse_chosen(const void *fdt) noexcept {
  int node = fdt_path_offset(fdt, "/chosen");
  if (node < 0) {
    return;
  }

  // bootargs 和 stdout-path 的指针直接指向 DTB blob 内部
  // DTB blob 必须在整个内核生命周期内保持有效
  g_platform_info.bootargs = static_cast<const char *>(fdt_getprop(fdt, node, "bootargs", nullptr));
  g_platform_info.stdout_path = static_cast<const char *>(fdt_getprop(fdt, node, "stdout-path", nullptr));

  // Parse initramfs address range (QEMU -initrd writes these to DTB)
  int len = 0;
  const void *prop = fdt_getprop(fdt, node, "linux,initrd-start", &len);
  if (prop && len >= 4) {
    // DTB stores as big-endian — can be 4 or 8 bytes depending on #address-cells
    if (len == 8) {
      g_platform_info.initrd_start = static_cast<PhysAddr>(read_fdt64_unaligned(prop));
    } else {
      g_platform_info.initrd_start = static_cast<PhysAddr>(fdt32_to_cpu(*static_cast<const fdt32_t *>(prop)));
    }
  }
  prop = fdt_getprop(fdt, node, "linux,initrd-end", &len);
  if (prop && len >= 4) {
    if (len == 8) {
      g_platform_info.initrd_end = static_cast<PhysAddr>(read_fdt64_unaligned(prop));
    } else {
      g_platform_info.initrd_end = static_cast<PhysAddr>(fdt32_to_cpu(*static_cast<const fdt32_t *>(prop)));
    }
  }
}

// ============================================================================
// 公开 API
// ============================================================================

bool parse_dtb(const void *dtb_ptr) noexcept {
  if (!dtb_ptr) {
    return false;
  }

  // 验证 DTB 魔数和头部
  int err = fdt_check_header(dtb_ptr);
  if (err != 0) {
    return false;
  }

  // 清空现有信息
  g_platform_info = {};
  g_platform_info.dtb_valid = true;

  // 按顺序解析各节点
  parse_cpus(dtb_ptr);
  parse_memory(dtb_ptr);
  parse_uart(dtb_ptr);
  parse_intc(dtb_ptr);
  parse_chosen(dtb_ptr);

  return true;
}

} // namespace moss::fdt
