#pragma once

// MOSS内核内存管理单元测试套件
// 测试内存分配、对齐、虚拟内存等核心内存管理功能

#include "../framework/test_framework.hpp"
#include "../framework/test_registry.hpp"

namespace moss::kernel::test {

// 手动注册函数声明
void register_memory_test_suite() noexcept;

// 测试用内存数据类型
struct TestMemoryBlock {
    void* ptr;
    usize size;
    usize alignment;
    bool is_valid;

    TestMemoryBlock() : ptr(nullptr), size(0), alignment(0), is_valid(false) {}
    TestMemoryBlock(void* p, usize s, usize a) : ptr(p), size(s), alignment(a), is_valid(p != nullptr) {}

    bool operator==(const TestMemoryBlock& other) const noexcept {
        return ptr == other.ptr && size == other.size && alignment == other.alignment;
    }

    // 检查指针对齐
    bool is_aligned() const noexcept {
        if (ptr == nullptr || alignment == 0) return false;
        return (reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)) == 0;
    }
};

// ============================================================================
// 基础内存操作测试
// ============================================================================

// 内存基础操作测试
MOSS_TEST_FUNCTION(test_memory_basic_operations);
MOSS_TEST_FUNCTION(test_memory_alignment_checks);
MOSS_TEST_FUNCTION(test_memory_zero_fill);
MOSS_TEST_FUNCTION(test_memory_copy_operations);

// ============================================================================
// 内存安全和边界检查测试
// ============================================================================

// 内存安全测试
MOSS_TEST_FUNCTION(test_memory_boundary_checks);
MOSS_TEST_FUNCTION(test_memory_null_pointer_safety);
MOSS_TEST_FUNCTION(test_memory_overflow_protection);
MOSS_TEST_FUNCTION(test_memory_size_validation);

// ============================================================================
// 内存工具函数测试
// ============================================================================

// 内存工具测试
MOSS_TEST_FUNCTION(test_memory_compare_functions);
MOSS_TEST_FUNCTION(test_memory_search_functions);
MOSS_TEST_FUNCTION(test_memory_pattern_fill);

// ============================================================================
// 虚拟内存概念测试
// ============================================================================

// 虚拟内存基础测试
MOSS_TEST_FUNCTION(test_virtual_address_validation);
MOSS_TEST_FUNCTION(test_physical_address_conversion);
MOSS_TEST_FUNCTION(test_page_boundary_calculations);

// ============================================================================
// 内存统计和监控测试
// ============================================================================

// 内存监控测试
MOSS_TEST_FUNCTION(test_memory_usage_tracking);
MOSS_TEST_FUNCTION(test_memory_pressure_detection);

// ============================================================================
// 测试套件声明和注册
// ============================================================================

// 声明内存管理测试套件
MOSS_DECLARE_TEST_SUITE(memory);

// 注册所有测试函数的声明将在.cpp文件中实现

} // namespace moss::kernel::test
