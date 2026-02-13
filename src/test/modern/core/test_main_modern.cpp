#include "moss_ut.hpp"

using namespace boost::ut;

extern "C" [[noreturn]] void modern_test_main() noexcept {
    using namespace moss::kernel;

    kernel_uart_puts("🚀 MOSS Modern Test Suite Starting...\n");
    kernel_uart_puts("Framework: Full boost::ut Compatible\n");
    kernel_uart_puts("Features: _i literals, complex expressions, enhanced errors\n\n");

    // Run all modern test suites
    boost::ut::run_all_tests();
}