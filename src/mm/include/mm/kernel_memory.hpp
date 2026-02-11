#pragma once

// MOSS内核全局内存分配接口
// 提供kmalloc/kfree等标准内核内存分配接口，集成世界级内存管理系统

#include "mm_interface.hpp"
#include "../../../include/types.hpp"

// C++ 内核接口
namespace moss::kernel::mm {

// ============================================================================
// 全局内存分配接口 - C++风格
// ============================================================================

// 标准内核内存分配
inline void* kmalloc(moss::kernel::usize size) noexcept {
    auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::NONE));
    if (result.is_ok()) {
        return reinterpret_cast<void*>(*result);
    }
    return nullptr;
}

// 零初始化分配
inline void* kzalloc(moss::kernel::usize size) noexcept {
    auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::ZERO_MEMORY));
    if (result.is_ok()) {
        return reinterpret_cast<void*>(*result);
    }
    return nullptr;
}

// 对齐分配
inline void* kmalloc_aligned(moss::kernel::usize size, moss::kernel::usize alignment) noexcept {
    auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, alignment, AllocFlags::NONE));
    if (result.is_ok()) {
        return reinterpret_cast<void*>(*result);
    }
    return nullptr;
}

// 高优先级分配（原子分配，不会阻塞）
inline void* kmalloc_atomic(moss::kernel::usize size) noexcept {
    auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::ATOMIC));
    if (result.is_ok()) {
        return reinterpret_cast<void*>(*result);
    }
    return nullptr;
}

// NUMA本地分配
inline void* kmalloc_numa(moss::kernel::usize size, numa_node_t node) noexcept {
    MemoryRequest request(size, AllocFlags::NUMA_LOCAL);
    request.preferred_node = node;
    auto result = UnifiedMemoryManager::allocate(request);
    if (result.is_ok()) {
        return reinterpret_cast<void*>(*result);
    }
    return nullptr;
}

// 大页分配
inline void* kmalloc_huge(moss::kernel::usize size) noexcept {
    auto result = UnifiedMemoryManager::allocate(MemoryRequest(size, AllocFlags::HUGE_PAGES));
    if (result.is_ok()) {
        return reinterpret_cast<void*>(*result);
    }
    return nullptr;
}

// 内存释放
inline void kfree(void* ptr) noexcept {
    if (ptr) {
        (void)UnifiedMemoryManager::free(reinterpret_cast<moss::kernel::VirtAddr>(ptr));
    }
}

// 带大小的释放（更高效）
inline void kfree_sized(void* ptr, moss::kernel::usize size) noexcept {
    if (ptr) {
        (void)UnifiedMemoryManager::free(reinterpret_cast<moss::kernel::VirtAddr>(ptr), size);
    }
}

// 重新分配
inline void* krealloc(void* ptr, moss::kernel::usize old_size, moss::kernel::usize new_size) noexcept {
    if (!ptr) {
        return kmalloc(new_size);
    }

    if (new_size == 0) {
        kfree_sized(ptr, old_size);
        return nullptr;
    }

    moss::kernel::VirtAddr addr = reinterpret_cast<moss::kernel::VirtAddr>(ptr);
    auto result = UnifiedMemoryManager::reallocate(addr, old_size, new_size);
    if (result.is_ok()) {
        return reinterpret_cast<void*>(addr);
    }
    return nullptr;
}

// ============================================================================
// 内存管理系统初始化和控制
// ============================================================================

// 初始化内存管理系统
inline bool initialize_kernel_memory() noexcept {
    // 配置统一内存管理系统
    UnifiedMemoryManager::SystemConfig config = {};

    // 配置Vmalloc
    config.vmalloc_config = {
        .enable_lazy_free = true,
        .lazy_free_threshold = 64 * 1024,  // 64KB
        .max_lazy_free_memory = 16 * 1024 * 1024,  // 16MB
        .enable_numa_awareness = true,
        .default_numa_policy = static_cast<moss::kernel::u32>(NUMAPolicy::DEFAULT)
    };

    // 配置内存回收
    config.reclaim_config = {
        .default_policy = ReclaimPolicy::BALANCED,
        .min_free_pages = 1024,  // 最小空闲页面数
        .target_free_pages = 4096,  // 目标空闲页面数
        .scan_config = {},  // 使用默认扫描配置
        .workingset_config = {}  // 使用默认工作集配置
    };

    // 配置内存压缩
    config.compaction_config = {
        .scanner_config = {},  // 使用默认扫描配置
        .migrator_config = {}, // 使用默认迁移配置
        .cma_config = {},      // 使用默认CMA配置
        .default_strategy = CompactionStrategy::MEDIUM,
        .fragmentation_threshold = 70.0,  // 70%碎片率触发
        .min_free_memory_threshold = 1024 * 1024,  // 1MB阈值
        .compaction_interval_ms = 5000     // 5秒间隔
    };

    // 配置NUMA
    config.numa_config = {
        .balancer_config = {
            .balance_interval_ms = 2000,
            .imbalance_threshold = 25,  // 25%
            .migration_rate_limit = 1000,  // 1000 pages/sec
            .memory_threshold_ratio = 0.8
        },
        .enable_auto_balancing = true,
        .enable_migration = true,
        .default_policy = NUMAPolicy::DEFAULT
    };

    // 配置大页
    config.hugepages_config = {
        .pool_config = {
            .pool_sizes = {128, 32, 16, 4},  // 每种大页的池大小
            .min_free_ratio = 20,
            .max_free_ratio = 40,
            .numa_aware = true,
            .dynamic_resizing = true
        },
        .thp_config = {
            .policy = THPPolicy::DEFER,
            .split_policy = SplitPolicy::LAZY,
            .defrag_policy = DefragPolicy::DEFER,
            .scan_interval_ms = 10000,
            .defrag_threshold = 50,
            .khugepaged_enabled = true
        },
        .hugetlb_config = {
            .default_huge_page_size = HUGE_PAGE_2MB,
            .max_huge_pages = 1024,
            .reserve_huge_pages = 256,
            .overcommit_enabled = false,
            .shared_memory_enabled = true
        },
        .enable_thp = true,
        .enable_hugetlb = true,
        .default_huge_page_size = HUGE_PAGE_2MB
    };

    // 配置监控系统
    config.monitoring_config = {
        .collector_config = {
            .enable_detailed_stats = true,
            .enable_performance_profiling = true,
            .enable_leak_detection = true,
            .sampling_interval_ms = 500,
            .history_depth = 16,
            .max_tracked_allocations = 1024
        },
        .detector_config = {
            .enable_stack_trace = false,  // 简化版本暂时禁用
            .enable_caller_tracking = true,
            .leak_check_interval_ms = 30000,  // 30秒
            .leak_threshold_minutes = 10,
            .min_leak_size = 1024
        },
        .profiler_config = {
            .enable_latency_profiling = true,
            .enable_throughput_profiling = true,
            .enable_pattern_analysis = true,
            .profiling_window_ms = 60000,  // 60秒
            .sample_rate = 100  // 1/100采样
        },
        .enable_real_time_alerts = true,
        .alert_check_interval_ms = 5000
    };

    // 全局配置
    config.enable_aggressive_optimization = true;
    config.enable_background_operations = true;
    config.background_interval_ms = 1000;
    config.memory_pressure_threshold = 80;
    config.min_free_memory = 128 * 1024 * 1024;  // 128MB

    auto result = UnifiedMemoryManager::initialize_system(config);
    return result.is_ok();
}

// 关闭内存管理系统
inline void shutdown_kernel_memory() noexcept {
    UnifiedMemoryManager::shutdown_system();
}

// 获取内存系统状态
inline bool is_memory_system_healthy() noexcept {
    return UnifiedMemoryManager::is_system_healthy();
}

// 触发内存回收
inline void trigger_memory_gc() noexcept {
    (void)UnifiedMemoryManager::trigger_memory_reclaim();
}

// 获取内存压力
inline MemoryPressure get_memory_pressure() noexcept {
    return UnifiedMemoryManager::get_memory_pressure();
}

// 获取内存统计信息
inline UnifiedMemoryManager::SystemPerformanceStats get_memory_stats() noexcept {
    return UnifiedMemoryManager::get_performance_stats();
}

// ============================================================================
// 调试和诊断接口
// ============================================================================

// 打印内存统计
inline void print_memory_stats() noexcept {
    [[maybe_unused]] auto stats = get_memory_stats();

    // 简化的统计输出（实际应该使用格式化输出）
    // 这里模拟输出到调试端口
    volatile moss::kernel::u32* uart_data = reinterpret_cast<volatile moss::kernel::u32*>(0x09000000);

    const char* msg = "\n=== MEMORY SYSTEM STATS ===\n";
    while (*msg) {
        *uart_data = static_cast<moss::kernel::u32>(*msg);
        msg++;
    }

    // 实际实现应该使用正确的格式化函数输出stats内容
}

// 检查内存泄漏
inline void check_memory_leaks() noexcept {
    auto leak_result = UnifiedMemoryManager::generate_leak_report();
    if (leak_result.is_ok()) {
        auto report = *leak_result;
        if (report.total_leaked_bytes > 0) {
            // 发现内存泄漏，输出警告
            volatile moss::kernel::u32* uart_data = reinterpret_cast<volatile moss::kernel::u32*>(0x09000000);
            const char* warning = "\n⚠️ MEMORY LEAKS DETECTED!\n";
            while (*warning) {
                *uart_data = static_cast<moss::kernel::u32>(*warning);
                warning++;
            }
        }
    }
}

} // namespace moss::kernel::mm

// ============================================================================
// C风格兼容接口 (供全局使用)
// ============================================================================

extern "C" {
    // 初始化内核内存系统
    bool moss_memory_init(void) noexcept;

    // 关闭内存系统
    void moss_memory_shutdown(void) noexcept;

    // 标准内存分配接口
    void* moss_kmalloc(moss::kernel::usize size) noexcept;
    void* moss_kzalloc(moss::kernel::usize size) noexcept;
    void* moss_kmalloc_aligned(moss::kernel::usize size, moss::kernel::usize alignment) noexcept;
    void* moss_kmalloc_atomic(moss::kernel::usize size) noexcept;
    void moss_kfree(void* ptr) noexcept;
    void moss_kfree_sized(void* ptr, moss::kernel::usize size) noexcept;
    void* moss_krealloc(void* ptr, moss::kernel::usize old_size, moss::kernel::usize new_size) noexcept;

    // 内存系统状态
    int moss_memory_is_healthy(void) noexcept;
    int moss_memory_get_pressure(void) noexcept;
    void moss_memory_gc(void) noexcept;
    void moss_memory_check_leaks(void) noexcept;
    void moss_memory_print_stats(void) noexcept;
}

// ============================================================================
// 注意：全局new/delete操作符已在runtime_support.cpp中定义
// 这里提供内核内存管理的具体实现接口，供现有的全局操作符调用
// ============================================================================
