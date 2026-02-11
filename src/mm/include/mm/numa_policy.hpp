#pragma once

// NUMA感知内存管理策略 - 现代多核系统优化
// 基于Linux内核NUMA策略，实现节点拓扑发现、本地化分配和负载均衡

#include "../../../include/types.hpp"
#include "../../../include/result.hpp"
#include "containers/atomic_types.hpp"
#include "containers/per_cpu_data.hpp"
#include "buddy_allocator_v2.hpp"

namespace moss::kernel::mm {

// NUMA错误类型
enum class NUMAError : u32 {
    InvalidNode = 1,
    NodeUnavailable = 2,
    AllocationFailed = 3,
    MigrationFailed = 4,
    TopologyInvalid = 5,
    PolicyViolation = 6,
    OutOfMemory = 7
};

// NUMA结果类型
template<typename T>
using NUMAResult = moss::kernel::Result<T, NUMAError>;
using NUMAVoidResult = moss::kernel::Result<void, NUMAError>;

// NUMA节点ID类型
using numa_node_t = moss::kernel::u32;
constexpr numa_node_t NUMA_NO_NODE = static_cast<numa_node_t>(-1);
constexpr moss::kernel::u32 MAX_NUMA_NODES = 16;

// NUMA分配策略
enum class NUMAPolicy : u32 {
    DEFAULT = 0,     // 默认策略 - 本地节点优先
    BIND = 1,        // 绑定策略 - 只从指定节点分配
    INTERLEAVE = 2,  // 交错策略 - 轮询所有节点
    PREFERRED = 3,   // 优选策略 - 优选节点，失败时fallback
    LOCAL = 4        // 严格本地 - 只允许本地节点
};

// NUMA节点状态
enum class NodeState : u8 {
    OFFLINE = 0,     // 节点离线
    ONLINE = 1,      // 节点在线
    PARTIAL = 2,     // 部分可用
    RESERVED = 3     // 保留节点
};

// NUMA节点距离矩阵
struct NUMADistance {
    static constexpr moss::kernel::u8 LOCAL_DISTANCE = 10;    // 本地距离
    static constexpr moss::kernel::u8 REMOTE_DISTANCE = 20;   // 远程距离
    static constexpr moss::kernel::u8 UNREACHABLE_DISTANCE = 255; // 不可达

    moss::kernel::u8 distance[MAX_NUMA_NODES][MAX_NUMA_NODES];

    NUMADistance() noexcept {
        // 初始化距离矩阵
        for (moss::kernel::u32 i = 0; i < MAX_NUMA_NODES; ++i) {
            for (moss::kernel::u32 j = 0; j < MAX_NUMA_NODES; ++j) {
                if (i == j) {
                    distance[i][j] = LOCAL_DISTANCE;
                } else {
                    distance[i][j] = UNREACHABLE_DISTANCE;
                }
            }
        }
    }

    [[nodiscard]] moss::kernel::u8 get_distance(numa_node_t from, numa_node_t to) const noexcept {
        if (from >= MAX_NUMA_NODES || to >= MAX_NUMA_NODES) {
            return UNREACHABLE_DISTANCE;
        }
        return distance[from][to];
    }

    void set_distance(numa_node_t from, numa_node_t to, moss::kernel::u8 dist) noexcept {
        if (from < MAX_NUMA_NODES && to < MAX_NUMA_NODES) {
            distance[from][to] = dist;
            distance[to][from] = dist; // 对称距离
        }
    }
};

// NUMA节点信息
struct NUMANode {
    numa_node_t node_id;                               // 节点ID
    NodeState state;                                   // 节点状态
    moss::kernel::PhysAddr memory_start;               // 内存起始地址
    moss::kernel::usize memory_size;                   // 内存大小
    moss::kernel::containers::AtomicSize free_memory;  // 可用内存
    moss::kernel::containers::AtomicSize allocated_memory; // 已分配内存

    // CPU亲和性
    moss::kernel::u64 cpu_mask;                        // 属于此节点的CPU掩码
    moss::kernel::u32 cpu_count;                       // CPU数量

    // 性能统计
    moss::kernel::containers::AtomicU64 local_allocations;  // 本地分配次数
    moss::kernel::containers::AtomicU64 remote_allocations; // 远程分配次数
    moss::kernel::containers::AtomicU64 migration_count;    // 页面迁移次数

    // 负载信息
    moss::kernel::containers::AtomicU32 memory_pressure;    // 内存压力值 (0-100)
    moss::kernel::u64 last_balance_time;                    // 上次负载均衡时间

    NUMANode() noexcept
        : node_id(NUMA_NO_NODE), state(NodeState::OFFLINE),
          memory_start(0), memory_size(0), free_memory(0), allocated_memory(0),
          cpu_mask(0), cpu_count(0),
          local_allocations(0), remote_allocations(0), migration_count(0),
          memory_pressure(0), last_balance_time(0) {}

    NUMANode(numa_node_t id, moss::kernel::PhysAddr start, moss::kernel::usize size) noexcept
        : node_id(id), state(NodeState::ONLINE),
          memory_start(start), memory_size(size), free_memory(size), allocated_memory(0),
          cpu_mask(0), cpu_count(0),
          local_allocations(0), remote_allocations(0), migration_count(0),
          memory_pressure(0), last_balance_time(0) {}
};

// 前向声明
class NUMATopologyImpl;
class NUMAAllocatorImpl;
class NUMABalancerImpl;

// NUMA拓扑发现和管理
class NUMATopology {
    friend class NUMATopologyImpl;

public:
    struct TopologyStats {
        moss::kernel::u32 online_nodes;
        moss::kernel::u32 total_nodes;
        moss::kernel::usize total_memory;
        moss::kernel::usize available_memory;
        double memory_balance_ratio;      // 内存平衡比例
        moss::kernel::u32 active_cpus;
    };

protected:
    NUMANode nodes_[MAX_NUMA_NODES];
    NUMADistance distance_matrix_;
    moss::kernel::containers::AtomicU32 online_node_count_;
    moss::kernel::u64 topology_version_;              // 拓扑版本号，用于检测变化

    // CPU到节点的映射
    numa_node_t cpu_to_node_[moss::kernel::MAX_CPUS];

public:
    NUMATopology() noexcept;

    // 拓扑发现
    NUMAVoidResult discover_topology() noexcept;
    NUMAVoidResult add_node(numa_node_t node_id, moss::kernel::PhysAddr start,
                           moss::kernel::usize size) noexcept;
    NUMAVoidResult remove_node(numa_node_t node_id) noexcept;

    // 节点状态管理
    NUMAVoidResult set_node_state(numa_node_t node_id, NodeState state) noexcept;
    [[nodiscard]] NodeState get_node_state(numa_node_t node_id) const noexcept;
    [[nodiscard]] bool is_node_online(numa_node_t node_id) const noexcept;

    // CPU-节点映射
    NUMAVoidResult bind_cpu_to_node(moss::kernel::u32 cpu_id, numa_node_t node_id) noexcept;
    [[nodiscard]] numa_node_t get_cpu_node(moss::kernel::u32 cpu_id) const noexcept;
    [[nodiscard]] numa_node_t get_current_node() const noexcept;

    // 距离管理
    void set_node_distance(numa_node_t from, numa_node_t to, moss::kernel::u8 distance) noexcept;
    [[nodiscard]] moss::kernel::u8 get_node_distance(numa_node_t from, numa_node_t to) const noexcept;
    [[nodiscard]] numa_node_t find_nearest_node(numa_node_t from) const noexcept;

    // 节点信息查询
    [[nodiscard]] const NUMANode* get_node(numa_node_t node_id) const noexcept;
    [[nodiscard]] NUMANode* get_node_mutable(numa_node_t node_id) noexcept;
    [[nodiscard]] moss::kernel::usize get_node_free_memory(numa_node_t node_id) const noexcept;
    [[nodiscard]] moss::kernel::u32 get_online_node_count() const noexcept;

    // 统计信息
    [[nodiscard]] TopologyStats get_topology_stats() const noexcept;
    void update_topology_version() noexcept { topology_version_++; }

private:
    void initialize_default_topology() noexcept;
    void detect_cpu_topology() noexcept;
    void calculate_distances() noexcept;
};

// NUMA感知分配器
class NUMAAllocator {
    friend class NUMAAllocatorImpl;

public:
    struct AllocationRequest {
        moss::kernel::usize size;          // 分配大小
        moss::kernel::usize alignment;     // 对齐要求
        NUMAPolicy policy;                 // NUMA策略
        numa_node_t preferred_node;        // 优选节点
        moss::kernel::u64 node_mask;       // 允许的节点掩码
        moss::kernel::u32 flags;           // 分配标志

        // 分配标志
        enum Flags : u32 {
            NONE = 0,
            ZERO_MEMORY = (1 << 0),        // 零初始化
            HIGH_PRIORITY = (1 << 1),      // 高优先级
            NO_FALLBACK = (1 << 2),        // 不允许fallback
            MIGRATE_ALLOWED = (1 << 3)     // 允许后续迁移
        };

        AllocationRequest(moss::kernel::usize sz, NUMAPolicy pol = NUMAPolicy::DEFAULT,
                         numa_node_t node = NUMA_NO_NODE) noexcept
            : size(sz), alignment(moss::kernel::PAGE_SIZE), policy(pol),
              preferred_node(node), node_mask(~0UL), flags(Flags::NONE) {}
    };

    struct AllocationStats {
        moss::kernel::usize total_allocations;
        moss::kernel::usize local_allocations;        // 本地节点分配
        moss::kernel::usize remote_allocations;       // 远程节点分配
        moss::kernel::usize fallback_allocations;     // fallback分配
        moss::kernel::usize failed_allocations;       // 失败的分配
        double locality_ratio;                        // 本地性比例
        moss::kernel::u64 average_allocation_time_us;
    };

protected:
    NUMATopology* topology_;
    AllocationStats stats_;
    moss::kernel::containers::AtomicU32 current_interleave_node_; // 用于INTERLEAVE策略

public:
    NUMAAllocator(NUMATopology* topo) noexcept : topology_(topo), stats_{}, current_interleave_node_(0) {}

    // 主分配接口
    [[nodiscard]] NUMAResult<moss::kernel::PhysAddr> allocate_pages(const AllocationRequest& request) noexcept;
    NUMAVoidResult free_pages(moss::kernel::PhysAddr addr, moss::kernel::usize size) noexcept;

    // 策略实现
    [[nodiscard]] NUMAResult<moss::kernel::PhysAddr> allocate_default(const AllocationRequest& request) noexcept;
    [[nodiscard]] NUMAResult<moss::kernel::PhysAddr> allocate_bind(const AllocationRequest& request) noexcept;
    [[nodiscard]] NUMAResult<moss::kernel::PhysAddr> allocate_interleave(const AllocationRequest& request) noexcept;
    [[nodiscard]] NUMAResult<moss::kernel::PhysAddr> allocate_preferred(const AllocationRequest& request) noexcept;
    [[nodiscard]] NUMAResult<moss::kernel::PhysAddr> allocate_local(const AllocationRequest& request) noexcept;

    // 节点选择算法
    [[nodiscard]] numa_node_t select_allocation_node(const AllocationRequest& request) const noexcept;
    [[nodiscard]] numa_node_t select_fallback_node(numa_node_t failed_node, const AllocationRequest& request) const noexcept;

    // 统计信息
    [[nodiscard]] AllocationStats get_stats() const noexcept { return stats_; }
    void reset_stats() noexcept;

private:
    [[nodiscard]] NUMAResult<moss::kernel::PhysAddr> allocate_from_node(numa_node_t node,
                                                                       const AllocationRequest& request) noexcept;
    void update_allocation_stats(numa_node_t allocated_node, numa_node_t preferred_node,
                                bool fallback_used) noexcept;
};

// NUMA负载均衡器
class NUMABalancer {
    friend class NUMABalancerImpl;

public:
    struct BalancerConfig {
        moss::kernel::u64 balance_interval_ms;         // 均衡间隔
        moss::kernel::u32 imbalance_threshold;         // 不均衡阈值 (%)
        moss::kernel::u32 migration_rate_limit;        // 迁移速率限制 (pages/sec)
        double memory_threshold_ratio;                 // 内存阈值比例
    };

    struct BalancerStats {
        moss::kernel::usize migrations_performed;
        moss::kernel::usize migrations_failed;
        moss::kernel::u64 total_balance_time_us;
        moss::kernel::u32 balance_cycles;
        double average_imbalance_before;               // 均衡前的平均不均衡度
        double average_imbalance_after;                // 均衡后的平均不均衡度
    };

protected:
    BalancerConfig config_;
    NUMATopology* topology_;
    NUMAAllocator* allocator_;
    BalancerStats stats_;
    moss::kernel::containers::AtomicBool balancer_active_;
    moss::kernel::u64 last_balance_time_;

public:
    NUMABalancer(const BalancerConfig& config, NUMATopology* topo, NUMAAllocator* alloc) noexcept;

    // 负载均衡控制
    NUMAVoidResult start_balancer() noexcept;
    void stop_balancer() noexcept;
    [[nodiscard]] bool is_balancer_active() const noexcept;

    // 均衡算法
    NUMAVoidResult balance_memory_load() noexcept;
    NUMAVoidResult migrate_pages_between_nodes(numa_node_t from, numa_node_t to,
                                              moss::kernel::usize page_count) noexcept;

    // 不均衡检测
    [[nodiscard]] double calculate_memory_imbalance() const noexcept;
    [[nodiscard]] bool should_balance() const noexcept;
    [[nodiscard]] NUMAResult<numa_node_t> find_source_node() const noexcept;
    [[nodiscard]] NUMAResult<numa_node_t> find_target_node(numa_node_t source) const noexcept;

    // 统计信息
    [[nodiscard]] BalancerStats get_stats() const noexcept { return stats_; }
    void reset_stats() noexcept;

private:
    NUMAVoidResult background_balance_thread() noexcept;
    [[nodiscard]] moss::kernel::usize calculate_migration_count(numa_node_t from, numa_node_t to) const noexcept;
};

// NUMA策略管理器 - 主控制器
class NUMAPolicyManager {
public:
    struct NUMAConfig {
        NUMABalancer::BalancerConfig balancer_config;
        bool enable_auto_balancing;                   // 启用自动均衡
        bool enable_migration;                        // 启用页面迁移
        NUMAPolicy default_policy;                    // 默认策略
    };

    struct NUMASystemStats {
        NUMATopology::TopologyStats topology_stats;
        NUMAAllocator::AllocationStats allocator_stats;
        NUMABalancer::BalancerStats balancer_stats;
        double overall_numa_efficiency;               // 整体NUMA效率
    };

protected:
    NUMAConfig config_;
    NUMATopology topology_;
    NUMAAllocator allocator_;
    NUMABalancer balancer_;
    moss::kernel::containers::AtomicBool system_initialized_;

public:
    static NUMAVoidResult initialize(const NUMAConfig& config) noexcept;

    // 系统控制
    NUMAVoidResult start_numa_system() noexcept;
    void stop_numa_system() noexcept;
    [[nodiscard]] bool is_numa_enabled() const noexcept;

    // 分配接口
    [[nodiscard]] NUMAResult<moss::kernel::PhysAddr> numa_alloc_pages(moss::kernel::usize count,
                                                                     NUMAPolicy policy = NUMAPolicy::DEFAULT,
                                                                     numa_node_t preferred_node = NUMA_NO_NODE) noexcept;
    NUMAVoidResult numa_free_pages(moss::kernel::PhysAddr addr, moss::kernel::usize count) noexcept;

    // 策略管理
    NUMAVoidResult set_default_policy(NUMAPolicy policy) noexcept;
    [[nodiscard]] NUMAPolicy get_default_policy() const noexcept;

    // 节点管理
    [[nodiscard]] numa_node_t get_current_numa_node() const noexcept;
    [[nodiscard]] numa_node_t get_preferred_node(moss::kernel::usize size) const noexcept;
    [[nodiscard]] bool is_numa_node_available(numa_node_t node) const noexcept;

    // 统计和监控
    [[nodiscard]] NUMASystemStats get_system_stats() const noexcept;
    [[nodiscard]] double get_numa_efficiency() const noexcept;
    [[nodiscard]] static NUMAPolicyManager& get_instance() noexcept;

private:
    NUMAPolicyManager(const NUMAConfig& config) noexcept;

    // 单例实例
    static bool initialized_;
    static NUMAPolicyManager* instance_;
};

// 全局便利接口
namespace numa {
    // 初始化NUMA系统
    inline NUMAVoidResult initialize(const NUMAPolicyManager::NUMAConfig& config) noexcept {
        return NUMAPolicyManager::initialize(config);
    }

    // NUMA感知页面分配
    inline NUMAResult<moss::kernel::PhysAddr> alloc_pages(moss::kernel::usize count,
                                                          NUMAPolicy policy = NUMAPolicy::DEFAULT) noexcept {
        return NUMAPolicyManager::get_instance().numa_alloc_pages(count, policy);
    }

    // 释放页面
    inline NUMAVoidResult free_pages(moss::kernel::PhysAddr addr, moss::kernel::usize count) noexcept {
        return NUMAPolicyManager::get_instance().numa_free_pages(addr, count);
    }

    // 获取当前NUMA节点
    inline numa_node_t get_current_node() noexcept {
        return NUMAPolicyManager::get_instance().get_current_numa_node();
    }

    // 获取NUMA效率
    inline double get_efficiency() noexcept {
        return NUMAPolicyManager::get_instance().get_numa_efficiency();
    }

    // 检查NUMA可用性
    inline bool is_available() noexcept {
        return NUMAPolicyManager::get_instance().is_numa_enabled();
    }
}

} // namespace moss::kernel::mm
