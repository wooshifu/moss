// MOSS FDT Parser - DTB 解析实现
// 使用 libfdt API 遍历 Device Tree Blob，提取硬件配置

module;

// libfdt C 头文件在全局模块片段中引入
extern "C" {
#include "libfdt.h"
}

module moss.fdt;

namespace moss::fdt {

// 全局平台信息实例
PlatformInfo g_platform_info = {};

// ============================================================================
// 内部辅助函数
// ============================================================================

/// 读取节点的 #address-cells 和 #size-cells 属性
static void read_cells(const void *fdt, int node, u32 &addr_cells,
                       u32 &size_cells) noexcept {
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
    value = (value << 32) |
            fdt32_to_cpu(*reinterpret_cast<const fdt32_t *>(ptr));
    ptr += 4;
  }
  return value;
}

/// 检查 compatible 属性中是否包含指定的字符串
/// DTB 的 compatible 是 null-terminated 字符串列表
static auto compatible_match(const void *fdt, int node,
                             const char *match) noexcept -> bool {
  int len = 0;
  const char *compat =
      static_cast<const char *>(fdt_getprop(fdt, node, "compatible", &len));
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

/// 解析 /cpus 节点，获取 CPU 数量
static void parse_cpus(const void *fdt) noexcept {
  int cpus_node = fdt_path_offset(fdt, "/cpus");
  if (cpus_node < 0) {
    g_platform_info.cpu_count = 1; // 默认单核
    return;
  }

  u32 count = 0;
  int node = 0;
  fdt_for_each_subnode(node, fdt, cpus_node) {
    // 检查节点类型是否为 "cpu"
    int len = 0;
    const char *device_type = static_cast<const char *>(
        fdt_getprop(fdt, node, "device_type", &len));
    if (device_type && len > 0) {
      if (strncmp(device_type, "cpu", 3) == 0) {
        count++;
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
    const char *device_type = static_cast<const char *>(
        fdt_getprop(fdt, node, "device_type", &len));
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
    g_platform_info.memory_regions[i] = {static_cast<PhysAddr>(base), size};
    total_size += size;
  }

  g_platform_info.memory_region_count = region_count;
  if (region_count > 0) {
    g_platform_info.total_memory_start =
        g_platform_info.memory_regions[0].base;
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

    if (compatible_match(fdt, offset, "arm,pl011") ||
        compatible_match(fdt, offset, "ns16550a") ||
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
    g_platform_info.uart.clock_freq =
        fdt32_to_cpu(*static_cast<const fdt32_t *>(clk));
  }

  g_platform_info.uart.valid = true;
}

/// 解析中断控制器节点（ARM GIC / RISC-V PLIC）
static void parse_intc(const void *fdt) noexcept {
  int intc_node = -1;
  int offset = -1;

  // 查找已知的中断控制器 compatible 字符串
  while (true) {
    offset = fdt_next_node(fdt, offset, nullptr);
    if (offset < 0) {
      break;
    }

    if (compatible_match(fdt, offset, "arm,cortex-a15-gic") ||
        compatible_match(fdt, offset, "arm,gic-400") ||
        compatible_match(fdt, offset, "arm,gic-v3") ||
        compatible_match(fdt, offset, "riscv,plic0") ||
        compatible_match(fdt, offset, "sifive,plic-1.0.0")) {
      intc_node = offset;
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

  // 读取 reg 属性（GIC 有两个区域: distributor + CPU interface）
  int len = 0;
  const void *reg = fdt_getprop(fdt, intc_node, "reg", &len);
  if (!reg || len <= 0) {
    return;
  }

  u32 entry_size = (addr_cells + size_cells) * 4;
  const u8 *ptr = static_cast<const u8 *>(reg);

  // 第一个 reg 条目 = distributor / PLIC 基地址
  u64 dist_base = read_cells_value(ptr, addr_cells);
  u64 dist_size = read_cells_value(ptr, size_cells);
  g_platform_info.intc.dist_base = static_cast<PhysAddr>(dist_base);
  g_platform_info.intc.dist_size = dist_size;

  // 第二个 reg 条目 = CPU interface（如果存在，仅 GICv2）
  if (static_cast<u32>(len) >= entry_size * 2) {
    u64 cpu_base = read_cells_value(ptr, addr_cells);
    u64 cpu_size = read_cells_value(ptr, size_cells);
    g_platform_info.intc.cpu_base = static_cast<PhysAddr>(cpu_base);
    g_platform_info.intc.cpu_size = cpu_size;
  }

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
  g_platform_info.bootargs = static_cast<const char *>(
      fdt_getprop(fdt, node, "bootargs", nullptr));
  g_platform_info.stdout_path = static_cast<const char *>(
      fdt_getprop(fdt, node, "stdout-path", nullptr));
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
