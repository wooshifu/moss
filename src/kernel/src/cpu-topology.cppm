// CPU Topology Management Module
//
// Provides unified CPU count management with hybrid static/dynamic allocation.
// Replaces the dual MAX_CPUS definitions in moss.types (8) and moss.arch (16)
// with a modern scalable approach inspired by Linux kernel's nr_cpu_ids.
//
// Design Philosophy:
// - Small systems (≤32 CPUs): Static arrays for optimal performance
// - Large systems (>32 CPUs): Dynamic allocation for unlimited scalability
// - Transparent switching based on runtime detection
// - Zero performance overhead for typical embedded/desktop scenarios

export module moss.kernel:cpu_topology;

import moss.std;
import moss.types;

export namespace moss::kernel::cpu_topology {

// ============================================================================
// Constants and Configuration
// ============================================================================

// Threshold for static vs dynamic allocation.
// Derived from moss::kernel::MAX_CPUS (single source of truth in types.cppm).
// Systems with ≤ STATIC_MAX_CPUS use zero-overhead static arrays.
inline constexpr u32 STATIC_MAX_CPUS = moss::kernel::MAX_CPUS;

// Theoretical maximum for very large systems (used for bitmasks)
// This is a safety limit, not a performance-critical constant
inline constexpr u32 ABSOLUTE_MAX_CPUS = 1024;

// ============================================================================
// Backward Compatibility and Unified Constants
// ============================================================================

// Convenience alias within cpu_topology namespace
inline constexpr usize MAX_CPUS_USIZE = static_cast<usize>(STATIC_MAX_CPUS);

// TODO: Gradually migrate all code to use nr_cpu_ids instead of MAX_CPUS

// ============================================================================
// Runtime CPU State
// ============================================================================

// Runtime-detected actual CPU count (set during early boot)
// This replaces the compile-time MAX_CPUS with dynamic detection
extern u32 nr_cpu_ids;

// Whether the system is using dynamic CPU data structures
// true = dynamic allocation, false = static arrays
extern bool use_dynamic_cpu_data;

// CPU state tracking - which CPUs are available at different levels
extern u32 nr_possible_cpus; // CPUs that could potentially be plugged in
extern u32 nr_present_cpus;  // CPUs that are physically present
extern u32 nr_online_cpus;   // CPUs available for scheduling
extern u32 nr_active_cpus;   // CPUs available for load balancing

// ============================================================================
// CPU Topology Information
// ============================================================================

// Per-CPU topology information (NUMA, cluster, core hierarchy)
struct CpuTopologyInfo {
  u32 cpu_id;       // Logical CPU ID (0 to nr_cpu_ids-1)
  u32 physical_id;  // Physical processor ID
  u32 core_id;      // Core within processor
  u32 cluster_id;   // Cluster (for big.LITTLE or similar)
  u32 numa_node;    // NUMA node ID
  bool is_big_core; // true for performance cores, false for efficiency cores

  // Hardware identifiers
  u64 mpidr;   // ARM64 MPIDR_EL1 value
  u32 apic_id; // x86_64 APIC ID
  u64 hart_id; // RISC-V hart ID
};

// Topology information for all CPUs (allocated based on nr_cpu_ids)
extern CpuTopologyInfo *cpu_topology_info;

// ============================================================================
// Initialization and Detection Functions
// ============================================================================

// Early CPU topology initialization (called during boot)
// Must be called before any per-CPU data structures are created
void early_cpu_topology_init() noexcept;

// Complete CPU topology setup after device tree parsing
void initialize_cpu_topology() noexcept;

// Clean up CPU topology data (for testing/shutdown)
void cleanup_cpu_topology() noexcept;

// ============================================================================
// Runtime CPU Query Functions
// ============================================================================

// Get the actual number of CPUs detected at runtime
[[nodiscard]] inline u32 get_cpu_count() noexcept { return nr_cpu_ids; }

// Get counts for different CPU states
[[nodiscard]] inline u32 get_possible_cpu_count() noexcept { return nr_possible_cpus; }
[[nodiscard]] inline u32 get_present_cpu_count() noexcept { return nr_present_cpus; }
[[nodiscard]] inline u32 get_online_cpu_count() noexcept { return nr_online_cpus; }
[[nodiscard]] inline u32 get_active_cpu_count() noexcept { return nr_active_cpus; }

// Check if a CPU ID is valid/available
[[nodiscard]] inline bool is_valid_cpu(u32 cpu_id) noexcept { return cpu_id < nr_cpu_ids; }

[[nodiscard]] bool is_cpu_possible(u32 cpu_id) noexcept;
[[nodiscard]] bool is_cpu_present(u32 cpu_id) noexcept;
[[nodiscard]] bool is_cpu_online(u32 cpu_id) noexcept;
[[nodiscard]] bool is_cpu_active(u32 cpu_id) noexcept;

// Get topology information for a specific CPU
[[nodiscard]] const CpuTopologyInfo *get_cpu_topology(u32 cpu_id) noexcept;

// Check if using dynamic allocation mode
[[nodiscard]] inline bool using_dynamic_cpu_data() noexcept { return use_dynamic_cpu_data; }

// ============================================================================
// CPU State Management Functions
// ============================================================================

// Mark a CPU as online/offline (for CPU hotplug support)
void set_cpu_online(u32 cpu_id, bool online) noexcept;
void set_cpu_active(u32 cpu_id, bool active) noexcept;

// CPU hotplug event handlers (future feature)
void notify_cpu_starting(u32 cpu_id) noexcept;
void notify_cpu_dying(u32 cpu_id) noexcept;

// ============================================================================
// Legacy Compatibility Functions
// ============================================================================

// Compatibility function for code still using MAX_CPUS
// TODO: Remove once all callers are migrated to nr_cpu_ids
[[nodiscard]] inline u32 get_max_cpus() noexcept { return nr_cpu_ids; }

} // namespace moss::kernel::cpu_topology

// ============================================================================
// Global alias for convenient access
// ============================================================================

export namespace moss::kernel {
// Re-export key functions at kernel namespace level for convenience
using cpu_topology::get_cpu_count;
using cpu_topology::is_cpu_online;
using cpu_topology::is_valid_cpu;
using cpu_topology::nr_cpu_ids;

// MAX_CPUS lives in types.cppm as the single source of truth
using cpu_topology::MAX_CPUS_USIZE;
} // namespace moss::kernel
