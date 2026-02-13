#pragma once

/**
 * @file moss_ut.hpp
 * @brief MOSS kernel integration wrapper for kernel-optimized ut.hpp
 *
 * This file provides the integration between the kernel-optimized ut.hpp
 * testing framework and the MOSS kernel environment. It includes the
 * minimal freestanding standard library and kernel-optimized ut.hpp.
 */

// Include kernel-optimized ut.hpp (it has everything we need)
#include "ut_kernel.hpp"

// ========================================================================
// MOSS Kernel Integration Layer
// ========================================================================

namespace moss::kernel {
    /**
     * @brief UART output for kernel test environment
     */
    inline void kernel_uart_puts(const char* str) noexcept {
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
    [[noreturn]] inline void kernel_test_exit(int exit_code) noexcept {
        // Print final status
        if (exit_code == 0) {
            kernel_uart_puts("🎉 All tests passed! Kernel testing successful.\n");
        } else {
            kernel_uart_puts("❌ Some tests failed. Check output above.\n");
        }

        // Architecture-specific QEMU exit using semihosting
#if defined(MOSS_ARCH_ARM64)
        // ARM64 semihosting exit call
        // 0x18 is SYS_EXIT, exit_code is in x1
        asm volatile(
            "mov x0, #0x18\n"          // SYS_EXIT
            "mov x1, %0\n"             // exit_code
            "hlt #0xF000\n"            // ARM64 semihosting breakpoint
            :
            : "r"(static_cast<unsigned long long>(exit_code))
            : "x0", "x1"
        );
#elif defined(MOSS_ARCH_X86_64)
        // For x86_64, use simple halt (no standard semihosting)
        asm volatile("hlt");
#else
        // Other architectures: busy loop
        for (volatile int i = 0; i < 1000000; ++i) {}
#endif

        // Fallback: infinite loop (should never reach here)
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
}

// ========================================================================
// Convenience Imports for Test Files
// ========================================================================

// Note: using namespace moved to individual test files to avoid header hygiene issues

