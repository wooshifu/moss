#include "test_memory.hpp"

namespace moss::kernel::test {

// ============================================================================
// 测试套件定义
// ============================================================================

MOSS_DEFINE_TEST_SUITE(memory);

// ============================================================================
// 基础内存操作测试实现
// ============================================================================

MOSS_TEST_FUNCTION(test_memory_basic_operations) {
    // 测试基础内存操作概念
    TestMemoryBlock block1;
    TestMemoryBlock block2(reinterpret_cast<void*>(0x1000), 4096, 16);

    // 验证默认初始化
    MOSS_ASSERT_NULL(block1.ptr);
    MOSS_ASSERT_EQ_U64(0, static_cast<u64>(block1.size));
    MOSS_ASSERT_EQ_U64(0, static_cast<u64>(block1.alignment));
    MOSS_ASSERT_FALSE(block1.is_valid);

    // 验证带参数初始化
    MOSS_ASSERT_NOT_NULL(block2.ptr);
    MOSS_ASSERT_EQ_U64(4096, static_cast<u64>(block2.size));
    MOSS_ASSERT_EQ_U64(16, static_cast<u64>(block2.alignment));
    MOSS_ASSERT_TRUE(block2.is_valid);
}

MOSS_TEST_FUNCTION(test_memory_alignment_checks) {
    // 测试内存对齐检查算法
    struct AlignmentTester {
        static bool is_aligned(uintptr_t addr, usize alignment) {
            if (alignment == 0) return false;
            return (addr & (alignment - 1)) == 0;
        }

        static uintptr_t align_up(uintptr_t addr, usize alignment) {
            return (addr + alignment - 1) & ~(alignment - 1);
        }

        static uintptr_t align_down(uintptr_t addr, usize alignment) {
            return addr & ~(alignment - 1);
        }
    };

    // 测试基本对齐检查
    MOSS_ASSERT_TRUE(AlignmentTester::is_aligned(0x1000, 16));   // 对齐
    MOSS_ASSERT_FALSE(AlignmentTester::is_aligned(0x1001, 16)); // 不对齐
    MOSS_ASSERT_TRUE(AlignmentTester::is_aligned(0x2000, 4096)); // 页对齐

    // 测试向上对齐
    MOSS_ASSERT_EQ_U64(0x1000, AlignmentTester::align_up(0x0FF1, 16));
    MOSS_ASSERT_EQ_U64(0x2000, AlignmentTester::align_up(0x1001, 4096));

    // 测试向下对齐
    MOSS_ASSERT_EQ_U64(0x0FF0, AlignmentTester::align_down(0x0FF9, 16));
    MOSS_ASSERT_EQ_U64(0x1000, AlignmentTester::align_down(0x1FFF, 4096));

    // 测试TestMemoryBlock的对齐检查
    TestMemoryBlock aligned_block(reinterpret_cast<void*>(0x1000), 256, 16);
    TestMemoryBlock unaligned_block(reinterpret_cast<void*>(0x1001), 256, 16);

    MOSS_ASSERT_TRUE(aligned_block.is_aligned());
    MOSS_ASSERT_FALSE(unaligned_block.is_aligned());
}

MOSS_TEST_FUNCTION(test_memory_zero_fill) {
    // 测试内存清零操作（概念验证）
    char test_buffer[64];

    // 模拟memset零填充
    for (usize i = 0; i < sizeof(test_buffer); i++) {
        test_buffer[i] = static_cast<char>(i & 0xFF); // 填充模式
    }

    // 验证非零状态
    bool has_non_zero = false;
    for (usize i = 0; i < sizeof(test_buffer); i++) {
        if (test_buffer[i] != 0) {
            has_non_zero = true;
            break;
        }
    }
    MOSS_ASSERT_TRUE(has_non_zero);

    // 模拟清零操作
    for (usize i = 0; i < sizeof(test_buffer); i++) {
        test_buffer[i] = 0;
    }

    // 验证清零结果
    bool all_zero = true;
    for (usize i = 0; i < sizeof(test_buffer); i++) {
        if (test_buffer[i] != 0) {
            all_zero = false;
            break;
        }
    }
    MOSS_ASSERT_TRUE(all_zero);
}

MOSS_TEST_FUNCTION(test_memory_copy_operations) {
    // 测试内存复制操作概念
    char source[32] = "Hello MOSS Kernel Testing!";
    char destination[32] = {0};

    // 手动实现memcpy概念
    for (usize i = 0; source[i] != '\0' && i < sizeof(destination) - 1; i++) {
        destination[i] = source[i];
    }

    // 验证复制结果
    bool copy_success = true;
    for (usize i = 0; source[i] != '\0' && i < sizeof(source); i++) {
        if (source[i] != destination[i]) {
            copy_success = false;
            break;
        }
    }
    MOSS_ASSERT_TRUE(copy_success);
}

// ============================================================================
// 内存安全和边界检查测试实现
// ============================================================================

MOSS_TEST_FUNCTION(test_memory_boundary_checks) {
    // 测试内存边界检查算法
    struct BoundaryChecker {
        static bool is_in_range(uintptr_t addr, uintptr_t start, usize size) {
            return addr >= start && addr < (start + size);
        }

        static bool ranges_overlap(uintptr_t start1, usize size1, uintptr_t start2, usize size2) {
            uintptr_t end1 = start1 + size1;
            uintptr_t end2 = start2 + size2;
            return !(end1 <= start2 || end2 <= start1);
        }
    };

    const uintptr_t BASE_ADDR = 0x40000000;
    const usize REGION_SIZE = 0x10000;

    // 测试范围检查
    MOSS_ASSERT_TRUE(BoundaryChecker::is_in_range(BASE_ADDR + 0x1000, BASE_ADDR, REGION_SIZE));
    MOSS_ASSERT_FALSE(BoundaryChecker::is_in_range(BASE_ADDR - 1, BASE_ADDR, REGION_SIZE));
    MOSS_ASSERT_FALSE(BoundaryChecker::is_in_range(BASE_ADDR + REGION_SIZE, BASE_ADDR, REGION_SIZE));

    // 测试重叠检查
    MOSS_ASSERT_TRUE(BoundaryChecker::ranges_overlap(0x1000, 0x1000, 0x1800, 0x1000));  // 重叠
    MOSS_ASSERT_FALSE(BoundaryChecker::ranges_overlap(0x1000, 0x1000, 0x2000, 0x1000)); // 不重叠
}

MOSS_TEST_FUNCTION(test_memory_null_pointer_safety) {
    // 测试空指针安全检查
    TestMemoryBlock null_block;
    TestMemoryBlock valid_block(reinterpret_cast<void*>(0x1000), 256, 8);

    // 验证空指针检测
    MOSS_ASSERT_FALSE(null_block.is_valid);
    MOSS_ASSERT_FALSE(null_block.is_aligned());

    // 验证有效指针
    MOSS_ASSERT_TRUE(valid_block.is_valid);
    MOSS_ASSERT_TRUE(valid_block.is_aligned());
}

MOSS_TEST_FUNCTION(test_memory_overflow_protection) {
    // 测试溢出保护算法
    const usize MAX_SIZE = static_cast<usize>(-1);
    struct OverflowChecker {
        static bool will_overflow_add(usize a, usize b, usize max_size) {
            return a > (max_size - b);
        }

        static bool will_overflow_mul(usize a, usize b, usize max_size) {
            if (a == 0 || b == 0) return false;
            return a > (max_size / b);
        }
    };

    // 测试加法溢出检查
    MOSS_ASSERT_FALSE(OverflowChecker::will_overflow_add(100, 200, MAX_SIZE));
    MOSS_ASSERT_TRUE(OverflowChecker::will_overflow_add(MAX_SIZE, 1, MAX_SIZE));
    MOSS_ASSERT_TRUE(OverflowChecker::will_overflow_add(MAX_SIZE - 5, 10, MAX_SIZE));

    // 测试乘法溢出检查
    MOSS_ASSERT_FALSE(OverflowChecker::will_overflow_mul(100, 200, MAX_SIZE));
    MOSS_ASSERT_FALSE(OverflowChecker::will_overflow_mul(0, MAX_SIZE, MAX_SIZE));
    MOSS_ASSERT_TRUE(OverflowChecker::will_overflow_mul(MAX_SIZE / 2, 3, MAX_SIZE));
}

MOSS_TEST_FUNCTION(test_memory_size_validation) {
    // 测试大小验证算法
    struct SizeValidator {
        static bool is_valid_size(usize size, usize min_size, usize max_size) {
            return size >= min_size && size <= max_size;
        }

        static bool is_power_of_two(usize size) {
            return size > 0 && (size & (size - 1)) == 0;
        }
    };

    // 测试大小范围验证
    MOSS_ASSERT_TRUE(SizeValidator::is_valid_size(1024, 512, 2048));
    MOSS_ASSERT_FALSE(SizeValidator::is_valid_size(256, 512, 2048));
    MOSS_ASSERT_FALSE(SizeValidator::is_valid_size(4096, 512, 2048));

    // 测试2的幂检查
    MOSS_ASSERT_TRUE(SizeValidator::is_power_of_two(1));
    MOSS_ASSERT_TRUE(SizeValidator::is_power_of_two(16));
    MOSS_ASSERT_TRUE(SizeValidator::is_power_of_two(4096));
    MOSS_ASSERT_FALSE(SizeValidator::is_power_of_two(0));
    MOSS_ASSERT_FALSE(SizeValidator::is_power_of_two(3));
    MOSS_ASSERT_FALSE(SizeValidator::is_power_of_two(1000));
}

// ============================================================================
// 内存工具函数测试实现
// ============================================================================

MOSS_TEST_FUNCTION(test_memory_compare_functions) {
    // 测试内存比较算法
    struct MemoryComparator {
        static int compare(const void* a, const void* b, usize size) {
            const unsigned char* pa = static_cast<const unsigned char*>(a);
            const unsigned char* pb = static_cast<const unsigned char*>(b);

            for (usize i = 0; i < size; i++) {
                if (pa[i] < pb[i]) return -1;
                if (pa[i] > pb[i]) return 1;
            }
            return 0;
        }
    };

    char buffer1[] = "MOSS";
    char buffer2[] = "MOSS";
    char buffer3[] = "moss";

    // 测试相等比较
    MOSS_ASSERT_EQ_U32(0, static_cast<u32>(MemoryComparator::compare(buffer1, buffer2, 4)));

    // 测试不等比较（ASCII: 'M' < 'm'）
    int result = MemoryComparator::compare(buffer1, buffer3, 4);
    MOSS_ASSERT_TRUE(result < 0);
}

MOSS_TEST_FUNCTION(test_memory_search_functions) {
    // 测试内存搜索算法
    struct MemorySearcher {
        static void* find_byte(void* haystack, int needle, usize size) {
            unsigned char* ptr = static_cast<unsigned char*>(haystack);
            unsigned char target = static_cast<unsigned char>(needle);

            for (usize i = 0; i < size; i++) {
                if (ptr[i] == target) {
                    return &ptr[i];
                }
            }
            return nullptr;
        }
    };

    char test_data[] = "Hello MOSS Kernel!";

    // 搜索存在的字符
    void* found_M = MemorySearcher::find_byte(test_data, 'M', sizeof(test_data));
    MOSS_ASSERT_NOT_NULL(found_M);

    // 搜索不存在的字符
    void* found_Z = MemorySearcher::find_byte(test_data, 'Z', sizeof(test_data));
    MOSS_ASSERT_NULL(found_Z);
}

MOSS_TEST_FUNCTION(test_memory_pattern_fill) {
    // 测试内存模式填充
    struct PatternFiller {
        static void fill_pattern(void* dest, u32 pattern, usize size) {
            unsigned char* ptr = static_cast<unsigned char*>(dest);
            for (usize i = 0; i < size; i++) {
                ptr[i] = static_cast<unsigned char>((pattern >> ((i % 4) * 8)) & 0xFF);
            }
        }
    };

    char buffer[16] = {0};
    const u32 PATTERN = 0xDEADBEEF;

    PatternFiller::fill_pattern(buffer, PATTERN, sizeof(buffer));

    // 验证模式填充结果
    bool pattern_correct = true;
    for (usize i = 0; i < sizeof(buffer); i++) {
        unsigned char expected = static_cast<unsigned char>((PATTERN >> ((i % 4) * 8)) & 0xFF);
        if (buffer[i] != static_cast<char>(expected)) {
            pattern_correct = false;
            break;
        }
    }
    MOSS_ASSERT_TRUE(pattern_correct);
}

// ============================================================================
// 虚拟内存概念测试实现
// ============================================================================

MOSS_TEST_FUNCTION(test_virtual_address_validation) {
    // 测试虚拟地址验证算法
    const uintptr_t KERNEL_BASE = 0x40000000;
    const uintptr_t USER_BASE = 0x10000000;
    const uintptr_t USER_LIMIT = 0x80000000;

    // 测试内核地址识别
    MOSS_ASSERT_TRUE(0x40000000 >= KERNEL_BASE);
    MOSS_ASSERT_TRUE(0x80000000 >= KERNEL_BASE);
    MOSS_ASSERT_FALSE(0x20000000 >= KERNEL_BASE);

    // 测试用户地址识别
    MOSS_ASSERT_TRUE(0x10000000 >= USER_BASE && 0x10000000 < USER_LIMIT);
    MOSS_ASSERT_TRUE(0x60000000 >= USER_BASE && 0x60000000 < USER_LIMIT);
    MOSS_ASSERT_FALSE(0x90000000 >= USER_BASE && 0x90000000 < USER_LIMIT);

    // 测试地址有效性
    MOSS_ASSERT_TRUE((0x40000000 >= KERNEL_BASE) || (0x40000000 >= USER_BASE && 0x40000000 < USER_LIMIT));
    MOSS_ASSERT_TRUE((0x20000000 >= KERNEL_BASE) || (0x20000000 >= USER_BASE && 0x20000000 < USER_LIMIT));
    MOSS_ASSERT_FALSE((0x05000000 >= KERNEL_BASE) || (0x05000000 >= USER_BASE && 0x05000000 < USER_LIMIT));
}

MOSS_TEST_FUNCTION(test_physical_address_conversion) {
    // 测试物理地址转换算法概念
    const uintptr_t KERNEL_VIRTUAL_BASE = 0x40000000;
    const uintptr_t PHYSICAL_BASE = 0x00000000;

    // 测试虚拟到物理地址转换
    uintptr_t virt_addr = 0x40001000;
    uintptr_t phys_addr = 0;

    if (virt_addr >= KERNEL_VIRTUAL_BASE) {
        phys_addr = virt_addr - KERNEL_VIRTUAL_BASE + PHYSICAL_BASE;
    }
    MOSS_ASSERT_EQ_U64(0x00001000, phys_addr);

    // 测试物理到虚拟地址转换
    uintptr_t converted_back = phys_addr - PHYSICAL_BASE + KERNEL_VIRTUAL_BASE;
    MOSS_ASSERT_EQ_U64(virt_addr, converted_back);

    // 测试无效地址转换
    uintptr_t invalid_addr = 0x20000000;
    uintptr_t invalid_conversion = 0;
    if (invalid_addr >= KERNEL_VIRTUAL_BASE) {
        invalid_conversion = invalid_addr - KERNEL_VIRTUAL_BASE + PHYSICAL_BASE;
    }
    MOSS_ASSERT_EQ_U64(0, invalid_conversion);
}

MOSS_TEST_FUNCTION(test_page_boundary_calculations) {
    // 测试页边界计算
    const usize PAGE_SIZE = 4096;
    const usize PAGE_MASK = PAGE_SIZE - 1;

    // 测试页对齐
    uintptr_t page_align_down = 0x40000001 & ~PAGE_MASK;
    uintptr_t page_align_up = (0x40000001 + PAGE_MASK) & ~PAGE_MASK;
    MOSS_ASSERT_EQ_U64(0x40000000, page_align_down);
    MOSS_ASSERT_EQ_U64(0x40001000, page_align_up);

    // 测试页数计算
    usize pages_100 = (100 + PAGE_MASK) / PAGE_SIZE;
    usize pages_4096 = (4096 + PAGE_MASK) / PAGE_SIZE;
    usize pages_4097 = (4097 + PAGE_MASK) / PAGE_SIZE;
    MOSS_ASSERT_EQ_U64(1, static_cast<u64>(pages_100));
    MOSS_ASSERT_EQ_U64(1, static_cast<u64>(pages_4096));
    MOSS_ASSERT_EQ_U64(2, static_cast<u64>(pages_4097));

    // 测试页对齐检查
    bool aligned_0x40000000 = (0x40000000 & PAGE_MASK) == 0;
    bool aligned_0x40001000 = (0x40001000 & PAGE_MASK) == 0;
    bool aligned_0x40000001 = (0x40000001 & PAGE_MASK) == 0;
    MOSS_ASSERT_TRUE(aligned_0x40000000);
    MOSS_ASSERT_TRUE(aligned_0x40001000);
    MOSS_ASSERT_FALSE(aligned_0x40000001);
}

// ============================================================================
// 内存统计和监控测试实现
// ============================================================================

MOSS_TEST_FUNCTION(test_memory_usage_tracking) {
    // 测试内存使用跟踪算法
    struct MemoryTracker {
        usize total_allocated;
        usize total_freed;
        usize peak_usage;
        usize current_usage;

        MemoryTracker() : total_allocated(0), total_freed(0), peak_usage(0), current_usage(0) {}

        void allocate(usize size) {
            total_allocated += size;
            current_usage += size;
            if (current_usage > peak_usage) {
                peak_usage = current_usage;
            }
        }

        void free(usize size) {
            total_freed += size;
            if (current_usage >= size) {
                current_usage -= size;
            }
        }
    };

    MemoryTracker tracker;

    // 模拟内存分配和释放
    tracker.allocate(1024);
    tracker.allocate(2048);
    MOSS_ASSERT_EQ_U64(3072, static_cast<u64>(tracker.current_usage));
    MOSS_ASSERT_EQ_U64(3072, static_cast<u64>(tracker.peak_usage));

    tracker.free(1024);
    MOSS_ASSERT_EQ_U64(2048, static_cast<u64>(tracker.current_usage));
    MOSS_ASSERT_EQ_U64(3072, static_cast<u64>(tracker.peak_usage)); // 峰值保持不变

    tracker.free(2048);
    MOSS_ASSERT_EQ_U64(0, static_cast<u64>(tracker.current_usage));
    MOSS_ASSERT_EQ_U64(3072, static_cast<u64>(tracker.total_allocated));
    MOSS_ASSERT_EQ_U64(3072, static_cast<u64>(tracker.total_freed));
}

MOSS_TEST_FUNCTION(test_memory_pressure_detection) {
    // 测试内存压力检测算法
    struct MemoryPressureDetector {
        enum class PressureLevel {
            Low = 0,
            Medium = 1,
            High = 2,
            Critical = 3
        };

        static PressureLevel detect_pressure(usize used, usize total) {
            if (total == 0) return PressureLevel::Critical;

            u32 usage_percent = static_cast<u32>((used * 100) / total);

            if (usage_percent >= 95) return PressureLevel::Critical;
            if (usage_percent >= 85) return PressureLevel::High;
            if (usage_percent >= 70) return PressureLevel::Medium;
            return PressureLevel::Low;
        }
    };

    const usize TOTAL_MEMORY = 1024 * 1024; // 1MB

    // 测试不同压力级别
    auto low_pressure = MemoryPressureDetector::detect_pressure(TOTAL_MEMORY * 50 / 100, TOTAL_MEMORY);
    MOSS_ASSERT_EQ_U32(0, static_cast<u32>(low_pressure));

    auto medium_pressure = MemoryPressureDetector::detect_pressure(TOTAL_MEMORY * 75 / 100, TOTAL_MEMORY);
    MOSS_ASSERT_EQ_U32(1, static_cast<u32>(medium_pressure));

    auto high_pressure = MemoryPressureDetector::detect_pressure(TOTAL_MEMORY * 90 / 100, TOTAL_MEMORY);
    MOSS_ASSERT_EQ_U32(2, static_cast<u32>(high_pressure));

    auto critical_pressure = MemoryPressureDetector::detect_pressure(TOTAL_MEMORY * 96 / 100, TOTAL_MEMORY);
    MOSS_ASSERT_EQ_U32(3, static_cast<u32>(critical_pressure));
}

// ============================================================================
// 测试注册
// ============================================================================

// 注册所有内存管理测试
static bool register_memory_tests() {
    // 基础内存操作测试
    [[maybe_unused]] auto result1 = g_test_suite_memory.add_test("memory_basic_operations", test_memory_basic_operations);
    [[maybe_unused]] auto result2 = g_test_suite_memory.add_test("memory_alignment_checks", test_memory_alignment_checks);
    [[maybe_unused]] auto result3 = g_test_suite_memory.add_test("memory_zero_fill", test_memory_zero_fill);
    [[maybe_unused]] auto result4 = g_test_suite_memory.add_test("memory_copy_operations", test_memory_copy_operations);

    // 内存安全和边界检查测试
    [[maybe_unused]] auto result5 = g_test_suite_memory.add_test("memory_boundary_checks", test_memory_boundary_checks);
    [[maybe_unused]] auto result6 = g_test_suite_memory.add_test("memory_null_pointer_safety", test_memory_null_pointer_safety);
    [[maybe_unused]] auto result7 = g_test_suite_memory.add_test("memory_overflow_protection", test_memory_overflow_protection);
    [[maybe_unused]] auto result8 = g_test_suite_memory.add_test("memory_size_validation", test_memory_size_validation);

    // 内存工具函数测试
    [[maybe_unused]] auto result9 = g_test_suite_memory.add_test("memory_compare_functions", test_memory_compare_functions);
    [[maybe_unused]] auto result10 = g_test_suite_memory.add_test("memory_search_functions", test_memory_search_functions);
    [[maybe_unused]] auto result11 = g_test_suite_memory.add_test("memory_pattern_fill", test_memory_pattern_fill);

    // 虚拟内存概念测试
    [[maybe_unused]] auto result12 = g_test_suite_memory.add_test("virtual_address_validation", test_virtual_address_validation);
    [[maybe_unused]] auto result13 = g_test_suite_memory.add_test("physical_address_conversion", test_physical_address_conversion);
    [[maybe_unused]] auto result14 = g_test_suite_memory.add_test("page_boundary_calculations", test_page_boundary_calculations);

    // 内存统计和监控测试
    [[maybe_unused]] auto result15 = g_test_suite_memory.add_test("memory_usage_tracking", test_memory_usage_tracking);
    [[maybe_unused]] auto result16 = g_test_suite_memory.add_test("memory_pressure_detection", test_memory_pressure_detection);

    return true;
}

// 全局自动注册
[[maybe_unused]] static bool memory_tests_registered = register_memory_tests();

// 手动注册套件到全局注册表（freestanding环境不能依赖全局构造器）
void register_memory_test_suite() noexcept {
    // freestanding环境中，全局构造器可能不执行，需要手动初始化测试套件
    // 重新构造测试套件以确保正确初始化
    new (&g_test_suite_memory) TestSuite("memory");

    // 强制执行测试注册（freestanding环境中全局变量可能未初始化）
    register_memory_tests();

    // 注册套件到全局注册表
    [[maybe_unused]] auto result = TestRegistry::get_instance().register_suite(&g_test_suite_memory);
}

} // namespace moss::kernel::test
