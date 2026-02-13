#pragma once
#include "moss_ut.hpp"

namespace moss::test::utils {
    // Memory alignment verification
    template<typename T>
    constexpr auto is_aligned(T* ptr) {
        return reinterpret_cast<uintptr_t>(ptr) % alignof(T) == 0;
    }

    // Performance timing
    struct PerformanceTimer {
        uint64_t start_cycles;

        PerformanceTimer() : start_cycles(get_test_timestamp_ns()) {}

        auto elapsed_less_than(uint64_t max_cycles) {
            return (get_test_timestamp_ns() - start_cycles) < max_cycles;
        }
    };

    // Container state validation
    template<typename Container>
    auto container_state(const Container& c, size_t expected_size, bool should_be_empty) {
        return c.size() == expected_size and c.empty() == should_be_empty;
    }
}