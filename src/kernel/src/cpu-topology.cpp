// CPU Topology Implementation
//
// Reads CPU count from the device tree (FDT) at boot and exposes it
// as num_cpu_ids. Falls back to 1 if the DTB is absent or invalid.

module moss.kernel;

import moss.std;
import moss.types;
import moss.fdt;
import moss.logging;

namespace moss::kernel::cpu_topology {

namespace log = moss::kernel::logging;

u32 num_cpu_ids = 1;

void early_cpu_topology_init() noexcept {
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

void initialize_cpu_topology() noexcept { log::klog::info("CPU topology initialized: {} CPUs", num_cpu_ids); }

} // namespace moss::kernel::cpu_topology
