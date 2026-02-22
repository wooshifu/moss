/**
 * @file test_main.cpp
 * @brief Standalone entry point for moss.test.elf
 *
 * Provides architecture-specific _start and the test main function.
 * This file is the entry point for the standalone test executable,
 * completely independent from the kernel binary (moss.elf).
 */

#include "framework/moss_ut.hpp"

// Force test registration from test case files
extern "C" void force_kernel_test_registration();

// ============================================================================
// Architecture-specific _start entry point
// ============================================================================

// Linker script symbols
extern "C" {
extern char _bss_start[];
extern char _bss_end[];
extern char _stack_top[];
}

// Clear BSS and call test_kernel_main
extern "C" [[noreturn]] void test_kernel_main() noexcept;

static void clear_bss() noexcept {
  for (char *p = _bss_start; p < _bss_end; ++p)
    *p = 0;
}

#if defined(MOSS_ARCH_ARM64)
asm(".section .text.boot, \"ax\"\n"
    ".global _start\n"
    "_start:\n"
    "    ldr x0, =_stack_top\n"
    "    mov sp, x0\n"
    "    bl _test_entry\n"
    "    b .\n");
#elif defined(MOSS_ARCH_X86_64)
asm(".section .text.boot, \"ax\"\n"
    ".global _start\n"
    "_start:\n"
    "    leaq _stack_top(%rip), %rsp\n"
    "    call _test_entry\n"
    "    hlt\n"
    "    jmp .\n");
#elif defined(MOSS_ARCH_RISCV)
asm(".section .text.boot, \"ax\"\n"
    ".global _start\n"
    "_start:\n"
    "    la sp, _stack_top\n"
    "    call _test_entry\n"
    "    j .\n");
#endif

extern "C" [[noreturn]] void _test_entry() noexcept {
  clear_bss();
  test_kernel_main();
}

// ============================================================================
// Test Main Function
// ============================================================================

extern "C" [[noreturn]] void test_kernel_main() noexcept {
  using namespace moss::kernel;

  kernel_uart_puts("\n");
  kernel_uart_puts("=====================================\n");
  kernel_uart_puts("  MOSS Kernel Unit Tests\n");
  kernel_uart_puts("=====================================\n");
  kernel_uart_puts("Framework: Kernel-Optimized ut.hpp\n");
  kernel_uart_puts("Binary:    moss.test.elf (standalone)\n");
#if defined(MOSS_ARCH_ARM64)
  kernel_uart_puts("Arch:      ARM64\n");
#elif defined(MOSS_ARCH_X86_64)
  kernel_uart_puts("Arch:      x86_64\n");
#elif defined(MOSS_ARCH_RISCV)
  kernel_uart_puts("Arch:      RISC-V\n");
#endif
  kernel_uart_puts("=====================================\n\n");

  moss::test::initialize_freestanding_std();

  // Force test registration to ensure static constructors run
  force_kernel_test_registration();

  // Run validation tests
  moss::test::run_validation_tests();

  // Run all registered tests and exit (never returns)
  moss::test::run_freestanding_validation_tests();
}
