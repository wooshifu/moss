#pragma once

// NUMA策略核心实现 - 展示关键NUMA算法
// 拓扑发现、本地化分配和负载均衡的具体实现

#include "numa_policy.hpp"
#include "../include/moss_std.hpp"
#include "../include/arch/arch_abstraction.hpp"

namespace moss::kernel::mm {

// NUMA拓扑发现实现
class NUMATopologyImpl {
public:
    // 智能拓扑发现算法 - 基于硬件信息
    static NUMAVoidResult discover_topology_impl(NUMATopology* topology) noexcept {
        // 1. 检测系统是否支持NUMA
        if (!is_numa_supported()) {
            // 单节点系统，创建默认拓扑
            return create_default_topology_impl(topology);
        }

        // 2. 扫描内存范围，识别NUMA节点
        auto discovery_result = scan_memory_ranges_impl(topology);
        if (!discovery_result.is_ok()) {
            return discovery_result;
        }

        // 3. 检测CPU拓扑和亲和性
        detect_cpu_topology_impl(topology);

        // 4. 计算节点间距离
        calculate_node_distances_impl(topology);

        // 5. 更新拓扑版本
        topology->update_topology_version();

        return NUMAVoidResult{};
    }

    // CPU-节点绑定算法
    static NUMAVoidResult bind_cpu_to_node_impl(NUMATopology* topology,
                                                moss::kernel::u32 cpu_id,
                                                numa_node_t node_id) noexcept {
        if (cpu_id >= moss::kernel::MAX_CPUS || node_id >= MAX_NUMA_NODES) {
            return NUMAVoidResult{NUMAError::InvalidNode};
        }

        if (!topology->is_node_online(node_id)) {
            return NUMAVoidResult{NUMAError::NodeUnavailable};
        }

        // 更新CPU-节点映射
        topology->cpu_to_node_[cpu_id] = node_id;

        // 更新节点的CPU信息
        NUMANode* node = topology->get_node_mutable(node_id);
        if (node) {
            node->cpu_mask |= (1UL << cpu_id);
            node->cpu_count++;
        }

        return NUMAVoidResult{};
    }

    // 智能最近节点查找
    static numa_node_t find_nearest_node_impl(const NUMATopology* topology, numa_node_t from) noexcept {
        if (from >= MAX_NUMA_NODES || !topology->is_node_online(from)) {
            return NUMA_NO_NODE;
        }

        numa_node_t nearest_node = NUMA_NO_NODE;
        moss::kernel::u8 min_distance = NUMADistance::UNREACHABLE_DISTANCE;

        moss::kernel::u32 online_count = topology->get_online_node_count();
        for (moss::kernel::u32 i = 0; i < online_count && i < MAX_NUMA_NODES; ++i) {
            if (i == from || !topology->is_node_online(i)) continue;

            moss::kernel::u8 distance = topology->get_node_distance(from, i);
            if (distance < min_distance) {
                min_distance = distance;
                nearest_node = i;
            }
        }

        return nearest_node;
    }

    // 当前节点检测 - 基于当前CPU
    static numa_node_t get_current_node_impl(const NUMATopology* topology) noexcept {
        moss::kernel::u32 cpu_id = moss::kernel::arch::get_current_cpu_id();
        if (cpu_id >= moss::kernel::MAX_CPUS) {
            return 0; // 回退到节点0
        }
        return topology->get_cpu_node(cpu_id);
    }

private:
    // 检查NUMA支持
    static bool is_numa_supported() noexcept {
        // 简化实现：检查系统是否是多核且有多个内存节点
        // 在实际系统中需要查询ACPI SRAT表或类似硬件信息
        return moss::kernel::MAX_CPUS > 4; // 假设4核以上支持NUMA
    }

    // 创建默认单节点拓扑
    static NUMAVoidResult create_default_topology_impl(NUMATopology* topology) noexcept {
        // 添加单个NUMA节点
        constexpr moss::kernel::PhysAddr DEFAULT_MEMORY_START = 0x40000000; // 1GB
        constexpr moss::kernel::usize DEFAULT_MEMORY_SIZE = 0x80000000;     // 2GB

        auto add_result = topology->add_node(0, DEFAULT_MEMORY_START, DEFAULT_MEMORY_SIZE);
        if (!add_result.is_ok()) {
            return add_result;
        }

        // 绑定所有CPU到节点0
        constexpr moss::kernel::u32 cpu_count = moss::kernel::MAX_CPUS;
        for (moss::kernel::u32 i = 0; i < cpu_count; ++i) {
            [[maybe_unused]] auto bind_result = topology->bind_cpu_to_node(i, 0);
        }

        return NUMAVoidResult{};
    }

    // 扫描内存范围
    static NUMAVoidResult scan_memory_ranges_impl(NUMATopology* topology) noexcept {
        // 简化实现：创建模拟的多节点拓扑
        // 在实际系统中需要解析ACPI SRAT或设备树

        // 节点0: 0x40000000 - 0x80000000 (1GB)
        auto result1 = topology->add_node(0, 0x40000000, 0x40000000);
        if (!result1.is_ok()) return result1;

        // 节点1: 0x80000000 - 0xC0000000 (1GB)
        auto result2 = topology->add_node(1, 0x80000000, 0x40000000);
        if (!result2.is_ok()) return result2;

        // 设置节点间距离
        topology->set_node_distance(0, 1, NUMADistance::REMOTE_DISTANCE);

        return NUMAVoidResult{};
    }

    // 检测CPU拓扑
    static void detect_cpu_topology_impl(NUMATopology* topology) noexcept {
        constexpr moss::kernel::u32 cpu_count = moss::kernel::MAX_CPUS;
        moss::kernel::u32 online_nodes = topology->get_online_node_count();

        if (online_nodes == 0) return;

        // 简化分配：CPU均匀分配到NUMA节点
        moss::kernel::u32 cpus_per_node = cpu_count / online_nodes;
        moss::kernel::u32 remaining_cpus = cpu_count % online_nodes;

        moss::kernel::u32 cpu_id = 0;
        for (moss::kernel::u32 node = 0; node < online_nodes; ++node) {
            moss::kernel::u32 node_cpu_count = cpus_per_node + (node < remaining_cpus ? 1 : 0);

            for (moss::kernel::u32 i = 0; i < node_cpu_count && cpu_id < cpu_count; ++i, ++cpu_id) {
                [[maybe_unused]] auto bind_result = topology->bind_cpu_to_node(cpu_id, node);
            }
        }
    }

    // 计算节点距离
    static void calculate_node_distances_impl(NUMATopology* topology) noexcept {
        moss::kernel::u32 online_count = topology->get_online_node_count();

        // 简化距离模型：基于节点ID差值
        for (moss::kernel::u32 i = 0; i < online_count; ++i) {
            for (moss::kernel::u32 j = 0; j < online_count; ++j) {
                if (i == j) {
                    topology->set_node_distance(i, j, NUMADistance::LOCAL_DISTANCE);
                } else {
                    // 距离与节点ID差值成正比
                    moss::kernel::u8 distance = NUMADistance::REMOTE_DISTANCE +
                                               static_cast<moss::kernel::u8>(moss::abs(static_cast<int>(i - j)) * 5);
                    topology->set_node_distance(i, j, distance);
                }
            }
        }
    }
};

// NUMA分配器实现
class NUMAAllocatorImpl {
public:
    // 智能节点选择算法
    static numa_node_t select_allocation_node_impl(const NUMAAllocator* allocator,
                                                   const NUMAAllocator::AllocationRequest& request) noexcept {
        switch (request.policy) {
            case NUMAPolicy::DEFAULT:
            case NUMAPolicy::LOCAL:
                return select_local_node_impl(allocator, request);

            case NUMAPolicy::BIND:
                return select_bind_node_impl(allocator, request);

            case NUMAPolicy::INTERLEAVE:
                return select_interleave_node_impl(allocator, request);

            case NUMAPolicy::PREFERRED:
                return select_preferred_node_impl(allocator, request);

            default:
                return allocator->topology_->get_current_node();
        }
    }

    // 从节点分配算法
    static NUMAResult<moss::kernel::PhysAddr> allocate_from_node_impl(
        NUMAAllocator* allocator, numa_node_t node,
        const NUMAAllocator::AllocationRequest& request) noexcept {

        if (!allocator->topology_->is_node_online(node)) {
            return NUMAResult<moss::kernel::PhysAddr>{NUMAError::NodeUnavailable};
        }

        // 检查节点是否有足够内存
        moss::kernel::usize free_memory = allocator->topology_->get_node_free_memory(node);
        if (free_memory < request.size) {
            return NUMAResult<moss::kernel::PhysAddr>{NUMAError::OutOfMemory};
        }

        // 使用Buddy分配器从指定节点分配
        moss::kernel::usize page_count = (request.size + PAGE_SIZE - 1) / PAGE_SIZE;
        moss::kernel::usize order = calculate_order(page_count);

        auto buddy_result = BuddyAllocatorV2::allocate_pages(order, MigrationType::MOVABLE);
        if (!buddy_result.is_ok()) {
            return NUMAResult<moss::kernel::PhysAddr>{NUMAError::AllocationFailed};
        }

        moss::kernel::PhysAddr allocated_addr = *buddy_result;

        // 更新节点统计信息
        update_node_stats_impl(allocator, node, request.size, true);

        // 更新分配统计
        numa_node_t current_node = allocator->topology_->get_current_node();
        bool is_local = (node == current_node);
        update_allocation_stats_impl(allocator, node, current_node, !is_local);

        return NUMAResult<moss::kernel::PhysAddr>{allocated_addr};
    }

    // fallback节点选择算法
    static numa_node_t select_fallback_node_impl(const NUMAAllocator* allocator,
                                                 numa_node_t failed_node,
                                                 const NUMAAllocator::AllocationRequest& request) noexcept {
        // 不允许fallback
        if (request.flags & NUMAAllocator::AllocationRequest::Flags::NO_FALLBACK) {
            return NUMA_NO_NODE;
        }

        // 按距离排序查找可用节点
        numa_node_t best_node = NUMA_NO_NODE;
        moss::kernel::u8 min_distance = NUMADistance::UNREACHABLE_DISTANCE;

        moss::kernel::u32 online_count = allocator->topology_->get_online_node_count();
        for (moss::kernel::u32 i = 0; i < online_count; ++i) {
            if (i == failed_node || !allocator->topology_->is_node_online(i)) continue;

            // 检查节点是否在允许的掩码中
            if (!(request.node_mask & (1UL << i))) continue;

            // 检查节点是否有足够内存
            moss::kernel::usize free_memory = allocator->topology_->get_node_free_memory(i);
            if (free_memory < request.size) continue;

            // 选择距离最近的节点
            moss::kernel::u8 distance = allocator->topology_->get_node_distance(failed_node, i);
            if (distance < min_distance) {
                min_distance = distance;
                best_node = i;
            }
        }

        return best_node;
    }

    // 默认策略实现 - 本地节点优先
    static NUMAResult<moss::kernel::PhysAddr> allocate_default_impl(
        NUMAAllocator* allocator, const NUMAAllocator::AllocationRequest& request) noexcept {

        numa_node_t local_node = allocator->topology_->get_current_node();

        // 尝试从本地节点分配
        auto local_result = allocate_from_node_impl(allocator, local_node, request);
        if (local_result.is_ok()) {
            return local_result;
        }

        // 本地分配失败，尝试fallback
        numa_node_t fallback_node = select_fallback_node_impl(allocator, local_node, request);
        if (fallback_node != NUMA_NO_NODE) {
            return allocate_from_node_impl(allocator, fallback_node, request);
        }

        return NUMAResult<moss::kernel::PhysAddr>{NUMAError::AllocationFailed};
    }

    // 交错策略实现
    static NUMAResult<moss::kernel::PhysAddr> allocate_interleave_impl(
        NUMAAllocator* allocator, const NUMAAllocator::AllocationRequest& request) noexcept {

        moss::kernel::u32 online_count = allocator->topology_->get_online_node_count();
        if (online_count == 0) {
            return NUMAResult<moss::kernel::PhysAddr>{NUMAError::NodeUnavailable};
        }

        // 获取下一个交错节点
        moss::kernel::u32 current_index = allocator->current_interleave_node_.load(moss::MemoryOrder::Relaxed);
        moss::kernel::u32 next_index = (current_index + 1) % online_count;
        allocator->current_interleave_node_.store(next_index, moss::MemoryOrder::Relaxed);

        numa_node_t target_node = current_index;

        // 尝试从目标节点分配
        auto result = allocate_from_node_impl(allocator, target_node, request);
        if (result.is_ok()) {
            return result;
        }

        // 失败时尝试其他节点（保持交错模式）
        for (moss::kernel::u32 i = 1; i < online_count; ++i) {
            numa_node_t try_node = (current_index + i) % online_count;
            auto try_result = allocate_from_node_impl(allocator, try_node, request);
            if (try_result.is_ok()) {
                return try_result;
            }
        }

        return NUMAResult<moss::kernel::PhysAddr>{NUMAError::AllocationFailed};
    }

private:
    // 本地节点选择
    static numa_node_t select_local_node_impl(const NUMAAllocator* allocator,
                                              const NUMAAllocator::AllocationRequest& request) noexcept {
        (void)request; // 未使用参数
        return allocator->topology_->get_current_node();
    }

    // 绑定节点选择
    static numa_node_t select_bind_node_impl(const NUMAAllocator* allocator,
                                             const NUMAAllocator::AllocationRequest& request) noexcept {
        (void)allocator; // 未使用参数
        return (request.preferred_node != NUMA_NO_NODE) ? request.preferred_node : 0;
    }

    // 交错节点选择
    static numa_node_t select_interleave_node_impl(const NUMAAllocator* allocator,
                                                   const NUMAAllocator::AllocationRequest& request) noexcept {
        (void)request; // 未使用参数
        moss::kernel::u32 online_count = allocator->topology_->get_online_node_count();
        if (online_count == 0) return 0;

        moss::kernel::u32 index = allocator->current_interleave_node_.load(moss::MemoryOrder::Relaxed);
        return index % online_count;
    }

    // 优选节点选择
    static numa_node_t select_preferred_node_impl(const NUMAAllocator* allocator,
                                                  const NUMAAllocator::AllocationRequest& request) noexcept {
        // 如果指定了优选节点且可用，使用它
        if (request.preferred_node != NUMA_NO_NODE &&
            allocator->topology_->is_node_online(request.preferred_node)) {
            return request.preferred_node;
        }

        // 否则回退到本地节点
        return allocator->topology_->get_current_node();
    }

    // 计算分配阶数
    static moss::kernel::usize calculate_order(moss::kernel::usize page_count) noexcept {
        if (page_count <= 1) return 0;

        moss::kernel::usize order = 0;
        moss::kernel::usize size = 1;
        while (size < page_count) {
            size <<= 1;
            order++;
        }
        return order;
    }

    // 更新节点统计信息
    static void update_node_stats_impl(NUMAAllocator* allocator, numa_node_t node,
                                      moss::kernel::usize size, bool is_allocation) noexcept {
        NUMANode* numa_node = allocator->topology_->get_node_mutable(node);
        if (!numa_node) return;

        if (is_allocation) {
            [[maybe_unused]] auto old_allocated = numa_node->allocated_memory.fetch_add(size, moss::MemoryOrder::Relaxed);
            [[maybe_unused]] auto old_free = numa_node->free_memory.fetch_sub(size, moss::MemoryOrder::Relaxed);
        } else {
            [[maybe_unused]] auto old_allocated = numa_node->allocated_memory.fetch_sub(size, moss::MemoryOrder::Relaxed);
            [[maybe_unused]] auto old_free = numa_node->free_memory.fetch_add(size, moss::MemoryOrder::Relaxed);
        }
    }

    // 更新分配统计信息
    static void update_allocation_stats_impl(NUMAAllocator* allocator, numa_node_t allocated_node,
                                            numa_node_t preferred_node, bool fallback_used) noexcept {
        allocator->stats_.total_allocations++;

        if (allocated_node == preferred_node) {
            allocator->stats_.local_allocations++;
        } else {
            allocator->stats_.remote_allocations++;
        }

        if (fallback_used) {
            allocator->stats_.fallback_allocations++;
        }

        // 更新本地性比例
        if (allocator->stats_.total_allocations > 0) {
            allocator->stats_.locality_ratio =
                static_cast<double>(allocator->stats_.local_allocations) / allocator->stats_.total_allocations;
        }
    }
};

// NUMA负载均衡器实现
class NUMABalancerImpl {
public:
    // 智能负载均衡算法
    static NUMAVoidResult balance_memory_load_impl(NUMABalancer* balancer) noexcept {
        if (!balancer->should_balance()) {
            return NUMAVoidResult{};
        }

        moss::kernel::u64 start_time = get_current_time_us();

        // 1. 计算当前不均衡度
        double imbalance_before = balancer->calculate_memory_imbalance();

        // 2. 查找源节点（负载最高）
        auto source_result = balancer->find_source_node();
        if (!source_result.is_ok()) {
            return NUMAVoidResult{source_result.error()};
        }
        numa_node_t source_node = *source_result;

        // 3. 查找目标节点（负载最低）
        auto target_result = balancer->find_target_node(source_node);
        if (!target_result.is_ok()) {
            return NUMAVoidResult{target_result.error()};
        }
        numa_node_t target_node = *target_result;

        // 4. 计算迁移页面数
        moss::kernel::usize migration_count = balancer->calculate_migration_count(source_node, target_node);
        if (migration_count == 0) {
            return NUMAVoidResult{};
        }

        // 5. 执行页面迁移
        auto migrate_result = balancer->migrate_pages_between_nodes(source_node, target_node, migration_count);
        if (!migrate_result.is_ok()) {
            balancer->stats_.migrations_failed++;
            return migrate_result;
        }

        // 6. 更新统计信息
        balancer->stats_.migrations_performed += migration_count;
        balancer->stats_.balance_cycles++;

        double imbalance_after = balancer->calculate_memory_imbalance();
        balancer->stats_.average_imbalance_before =
            (balancer->stats_.average_imbalance_before * (balancer->stats_.balance_cycles - 1) + imbalance_before) /
            balancer->stats_.balance_cycles;
        balancer->stats_.average_imbalance_after =
            (balancer->stats_.average_imbalance_after * (balancer->stats_.balance_cycles - 1) + imbalance_after) /
            balancer->stats_.balance_cycles;

        moss::kernel::u64 balance_time = get_current_time_us() - start_time;
        balancer->stats_.total_balance_time_us += balance_time;

        balancer->last_balance_time_ = get_current_time_us();

        return NUMAVoidResult{};
    }

    // 不均衡度计算算法 - 基于内存使用率差异
    static double calculate_memory_imbalance_impl(const NUMABalancer* balancer) noexcept {
        moss::kernel::u32 online_count = balancer->topology_->get_online_node_count();
        if (online_count <= 1) return 0.0;

        double total_utilization = 0.0;
        double max_utilization = 0.0;
        double min_utilization = 1.0;

        moss::kernel::u32 valid_nodes = 0;

        for (moss::kernel::u32 i = 0; i < online_count; ++i) {
            if (!balancer->topology_->is_node_online(i)) continue;

            const NUMANode* node = balancer->topology_->get_node(i);
            if (!node || node->memory_size == 0) continue;

            double utilization = static_cast<double>(node->allocated_memory.load(moss::MemoryOrder::Relaxed)) /
                                node->memory_size;

            total_utilization += utilization;
            max_utilization = moss::max(max_utilization, utilization);
            min_utilization = moss::min(min_utilization, utilization);
            valid_nodes++;
        }

        if (valid_nodes <= 1) return 0.0;

        // 不均衡度 = (最大利用率 - 最小利用率) / 平均利用率
        double average_utilization = total_utilization / valid_nodes;
        if (average_utilization < 0.01) return 0.0; // 避免除零

        return (max_utilization - min_utilization) / average_utilization;
    }

    // 查找源节点（最高负载）
    static NUMAResult<numa_node_t> find_source_node_impl(const NUMABalancer* balancer) noexcept {
        numa_node_t source_node = NUMA_NO_NODE;
        double max_utilization = 0.0;

        moss::kernel::u32 online_count = balancer->topology_->get_online_node_count();
        for (moss::kernel::u32 i = 0; i < online_count; ++i) {
            if (!balancer->topology_->is_node_online(i)) continue;

            const NUMANode* node = balancer->topology_->get_node(i);
            if (!node || node->memory_size == 0) continue;

            double utilization = static_cast<double>(node->allocated_memory.load(moss::MemoryOrder::Relaxed)) /
                                node->memory_size;

            if (utilization > max_utilization) {
                max_utilization = utilization;
                source_node = i;
            }
        }

        if (source_node == NUMA_NO_NODE) {
            return NUMAResult<numa_node_t>{NUMAError::NodeUnavailable};
        }

        return NUMAResult<numa_node_t>{source_node};
    }

    // 查找目标节点（最低负载，排除源节点）
    static NUMAResult<numa_node_t> find_target_node_impl(const NUMABalancer* balancer,
                                                         numa_node_t source_node) noexcept {
        numa_node_t target_node = NUMA_NO_NODE;
        double min_utilization = 1.0;

        moss::kernel::u32 online_count = balancer->topology_->get_online_node_count();
        for (moss::kernel::u32 i = 0; i < online_count; ++i) {
            if (i == source_node || !balancer->topology_->is_node_online(i)) continue;

            const NUMANode* node = balancer->topology_->get_node(i);
            if (!node || node->memory_size == 0) continue;

            double utilization = static_cast<double>(node->allocated_memory.load(moss::MemoryOrder::Relaxed)) /
                                node->memory_size;

            if (utilization < min_utilization) {
                min_utilization = utilization;
                target_node = i;
            }
        }

        if (target_node == NUMA_NO_NODE) {
            return NUMAResult<numa_node_t>{NUMAError::NodeUnavailable};
        }

        return NUMAResult<numa_node_t>{target_node};
    }

    // 计算迁移数量
    static moss::kernel::usize calculate_migration_count_impl(const NUMABalancer* balancer,
                                                             numa_node_t from, numa_node_t to) noexcept {
        const NUMANode* source_node = balancer->topology_->get_node(from);
        const NUMANode* target_node = balancer->topology_->get_node(to);

        if (!source_node || !target_node) return 0;

        moss::kernel::usize source_allocated = source_node->allocated_memory.load(moss::MemoryOrder::Relaxed);
        moss::kernel::usize target_allocated = target_node->allocated_memory.load(moss::MemoryOrder::Relaxed);

        if (source_allocated <= target_allocated) return 0;

        // 迁移差值的一半，但不超过速率限制
        moss::kernel::usize difference = source_allocated - target_allocated;
        moss::kernel::usize migration_size = difference / 2;
        moss::kernel::usize migration_pages = migration_size / PAGE_SIZE;

        // 应用速率限制
        moss::kernel::u32 rate_limit = balancer->config_.migration_rate_limit;
        return moss::min(migration_pages, static_cast<moss::kernel::usize>(rate_limit));
    }

private:
    static moss::kernel::u64 get_current_time_us() noexcept {
        static moss::kernel::containers::AtomicU64 fake_time{0};
        return fake_time.fetch_add(1000, moss::MemoryOrder::Relaxed);
    }
};

} // namespace moss::kernel::mm
