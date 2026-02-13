#pragma once
#include "../../moss_ut.hpp"
#include "moss_std.hpp"
#include "arch/arch_abstraction.hpp"

namespace moss::test::utils {
    // Memory alignment verification
    template<typename T>
    constexpr auto is_aligned(T* ptr) {
        return reinterpret_cast<uintptr_t>(ptr) % alignof(T) == 0;
    }

    // Performance timing
    struct PerformanceTimer {
        u64 start_cycles;

        PerformanceTimer() : start_cycles(moss::kernel::arch::get_timestamp_counter()) {}

        auto elapsed_less_than(u64 max_cycles) {
            return (moss::kernel::arch::get_timestamp_counter() - start_cycles) < max_cycles;
        }
    };

    // Container state validation
    template<typename Container>
    auto container_state(const Container& c, size_t expected_size, bool should_be_empty) {
        return c.size() == expected_size and c.empty() == should_be_empty;
    }
}
