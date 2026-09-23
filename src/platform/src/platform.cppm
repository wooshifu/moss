// Runtime hardware resources. Firmware parsers populate these before driver use.
export module moss.platform;
import moss.std;
import moss.types;

export namespace moss::kernel::platform {
using moss::u16;
using moss::u32;
using moss::u64;
using moss::u8;
/// DTB 中最大支持的内存区域数
// 8 是固定启动存储的容量策略，并非 DTB 限制；超过容量会使内存发现失败。
// 当前仓库未记录选择 8 个区域的具体规模依据。
constexpr u32 MAX_MEMORY_REGIONS = 8;

/// 单个物理内存区域（来自 /memory 节点的 reg 属性）
struct MemoryRegion {
  PhysAddr base;
  u64 size;
};

/// UART 设备信息（来自 compatible = "arm,pl011" / "ns16550a" 节点）
enum class UartKind : u8 { None, Pl011, Ns16550 };

struct UartInfo {
  UartKind kind;
  u8 reg_shift;
  u8 reg_width;
  bool port_io;
  PhysAddr base_addr;
  u64 size;
  u32 clock_freq;
  u32 irq;
  bool valid;
};

/// 中断控制器信息（GIC / BCM2836 / PLIC）
struct InterruptControllerInfo {
  PhysAddr dist_base;   // GIC distributor、BCM2836 local controller 或 PLIC
  PhysAddr cpu_base;    // GICv2: GICC; BCM2836: cascaded ARMCTRL; GICv3: unused
  PhysAddr redist_base; // GICv3: GICR redistributor base (0 for GICv2/PLIC)
  u64 dist_size;
  u64 cpu_size;
  u64 redist_size; // GICv3: GICR region size
  u8 gic_version;  // Legacy controller tag: 0=unknown/PLIC, 1=BCM2836, 2=GICv2, 3=GICv3.
  bool valid;
};

/// 完整的平台硬件信息，从 DTB 解析填充
enum class CpuEnableMethod : u8 { None, Psci, SpinTable, Sbi };
struct CpuInfo {
  u64 hardware_id;
  u64 release_address;
  CpuEnableMethod enable_method;
};

struct PlatformInfo {
  // CPU/PLIC 的 16 个槽位须与 BOOT_MAX_CPUS 和各架构启动数组保持一致。
  CpuInfo cpus[16];
  bool psci_valid;
  bool psci_smc;
  // IA-PC ACPI FADT RESET_REG is a platform-supplied system-reset port.
  u16 acpi_reset_port;
  u8 acpi_reset_value;
  bool acpi_reset_valid;
  u32 timer_interrupt;
  u32 plic_contexts[16];
  // ISA 硬件定义 IRQ0..15；GSI 映射和 ACPI 极性/触发标志必须同步索引。
  u32 isa_gsi[16];
  u16 isa_flags[16];
  // DTB 有效性
  bool dtb_valid;        // DTB 存在且解析成功
  bool memory_map_valid; // Firmware RAM discovery, independent of the boot protocol.

  // CPU 拓扑（来自 /cpus 节点）
  u32 cpu_count;
  u32 boot_cpu_id;
  u64 timebase_frequency;

  // RISC-V 64 MMU type from DTB (3 = Sv39, 4 = Sv48, 5 = Sv57; 0 = unknown)
  u8 mmu_levels;

  // 物理内存区域（来自 /memory 节点）
  MemoryRegion memory_regions[MAX_MEMORY_REGIONS];
  u32 memory_region_count;
  PhysAddr total_memory_start; // 第一个区域的基地址
  u64 total_memory_size;       // 所有区域大小之和
  // 32 是 DTB、自描述保留区及 /reserved-memory 共用的固定容量；溢出时
  // 必须拒绝内存布局，避免把未记录的保留 RAM 交给分配器。规模依据未记录。
  MemoryRegion reserved_regions[32];
  u32 reserved_region_count;

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

inline PlatformInfo hardware{};
// Logical CPU zero is always the boot CPU; firmware IDs need not be dense.
[[nodiscard]] inline bool order_cpus(u64 boot_id) noexcept {
  if (!hardware.cpu_count || hardware.cpu_count > 16) {
    return false;
  }
  u32 boot = 16;
  for (u32 i = 0; i < hardware.cpu_count; ++i) {
    if (hardware.cpus[i].hardware_id == boot_id) {
      boot = i;
    }
    for (u32 j = 0; j < i; ++j) {
      if (hardware.cpus[i].hardware_id == hardware.cpus[j].hardware_id) {
        return false;
      }
    }
  }
  if (boot == 16) {
    return false;
  }
  auto first = hardware.cpus[0];
  hardware.cpus[0] = hardware.cpus[boot];
  hardware.cpus[boot] = first;
  auto context = hardware.plic_contexts[0];
  hardware.plic_contexts[0] = hardware.plic_contexts[boot];
  hardware.plic_contexts[boot] = context;
  return true;
}
[[nodiscard]] inline u32 logical_cpu(u64 hardware_id) noexcept {
  if (!hardware.cpu_count) {
    return 0; // Before firmware discovery only the boot CPU can enter C++.
  }
  for (u32 i = 0; i < hardware.cpu_count; ++i) {
    if (hardware.cpus[i].hardware_id == hardware_id) {
      return i;
    }
  }
  return 16; // Invalid ID; callers must not index per-CPU storage.
}
[[nodiscard]] inline VirtAddr uart_base() noexcept { return hardware.uart.base_addr; }
[[nodiscard]] inline VirtAddr intc_dist_base() noexcept { return hardware.intc.dist_base; }
[[nodiscard]] inline VirtAddr intc_cpu_base() noexcept { return hardware.intc.cpu_base; }
[[nodiscard]] inline VirtAddr intc_redist_base() noexcept { return hardware.intc.redist_base; }
[[nodiscard]] inline PhysAddr ram_base() noexcept { return hardware.total_memory_start; }
[[nodiscard]] inline u64 ram_size() noexcept { return hardware.total_memory_size; }
[[nodiscard]] inline u32 timer_irq() noexcept { return hardware.timer_interrupt; }
[[nodiscard]] inline u64 timer_frequency() noexcept { return hardware.timebase_frequency; }

// Virtual address layout is an architecture policy, independent of physical RAM.
// These bases select upper canonical address regions: ARM64's TTBR1 region,
// the sign-extended top 2 GiB on RV64, and the upper 48-bit half on x64.
// They describe virtual layout, not a firmware-selected physical load address.
[[nodiscard]] constexpr VirtAddr kernel_virt_base() noexcept {
#if defined(MOSS_ARCH_ARM64)
  return 0xFFFF000000000000ULL;
#elif defined(MOSS_ARCH_RISCV64)
  return 0xFFFFFFFF80000000ULL;
#else
  return 0xFFFF800000000000ULL;
#endif
}
} // namespace moss::kernel::platform
