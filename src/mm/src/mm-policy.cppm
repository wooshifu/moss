// MOSS Memory Management Module - Policy partition
// Contains: NumaPolicy (full), VmallocAllocator

export module moss.mm:policy;

import :core;

import moss.std;
import moss.types;
import moss.result;
import moss.containers;
import moss.arch;
import moss.hal.mmu;
import moss.logging;

// ========================================================================
// numa_policy.hpp
// ========================================================================
export namespace moss::kernel::mm {

// Re-export basic types used throughout
using moss::kernel::PhysAddr;
using moss::kernel::VirtAddr;
using moss::kernel::ErrorCode;
using moss::kernel::KernelResult;
using moss::kernel::VoidResult;

namespace log = moss::kernel::logging;

// PageAttr and PagePerms re-exported from MMU HAL (needed by VmallocRequest)
namespace PageAttr = ::moss::kernel::hal::mmu::PageAttr;
namespace PagePerms = ::moss::kernel::hal::mmu::PagePerms;

enum class NUMAError : u32 {
    InvalidNode = 1,
    NodeUnavailable = 2,
    AllocationFailed = 3,
    MigrationFailed = 4,
    TopologyInvalid = 5,
    PolicyViolation = 6,
    OutOfMemory = 7
};

template<typename T>
using NUMAResult = moss::kernel::Result<T, NUMAError>;
using NUMAVoidResult = moss::kernel::Result<void, NUMAError>;

enum class NUMAPolicy : u32 {
    DEFAULT = 0,
    BIND = 1,
    INTERLEAVE = 2,
    PREFERRED = 3,
    LOCAL = 4
};

enum class NodeState : u8 {
    OFFLINE = 0,
    ONLINE = 1,
    PARTIAL = 2,
    RESERVED = 3
};

struct NUMADistance {
    static constexpr u8 LOCAL_DISTANCE = 10;
    static constexpr u8 REMOTE_DISTANCE = 20;
    static constexpr u8 UNREACHABLE_DISTANCE = 255;

    u8 distance[MAX_NUMA_NODES][MAX_NUMA_NODES];

    NUMADistance() noexcept {
        for (u32 i = 0; i < MAX_NUMA_NODES; ++i) {
            for (u32 j = 0; j < MAX_NUMA_NODES; ++j) {
                if (i == j) {
                    distance[i][j] = LOCAL_DISTANCE;
                } else {
                    distance[i][j] = UNREACHABLE_DISTANCE;
                }
            }
        }
    }

    [[nodiscard]] u8 get_distance(numa_node_t from, numa_node_t to) const noexcept {
        if (from >= MAX_NUMA_NODES || to >= MAX_NUMA_NODES) { return UNREACHABLE_DISTANCE; }
        return distance[from][to];
    }

    void set_distance(numa_node_t from, numa_node_t to, u8 dist) noexcept {
        if (from < MAX_NUMA_NODES && to < MAX_NUMA_NODES) {
            distance[from][to] = dist;
            distance[to][from] = dist;
        }
    }
};

struct NUMANode {
    numa_node_t node_id;
    NodeState state;
    PhysAddr memory_start;
    usize memory_size;
    moss::kernel::containers::AtomicSize free_memory;
    moss::kernel::containers::AtomicSize allocated_memory;
    u64 cpu_mask;
    u32 cpu_count;
    moss::kernel::containers::AtomicU64 local_allocations;
    moss::kernel::containers::AtomicU64 remote_allocations;
    moss::kernel::containers::AtomicU64 migration_count;
    moss::kernel::containers::AtomicU32 memory_pressure;
    u64 last_balance_time;

    NUMANode() noexcept
        : node_id(NUMA_NO_NODE), state(NodeState::OFFLINE),
          memory_start(0), memory_size(0), free_memory(0), allocated_memory(0),
          cpu_mask(0), cpu_count(0),
          local_allocations(0), remote_allocations(0), migration_count(0),
          memory_pressure(0), last_balance_time(0) {}

    NUMANode(numa_node_t id, PhysAddr start, usize size) noexcept
        : node_id(id), state(NodeState::ONLINE),
          memory_start(start), memory_size(size), free_memory(size), allocated_memory(0),
          cpu_mask(0), cpu_count(0),
          local_allocations(0), remote_allocations(0), migration_count(0),
          memory_pressure(0), last_balance_time(0) {}
};

class NUMATopologyImpl;
class NUMAAllocatorImpl;
class NUMABalancerImpl;

class NUMATopology {
    friend class NUMATopologyImpl;
public:
    struct TopologyStats {
        u32 online_nodes;
        u32 total_nodes;
        usize total_memory;
        usize available_memory;
        double memory_balance_ratio;
        u32 active_cpus;
    };

protected:
    NUMANode nodes_[MAX_NUMA_NODES];
    NUMADistance distance_matrix_;
    moss::kernel::containers::AtomicU32 online_node_count_;
    u64 topology_version_;
    numa_node_t cpu_to_node_[moss::kernel::MAX_CPUS];

public:
    NUMATopology() noexcept;
    NUMAVoidResult discover_topology() noexcept;
    NUMAVoidResult add_node(numa_node_t node_id, PhysAddr start, usize size) noexcept;
    NUMAVoidResult remove_node(numa_node_t node_id) noexcept;
    NUMAVoidResult set_node_state(numa_node_t node_id, NodeState state) noexcept;
    [[nodiscard]] NodeState get_node_state(numa_node_t node_id) const noexcept;
    [[nodiscard]] bool is_node_online(numa_node_t node_id) const noexcept;
    NUMAVoidResult bind_cpu_to_node(u32 cpu_id, numa_node_t node_id) noexcept;
    [[nodiscard]] numa_node_t get_cpu_node(u32 cpu_id) const noexcept;
    [[nodiscard]] numa_node_t get_current_node() const noexcept;
    void set_node_distance(numa_node_t from, numa_node_t to, u8 distance) noexcept;
    [[nodiscard]] u8 get_node_distance(numa_node_t from, numa_node_t to) const noexcept;
    [[nodiscard]] numa_node_t find_nearest_node(numa_node_t from) const noexcept;
    [[nodiscard]] const NUMANode* get_node(numa_node_t node_id) const noexcept;
    [[nodiscard]] NUMANode* get_node_mutable(numa_node_t node_id) noexcept;
    [[nodiscard]] usize get_node_free_memory(numa_node_t node_id) const noexcept;
    [[nodiscard]] u32 get_online_node_count() const noexcept;
    [[nodiscard]] TopologyStats get_topology_stats() const noexcept;
    void update_topology_version() noexcept { topology_version_++; }
private:
    void initialize_default_topology() noexcept;
    void detect_cpu_topology() noexcept;
    void calculate_distances() noexcept;
};

class NUMAAllocator {
    friend class NUMAAllocatorImpl;
public:
    struct AllocationRequest {
        usize size;
        usize alignment;
        NUMAPolicy policy;
        numa_node_t preferred_node;
        u64 node_mask;
        u32 flags;
        enum Flags : u32 {
            NONE = 0,
            ZERO_MEMORY = (1 << 0),
            HIGH_PRIORITY = (1 << 1),
            NO_FALLBACK = (1 << 2),
            MIGRATE_ALLOWED = (1 << 3)
        };
        AllocationRequest(usize sz, NUMAPolicy pol = NUMAPolicy::DEFAULT,
                         numa_node_t node = NUMA_NO_NODE) noexcept
            : size(sz), alignment(PAGE_SIZE), policy(pol),
              preferred_node(node), node_mask(~0UL), flags(Flags::NONE) {}
    };

    struct AllocationStats {
        usize total_allocations;
        usize local_allocations;
        usize remote_allocations;
        usize fallback_allocations;
        usize failed_allocations;
        double locality_ratio;
        u64 average_allocation_time_us;
    };

protected:
    NUMATopology* topology_;
    AllocationStats stats_;
    moss::kernel::containers::AtomicU32 current_interleave_node_;

public:
    NUMAAllocator(NUMATopology* topo) noexcept : topology_(topo), stats_{}, current_interleave_node_(0) {}
    [[nodiscard]] NUMAResult<PhysAddr> allocate_pages(const AllocationRequest& request) noexcept;
    NUMAVoidResult free_pages(PhysAddr addr, usize size) noexcept;
    [[nodiscard]] NUMAResult<PhysAddr> allocate_default(const AllocationRequest& request) noexcept;
    [[nodiscard]] NUMAResult<PhysAddr> allocate_bind(const AllocationRequest& request) noexcept;
    [[nodiscard]] NUMAResult<PhysAddr> allocate_interleave(const AllocationRequest& request) noexcept;
    [[nodiscard]] NUMAResult<PhysAddr> allocate_preferred(const AllocationRequest& request) noexcept;
    [[nodiscard]] NUMAResult<PhysAddr> allocate_local(const AllocationRequest& request) noexcept;
    [[nodiscard]] numa_node_t select_allocation_node(const AllocationRequest& request) const noexcept;
    [[nodiscard]] numa_node_t select_fallback_node(numa_node_t failed_node, const AllocationRequest& request) const noexcept;
    [[nodiscard]] AllocationStats get_stats() const noexcept { return stats_; }
    void reset_stats() noexcept;
private:
    [[nodiscard]] NUMAResult<PhysAddr> allocate_from_node(numa_node_t node, const AllocationRequest& request) noexcept;
    void update_allocation_stats(numa_node_t allocated_node, numa_node_t preferred_node, bool fallback_used) noexcept;
};

class NUMABalancer {
    friend class NUMABalancerImpl;
public:
    struct BalancerConfig {
        u64 balance_interval_ms;
        u32 imbalance_threshold;
        u32 migration_rate_limit;
        double memory_threshold_ratio;
    };
    struct BalancerStats {
        usize migrations_performed;
        usize migrations_failed;
        u64 total_balance_time_us;
        u32 balance_cycles;
        double average_imbalance_before;
        double average_imbalance_after;
    };
protected:
    BalancerConfig config_;
    NUMATopology* topology_;
    NUMAAllocator* allocator_;
    BalancerStats stats_;
    moss::kernel::containers::AtomicBool balancer_active_;
    u64 last_balance_time_;
public:
    NUMABalancer(const BalancerConfig& config, NUMATopology* topo, NUMAAllocator* alloc) noexcept;
    NUMAVoidResult start_balancer() noexcept;
    void stop_balancer() noexcept;
    [[nodiscard]] bool is_balancer_active() const noexcept;
    NUMAVoidResult balance_memory_load() noexcept;
    NUMAVoidResult migrate_pages_between_nodes(numa_node_t from, numa_node_t to, usize page_count) noexcept;
    [[nodiscard]] double calculate_memory_imbalance() const noexcept;
    [[nodiscard]] bool should_balance() const noexcept;
    [[nodiscard]] NUMAResult<numa_node_t> find_source_node() const noexcept;
    [[nodiscard]] NUMAResult<numa_node_t> find_target_node(numa_node_t source) const noexcept;
    [[nodiscard]] BalancerStats get_stats() const noexcept { return stats_; }
    void reset_stats() noexcept;
private:
    NUMAVoidResult background_balance_thread() noexcept;
    [[nodiscard]] usize calculate_migration_count(numa_node_t from, numa_node_t to) const noexcept;
};

class NUMAPolicyManager {
public:
    struct NUMAConfig {
        NUMABalancer::BalancerConfig balancer_config;
        bool enable_auto_balancing;
        bool enable_migration;
        NUMAPolicy default_policy;
    };
    struct NUMASystemStats {
        NUMATopology::TopologyStats topology_stats;
        NUMAAllocator::AllocationStats allocator_stats;
        NUMABalancer::BalancerStats balancer_stats;
        double overall_numa_efficiency;
    };
protected:
    NUMAConfig config_;
    NUMATopology topology_;
    NUMAAllocator allocator_;
    NUMABalancer balancer_;
    moss::kernel::containers::AtomicBool system_initialized_;
public:
    static NUMAVoidResult initialize(const NUMAConfig& config) noexcept;
    NUMAVoidResult start_numa_system() noexcept;
    void stop_numa_system() noexcept;
    [[nodiscard]] bool is_numa_enabled() const noexcept;
    [[nodiscard]] NUMAResult<PhysAddr> numa_alloc_pages(usize count,
                                                                     NUMAPolicy policy = NUMAPolicy::DEFAULT,
                                                                     numa_node_t preferred_node = NUMA_NO_NODE) noexcept;
    NUMAVoidResult numa_free_pages(PhysAddr addr, usize count) noexcept;
    NUMAVoidResult set_default_policy(NUMAPolicy policy) noexcept;
    [[nodiscard]] NUMAPolicy get_default_policy() const noexcept;
    [[nodiscard]] numa_node_t get_current_numa_node() const noexcept;
    [[nodiscard]] numa_node_t get_preferred_node(usize size) const noexcept;
    [[nodiscard]] bool is_numa_node_available(numa_node_t node) const noexcept;
    [[nodiscard]] NUMASystemStats get_system_stats() const noexcept;
    [[nodiscard]] double get_numa_efficiency() const noexcept;
    [[nodiscard]] static NUMAPolicyManager& get_instance() noexcept;
private:
    NUMAPolicyManager(const NUMAConfig& config) noexcept;
    static bool initialized_;
    static NUMAPolicyManager* instance_;
};

namespace numa {
    inline NUMAVoidResult initialize(const NUMAPolicyManager::NUMAConfig& config) noexcept {
        return NUMAPolicyManager::initialize(config);
    }
    inline NUMAResult<PhysAddr> alloc_pages(usize count,
                                                          NUMAPolicy policy = NUMAPolicy::DEFAULT) noexcept {
        return NUMAPolicyManager::get_instance().numa_alloc_pages(count, policy);
    }
    inline NUMAVoidResult free_pages(PhysAddr addr, usize count) noexcept {
        return NUMAPolicyManager::get_instance().numa_free_pages(addr, count);
    }
    inline numa_node_t get_current_node() noexcept {
        return NUMAPolicyManager::get_instance().get_current_numa_node();
    }
    inline double get_efficiency() noexcept {
        return NUMAPolicyManager::get_instance().get_numa_efficiency();
    }
    inline bool is_available() noexcept {
        return NUMAPolicyManager::get_instance().is_numa_enabled();
    }
}

// ========================================================================
// vmalloc_allocator.hpp
// ========================================================================

enum class VmallocError : u32 {
    OutOfMemory = 1,
    InvalidAddress = 2,
    InvalidSize = 3,
    AlignmentError = 4,
    MappingFailed = 5,
    UnmappingFailed = 6,
    AddressSpaceExhausted = 7,
    InitializationFailed = 8,
    PermissionDenied = 9,
    OverlappingRegion = 10,
    RegionNotFound = 11,
    LazyFreeFailed = 12
};

template<typename T>
using VmallocResult = moss::kernel::Result<T, VmallocError>;
using VmallocVoidResult = moss::kernel::Result<void, VmallocError>;

enum class VmAreaType : u32 {
    VMALLOC = 0,
    IOREMAP = 1,
    MODULE = 2,
    KERNEL_STACK = 3,
    GUARD_PAGE = 4,
    DMA = 5,
    PERCPU = 6,
    HUGE_PAGE = 7
};

struct VmallocRequest {
    usize size;
    usize alignment;
    VmAreaType type;
    u64 permissions;
    numa_node_t preferred_node;
    u32 flags;

    enum Flags : u32 {
        NONE = 0,
        ZERO_MEMORY = (1 << 0),
        GUARD_PAGES = (1 << 1),
        NO_LAZY_FREE = (1 << 2),
        HUGE_PAGES = (1 << 3),
        DMA_COHERENT = (1 << 4),
        EXECUTABLE = (1 << 5)
    };

    VmallocRequest(usize sz, VmAreaType t = VmAreaType::VMALLOC) noexcept
        : size(sz), alignment(PAGE_SIZE), type(t),
          permissions(PagePerms::KERNEL_RW),
          preferred_node(NUMA_NO_NODE), flags(Flags::GUARD_PAGES) {}
};

enum class RBColor : u8 {
    RED = 0,
    BLACK = 1
};

struct VmArea {
    VirtAddr start;
    VirtAddr end;
    usize size;
    VmAreaType type;
    u64 permissions;
    numa_node_t numa_node;
    u32 flags;

    // Red-black tree linkage
    VmArea* parent;
    VmArea* left;
    VmArea* right;
    RBColor color;

    // Linked list for ordered traversal
    VmArea* list_next;
    VmArea* list_prev;

    // Statistics
    u64 creation_time;
    moss::kernel::containers::AtomicU64 access_count;
    moss::kernel::containers::AtomicU32 ref_count;

    VmArea() noexcept
        : start(0), end(0), size(0), type(VmAreaType::VMALLOC),
          permissions(0), numa_node(NUMA_NO_NODE), flags(0),
          parent(nullptr), left(nullptr), right(nullptr), color(RBColor::RED),
          list_next(nullptr), list_prev(nullptr),
          creation_time(0), access_count(0), ref_count(0) {}

    VmArea(VirtAddr s, usize sz, VmAreaType t, u64 perms) noexcept
        : start(s), end(s + sz), size(sz), type(t),
          permissions(perms), numa_node(NUMA_NO_NODE), flags(0),
          parent(nullptr), left(nullptr), right(nullptr), color(RBColor::RED),
          list_next(nullptr), list_prev(nullptr),
          creation_time(0), access_count(0), ref_count(0) {}

    [[nodiscard]] bool contains(VirtAddr addr) const noexcept {
        return addr >= start && addr < end;
    }

    [[nodiscard]] bool overlaps(VirtAddr other_start, VirtAddr other_end) const noexcept {
        return start < other_end && other_start < end;
    }
};

class VirtualAddressSpace {
public:
    VirtualAddressSpace(VirtAddr space_start, VirtAddr space_end) noexcept;

    [[nodiscard]] VmallocResult<VirtAddr> allocate_range(usize size, usize alignment = PAGE_SIZE) noexcept;
    VmallocVoidResult free_range(VirtAddr addr) noexcept;
    [[nodiscard]] VmArea* find_area(VirtAddr addr) const noexcept;
    [[nodiscard]] VmArea* find_area_containing(VirtAddr addr) const noexcept;

    struct AddressSpaceStats {
        usize total_size;
        usize allocated_size;
        usize free_size;
        usize area_count;
        usize largest_free_gap;
        usize fragmentation_ratio;
    };

    [[nodiscard]] AddressSpaceStats get_stats() const noexcept;

private:
    VirtAddr space_start_;
    VirtAddr space_end_;
    VmArea* rb_root_;
    VmArea* area_list_head_;
    usize area_count_;
    moss::kernel::containers::AtomicSize allocated_size_;
    mutable moss::kernel::containers::IrqSpinLock lock_;

    // Red-black tree operations
    void rb_insert(VmArea* area) noexcept;
    void rb_remove(VmArea* area) noexcept;
    void rb_insert_fixup(VmArea* area) noexcept;
    void rb_remove_fixup(VmArea* area) noexcept;
    void rb_rotate_left(VmArea* area) noexcept;
    void rb_rotate_right(VmArea* area) noexcept;
    void rb_transplant(VmArea* old_area, VmArea* new_area) noexcept;
    [[nodiscard]] VmArea* rb_minimum(VmArea* area) const noexcept;

    // List operations
    void list_insert(VmArea* area) noexcept;
    void list_remove(VmArea* area) noexcept;

    // Gap finding
    [[nodiscard]] VmallocResult<VirtAddr> find_free_gap(usize size, usize alignment) const noexcept;
};

class LazyFreeManager {
public:
    struct LazyFreeEntry {
        VirtAddr start;
        usize size;
        u64 free_time;
        LazyFreeEntry* next;
    };

    LazyFreeManager() noexcept;

    VmallocVoidResult add_lazy_free(VirtAddr addr, usize size) noexcept;
    VmallocVoidResult flush_lazy_frees() noexcept;
    VmallocVoidResult flush_if_needed() noexcept;

    [[nodiscard]] usize get_pending_count() const noexcept { return pending_count_; }
    [[nodiscard]] usize get_pending_size() const noexcept { return pending_size_; }

private:
    LazyFreeEntry* pending_list_;
    usize pending_count_;
    usize pending_size_;
    moss::kernel::containers::IrqSpinLock lock_;
    static constexpr usize MAX_PENDING_SIZE = 64 * 1024 * 1024;  // 64MB
    static constexpr usize MAX_PENDING_COUNT = 256;

    VmallocVoidResult do_flush(LazyFreeEntry* entry) noexcept;
};

class VmallocAllocator {
public:
    struct VmallocConfig {
        bool enable_lazy_free;
        usize lazy_free_threshold;
        usize max_lazy_free_memory;
        bool enable_numa_awareness;
        u32 default_numa_policy;
    };

    static VmallocVoidResult initialize() noexcept;

    [[nodiscard]] static VmallocResult<void*> vmalloc(usize size) noexcept;
    [[nodiscard]] static VmallocResult<void*> vmalloc_aligned(usize size, usize alignment) noexcept;
    [[nodiscard]] static VmallocResult<void*> vmalloc_request(const VmallocRequest& request) noexcept;
    static VmallocVoidResult vfree(void* addr) noexcept;

    [[nodiscard]] static VmallocResult<void*> ioremap(PhysAddr phys_addr, usize size) noexcept;
    static VmallocVoidResult iounmap(void* addr) noexcept;

    [[nodiscard]] static VmallocResult<void*> alloc_kernel_stack(usize stack_size = 16 * PAGE_SIZE) noexcept;
    static VmallocVoidResult free_kernel_stack(void* stack_base) noexcept;

    [[nodiscard]] static VmallocResult<void*> alloc_module_space(usize size) noexcept;
    static VmallocVoidResult free_module_space(void* addr) noexcept;

    struct VmallocStats {
        usize total_vmalloc_size;
        usize allocated_vmalloc_size;
        usize free_vmalloc_size;
        usize area_count;
        usize ioremap_count;
        usize kernel_stack_count;
        usize lazy_free_pending;
        usize largest_free_block;
    };

    [[nodiscard]] static VmallocStats get_stats() noexcept;

    static VmallocVoidResult flush_lazy_frees() noexcept;

private:
    static bool initialized_;
    static VirtualAddressSpace* address_space_;
    static LazyFreeManager* lazy_free_manager_;

    static VmallocVoidResult map_vmalloc_pages(VirtAddr vaddr, usize size,
                                               u64 permissions, numa_node_t node) noexcept;
    static VmallocVoidResult unmap_vmalloc_pages(VirtAddr vaddr, usize size) noexcept;
    static VmallocVoidResult setup_guard_pages(VirtAddr start, usize total_size, usize usable_size) noexcept;
};

namespace vmalloc {
    [[nodiscard]] inline VmallocResult<void*> alloc(usize size) noexcept {
        return VmallocAllocator::vmalloc(size);
    }
    [[nodiscard]] inline VmallocResult<void*> alloc_aligned(usize size, usize alignment) noexcept {
        return VmallocAllocator::vmalloc_aligned(size, alignment);
    }
    inline VmallocVoidResult free(void* addr) noexcept {
        return VmallocAllocator::vfree(addr);
    }
    [[nodiscard]] inline VmallocResult<void*> ioremap(PhysAddr phys_addr, usize size) noexcept {
        return VmallocAllocator::ioremap(phys_addr, size);
    }
    inline VmallocVoidResult iounmap(void* addr) noexcept {
        return VmallocAllocator::iounmap(addr);
    }
    [[nodiscard]] inline VmallocResult<void*> alloc_stack(usize size = 16 * PAGE_SIZE) noexcept {
        return VmallocAllocator::alloc_kernel_stack(size);
    }
    inline VmallocVoidResult free_stack(void* base) noexcept {
        return VmallocAllocator::free_kernel_stack(base);
    }
}

} // namespace moss::kernel::mm
