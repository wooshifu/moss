/**
 * @file test_main.cpp
 * @brief MOSS Kernel ut.hpp Testing Framework - Modern C++ Testing
 *
 * This is the main entry point for MOSS kernel tests using the kernel-optimized
 * ut.hpp framework. Provides modern C++ testing syntax in freestanding environment.
 */

#include "moss_ut.hpp"

// Force test registration from kernel_modern_validation.cpp
extern "C" void force_kernel_test_registration();
extern "C" void force_boost_ut_demo_registration();

// Modern test framework entry point
extern "C" [[noreturn]] void modern_test_main() noexcept;

// ============================================================================
// Main Test Function
// ============================================================================

extern "C" [[noreturn]] void test_kernel_main() noexcept {
    using namespace moss::kernel;

    // Initialize UART output
    kernel_uart_puts("\n");
    kernel_uart_puts("=====================================\n");
    kernel_uart_puts("🚀 MOSS Kernel ut.hpp Testing\n");
    kernel_uart_puts("=====================================\n");
    kernel_uart_puts("Framework: Kernel-Optimized ut.hpp\n");
    kernel_uart_puts("Standard Library: Minimal Freestanding\n");
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

    kernel_uart_puts("🔧 Modern ut.hpp framework initialized\n");
    kernel_uart_puts("🧪 Running modern syntax tests...\n\n");

    // Force test registration to ensure static constructors run
    force_kernel_test_registration();
    force_boost_ut_demo_registration();

    // Run modern tests first
    kernel_uart_puts("🚀 Running Modern Test Suite...\n");
    modern_test_main(); // This function never returns
}

// Note: _start is provided by kernel's boot system
// test_kernel_main() is called by kernel_run_unit_tests() in kernel_main.cpp

// ============================================================================
// Modern ut.hpp Testing Notes
// ============================================================================

/*
 * KERNEL-OPTIMIZED ut.hpp ARCHITECTURE:
 *
 * This implementation uses kernel-optimized ut.hpp with:
 * - Modern C++ testing syntax: expect(), "test_name"_test
 * - Minimal freestanding standard library
 * - Direct UART output for kernel environment
 * - Static memory allocation only
 * - Architecture-specific optimizations
 * - Zero heap allocation
 *
 * BENEFITS:
 * - Modern testing patterns and syntax
 * - Automatic test discovery and registration
 * - Familiar ut.hpp API for developers
 * - Minimal resource usage
 * - Fast compilation
 * - Better performance in kernel environment
 *
 * IMPLEMENTATION DETAILS:
 * - Tests use ut.hpp string literal operators
 * - Automatic test registration through static constructors
 * - UART output integrated directly
 * - Exit handling uses architecture-specific halt instructions
 * - No exceptions or RTTI - fully freestanding compliant
 * - Real expect() assertions with detailed failure reporting
 */
