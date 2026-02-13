#include "../../moss_ut.hpp"
#include "../fixtures/kernel_fixtures.hpp"
#include "../core/test_utilities.hpp"

using namespace boost::ut;
using namespace moss::test::utils;
using namespace moss::test::fixtures;
using namespace moss::kernel; // For types

void run_memory_tests() {
    "memory"_suite([] {

        "memory_alignment_verification"_test([] {
            MemoryTestFixture fixture;

            // Test aligned allocation concepts
            alignas(16) char buffer[64];
            auto* ptr = &buffer[0];

            expect(is_aligned<char>(ptr));
            expect(reinterpret_cast<uintptr_t>(ptr) % 16u == 0u);
        });

        "memory_block_operations"_test([] {
            struct TestMemoryBlock {
                void* ptr;
                size_t size;
                size_t alignment;
                bool is_valid;

                TestMemoryBlock() : ptr(nullptr), size(0), alignment(0), is_valid(false) {}
                TestMemoryBlock(void* p, size_t s, size_t a) : ptr(p), size(s), alignment(a), is_valid(true) {}
            };

            TestMemoryBlock block1;
            TestMemoryBlock block2(reinterpret_cast<void*>(0x1000), 4096, 16);

            // Verify default initialization
            expect(block1.ptr == nullptr and
                   block1.size == 0u and
                   block1.alignment == 0u and
                   !block1.is_valid);

            // Verify parameterized initialization
            expect(block2.ptr != nullptr and
                   block2.size == 4096u and
                   block2.alignment == 16u and
                   block2.is_valid);
        });

        "memory_bounds_checking"_test([] {
            constexpr size_t BUFFER_SIZE = 256;
            char test_buffer[BUFFER_SIZE];

            // Test buffer boundaries
            expect(sizeof(test_buffer) == BUFFER_SIZE);

            // Test access patterns
            test_buffer[0] = 'A';
            test_buffer[BUFFER_SIZE-1] = 'Z';

            expect(test_buffer[0] == 'A' and test_buffer[BUFFER_SIZE-1] == 'Z');
        });

    });
}
