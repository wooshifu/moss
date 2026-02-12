#pragma once

// MOSS内核容器库单元测试套件
// 测试无锁队列、Per-CPU数据、Slab分配器等核心容器组件

#include "../framework/test_framework.hpp"
#include "../../containers/include/containers/lockfree_queue.hpp"
#include "../../containers/include/containers/per_cpu_data.hpp"
#include "../../containers/include/containers/atomic_types.hpp"

namespace moss::kernel::test {

// 测试数据类型
struct TestItem {
    u32 id;
    u64 value;
    char data[16];

    TestItem() : id(0), value(0), data{} {}
    TestItem(u32 i, u64 v) : id(i), value(v), data{} {}

    bool operator==(const TestItem& other) const noexcept {
        return id == other.id && value == other.value;
    }
};

// ============================================================================
// SPSC无锁队列测试
// ============================================================================

// 基本功能测试
MOSS_TEST_FUNCTION(test_spsc_queue_basic_operations);
MOSS_TEST_FUNCTION(test_spsc_queue_capacity_limits);
MOSS_TEST_FUNCTION(test_spsc_queue_empty_full_states);
MOSS_TEST_FUNCTION(test_spsc_queue_wraparound);
MOSS_TEST_FUNCTION(test_spsc_queue_move_semantics);

// 并发安全测试（模拟）
MOSS_TEST_FUNCTION(test_spsc_queue_producer_consumer_pattern);
MOSS_TEST_FUNCTION(test_spsc_queue_memory_ordering);

// ============================================================================
// Per-CPU数据结构测试
// ============================================================================

// 基本功能测试
MOSS_TEST_FUNCTION(test_per_cpu_data_construction);
MOSS_TEST_FUNCTION(test_per_cpu_data_access);
MOSS_TEST_FUNCTION(test_per_cpu_data_initialization);
MOSS_TEST_FUNCTION(test_per_cpu_data_cache_alignment);

// 多CPU模拟测试
MOSS_TEST_FUNCTION(test_per_cpu_data_isolation);
MOSS_TEST_FUNCTION(test_per_cpu_data_aggregation);

// ============================================================================
// 基础容器安全测试
// ============================================================================

// 基础容器操作测试
MOSS_TEST_FUNCTION(test_basic_container_safety);

// ============================================================================
// 原子类型测试
// ============================================================================

// 原子操作测试
MOSS_TEST_FUNCTION(test_atomic_basic_operations);
MOSS_TEST_FUNCTION(test_atomic_memory_ordering);
MOSS_TEST_FUNCTION(test_atomic_ptr_operations);

// 缓存对齐原子类型测试
MOSS_TEST_FUNCTION(test_cache_aligned_atomic);

// ============================================================================
// 容器集成测试
// ============================================================================

// 容器组合使用测试
MOSS_TEST_FUNCTION(test_containers_integration_scenario);
MOSS_TEST_FUNCTION(test_containers_performance_baseline);

// ============================================================================
// 测试套件声明和注册
// ============================================================================

// 声明容器测试套件
MOSS_DECLARE_TEST_SUITE(containers);

// 注册所有测试函数的声明将在.cpp文件中实现

} // namespace moss::kernel::test
