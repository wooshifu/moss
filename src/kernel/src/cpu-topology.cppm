// CPU Topology Module
//
// Runtime CPU count detection from device tree.
// The compile-time MAX_CPUS (types.cppm) sizes static arrays;
// num_cpu_ids holds the actual count discovered at boot.

export module moss.kernel:cpu_topology;

import moss.std;
import moss.types;

export namespace moss::kernel::cpu_topology {

// Runtime-detected CPU count (set during early boot, default 1)
extern u32 num_cpu_ids;

// Early init: read FDT and set num_cpu_ids
void early_cpu_topology_init() noexcept;

// Second-phase init (currently logs only; placeholder for future per-CPU setup)
void initialize_cpu_topology() noexcept;

// Safe accessor
[[nodiscard]] inline u32 get_cpu_count() noexcept { return num_cpu_ids; }

} // namespace moss::kernel::cpu_topology

// Re-export at kernel namespace level
export namespace moss::kernel {
using cpu_topology::get_cpu_count;
using cpu_topology::num_cpu_ids;
} // namespace moss::kernel
