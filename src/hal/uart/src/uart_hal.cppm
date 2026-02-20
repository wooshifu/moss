// MOSS UART/Console Hardware Abstraction Layer
//
// Provides architecture-specific character and string output operations.
// This replaces the scattered early_debug_print(), debug_print(), early_print()
// implementations found across 15+ files with a single, unified module.
//
// Output backends:
//   ARM64:  PL011 UART (MMIO, with TXFF wait)
//   x86_64: COM1 serial port (I/O ports 0x3F8/0x3FD)
//   RISC-V: NS16550 UART (MMIO)
//
// Usage:
//   import moss.hal.uart;
//   moss::kernel::hal::uart::puts("Hello kernel\n");
//   moss::kernel::hal::uart::put_hex(0xDEADBEEF);

module;

#include "arch_detect.h"

export module moss.hal.uart;

import moss.std;
import moss.types;
import moss.platform;

export namespace moss::kernel::hal::uart {

using moss::u8;
using moss::u16;
using moss::u32;
using moss::u64;

// ============================================================================
// Low-level putc — architecture-specific single character output
// ============================================================================

inline void putc(char c) noexcept {
#if defined(MOSS_ARCH_ARM64)
  // PL011 UART: data register at base+0x00, flags register at base+0x18
  // TXFF (TX FIFO Full) is bit 5 of the flags register
  auto base = platform::uart_base();
  volatile u32 *uart_data  = reinterpret_cast<volatile u32 *>(base);
  volatile u32 *uart_flags = reinterpret_cast<volatile u32 *>(base + 0x18);

  // Wait for TX FIFO space
  while (*uart_flags & (1U << 5)) {}
  *uart_data = static_cast<u32>(static_cast<unsigned char>(c));

#elif defined(MOSS_ARCH_X86_64)
  // COM1 serial port: data at 0x3F8, Line Status Register at 0x3FD
  // Wait for THR empty (bit 5 of LSR)
  for (;;) {
    u8 lsr;
    asm volatile("inb %1, %0" : "=a"(lsr) : "Nd"(static_cast<u16>(0x3FD)));
    if (lsr & 0x20) break;
  }
  asm volatile("outb %0, %1" :: "a"(static_cast<u8>(c)),
               "Nd"(static_cast<u16>(0x3F8)));

#elif defined(MOSS_ARCH_RISCV)
  // NS16550 UART: TX data register at base+0x00, Line Status at base+0x14
  // Wait for THR empty (bit 5 of LSR) before transmitting
  auto base = platform::uart_base();
  volatile u32 *uart_data = reinterpret_cast<volatile u32 *>(base);
  volatile u32 *uart_lsr  = reinterpret_cast<volatile u32 *>(base + 0x14);
  while ((*uart_lsr & (1U << 5)) == 0) {}
  *uart_data = static_cast<u32>(static_cast<unsigned char>(c));
#endif
}

// ============================================================================
// Low-level getc — architecture-specific single character input (non-blocking)
// Returns 0-255 on success, -1 if no character available.
// ============================================================================

inline int getc() noexcept {
#if defined(MOSS_ARCH_ARM64)
  // PL011 UART: data register at base+0x00, flags register at base+0x18
  // RXFE (RX FIFO Empty) is bit 4 of the flags register
  auto base = platform::uart_base();
  volatile u32 *uart_data  = reinterpret_cast<volatile u32 *>(base);
  volatile u32 *uart_flags = reinterpret_cast<volatile u32 *>(base + 0x18);

  if (*uart_flags & (1U << 4)) return -1;  // RXFE: RX FIFO empty
  return static_cast<int>(*uart_data & 0xFFU);

#elif defined(MOSS_ARCH_X86_64)
  // COM1 serial port: Line Status Register at 0x3FD, data at 0x3F8
  // DR (Data Ready) is bit 0 of LSR
  u8 lsr;
  asm volatile("inb %1, %0" : "=a"(lsr) : "Nd"(static_cast<u16>(0x3FD)));
  if (!(lsr & 0x01)) return -1;  // No data ready
  u8 data;
  asm volatile("inb %1, %0" : "=a"(data) : "Nd"(static_cast<u16>(0x3F8)));
  return static_cast<int>(data);

#elif defined(MOSS_ARCH_RISCV)
  // NS16550 UART: data register at base+0x00, Line Status at base+0x14
  // DR (Data Ready) is bit 0 of LSR
  auto base = platform::uart_base();
  volatile u32 *uart_data = reinterpret_cast<volatile u32 *>(base);
  volatile u32 *uart_lsr  = reinterpret_cast<volatile u32 *>(base + 0x14);

  if ((*uart_lsr & 0x01) == 0) return -1;  // No data ready
  return static_cast<int>(*uart_data & 0xFFU);

#else
  return -1;
#endif
}

// ============================================================================
// String output — with automatic \n → \r\n conversion
// ============================================================================

inline void puts(const char *str) noexcept {
  if (!str) return;
  while (*str) {
    if (*str == '\n') putc('\r');
    putc(*str++);
  }
}

// ============================================================================
// Hexadecimal output — 64-bit value with "0x" prefix
// ============================================================================

inline void put_hex(u64 value) noexcept {
  constexpr char hex_chars[] = "0123456789ABCDEF";
  char buffer[19] = "0x"; // "0x" + 16 hex digits + null

  for (int i = 15; i >= 0; i--) {
    buffer[2 + (15 - i)] = hex_chars[(value >> (i * 4)) & 0xF];
  }
  buffer[18] = '\0';
  puts(buffer);
}

// ============================================================================
// Hexadecimal output — without "0x" prefix
// ============================================================================

inline void put_hex_plain(u64 value) noexcept {
  constexpr char hex_chars[] = "0123456789ABCDEF";
  char buffer[17]; // 16 hex digits + null

  for (int i = 15; i >= 0; i--) {
    buffer[15 - i] = hex_chars[(value >> (i * 4)) & 0xF];
  }
  buffer[16] = '\0';
  puts(buffer);
}

// ============================================================================
// Decimal output — unsigned integer
// ============================================================================

inline void put_dec(u64 value) noexcept {
  if (value == 0) {
    putc('0');
    return;
  }

  char buffer[21]; // max 20 digits for u64 + null
  int pos = 0;

  while (value > 0) {
    buffer[pos++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  }

  // Output in reverse order
  for (int i = pos - 1; i >= 0; i--) {
    putc(buffer[i]);
  }
}

// ============================================================================
// Convenience: print string + hex value on one line
// ============================================================================

inline void log(const char *prefix, u64 value) noexcept {
  puts(prefix);
  put_hex(value);
  putc('\r');
  putc('\n');
}

} // namespace moss::kernel::hal::uart

// Note: The old `extern "C" void early_debug_print(...)` is kept in
// kernel_main.cpp for backward compatibility with assembly and test code.
// New code should `import moss.hal.uart;` and call hal::uart::puts() directly.
