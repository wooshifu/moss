#include "../../moss_ut.hpp"

using namespace boost::ut;

extern "C" [[noreturn]] void modern_test_main() noexcept {
    using namespace moss::kernel;

    kernel_uart_puts("\n");
    kernel_uart_puts("============================================\n");
    kernel_uart_puts("🚀 MOSS Modern Test Suite v2.0\n");
    kernel_uart_puts("============================================\n");
    kernel_uart_puts("Framework: boost::ut Compatible\n");
    kernel_uart_puts("Features: _i literals, and/or operators\n");
    kernel_uart_puts("Enhanced: Expected vs Actual value display\n");
    kernel_uart_puts("Environment: Kernel Freestanding Mode\n");
    kernel_uart_puts("Architecture: ARM64 (Cortex-A57)\n");
    kernel_uart_puts("============================================\n");
    kernel_uart_puts("\n🧪 Executing Modern Test Suites:\n");
    kernel_uart_puts("  • Containers (SPSC Queue, Lock-free ops)\n");
    kernel_uart_puts("  • Memory (Alignment, Bounds Checking)\n");
    kernel_uart_puts("  • Scheduler (Load Balancing, Timing)\n");
    kernel_uart_puts("\n");

    // Run all modern test suites
    boost::ut::run_all_tests();
}
