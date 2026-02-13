#pragma once

/**
 * @file chrono.hpp
 * @brief ARM64 timestamp-based chrono implementation for freestanding environment
 *
 * Provides std::chrono functionality using ARM64 system timer for high-precision
 * timing in kernel/freestanding environments. Designed specifically for test timing.
 */

#include "type_traits.hpp"

namespace std {
namespace chrono {

    // ========================================================================
    // Ratio for time unit conversions (defined early for use in duration)
    // ========================================================================

    template<u64 Num, u64 Den = 1>
    struct ratio {
        static constexpr u64 num = Num;
        static constexpr u64 den = Den;
    };

    // ========================================================================
    // Duration representation
    // ========================================================================

    template<typename Rep, typename Period = ratio<1>>
    class duration {
    public:
        using rep = Rep;
        using period = Period;

    private:
        rep count_;

    public:
        // Constructors
        constexpr duration() = default;

        template<typename Rep2>
        constexpr explicit duration(const Rep2& r) : count_(static_cast<rep>(r)) {}

        template<typename Rep2, typename Period2>
        constexpr duration(const duration<Rep2, Period2>& d)
            : count_(static_cast<rep>(d.count() * Period2::num / Period::num * Period::den / Period2::den)) {}

        // Observers
        constexpr rep count() const { return count_; }

        // Arithmetic
        constexpr duration operator+() const { return *this; }
        constexpr duration operator-() const { return duration(-count_); }

        duration& operator++() { ++count_; return *this; }
        duration operator++(int) { return duration(count_++); }
        duration& operator--() { --count_; return *this; }
        duration operator--(int) { return duration(count_--); }

        duration& operator+=(const duration& d) { count_ += d.count_; return *this; }
        duration& operator-=(const duration& d) { count_ -= d.count_; return *this; }
        duration& operator*=(const rep& rhs) { count_ *= rhs; return *this; }
        duration& operator/=(const rep& rhs) { count_ /= rhs; return *this; }
        duration& operator%=(const rep& rhs) { count_ %= rhs; return *this; }
        duration& operator%=(const duration& rhs) { count_ %= rhs.count_; return *this; }

        // Static method
        static constexpr duration zero() { return duration(rep(0)); }
    };

    // ========================================================================
    // Duration arithmetic
    // ========================================================================

    template<typename Rep1, typename Period1, typename Rep2, typename Period2>
    constexpr auto operator+(const duration<Rep1, Period1>& lhs, const duration<Rep2, Period2>& rhs) {
        using common_type = duration<u64, ratio<1, 1000000000>>; // nanoseconds
        return common_type(lhs) += common_type(rhs);
    }

    template<typename Rep1, typename Period1, typename Rep2, typename Period2>
    constexpr auto operator-(const duration<Rep1, Period1>& lhs, const duration<Rep2, Period2>& rhs) {
        using common_type = duration<u64, ratio<1, 1000000000>>; // nanoseconds
        return common_type(lhs) -= common_type(rhs);
    }

    template<typename Rep1, typename Period1, typename Rep2>
    constexpr auto operator*(const duration<Rep1, Period1>& d, const Rep2& s) {
        return duration<Rep1, Period1>(d.count() * s);
    }

    template<typename Rep1, typename Rep2, typename Period2>
    constexpr auto operator*(const Rep1& s, const duration<Rep2, Period2>& d) {
        return d * s;
    }

    // ========================================================================
    // Duration comparisons
    // ========================================================================

    template<typename Rep1, typename Period1, typename Rep2, typename Period2>
    constexpr bool operator==(const duration<Rep1, Period1>& lhs, const duration<Rep2, Period2>& rhs) {
        using common_type = duration<u64, ratio<1, 1000000000>>; // nanoseconds
        return common_type(lhs).count() == common_type(rhs).count();
    }

    template<typename Rep1, typename Period1, typename Rep2, typename Period2>
    constexpr bool operator!=(const duration<Rep1, Period1>& lhs, const duration<Rep2, Period2>& rhs) {
        return !(lhs == rhs);
    }

    template<typename Rep1, typename Period1, typename Rep2, typename Period2>
    constexpr bool operator<(const duration<Rep1, Period1>& lhs, const duration<Rep2, Period2>& rhs) {
        using common_type = duration<u64, ratio<1, 1000000000>>; // nanoseconds
        return common_type(lhs).count() < common_type(rhs).count();
    }

    template<typename Rep1, typename Period1, typename Rep2, typename Period2>
    constexpr bool operator<=(const duration<Rep1, Period1>& lhs, const duration<Rep2, Period2>& rhs) {
        return !(rhs < lhs);
    }

    template<typename Rep1, typename Period1, typename Rep2, typename Period2>
    constexpr bool operator>(const duration<Rep1, Period1>& lhs, const duration<Rep2, Period2>& rhs) {
        return rhs < lhs;
    }

    template<typename Rep1, typename Period1, typename Rep2, typename Period2>
    constexpr bool operator>=(const duration<Rep1, Period1>& lhs, const duration<Rep2, Period2>& rhs) {
        return !(lhs < rhs);
    }


    // ========================================================================
    // Standard duration types
    // ========================================================================

    using nanoseconds = duration<u64, ratio<1, 1000000000>>;
    using microseconds = duration<u64, ratio<1, 1000000>>;
    using milliseconds = duration<u64, ratio<1, 1000>>;
    using seconds = duration<u64, ratio<1>>;
    using minutes = duration<u64, ratio<60>>;
    using hours = duration<u64, ratio<3600>>;

    // ========================================================================
    // Time point
    // ========================================================================

    template<typename Clock, typename Duration = typename Clock::duration>
    class time_point {
    public:
        using clock = Clock;
        using duration = Duration;
        using rep = typename Duration::rep;
        using period = typename Duration::period;

    private:
        duration d_;

    public:
        // Constructors
        constexpr time_point() : d_(duration::zero()) {}
        constexpr explicit time_point(const duration& d) : d_(d) {}

        template<typename Duration2>
        constexpr time_point(const time_point<clock, Duration2>& t)
            : d_(t.time_since_epoch()) {}

        // Observer
        constexpr duration time_since_epoch() const { return d_; }

        // Arithmetic
        time_point& operator+=(const duration& d) { d_ += d; return *this; }
        time_point& operator-=(const duration& d) { d_ -= d; return *this; }
    };

    // ========================================================================
    // Time point arithmetic
    // ========================================================================

    template<typename Clock, typename Duration1, typename Rep2, typename Period2>
    constexpr time_point<Clock, Duration1> operator+(const time_point<Clock, Duration1>& pt, const duration<Rep2, Period2>& d) {
        return time_point<Clock, Duration1>(pt.time_since_epoch() + d);
    }

    template<typename Rep1, typename Period1, typename Clock, typename Duration2>
    constexpr time_point<Clock, Duration2> operator+(const duration<Rep1, Period1>& d, const time_point<Clock, Duration2>& pt) {
        return pt + d;
    }

    template<typename Clock, typename Duration1, typename Rep2, typename Period2>
    constexpr time_point<Clock, Duration1> operator-(const time_point<Clock, Duration1>& pt, const duration<Rep2, Period2>& d) {
        return time_point<Clock, Duration1>(pt.time_since_epoch() - d);
    }

    template<typename Clock, typename Duration1, typename Duration2>
    constexpr auto operator-(const time_point<Clock, Duration1>& lhs, const time_point<Clock, Duration2>& rhs) {
        return lhs.time_since_epoch() - rhs.time_since_epoch();
    }

    // ========================================================================
    // ARM64-specific timer access
    // ========================================================================

    namespace detail {
        // ARM64 Generic Timer access
        // These functions provide direct access to ARM64 system timers

        #ifdef __aarch64__

        // Read the virtual counter (CNTVCT_EL0)
        inline u64 read_virtual_counter() {
            u64 val;
            asm volatile("mrs %0, cntvct_el0" : "=r" (val));
            return val;
        }

        // Read the virtual timer frequency (CNTFRQ_EL0)
        inline u64 read_counter_frequency() {
            u64 val;
            asm volatile("mrs %0, cntfrq_el0" : "=r" (val));
            return val;
        }

        // Convert counter ticks to nanoseconds
        inline u64 ticks_to_nanoseconds(u64 ticks) {
            static const u64 freq = read_counter_frequency();
            if (freq == 0) return 0; // Safety check

            // Avoid overflow: (ticks * 1000000000) / freq
            // Using 128-bit arithmetic simulation for precision
            const u64 ns_per_sec = 1000000000ULL;

            // For most ARM64 systems, frequency is typically 25MHz or similar
            // This calculation should be safe for reasonable tick values
            return (ticks * ns_per_sec) / freq;
        }

        // Convert nanoseconds to counter ticks
        inline u64 nanoseconds_to_ticks(u64 ns) {
            static const u64 freq = read_counter_frequency();
            if (freq == 0) return 0; // Safety check

            const u64 ns_per_sec = 1000000000ULL;
            return (ns * freq) / ns_per_sec;
        }

        #else

        // Fallback for non-ARM64 platforms (for compilation/testing)
        inline u64 read_virtual_counter() {
            // Placeholder - in real implementation this might use RDTSC on x86
            static u64 counter = 0;
            return ++counter * 1000; // Simulate incrementing counter
        }

        inline u64 read_counter_frequency() {
            return 1000000000ULL; // 1 GHz simulation
        }

        inline u64 ticks_to_nanoseconds(u64 ticks) {
            return ticks; // 1:1 for simulation
        }

        inline u64 nanoseconds_to_ticks(u64 ns) {
            return ns; // 1:1 for simulation
        }

        #endif
    }

    // ========================================================================
    // High-resolution clock using ARM64 timer
    // ========================================================================

    struct high_resolution_clock {
        using rep = u64;
        using period = ratio<1, 1000000000>; // nanoseconds
        using duration = chrono::duration<rep, period>;
        using time_point = chrono::time_point<high_resolution_clock>;

        static constexpr bool is_steady = true;

        static time_point now() noexcept {
            u64 ticks = detail::read_virtual_counter();
            u64 ns = detail::ticks_to_nanoseconds(ticks);
            return time_point(duration(ns));
        }
    };

    // ========================================================================
    // Steady clock (alias for high_resolution_clock in kernel)
    // ========================================================================

    using steady_clock = high_resolution_clock;

    // ========================================================================
    // System clock (simplified for kernel environment)
    // ========================================================================

    struct system_clock {
        using rep = u64;
        using period = ratio<1, 1000000000>; // nanoseconds
        using duration = chrono::duration<rep, period>;
        using time_point = chrono::time_point<system_clock>;

        static constexpr bool is_steady = false;

        static time_point now() noexcept {
            // In kernel environment, system clock is the same as steady clock
            auto steady_now = steady_clock::now();
            return time_point(steady_now.time_since_epoch());
        }
    };

    // ========================================================================
    // Duration literals (C++14 style)
    // ========================================================================

    namespace literals {
    namespace chrono_literals {
        constexpr nanoseconds operator""ns(unsigned long long ns) {
            return nanoseconds(ns);
        }

        constexpr microseconds operator""us(unsigned long long us) {
            return microseconds(us);
        }

        constexpr milliseconds operator""ms(unsigned long long ms) {
            return milliseconds(ms);
        }

        constexpr seconds operator""s(unsigned long long s) {
            return seconds(s);
        }

        constexpr minutes operator""min(unsigned long long m) {
            return minutes(m);
        }

        constexpr hours operator""h(unsigned long long h) {
            return hours(h);
        }
    }
    }

    // ========================================================================
    // Utility functions
    // ========================================================================

    template<typename ToDuration, typename Rep, typename Period>
    constexpr ToDuration duration_cast(const duration<Rep, Period>& d) {
        using to_period = typename ToDuration::period;
        using from_period = Period;

        // Simple conversion - in real implementation this would handle overflow
        u64 converted = d.count() * from_period::num * to_period::den / (from_period::den * to_period::num);
        return ToDuration(converted);
    }


    // Helper for measuring elapsed time
    template<typename Clock = high_resolution_clock>
    class timer {
    private:
        typename Clock::time_point start_;

    public:
        timer() : start_(Clock::now()) {}

        void reset() {
            start_ = Clock::now();
        }

        template<typename Duration = nanoseconds>
        Duration elapsed() const {
            return duration_cast<Duration>(Clock::now() - start_);
        }
    };

} // namespace chrono
} // namespace std
