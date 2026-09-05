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
        // x86_64 serial port output (COM1 data register at I/O port 0x3F8)
        while (*str) {
            const auto value = static_cast<unsigned char>(*str++);
            asm volatile("outb %0, %1" : : "a"(value), "Nd"(static_cast<unsigned short>(0x3F8)));
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
     *
     * ARM64 AArch64 semihosting: SYS_EXIT (0x18) expects x1 to point to
     * a parameter block {uint64_t reason, uint64_t code}.
     * ADP_Stopped_ApplicationExit = 0x20026
     */
    [[noreturn]] inline void kernel_test_exit(int exit_code) noexcept {
        if (exit_code == 0) {
            kernel_uart_puts("🎉 All tests passed! Kernel testing successful.\n");
        } else {
            kernel_uart_puts("❌ Some tests failed. Check output above.\n");
        }

#if defined(MOSS_ARCH_ARM64)
        // ARM64 AArch64 semihosting SYS_EXIT (0x18):
        //   x1 -> parameter block: { reason (uint64), exit_code (uint64) }
        //   reason = ADP_Stopped_ApplicationExit (0x20026)
        volatile unsigned long long exit_block[2] = {
            0x20026,                                       // ADP_Stopped_ApplicationExit
            static_cast<unsigned long long>(exit_code)     // exit code
        };
        asm volatile(
            "mov x0, #0x18\n"          // SYS_EXIT
            "mov x1, %0\n"             // pointer to parameter block
            "hlt #0xF000\n"            // AArch64 semihosting trap
            :
            : "r"(&exit_block)
            : "x0", "x1", "memory"
        );
#elif defined(MOSS_ARCH_X86_64)
        // x86_64: use ISA debug exit device (port 0x501)
        // QEMU -device isa-debug-exit maps port writes to exit code
        // Formula: QEMU exit code = (value << 1) | 1
        // Reserve values below 0x10 for real QEMU failures so the runner can
        // distinguish a successful test exit from QEMU's ordinary exit code 1.
        constexpr int qemu_debug_exit_base = 0x10;
        const int qemu_exit_value = qemu_debug_exit_base + exit_code;
        asm volatile(
            "mov $0x501, %%dx\n"
            "mov %0, %%eax\n"
            "outb %%al, %%dx\n"
            :
            : "r"(qemu_exit_value)
            : "eax", "edx"
        );
#elif defined(MOSS_ARCH_RISCV)
        // RISC-V: use SBI SRST extension or HTIF for QEMU exit
        // sbi_system_reset(RESET_TYPE_SHUTDOWN=0, RESET_REASON=0)
        // SBI call: a7=SRST ext (0x53525354), a6=0, a0=0, a1=0
        asm volatile(
            "li a7, 0x53525354\n"      // SRST extension ID
            "li a6, 0\n"              // function: system_reset
            "li a0, 0\n"              // RESET_TYPE_SHUTDOWN
            "li a1, 0\n"              // RESET_REASON_NONE
            "ecall\n"
            ::: "a0", "a1", "a6", "a7"
        );
#endif

        while (true) {
#if defined(MOSS_ARCH_ARM64)
            asm volatile("wfi");
#elif defined(MOSS_ARCH_X86_64)
            asm volatile("hlt");
#elif defined(MOSS_ARCH_RISCV)
            asm volatile("wfi");
#endif
        }
    }
}

// ========================================================================
// Convenience Imports for Test Files
// ========================================================================

// Note: using namespace moved to individual test files to avoid header hygiene issues
