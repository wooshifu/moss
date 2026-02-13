#pragma once
#include "../../moss_ut.hpp"
#include "../core/test_utilities.hpp"
#include "moss_std.hpp"

namespace moss::test::fixtures {
    struct KernelTestFixture {
        void setup() { /* Initialize kernel test environment */ }
        void teardown() { /* Clean up resources */ }
    };

    struct ContainerTestFixture : KernelTestFixture {
        static constexpr size_t TEST_QUEUE_SIZE = 8;
        static constexpr size_t TEST_BUFFER_SIZE = 64;
    };

    struct MemoryTestFixture : KernelTestFixture {
        static constexpr size_t TEST_ALLOCATION_SIZE = 4096;
        static constexpr size_t TEST_ALIGNMENT = 16;
    };
}
