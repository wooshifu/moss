#include "../../moss_ut.hpp"

using namespace boost::ut;

// Forward declare our test suites
extern void run_containers_tests();

extern "C" [[noreturn]] void modern_test_main() noexcept {
    using namespace moss::kernel;

    kernel_uart_puts("🚀 MOSS Modern Test Suite Starting...\n");
    kernel_uart_puts("Framework: Full boost::ut Compatible\n");
    kernel_uart_puts("Features: _i literals, complex expressions, enhanced errors\n\n");

    // Run specific test suites directly
    kernel_uart_puts("Running containers test suite...\n");
    run_containers_tests();

    kernel_uart_puts("🎉 All modern tests completed successfully!\n");
    kernel_test_exit(0);
}
