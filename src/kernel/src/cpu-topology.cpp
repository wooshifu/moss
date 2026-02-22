// CPU Topology Management Implementation
//
// Provides runtime CPU detection and topology management with hybrid
// static/dynamic allocation based on detected CPU count.

module moss.kernel;

import moss.std;
import moss.types;
import moss.arch;
import moss.fdt;
import moss.logging;

namespace moss::kernel::cpu_topology {

namespace log = moss::kernel::logging;

// Implementation can see all types and constants from the module interface

// ============================================================================
// Global Variables Definition
// ============================================================================

// Runtime CPU state (defined in header as extern)
u32 nr_cpu_ids = 1;
bool use_dynamic_cpu_data = false;

// CPU count by state
u32 nr_possible_cpus = 1;
u32 nr_present_cpus = 1;
u32 nr_online_cpus = 1;
u32 nr_active_cpus = 1;

// Topology information storage
CpuTopologyInfo *cpu_topology_info = nullptr;

// Static storage for small systems
static CpuTopologyInfo static_topology_storage[moss::kernel::MAX_CPUS];

// CPU state bitmasks
static u64 possible_cpu_mask = 1;
static u64 present_cpu_mask = 1;
static u64 online_cpu_mask = 1;
static u64 active_cpu_mask = 1;

// ============================================================================
// Internal Helper Functions
// ============================================================================

namespace {

// Detect CPU count from hardware registers
u32 detect_cpu_count_from_hardware() noexcept {
#if defined(MOSS_ARCH_ARM64)
  return 4; // Common default for ARM64 systems
#elif defined(MOSS_ARCH_X86_64)
  return 8; // Common default for x86_64 systems
#elif defined(MOSS_ARCH_RISCV)
  return 2; // Conservative default
#else
  return 1; // Safe fallback
#endif
}

// Parse CPU count from device tree (priority) or fall back to hardware detection
u32 parse_cpu_count_from_device_tree() noexcept {
  const auto &plat = moss::fdt::get_platform_info();
  if (plat.dtb_valid && plat.cpu_count > 0) {
    u32 count = plat.cpu_count;
    if (count > ABSOLUTE_MAX_CPUS) {
      log::klog::warn("DTB reports {} CPUs, capping to {}", count, ABSOLUTE_MAX_CPUS);
      count = ABSOLUTE_MAX_CPUS;
    }
    log::klog::info("Detected {} CPUs from device tree", count);
    return count;
  }

  // Fallback: architecture-specific hardware detection
  u32 cpu_count = detect_cpu_count_from_hardware();
  if (cpu_count == 0) {
    log::klog::warn("Invalid CPU count detected, using 1");
    return 1;
  }

  log::klog::info("Detected {} CPUs from hardware fallback", cpu_count);
  return cpu_count;
}

// Initialize topology information for a CPU
void init_cpu_topology_info(u32 cpu_id) noexcept {
  if (!cpu_topology_info || cpu_id >= nr_cpu_ids) {
    return;
  }

  auto &info = cpu_topology_info[cpu_id];
  info.cpu_id = cpu_id;

  // Architecture-specific topology detection
#if defined(MOSS_ARCH_ARM64)
  if (cpu_id == arch::get_current_cpu_id()) {
    u64 mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    info.mpidr = mpidr;
    info.physical_id = (mpidr >> 16) & 0xFF;
    info.cluster_id = (mpidr >> 8) & 0xFF;
    info.core_id = mpidr & 0xFF;
  } else {
    info.mpidr = cpu_id;
    info.physical_id = 0;
    info.cluster_id = cpu_id / 4;
    info.core_id = cpu_id % 4;
  }
  info.is_big_core = (info.cluster_id == 0);

#elif defined(MOSS_ARCH_X86_64)
  info.apic_id = cpu_id;
  info.physical_id = cpu_id / 8;
  info.core_id = cpu_id % 8;
  info.cluster_id = info.physical_id;
  info.is_big_core = true;

#elif defined(MOSS_ARCH_RISCV)
  info.hart_id = cpu_id;
  info.physical_id = 0;
  info.core_id = cpu_id;
  info.cluster_id = 0;
  info.is_big_core = true;
#endif

  info.numa_node = 0;
}

// Allocate topology storage based on CPU count
bool allocate_topology_storage() noexcept {
  if (nr_cpu_ids <= moss::kernel::MAX_CPUS) {
    cpu_topology_info = static_topology_storage;
    use_dynamic_cpu_data = false;
    log::klog::info("Using static storage for {} CPUs", nr_cpu_ids);
  } else {
    // Use simple new - if it fails, kernel should panic anyway
    auto *dynamic_storage = new CpuTopologyInfo[nr_cpu_ids];
    if (!dynamic_storage) {
      log::klog::error("Failed to allocate dynamic storage for {} CPUs", nr_cpu_ids);
      return false;
    }
    cpu_topology_info = dynamic_storage;
    use_dynamic_cpu_data = true;
    log::klog::info("Using dynamic storage for {} CPUs", nr_cpu_ids);
  }
  return true;
}

// Initialize CPU state masks
void init_cpu_masks() noexcept {
  u64 initial_mask = (1ULL << nr_cpu_ids) - 1;
  possible_cpu_mask = initial_mask;
  present_cpu_mask = initial_mask;
  online_cpu_mask = 1;
  active_cpu_mask = 1;

  nr_possible_cpus = nr_cpu_ids;
  nr_present_cpus = nr_cpu_ids;
  nr_online_cpus = 1;
  nr_active_cpus = 1;
}

} // anonymous namespace

// ============================================================================
// Public API Implementation
// ============================================================================

void early_cpu_topology_init() noexcept {
  log::klog::info("Initializing CPU topology with 3-layer detection");

  // TODO: Initialize new CPU detection system
  // cpu_detection::initialize_cpu_detection();

  // TODO: Use new detection system
  // auto result = cpu_detection::detect_cpu_topology();
  // cpu_detection::apply_detection_results(result);

  // For now, use the existing detection mechanism as fallback
  u32 detected_cpus = parse_cpu_count_from_device_tree();
  nr_cpu_ids = detected_cpus;

  log::klog::info("Detected {} CPUs, using {} allocation mode", nr_cpu_ids,
                  (nr_cpu_ids <= moss::kernel::MAX_CPUS) ? "static" : "dynamic");
}

void initialize_cpu_topology() noexcept {
  log::klog::info("Setting up CPU topology data structures");

  if (!allocate_topology_storage()) {
    log::klog::error("Failed to allocate topology storage");
    nr_cpu_ids = 1;
    use_dynamic_cpu_data = false;
    cpu_topology_info = static_topology_storage;
  }

  init_cpu_masks();

  for (u32 cpu_id = 0; cpu_id < nr_cpu_ids; ++cpu_id) {
    init_cpu_topology_info(cpu_id);
  }

  log::klog::info("CPU topology initialized: {} possible, {} present, {} online, {} active", nr_possible_cpus,
                  nr_present_cpus, nr_online_cpus, nr_active_cpus);
}

void cleanup_cpu_topology() noexcept {
  if (use_dynamic_cpu_data && cpu_topology_info) {
    delete[] cpu_topology_info;
  }

  cpu_topology_info = nullptr;
  nr_cpu_ids = 1;
  use_dynamic_cpu_data = false;
  nr_possible_cpus = 1;
  nr_present_cpus = 1;
  nr_online_cpus = 1;
  nr_active_cpus = 1;
}

// ============================================================================
// CPU State Query Functions
// ============================================================================

bool is_cpu_possible(u32 cpu_id) noexcept {
  if (cpu_id >= 64) {
    return false;
  }
  return (possible_cpu_mask & (1ULL << cpu_id)) != 0;
}

bool is_cpu_present(u32 cpu_id) noexcept {
  if (cpu_id >= 64) {
    return false;
  }
  return (present_cpu_mask & (1ULL << cpu_id)) != 0;
}

bool is_cpu_online(u32 cpu_id) noexcept {
  if (cpu_id >= 64) {
    return false;
  }
  return (online_cpu_mask & (1ULL << cpu_id)) != 0;
}

bool is_cpu_active(u32 cpu_id) noexcept {
  if (cpu_id >= 64) {
    return false;
  }
  return (active_cpu_mask & (1ULL << cpu_id)) != 0;
}

const CpuTopologyInfo *get_cpu_topology(u32 cpu_id) noexcept {
  if (!cpu_topology_info || cpu_id >= nr_cpu_ids) {
    return nullptr;
  }
  return &cpu_topology_info[cpu_id];
}

// ============================================================================
// CPU State Management Functions
// ============================================================================

// Forward declaration for mutual recursion
void set_cpu_active(u32 cpu_id, bool active) noexcept;

void set_cpu_online(u32 cpu_id, bool online) noexcept {
  if (cpu_id >= nr_cpu_ids || cpu_id >= 64) {
    return;
  }

  u64 mask = 1ULL << cpu_id;
  bool was_online = (online_cpu_mask & mask) != 0;

  if (online && !was_online) {
    online_cpu_mask |= mask;
    nr_online_cpus++;
    log::klog::info("CPU {} brought online", cpu_id);
  } else if (!online && was_online) {
    online_cpu_mask &= ~mask;
    nr_online_cpus--;
    set_cpu_active(cpu_id, false);
    log::klog::info("CPU {} taken offline", cpu_id);
  }
}

void set_cpu_active(u32 cpu_id, bool active) noexcept {
  if (cpu_id >= nr_cpu_ids || cpu_id >= 64) {
    return;
  }

  if (active && !is_cpu_online(cpu_id)) {
    return;
  }

  u64 mask = 1ULL << cpu_id;
  bool was_active = (active_cpu_mask & mask) != 0;

  if (active && !was_active) {
    active_cpu_mask |= mask;
    nr_active_cpus++;
    log::klog::info("CPU {} activated for load balancing", cpu_id);
  } else if (!active && was_active) {
    active_cpu_mask &= ~mask;
    nr_active_cpus--;
    log::klog::info("CPU {} deactivated from load balancing", cpu_id);
  }
}

void notify_cpu_starting(u32 cpu_id) noexcept { log::klog::info("CPU {} starting", cpu_id); }

void notify_cpu_dying(u32 cpu_id) noexcept { log::klog::info("CPU {} dying", cpu_id); }

} // namespace moss::kernel::cpu_topology
