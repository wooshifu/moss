// CPU Topology Module
//
// Runtime CPU count detection from device tree.
// The compile-time MAX_CPUS (types.cppm) sizes static arrays;
// num_cpu_ids holds the actual count discovered at boot.

export module moss.kernel:cpu_topology;

import moss.std;
import moss.types;
import moss.fdt;
import moss.logging;

namespace moss::kernel::cpu_topology {

namespace log = moss::kernel::logging;

// Runtime-detected CPU count (set during early boot, default 1)
export extern u32 num_cpu_ids;
u32 num_cpu_ids = 1;

// Safe accessor
export [[nodiscard]] inline u32 get_cpu_count() noexcept { return num_cpu_ids; }

// Early init: read FDT and set num_cpu_ids
export void early_cpu_topology_init() noexcept {
  const auto &plat = moss::fdt::get_platform_info();
  if (plat.dtb_valid && plat.cpu_count > 0) {
    num_cpu_ids = plat.cpu_count;
    if (num_cpu_ids > moss::kernel::MAX_CPUS) {
      log::klog::warn("DTB reports {} CPUs, capping to {}", num_cpu_ids, moss::kernel::MAX_CPUS);
      num_cpu_ids = static_cast<u32>(moss::kernel::MAX_CPUS);
    }
  }
  log::klog::info("CPU topology: {} CPUs detected", num_cpu_ids);
}

} // namespace moss::kernel::cpu_topology

// Re-export at kernel namespace level
export namespace moss::kernel {
using cpu_topology::get_cpu_count;
using cpu_topology::num_cpu_ids;
} // namespace moss::kernel
