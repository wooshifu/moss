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
import moss.platform;

// Wrap TU-local fdt32_to_cpu (macro/inline from libfdt) in a module-internal
// function. Must be non-static and outside export blocks to avoid both
// TU-locality and exposure diagnostics (-WTU-local-entity-exposure).
moss::u32 moss_fdt32_to_cpu(fdt32_t val) noexcept { return fdt32_to_cpu(val); }

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
u64 read_fdt64_unaligned(const void *ptr) noexcept {
  const auto *p = static_cast<const u8 *>(ptr);
  // DTB is big-endian: first 4 bytes = high word, next 4 = low word
  auto hi = static_cast<u64>(moss_fdt32_to_cpu(*reinterpret_cast<const fdt32_t *>(p)));
  auto lo = static_cast<u64>(moss_fdt32_to_cpu(*reinterpret_cast<const fdt32_t *>(p + 4)));
  return (hi << 32) | lo;
}

using moss::kernel::platform::CpuEnableMethod;
using moss::kernel::platform::InterruptControllerInfo;
using moss::kernel::platform::MAX_MEMORY_REGIONS;
using moss::kernel::platform::MemoryRegion;
using moss::kernel::platform::PlatformInfo;
using moss::kernel::platform::UartInfo;
using moss::kernel::platform::UartKind;
inline auto &g_platform_info = moss::kernel::platform::hardware;

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

// ============================================================================
// 内部辅助函数
// ============================================================================

/// 读取节点的 #address-cells 和 #size-cells 属性
// DTB 的一个 cell 固定为 4 字节大端 u32；缺省地址/长度 cell 数由调用方
// 按所在节点提供。这里只支持 1 或 2 个 cell，以容纳最多 64 位地址。
static void read_cells(const void *fdt, int node, u32 &addr_cells, u32 &size_cells) noexcept {
  int len = 0;
  const void *prop = fdt_getprop(fdt, node, "#address-cells", &len);
  if (prop) {
    addr_cells = len == 4 ? moss_fdt32_to_cpu(*static_cast<const fdt32_t *>(prop)) : 0;
  }

  prop = fdt_getprop(fdt, node, "#size-cells", &len);
  if (prop) {
    size_cells = len == 4 ? moss_fdt32_to_cpu(*static_cast<const fdt32_t *>(prop)) : 0;
  }
}

/// 从 cells 数组中读取一个多 cell 值（支持 1-cell 和 2-cell 地址）
static auto read_cells_value(const u8 *&ptr, u32 num_cells) noexcept -> u64 {
  u64 value = 0;
  for (u32 i = 0; i < num_cells; i++) {
    value = (value << 32) | moss_fdt32_to_cpu(*reinterpret_cast<const fdt32_t *>(ptr));
    ptr += 4;
  }
  return value;
}

/// 检查 compatible 属性中是否包含指定的字符串
/// DTB 的 compatible 是 null-terminated 字符串列表
static auto compatible_match(const void *fdt, int node, const char *match) noexcept -> bool {
  int len = 0;
  const char *compat = static_cast<const char *>(fdt_getprop(fdt, node, "compatible", &len));
  return compat && len > 0 && fdt_stringlist_contains(compat, len, match);
}

static bool enabled(const void *fdt, int node) noexcept {
  // 属性长度含结尾 NUL，所以 "ok"/"okay" 分别是 3/5 字节；缺少 status
  // 按 DT 约定视为可用，精确长度避免接受未终止或带额外后缀的数据。
  int length = 0;
  const char *status = static_cast<const char *>(fdt_getprop(fdt, node, "status", &length));
  return !status || (length == 3 && strncmp(status, "ok", 3) == 0) || (length == 5 && strncmp(status, "okay", 5) == 0);
}

static bool property_u32(const void *fdt, int node, const char *name, u32 &value) noexcept {
  int length = 0;
  const auto *data = static_cast<const u8 *>(fdt_getprop(fdt, node, name, &length));
  if (!data || length != 4) {
    return false;
  }
  value = static_cast<u32>(read_cells_value(data, 1));
  return true;
}

// Translate a bus-relative reg through every ancestor's ranges.
// An absent ranges is not an identity mapping; an empty ranges is.
static bool read_reg(const void *fdt, int node, u32 index, u64 &base, u64 &size) noexcept {
  int bus = fdt_parent_offset(fdt, node);
  if (bus < 0) {
    return false;
  }
  u32 ac = 2, sc = 1;
  read_cells(fdt, bus, ac, sc);
  int length = 0;
  const auto *data = static_cast<const u8 *>(fdt_getprop(fdt, node, "reg", &length));
  if (!data || ac < 1 || ac > 2 || sc < 1 || sc > 2 || length <= 0) {
    return false;
  }
  u32 stride = (ac + sc) * 4;
  if (static_cast<u32>(length) % stride || index >= static_cast<u32>(length) / stride) {
    return false;
  }
  data += static_cast<usize>(index) * stride;
  base = read_cells_value(data, ac);
  size = read_cells_value(data, sc);
  if (!size || base + size < base) {
    return false;
  }
  while (bus > 0) {
    int parent = fdt_parent_offset(fdt, bus);
    u32 pac = 2, psc = 1;
    read_cells(fdt, parent, pac, psc);
    const auto *ranges = static_cast<const u8 *>(fdt_getprop(fdt, bus, "ranges", &length));
    if (!ranges || ac < 1 || ac > 2 || pac < 1 || pac > 2 || sc < 1 || sc > 2) {
      return false;
    }
    if (length) {
      stride = (ac + pac + sc) * 4;
      if (length < 0 || static_cast<u32>(length) % stride) {
        return false;
      }
      bool found = false;
      for (u32 i = 0; i < static_cast<u32>(length) / stride; ++i) {
        u64 child = read_cells_value(ranges, ac);
        u64 translated = read_cells_value(ranges, pac);
        u64 extent = read_cells_value(ranges, sc);
        if (base >= child && base - child <= extent && size <= extent - (base - child)) {
          u64 offset = base - child;
          if (translated + offset < translated || translated + offset + size < translated + offset) {
            return false;
          }
          base = translated + offset;
          found = true;
          break;
        }
      }
      if (!found) {
        return false;
      }
    }
    bus = parent;
    ac = pac;
    sc = psc;
  }
  // 当前内核启动映射仅覆盖低 4 GiB 物理地址；这是启动实现约束，不是
  // DTB 的地址宽度限制。扩大映射时须同步调整发现和分配器的边界。
  return base < 0x100000000ULL && size <= 0x100000000ULL - base;
}

static u32 interrupt_number(const void *fdt, int node, u32 index = 0) noexcept {
  // 支持 PLIC 的单 cell 源编号及 GIC 的三 cell 类型/编号/标志布局。
  // GIC 类型 0=SPI、1=PPI，编号分别相对 INTID 32/16 起算，SGI 占 0..15。
  int parent_node = node;
  u32 phandle = 0;
  while (parent_node >= 0 && !property_u32(fdt, parent_node, "interrupt-parent", phandle)) {
    parent_node = fdt_parent_offset(fdt, parent_node);
  }
  int controller = fdt_node_offset_by_phandle(fdt, phandle);
  u32 cells = 0;
  if (controller < 0 || !property_u32(fdt, controller, "#interrupt-cells", cells) || (cells != 1 && cells != 3)) {
    return 0;
  }
  int length = 0;
  const auto *data = static_cast<const u8 *>(fdt_getprop(fdt, node, "interrupts", &length));
  if (!data || length <= 0 || static_cast<u32>(length) < (index + 1) * cells * 4) {
    return 0;
  }
  data += static_cast<usize>(index) * cells * 4;
  u32 first = static_cast<u32>(read_cells_value(data, 1));
  if (cells == 1) {
    return first;
  }
  u32 number = static_cast<u32>(read_cells_value(data, 1));
  return first <= 1 ? number + (first == 0 ? 32 : 16) : 0;
}

static void parse_timer_and_firmware(const void *fdt) noexcept {
  int node = -1;
  while ((node = fdt_next_node(fdt, node, nullptr)) >= 0) {
    if (!enabled(fdt, node)) {
      continue;
    }
    if (compatible_match(fdt, node, "arm,armv8-timer")) {
      // ARM timer binding 顺序为 secure/nonsecure physical、virtual、hyp；
      // 索引 2 对应 HAL 使用的 cntv_* 虚拟计时器，不能改成另一条 PPI。
      g_platform_info.timer_interrupt = interrupt_number(fdt, node, 2); // virtual timer PPI
    }
    if (compatible_match(fdt, node, "arm,psci-0.2") || compatible_match(fdt, node, "arm,psci-1.0")) {
      int length = 0;
      const char *method = static_cast<const char *>(fdt_getprop(fdt, node, "method", &length));
      g_platform_info.psci_smc = method && length == 4 && strncmp(method, "smc", 4) == 0;
      g_platform_info.psci_valid =
          g_platform_info.psci_smc || (method && length == 4 && strncmp(method, "hvc", 4) == 0);
    }
  }
#if defined(MOSS_ARCH_RISCV64)
  g_platform_info.timer_interrupt = 5; // architectural supervisor timer interrupt
#endif
}

// ============================================================================
// 各节点的解析函数
// ============================================================================

/// 解析 /cpus 节点，获取 CPU 数量和 MMU 类型
static void parse_cpus(const void *fdt) noexcept {
  int cpus_node = fdt_path_offset(fdt, "/cpus");
  if (cpus_node < 0) {
    return;
  }

  u32 count = 0;
  int timebase_len = 0;
  const auto *timebase = static_cast<const fdt32_t *>(fdt_getprop(fdt, cpus_node, "timebase-frequency", &timebase_len));
  if (timebase && timebase_len == 4) {
    g_platform_info.timebase_frequency = fdt32_to_cpu(*timebase);
  }
  bool mmu_detected = false;
  int node = 0;
  fdt_for_each_subnode(node, fdt, cpus_node) {
    // 检查节点类型是否为 "cpu"
    int len = 0;
    const char *device_type = static_cast<const char *>(fdt_getprop(fdt, node, "device_type", &len));
    if (device_type && len > 0) {
      if (strncmp(device_type, "cpu", 3) == 0) {
        if (!enabled(fdt, node)) {
          continue;
        }
        // 16 与 BOOT_MAX_CPUS/平台数组容量一致；17 作为超容量哨兵交给
        // 启动层拒绝拓扑，而不把多核机器悄悄截断为 16 核。
        if (count == 16) {
          g_platform_info.cpu_count = 17; // Fail at the boot contract boundary.
          return;
        }
        u32 address_cells = 1, size_cells = 0;
        read_cells(fdt, cpus_node, address_cells, size_cells);
        const auto *reg = static_cast<const u8 *>(fdt_getprop(fdt, node, "reg", &len));
        if (!reg || address_cells < 1 || address_cells > 2 || len != static_cast<int>(address_cells * 4)) {
          g_platform_info.cpu_count = 0;
          return;
        }
        auto &cpu = g_platform_info.cpus[count];
        cpu.hardware_id = read_cells_value(reg, address_cells);
        const char *method = static_cast<const char *>(fdt_getprop(fdt, node, "enable-method", &len));
        if (method && len == 5 && strncmp(method, "psci", 5) == 0) {
          cpu.enable_method = CpuEnableMethod::Psci;
        } else if (method && len == 11 && strncmp(method, "spin-table", 11) == 0) {
          cpu.enable_method = CpuEnableMethod::SpinTable;
          const auto *release = fdt_getprop(fdt, node, "cpu-release-addr", &len);
          if (release && len == 8) {
            cpu.release_address = read_fdt64_unaligned(release);
          }
        }
#if defined(MOSS_ARCH_RISCV64)
        cpu.enable_method = CpuEnableMethod::Sbi;
#endif
        count++;

        // Read mmu-type from first CPU node (e.g. "riscv64,sv39", "riscv64,sv48")
        if (!mmu_detected) {
          int mmu_len = 0;
          const char *mmu_type = static_cast<const char *>(fdt_getprop(fdt, node, "mmu-type", &mmu_len));
          if (mmu_type && mmu_len > 0) {
            // Parse "riscv64,svNN" — look for the digit after "sv"
            // Valid values: "riscv64,sv39" → 3, "riscv64,sv48" → 4, "riscv64,sv57" → 5
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

  g_platform_info.cpu_count = count;
}

/// 解析 /memory 节点，获取物理内存布局
static void parse_memory(const void *fdt) noexcept {
  int node = 0;
  auto &info = g_platform_info;
  fdt_for_each_subnode(node, fdt, 0) {
    int length = 0;
    const auto *type = static_cast<const char *>(fdt_getprop(fdt, node, "device_type", &length));
    if (!type || length != 7 || strncmp(type, "memory", 7) || !enabled(fdt, node)) {
      continue;
    }
    const void *reg = fdt_getprop(fdt, node, "reg", &length);
    u32 addresses = 2, sizes = 1;
    read_cells(fdt, 0, addresses, sizes);
    u32 cells = addresses + sizes;
    if (!reg || length <= 0 || addresses < 1 || addresses > 2 || sizes < 1 || sizes > 2 ||
        static_cast<u32>(length) % (cells * 4)) {
      info.memory_region_count = 0;
      return;
    }
    for (u32 index = 0; index < static_cast<u32>(length) / (cells * 4); ++index) {
      u64 base = 0, size = 0;
      if (info.memory_region_count == MAX_MEMORY_REGIONS || !read_reg(fdt, node, index, base, size) ||
          base >= 0x100000000ULL || size > 0x100000000ULL - base) {
        info.memory_region_count = 0;
        return;
      }
      for (u32 i = 0; i < info.memory_region_count; ++i) {
        const auto &other = info.memory_regions[i];
        if (base < other.base + other.size && other.base < base + size) {
          info.memory_region_count = 0;
          return;
        }
      }
      if (!info.memory_region_count || base < info.total_memory_start) {
        info.total_memory_start = base;
      }
      info.memory_regions[info.memory_region_count++] = {.base = base, .size = size};
      info.total_memory_size += size;
    }
  }
}

/// 解析 UART 设备节点
/// 支持 ARM PL011 (arm,pl011) 和 NS16550 (ns16550a) 兼容设备
static void parse_uart(const void *fdt) noexcept {
  int node = -1;
  if (g_platform_info.stdout_path) {
    const char *path = g_platform_info.stdout_path;
    int length = 0;
    while (path[length] && path[length] != ':') {
      ++length;
    }
    node = fdt_path_offset_namelen(fdt, path, length);
  } else {
    while ((node = fdt_next_node(fdt, node, nullptr)) >= 0) {
      if (enabled(fdt, node) && (compatible_match(fdt, node, "arm,pl011") || compatible_match(fdt, node, "ns16550a") ||
                                 compatible_match(fdt, node, "ns16550"))) {
        break;
      }
    }
  }
  if (node < 0 || !enabled(fdt, node)) {
    return;
  }
  auto &uart = g_platform_info.uart;
  if (compatible_match(fdt, node, "arm,pl011")) {
    uart.kind = UartKind::Pl011;
    uart.reg_width = 4;
  } else if (compatible_match(fdt, node, "ns16550a") || compatible_match(fdt, node, "ns16550")) {
    uart.kind = UartKind::Ns16550;
    u32 shift = 0, width = 1;
    (void)property_u32(fdt, node, "reg-shift", shift);
    (void)property_u32(fdt, node, "reg-io-width", width);
    // NS16550 HAL 仅实现 1/4 字节访问；shift<=3 将相邻寄存器间距限制
    // 为至多 8 字节。shift 上限的具体硬件兼容范围尚未记录。
    if (shift > 3 || (width != 1 && width != 4)) {
      return;
    }
    uart.reg_shift = static_cast<u8>(shift);
    uart.reg_width = static_cast<u8>(width);
  } else {
    return;
  }
  u64 base = 0, size = 0;
  // PL011 须覆盖 ICR(0x44)+4 字节；16550 有 8 个寄存器，按 reg-shift
  // 扩展其地址跨度。较小 reg 区域不能满足 HAL 实际访问的寄存器范围。
  if (!read_reg(fdt, node, 0, base, size) ||
      size < (uart.kind == UartKind::Pl011 ? 0x48ULL : (8ULL << uart.reg_shift))) {
    return;
  }
  uart.base_addr = base;
  uart.size = size;
  uart.irq = interrupt_number(fdt, node);
  (void)property_u32(fdt, node, "clock-frequency", uart.clock_freq);
  uart.valid = true;
}

/// 解析中断控制器节点（ARM GIC / RISC-V 64 PLIC）
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

    if (!enabled(fdt, offset)) {
      continue;
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
    if (compatible_match(fdt, offset, "riscv64,plic0") || compatible_match(fdt, offset, "sifive,plic-1.0.0")) {
      intc_node = offset;
      detected_version = 0;
      break;
    }
  }

  if (intc_node < 0) {
    return;
  }

  auto &intc = g_platform_info.intc;
  if (!read_reg(fdt, intc_node, 0, intc.dist_base, intc.dist_size)) {
    return;
  }
  if (detected_version == 3) {
    if (!read_reg(fdt, intc_node, 1, intc.redist_base, intc.redist_size)) {
      return;
    }
    // 当前 HAL 逐个扫描标准 RD+SGI frame（各 64 KiB，合计 0x20000）；
    // 非标准 stride 或多个 region 需要先扩展扫描逻辑，不能仅放宽验证。
    int length = 0;
    const auto *stride = fdt_getprop(fdt, intc_node, "redistributor-stride", &length);
    u32 regions = 1;
    (void)property_u32(fdt, intc_node, "#redistributor-regions", regions);
    if (regions != 1 || (stride && (length != 8 || read_fdt64_unaligned(stride) != 0x20000)) ||
        intc.redist_size < 0x20000) {
      return;
    }
  } else if (detected_version == 2) {
    if (!read_reg(fdt, intc_node, 1, intc.cpu_base, intc.cpu_size)) {
      return;
    }
  }
  if (detected_version == 0) {
    // PLIC contexts are the positions in interrupts-extended, not hart*2+1.
    int len = 0;
    const auto *interrupts = static_cast<const fdt32_t *>(fdt_getprop(fdt, intc_node, "interrupts-extended", &len));
    // 每个 context 是 phandle+单 cell 中断号，即 2*4=8 字节；中断号 9
    // 标识 supervisor external，须保留其在整张列表中的索引作为 context。
    if (!interrupts || len < 8 || len % 8 || !g_platform_info.cpu_count || g_platform_info.cpu_count > 16) {
      return;
    }
    for (u32 cpu = 0; cpu < g_platform_info.cpu_count; ++cpu) {
      // 缺失 context 用 ~0U 标记，后续窗口边界检查会拒绝该 CPU 的配置。
      g_platform_info.plic_contexts[cpu] = ~0U;
    }
    for (int index = 0; index < len / 8; ++index) {
      int controller = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(interrupts[static_cast<usize>(index) * 2]));
      u32 interrupt_cells = 0;
      if (controller < 0 || !property_u32(fdt, controller, "#interrupt-cells", interrupt_cells) ||
          interrupt_cells != 1) {
        return;
      }
      if (fdt32_to_cpu(interrupts[index * 2 + 1]) != 9) {
        continue; // Supervisor external interrupt.
      }
      int cpu_node = fdt_parent_offset(fdt, controller);
      int cpus_node = fdt_parent_offset(fdt, cpu_node);
      u32 cells = 1;
      (void)property_u32(fdt, cpus_node, "#address-cells", cells);
      int reg_len = 0;
      const auto *reg = static_cast<const u8 *>(fdt_getprop(fdt, cpu_node, "reg", &reg_len));
      if (!reg || cells < 1 || cells > 2 || reg_len != static_cast<int>(cells * 4)) {
        return;
      }
      u64 hart = read_cells_value(reg, cells);
      for (u32 cpu = 0; cpu < g_platform_info.cpu_count; ++cpu) {
        if (g_platform_info.cpus[cpu].hardware_id == hart) {
          g_platform_info.plic_contexts[cpu] = static_cast<u32>(index);
        }
      }
    }
    // 控制窗口从 PLIC+0x200000 起，每个 context 占 0x1000 字节；
    // 缺失 context 的哨兵也会越界，使控制器保持无效，避免配置错误 hart。
    for (u32 cpu = 0; cpu < g_platform_info.cpu_count; ++cpu) {
      u64 context_end = 0x200000ULL + (static_cast<u64>(g_platform_info.plic_contexts[cpu]) + 1) * 0x1000;
      if (context_end > intc.dist_size) {
        return;
      }
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
  auto string_property = [&](const char *name) -> const char * {
    int length = 0;
    const auto *value = static_cast<const char *>(fdt_getprop(fdt, node, name, &length));
    return value && length > 0 && value[length - 1] == '\0' ? value : nullptr;
  };
  g_platform_info.bootargs = string_property("bootargs");
  g_platform_info.stdout_path = string_property("stdout-path");

  // Parse initramfs address range (QEMU -initrd writes these to DTB)
  int len = 0;
  const void *prop = fdt_getprop(fdt, node, "linux,initrd-start", &len);
  if (prop && len >= 4) {
    // DTB stores as big-endian — can be 4 or 8 bytes depending on #address-cells
    if (len == 8) {
      g_platform_info.initrd_start = static_cast<PhysAddr>(read_fdt64_unaligned(prop));
    } else {
      g_platform_info.initrd_start = static_cast<PhysAddr>(moss_fdt32_to_cpu(*static_cast<const fdt32_t *>(prop)));
    }
  }
  prop = fdt_getprop(fdt, node, "linux,initrd-end", &len);
  if (prop && len >= 4) {
    if (len == 8) {
      g_platform_info.initrd_end = static_cast<PhysAddr>(read_fdt64_unaligned(prop));
    } else {
      g_platform_info.initrd_end = static_cast<PhysAddr>(moss_fdt32_to_cpu(*static_cast<const fdt32_t *>(prop)));
    }
  }
}

// ============================================================================
// 公开 API
// ============================================================================

bool parse_dtb(const void *dtb_ptr) noexcept {
  g_platform_info = {};
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

  // CPU 必须先于 PLIC context 解析；chosen 必须先于 UART，以遵循 stdout
  // 设备选择。bootargs/stdout 借用 DTB，后续保留其 RAM 防止分配器回收。
  parse_cpus(dtb_ptr);
  parse_memory(dtb_ptr);
  parse_chosen(dtb_ptr);
  parse_intc(dtb_ptr);
  parse_uart(dtb_ptr);
  parse_timer_and_firmware(dtb_ptr);

  g_platform_info.memory_map_valid = g_platform_info.memory_region_count > 0;
  auto reserve = [](PhysAddr base, u64 size) {
    auto &info = g_platform_info;
    if (!size) {
      return;
    }
    if (base + size < base || info.reserved_region_count == 32) {
      info.memory_map_valid = false;
      return;
    }
    info.reserved_regions[info.reserved_region_count++] = {.base = base, .size = size};
  };
  reserve(reinterpret_cast<PhysAddr>(dtb_ptr), fdt_totalsize(dtb_ptr));
  for (int i = 0; i < fdt_num_mem_rsv(dtb_ptr); ++i) {
    uint64_t base = 0;
    uint64_t size = 0;
    if (fdt_get_mem_rsv(dtb_ptr, i, &base, &size) == 0) {
      reserve(base, size);
    }
  }
  int reserved = fdt_path_offset(dtb_ptr, "/reserved-memory");
  if (reserved >= 0) {
    u32 address_cells = 2, size_cells = 2;
    read_cells(dtb_ptr, reserved, address_cells, size_cells);
    if (address_cells < 1 || address_cells > 2 || size_cells < 1 || size_cells > 2) {
      g_platform_info.memory_map_valid = false;
    } else {
      int node;
      fdt_for_each_subnode(node, dtb_ptr, reserved) {
        if (!enabled(dtb_ptr, node)) {
          continue;
        }
        int length = 0;
        const auto *data = static_cast<const u8 *>(fdt_getprop(dtb_ptr, node, "reg", &length));
        u32 stride = (address_cells + size_cells) * 4;
        if (!data || length <= 0 || static_cast<u32>(length) % stride != 0) {
          g_platform_info.memory_map_valid = false;
          continue;
        }
        for (u32 i = 0; i < static_cast<u32>(length) / stride; ++i) {
          u64 base = read_cells_value(data, address_cells);
          u64 size = read_cells_value(data, size_cells);
          reserve(base, size);
        }
      }
    }
  }
  if (g_platform_info.initrd_end < g_platform_info.initrd_start || fdt_num_mem_rsv(dtb_ptr) < 0) {
    g_platform_info.memory_map_valid = false;
  }

  return true;
}

} // namespace moss::fdt
