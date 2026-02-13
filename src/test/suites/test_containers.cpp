#include "test_containers.hpp"

namespace moss::kernel::test {

using namespace moss::kernel::containers;

// ============================================================================
// 测试套件定义
// ============================================================================

MOSS_DEFINE_TEST_SUITE(containers);

// ============================================================================
// SPSC无锁队列测试实现
// ============================================================================

MOSS_TEST_FUNCTION(test_spsc_queue_basic_operations) {
    SPSCQueue<u32, 8> queue;

    // 测试初始状态
    MOSS_ASSERT_TRUE(queue.empty());
    MOSS_ASSERT_FALSE(queue.full());
    MOSS_ASSERT_EQ_U32(0, static_cast<u32>(static_cast<u32>(queue.approximate_size())));

    // 测试单个元素入队出队
    MOSS_ASSERT_TRUE(queue.try_enqueue(42U));
    MOSS_ASSERT_FALSE(queue.empty());
    MOSS_ASSERT_EQ_U32(1, static_cast<u32>(queue.approximate_size()));

    u32 result = 0;
    MOSS_ASSERT_TRUE(queue.try_dequeue(result));
    MOSS_ASSERT_EQ_U32(42, result);
    MOSS_ASSERT_TRUE(queue.empty());
    MOSS_ASSERT_EQ_U32(0, static_cast<u32>(static_cast<u32>(queue.approximate_size())));

    // 测试连续入队出队
    for (u32 i = 1; i <= 5; i++) {
        MOSS_ASSERT_TRUE(queue.try_enqueue(i));
    }

    for (u32 i = 1; i <= 5; i++) {
        MOSS_ASSERT_TRUE(queue.try_dequeue(result));
        MOSS_ASSERT_EQ_U32(i, result);
    }
}

MOSS_TEST_FUNCTION(test_spsc_queue_capacity_limits) {
    SPSCQueue<u32, 4> queue; // 容量为4的队列

    // 填满队列（4-1=3个元素，因为需要保留一个位置区分满和空）
    for (u32 i = 0; i < 3; i++) {
        MOSS_ASSERT_TRUE(queue.try_enqueue(i));
    }

    MOSS_ASSERT_TRUE(queue.full());

    // 尝试再入队应该失败
    MOSS_ASSERT_FALSE(queue.try_enqueue(999U));

    // 出队一个元素后应该能再入队
    u32 result;
    MOSS_ASSERT_TRUE(queue.try_dequeue(result));
    MOSS_ASSERT_EQ_U32(0, result);
    MOSS_ASSERT_FALSE(queue.full());

    MOSS_ASSERT_TRUE(queue.try_enqueue(100U));
}

MOSS_TEST_FUNCTION(test_spsc_queue_empty_full_states) {
    SPSCQueue<TestItem, 8> queue;

    // 空队列测试
    MOSS_ASSERT_TRUE(queue.empty());
    MOSS_ASSERT_FALSE(queue.full());

    TestItem item;
    MOSS_ASSERT_FALSE(queue.try_dequeue(item));

    // 填满队列测试
    for (u32 i = 0; i < 7; i++) { // 7个元素填满容量8的队列
        TestItem test_item(i, i * 100);
        MOSS_ASSERT_TRUE(queue.try_enqueue(test_item));
    }

    MOSS_ASSERT_TRUE(queue.full());
    MOSS_ASSERT_FALSE(queue.empty());

    TestItem overflow_item(999, 999);
    MOSS_ASSERT_FALSE(queue.try_enqueue(overflow_item));
}

MOSS_TEST_FUNCTION(test_spsc_queue_wraparound) {
    SPSCQueue<u32, 4> queue;

    // 填满队列
    for (u32 i = 0; i < 3; i++) {
        MOSS_ASSERT_TRUE(queue.try_enqueue(i));
    }

    // 出队所有元素
    u32 result;
    for (u32 i = 0; i < 3; i++) {
        MOSS_ASSERT_TRUE(queue.try_dequeue(result));
        MOSS_ASSERT_EQ_U32(i, result);
    }

    // 重新填满队列测试环绕
    for (u32 i = 100; i < 103; i++) {
        MOSS_ASSERT_TRUE(queue.try_enqueue(i));
    }

    // 验证环绕后的数据正确性
    for (u32 i = 100; i < 103; i++) {
        MOSS_ASSERT_TRUE(queue.try_dequeue(result));
        MOSS_ASSERT_EQ_U32(i, result);
    }
}

MOSS_TEST_FUNCTION(test_spsc_queue_move_semantics) {
    SPSCQueue<TestItem, 8> queue;

    TestItem original(42, 12345);
    TestItem moved_item = original;  // 复制以保留原始值

    // 测试移动语义入队
    MOSS_ASSERT_TRUE(queue.try_enqueue(static_cast<TestItem&&>(moved_item)));

    // 测试移动语义出队
    TestItem result;
    MOSS_ASSERT_TRUE(queue.try_dequeue(result));
    MOSS_ASSERT_EQ_U32(42, result.id);
    MOSS_ASSERT_EQ_U64(12345, result.value);
}

MOSS_TEST_FUNCTION(test_spsc_queue_producer_consumer_pattern) {
    SPSCQueue<u32, 16> queue;

    // 模拟生产者-消费者模式（在单线程环境中模拟）
    const u32 NUM_ITEMS = 10;

    // 生产者阶段
    for (u32 i = 0; i < NUM_ITEMS; i++) {
        MOSS_ASSERT_TRUE(queue.try_enqueue(i * 2));
    }

    MOSS_ASSERT_EQ_U32(NUM_ITEMS, static_cast<u32>(queue.approximate_size()));

    // 消费者阶段
    u32 consumed_count = 0;
    u32 result;
    while (queue.try_dequeue(result)) {
        MOSS_ASSERT_EQ_U32(consumed_count * 2, result);
        consumed_count++;
    }

    MOSS_ASSERT_EQ_U32(NUM_ITEMS, consumed_count);
    MOSS_ASSERT_TRUE(queue.empty());
}

MOSS_TEST_FUNCTION(test_spsc_queue_memory_ordering) {
    SPSCQueue<u64, 8> queue;

    // 测试内存顺序约束（基础验证）
    const u64 MAGIC_VALUE = 0xDEADBEEFCAFEBABE;

    MOSS_ASSERT_TRUE(queue.try_enqueue(MAGIC_VALUE));

    u64 result = 0;
    MOSS_ASSERT_TRUE(queue.try_dequeue(result));
    MOSS_ASSERT_EQ_U64(MAGIC_VALUE, result);
}

// ============================================================================
// Per-CPU数据结构测试实现
// ============================================================================

MOSS_TEST_FUNCTION(test_per_cpu_data_construction) {
    // 默认构造测试
    PerCpuData<u32> cpu_counters;

    // 验证每个CPU的数据都是默认初始化的
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        MOSS_ASSERT_EQ_U32(0, cpu_counters.get_cpu(cpu));
    }
}

MOSS_TEST_FUNCTION(test_per_cpu_data_access) {
    PerCpuData<u64> cpu_data;

    // 测试设置和获取
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        u64 expected_value = (cpu + 1) * 1000;
        cpu_data.get_cpu(cpu) = expected_value;
        MOSS_ASSERT_EQ_U64(expected_value, cpu_data.get_cpu(cpu));
    }

    // 验证CPU之间的数据独立性
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        u64 expected_value = (cpu + 1) * 1000;
        MOSS_ASSERT_EQ_U64(expected_value, cpu_data.get_cpu(cpu));
    }
}

MOSS_TEST_FUNCTION(test_per_cpu_data_initialization) {
    // 使用统一值构造
    const u32 INIT_VALUE = 42;
    PerCpuData<u32> initialized_data(INIT_VALUE);

    // 验证所有CPU都有相同的初始值
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        MOSS_ASSERT_EQ_U32(INIT_VALUE, initialized_data.get_cpu(cpu));
    }
}

MOSS_TEST_FUNCTION(test_per_cpu_data_cache_alignment) {
    PerCpuData<u32> cpu_data;

    // 基础验证：确保可以访问所有CPU的数据
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        cpu_data.get_cpu(cpu) = cpu * 10;
    }

    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        MOSS_ASSERT_EQ_U32(cpu * 10, cpu_data.get_cpu(cpu));
    }
}

MOSS_TEST_FUNCTION(test_per_cpu_data_isolation) {
    PerCpuData<TestItem> cpu_items;

    // 为每个CPU设置不同的数据
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        cpu_items.get_cpu(cpu) = TestItem(cpu, cpu * 100);
    }

    // 验证数据隔离：修改一个CPU的数据不影响其他CPU
    cpu_items.get_cpu(0).id = 9999;
    cpu_items.get_cpu(0).value = 8888;

    // CPU 0 应该有新值
    MOSS_ASSERT_EQ_U32(9999, cpu_items.get_cpu(0).id);
    MOSS_ASSERT_EQ_U64(8888, cpu_items.get_cpu(0).value);

    // 其他CPU应该保持原值
    for (u32 cpu = 1; cpu < MAX_CPUS; cpu++) {
        MOSS_ASSERT_EQ_U32(cpu, cpu_items.get_cpu(cpu).id);
        MOSS_ASSERT_EQ_U64(cpu * 100, cpu_items.get_cpu(cpu).value);
    }
}

MOSS_TEST_FUNCTION(test_per_cpu_data_aggregation) {
    PerCpuData<u32> cpu_counters;

    // 为每个CPU设置不同的计数值
    u32 total_expected = 0;
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        u32 value = (cpu + 1) * 10;
        cpu_counters.get_cpu(cpu) = value;
        total_expected += value;
    }

    // 聚合所有CPU的数据
    u32 total_actual = 0;
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        total_actual += cpu_counters.get_cpu(cpu);
    }

    MOSS_ASSERT_EQ_U32(total_expected, total_actual);
}

// ============================================================================
// 基础容器安全测试实现
// ============================================================================

MOSS_TEST_FUNCTION(test_basic_container_safety) {
    // 基础容器安全性测试
    // 验证容器组件基本功能正常，不会导致系统崩溃

    // 测试基础数据类型操作
    TestItem item1(1, 100);
    TestItem item2(2, 200);

    MOSS_ASSERT_EQ_U32(1, item1.id);
    MOSS_ASSERT_EQ_U64(100, item1.value);
    MOSS_ASSERT_EQ_U32(2, item2.id);
    MOSS_ASSERT_EQ_U64(200, item2.value);

    // 测试相等比较
    MOSS_ASSERT_TRUE(item1 == item1);
    MOSS_ASSERT_FALSE(item1 == item2);
}

// ============================================================================
// 原子类型测试实现
// ============================================================================

MOSS_TEST_FUNCTION(test_atomic_basic_operations) {
    AtomicU32 atomic_val(0);

    // 测试基本原子操作
    MOSS_ASSERT_EQ_U32(0, atomic_val.load(MemoryOrder::Relaxed));

    atomic_val.store(42, MemoryOrder::Relaxed);
    MOSS_ASSERT_EQ_U32(42, atomic_val.load(MemoryOrder::Relaxed));

    // 测试fetch_add
    u32 old_val = atomic_val.fetch_add(8, MemoryOrder::Relaxed);
    MOSS_ASSERT_EQ_U32(42, old_val);
    MOSS_ASSERT_EQ_U32(50, atomic_val.load(MemoryOrder::Relaxed));

    // 测试compare_exchange
    u32 expected = 50;
    bool success = atomic_val.compare_exchange_weak(expected, 100, MemoryOrder::Relaxed);
    MOSS_ASSERT_TRUE(success);
    MOSS_ASSERT_EQ_U32(100, atomic_val.load(MemoryOrder::Relaxed));
}

MOSS_TEST_FUNCTION(test_atomic_memory_ordering) {
    AtomicU64 atomic_val(0);

    // 测试不同的内存序
    atomic_val.store(12345, MemoryOrder::Release);
    u64 val = atomic_val.load(MemoryOrder::Acquire);
    MOSS_ASSERT_EQ_U64(12345, val);

    // 测试sequentially consistent ordering
    atomic_val.store(67890, MemoryOrder::SeqCst);
    val = atomic_val.load(MemoryOrder::SeqCst);
    MOSS_ASSERT_EQ_U64(67890, val);
}

MOSS_TEST_FUNCTION(test_atomic_ptr_operations) {
    TestItem item1(1, 100);
    TestItem item2(2, 200);

    AtomicPtr<TestItem> atomic_ptr(&item1);

    // 测试原子指针操作
    TestItem* ptr = atomic_ptr.load(MemoryOrder::Relaxed);
    MOSS_ASSERT_EQ_PTR(&item1, ptr);
    MOSS_ASSERT_EQ_U32(1, ptr->id);

    // 测试exchange
    TestItem* old_ptr = atomic_ptr.exchange(&item2, MemoryOrder::Relaxed);
    MOSS_ASSERT_EQ_PTR(&item1, old_ptr);

    ptr = atomic_ptr.load(MemoryOrder::Relaxed);
    MOSS_ASSERT_EQ_PTR(&item2, ptr);
    MOSS_ASSERT_EQ_U32(2, ptr->id);
}

MOSS_TEST_FUNCTION(test_cache_aligned_atomic) {
    CacheAlignedAtomic<u32> aligned_atomic(0);

    // 基本功能测试
    aligned_atomic.store(777, MemoryOrder::Relaxed);
    MOSS_ASSERT_EQ_U32(777, aligned_atomic.load(MemoryOrder::Relaxed));

    // 验证对齐（基础检查）
    uintptr_t addr = reinterpret_cast<uintptr_t>(&aligned_atomic);
    MOSS_ASSERT_EQ_U64(0, addr & (CACHE_LINE_SIZE - 1)); // 检查缓存行对齐
}

// ============================================================================
// 容器集成测试实现
// ============================================================================

MOSS_TEST_FUNCTION(test_containers_integration_scenario) {
    // 集成场景：模拟内核中容器的典型使用模式

    // 1. Per-CPU工作队列
    PerCpuData<SPSCQueue<u32, 16>> cpu_work_queues;

    // 2. 全局统计计数器
    PerCpuData<u32> cpu_counters;

    // 模拟多CPU工作分配
    const u32 WORK_ITEMS_PER_CPU = 5;

    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        // 每个CPU分配工作
        for (u32 i = 0; i < WORK_ITEMS_PER_CPU; i++) {
            u32 work_item = cpu * 1000 + i;
            MOSS_ASSERT_TRUE(cpu_work_queues.get_cpu(cpu).try_enqueue(work_item));
        }

        // 每个CPU处理工作
        u32 processed_items = 0;
        u32 work_item;
        while (cpu_work_queues.get_cpu(cpu).try_dequeue(work_item)) {
            processed_items++;
            // 更新Per-CPU计数器
            cpu_counters.get_cpu(cpu)++;
        }

        MOSS_ASSERT_EQ_U32(WORK_ITEMS_PER_CPU, processed_items);
        MOSS_ASSERT_EQ_U32(WORK_ITEMS_PER_CPU, cpu_counters.get_cpu(cpu));
    }

    // 聚合所有CPU的处理结果
    u32 total_processed = 0;
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        total_processed += cpu_counters.get_cpu(cpu);
    }

    MOSS_ASSERT_EQ_U32(MAX_CPUS * WORK_ITEMS_PER_CPU, total_processed);
}

MOSS_TEST_FUNCTION(test_containers_performance_baseline) {
    // 性能基线测试：确保容器在基本操作上的性能合理

    const u32 ITERATIONS = 1000;

    // SPSC队列性能基线
    SPSCQueue<u32, 1024> perf_queue;

    u64 start_time = get_test_timestamp_ns();

    // 批量入队
    for (u32 i = 0; i < ITERATIONS; i++) {
        MOSS_ASSERT_TRUE(perf_queue.try_enqueue(i));
    }

    // 批量出队
    u32 result;
    for (u32 i = 0; i < ITERATIONS; i++) {
        MOSS_ASSERT_TRUE(perf_queue.try_dequeue(result));
        MOSS_ASSERT_EQ_U32(i, result);
    }

    u64 end_time = get_test_timestamp_ns();
    u64 duration_ns = end_time - start_time;

    // 性能断言：确保操作在合理时间内完成
    // 这里设置一个宽松的限制，实际值需要根据硬件调整
    MOSS_ASSERT_TRUE(duration_ns < 10000000); // 10ms limit for 1000 operations
}

// ============================================================================
// 测试注册
// ============================================================================

// 注册所有容器测试
static bool register_container_tests() {
    // SPSC队列测试
    [[maybe_unused]] auto result1 = g_test_suite_containers.add_test("spsc_queue_basic_operations", test_spsc_queue_basic_operations);
    [[maybe_unused]] auto result2 = g_test_suite_containers.add_test("spsc_queue_capacity_limits", test_spsc_queue_capacity_limits);
    [[maybe_unused]] auto result3 = g_test_suite_containers.add_test("spsc_queue_empty_full_states", test_spsc_queue_empty_full_states);
    [[maybe_unused]] auto result4 = g_test_suite_containers.add_test("spsc_queue_wraparound", test_spsc_queue_wraparound);
    [[maybe_unused]] auto result5 = g_test_suite_containers.add_test("spsc_queue_move_semantics", test_spsc_queue_move_semantics);
    [[maybe_unused]] auto result6 = g_test_suite_containers.add_test("spsc_queue_producer_consumer", test_spsc_queue_producer_consumer_pattern);
    [[maybe_unused]] auto result7 = g_test_suite_containers.add_test("spsc_queue_memory_ordering", test_spsc_queue_memory_ordering);

    // Per-CPU数据测试
    [[maybe_unused]] auto result8 = g_test_suite_containers.add_test("per_cpu_data_construction", test_per_cpu_data_construction);
    [[maybe_unused]] auto result9 = g_test_suite_containers.add_test("per_cpu_data_access", test_per_cpu_data_access);
    [[maybe_unused]] auto result10 = g_test_suite_containers.add_test("per_cpu_data_initialization", test_per_cpu_data_initialization);
    [[maybe_unused]] auto result11 = g_test_suite_containers.add_test("per_cpu_data_cache_alignment", test_per_cpu_data_cache_alignment);
    [[maybe_unused]] auto result12 = g_test_suite_containers.add_test("per_cpu_data_isolation", test_per_cpu_data_isolation);
    [[maybe_unused]] auto result13 = g_test_suite_containers.add_test("per_cpu_data_aggregation", test_per_cpu_data_aggregation);

    // 基础容器安全测试
    [[maybe_unused]] auto result14 = g_test_suite_containers.add_test("basic_container_safety", test_basic_container_safety);
    [[maybe_unused]] auto result15 = g_test_suite_containers.add_test("atomic_basic_operations", test_atomic_basic_operations);
    [[maybe_unused]] auto result16 = g_test_suite_containers.add_test("atomic_memory_ordering", test_atomic_memory_ordering);
    [[maybe_unused]] auto result17 = g_test_suite_containers.add_test("atomic_ptr_operations", test_atomic_ptr_operations);
    [[maybe_unused]] auto result18 = g_test_suite_containers.add_test("cache_aligned_atomic", test_cache_aligned_atomic);

    // 集成测试
    [[maybe_unused]] auto result19 = g_test_suite_containers.add_test("containers_integration_scenario", test_containers_integration_scenario);
    [[maybe_unused]] auto result20 = g_test_suite_containers.add_test("containers_performance_baseline", test_containers_performance_baseline);

    return true;
}

// 全局自动注册
[[maybe_unused]] static bool container_tests_registered = register_container_tests();

// 手动注册套件到全局注册表（freestanding环境不能依赖全局构造器）
void register_container_test_suite() noexcept {
    // freestanding环境中，全局构造器可能不执行，需要手动初始化测试套件
    // 重新构造测试套件以确保正确初始化
    new (&g_test_suite_containers) TestSuite("containers");

    // 强制执行测试注册（freestanding环境中全局变量可能未初始化）
    register_container_tests();

    // 注册套件到全局注册表
    [[maybe_unused]] auto result = TestRegistry::get_instance().register_suite(&g_test_suite_containers);
}

} // namespace moss::kernel::test
