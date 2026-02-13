/**
 * @file test_main.cpp
 * @brief MOSS Kernel Lightweight Testing Framework - Pure Kernel Implementation
 *
 * This is the main entry point for MOSS kernel tests using our lightweight kernel testing
 * framework. No complex standard library dependencies, optimized for freestanding environment.
 */

#include "kernel_ut.hpp"

// Architecture-specific UART output for kernel environment
namespace moss::kernel {

/**
 * @brief Direct UART output for kernel test environment
 */
void kernel_uart_puts(const char* str) noexcept {
#if defined(MOSS_ARCH_ARM64)
    volatile char* uart_base = reinterpret_cast<volatile char*>(0x09000000);
    while (*str) {
        *uart_base = *str++;
    }
#elif defined(MOSS_ARCH_X86_64)
    // x86_64 serial port output
    volatile uint16_t* serial = reinterpret_cast<volatile uint16_t*>(0x3F8);
    while (*str) {
        *serial = static_cast<uint16_t>(*str++);
    }
#elif defined(MOSS_ARCH_RISCV)
    // RISC-V UART output (QEMU virt platform)
    volatile char* uart_base = reinterpret_cast<volatile char*>(0x10000000);
    while (*str) {
        *uart_base = *str++;
    }
#endif
}

/**
 * @brief Kernel test exit with architecture-specific halt
 */
[[noreturn]] void kernel_test_exit(int exit_code) noexcept {
    // Print final status
    if (exit_code == 0) {
        kernel_uart_puts("🎉 All tests passed! Kernel testing successful.\n");
    } else {
        kernel_uart_puts("❌ Some tests failed. Check output above.\n");
    }

    // Architecture-specific halt
    while (true) {
#if defined(MOSS_ARCH_ARM64)
        asm volatile("wfi");
#elif defined(MOSS_ARCH_X86_64)
        asm volatile("hlt");
#else
        for (volatile int i = 0; i < 1000000; ++i) {}
#endif
    }
}

} // namespace moss::kernel

// ============================================================================
// Main Test Function
// ============================================================================

extern "C" [[noreturn]] void test_kernel_main() noexcept {
    using namespace moss::kernel;

    // Initialize UART output
    kernel_uart_puts("\n");
    kernel_uart_puts("=====================================\n");
    kernel_uart_puts("🚀 MOSS Kernel Lightweight Testing\n");
    kernel_uart_puts("=====================================\n");
    kernel_uart_puts("Framework: Custom Kernel Testing\n");
    kernel_uart_puts("Standard Library: None (Freestanding)\n");
    kernel_uart_puts("Environment: Pure Kernel Mode\n");
#if defined(MOSS_ARCH_ARM64)
    kernel_uart_puts("Architecture: ARM64\n");
#elif defined(MOSS_ARCH_X86_64)
    kernel_uart_puts("Architecture: x86_64\n");
#elif defined(MOSS_ARCH_RISCV)
    kernel_uart_puts("Architecture: RISC-V\n");
#endif
    kernel_uart_puts("=====================================\n\n");

    // Initialize (minimal setup needed)
    moss::test::initialize_freestanding_std();

    kernel_uart_puts("🔧 Kernel testing framework initialized\n");
    kernel_uart_puts("🧪 Running validation tests...\n\n");

    // Run basic kernel tests
    moss::test::run_validation_tests();

    // Run validation tests and exit (this function never returns)
    moss::test::run_freestanding_validation_tests();
}

// ============================================================================
// Architecture-Specific Entry Points
// ============================================================================

#if defined(MOSS_ARCH_ARM64)
extern "C" [[noreturn]] __attribute__((section(".text.boot"))) void _start() noexcept {
    // ARM64 minimal startup
    volatile char* uart = reinterpret_cast<volatile char*>(0x09000000);

    const char startup_msg[] = "🔥 MOSS Kernel Test Starting...\n";
    for (int i = 0; startup_msg[i] != 0; ++i) {
        *uart = startup_msg[i];
    }

    test_kernel_main();
}
#endif

#if defined(MOSS_ARCH_X86_64)
extern "C" [[noreturn]] __attribute__((section(".text.boot"))) void _start() noexcept {
    test_kernel_main();
}
#endif

#if defined(MOSS_ARCH_RISCV)
extern "C" [[noreturn]] __attribute__((section(".text.boot"))) void _start() noexcept {
    test_kernel_main();
}
#endif

// ============================================================================
// Lightweight Kernel Testing Notes
// ============================================================================

/*
 * LIGHTWEIGHT TESTING ARCHITECTURE:
 *
 * This implementation uses a custom lightweight testing framework with:
 * - No standard library dependencies
 * - Direct UART output for kernel environment
 * - Static memory allocation only
 * - Architecture-specific optimizations
 * - Zero heap allocation
 *
 * BENEFITS:
 * - Minimal resource usage
 * - Fast compilation
 * - No complex dependencies
 * - Better performance in kernel environment
 * - Easy to understand and maintain
 *
 * IMPLEMENTATION DETAILS:
 * - All tests use static initialization and execute automatically
 * - UART output integrated directly without iostream overhead
 * - Exit handling uses architecture-specific halt instructions
 * - No exceptions or RTTI - fully freestanding compliant
 * - Simple expect() macro for assertions
 */
