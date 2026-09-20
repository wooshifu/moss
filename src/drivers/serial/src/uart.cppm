// MOSS UART/Console Hardware Abstraction Layer
//
// Provides architecture-specific character and string output operations.
// This replaces the scattered early_debug_print(), debug_print(), early_print()
// implementations found across 15+ files with a single, unified module.
//
// Output backends:
//   ARM64:  PL011 UART (MMIO, with TXFF wait)
//   x64: COM1 serial port (I/O ports 0x3F8/0x3FD)
//   RISC-V 64: NS16550 UART (MMIO)
//
// Usage:
//   import moss.drivers.uart;
//   moss::kernel::drivers::uart::puts("Hello kernel\n");
//   moss::kernel::drivers::uart::put_hex(0xDEADBEEF);

export module moss.drivers.uart;

import moss.std;
import moss.types;
import moss.platform;

export namespace moss::kernel::drivers::uart {

using moss::u16;
using moss::u32;
using moss::u64;
using moss::u8;

inline u32 transmit_lock = 0;

// UART is below the containers/arch modules. Keep its lock freestanding and
// mask local IRQs so an interrupt cannot recursively wait on its own writer.
// IRQ masks use DAIF immediate bit 1 on ARM64, RFLAGS.IF bit 9 on x64,
// and sstatus.SIE bit 1 on RV64; only restore the caller's prior enable state.
class TransmitGuard {
  u64 flags_ = 0;

public:
  TransmitGuard() noexcept {
#if defined(MOSS_ARCH_ARM64)
    asm volatile("mrs %0, daif; msr daifset, #2" : "=r"(flags_)::"memory");
#elif defined(MOSS_ARCH_X64)
    asm volatile("pushfq; popq %0; cli" : "=r"(flags_)::"memory");
#else
    asm volatile("csrrc %0, sstatus, %1" : "=r"(flags_) : "r"(2ULL) : "memory");
#endif
    while (__atomic_exchange_n(&transmit_lock, 1U, __ATOMIC_ACQUIRE)) {
      while (__atomic_load_n(&transmit_lock, __ATOMIC_RELAXED)) {
        asm volatile("" ::: "memory");
      }
    }
  }
  ~TransmitGuard() {
    __atomic_store_n(&transmit_lock, 0U, __ATOMIC_RELEASE);
#if defined(MOSS_ARCH_ARM64)
    asm volatile("msr daif, %0" ::"r"(flags_) : "memory");
#elif defined(MOSS_ARCH_X64)
    if (flags_ & (1ULL << 9)) {
      asm volatile("sti" ::: "memory");
    }
#else
    if (flags_ & 2) {
      asm volatile("csrsi sstatus, 2" ::: "memory");
    }
#endif
  }
};

// ============================================================================
// Low-level putc — architecture-specific single character output
// ============================================================================

inline u32 read_register(u32 offset) noexcept {
  const auto &uart = platform::hardware.uart;
  // Firmware reg-shift scales register indices to byte addresses; reg-width=4
  // requires word MMIO accesses even when the register payload is one byte.
  auto address = uart.base_addr + (static_cast<u64>(offset) << uart.reg_shift);
#if defined(MOSS_ARCH_X64)
  if (uart.port_io) {
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(static_cast<u16>(address)));
    return value;
  }
#endif
  return uart.reg_width == 4 ? *reinterpret_cast<volatile u32 *>(address) : *reinterpret_cast<volatile u8 *>(address);
}

inline void write_register(u32 offset, u32 value) noexcept {
  const auto &uart = platform::hardware.uart;
  auto address = uart.base_addr + (static_cast<u64>(offset) << uart.reg_shift);
#if defined(MOSS_ARCH_X64)
  if (uart.port_io) {
    asm volatile("outb %0, %1" : : "a"(static_cast<u8>(value)), "Nd"(static_cast<u16>(address)));
    return;
  }
#endif
  if (uart.reg_width == 4) {
    *reinterpret_cast<volatile u32 *>(address) = value;
  } else {
    *reinterpret_cast<volatile u8 *>(address) = static_cast<u8>(value);
  }
}

inline void putc_unlocked(char c) noexcept {
  const auto &uart = platform::hardware.uart;
  if (!uart.valid) {
    return; // Console becomes available after firmware discovery.
  }
  if (uart.kind == platform::UartKind::Pl011) {
    // PL011 FR is at byte offset 0x18; TXFF bit 5 prevents overwriting a full FIFO.
    while (read_register(0x18) & (1U << 5)) {
    }
  } else {
    // 16550 LSR register 5: THRE bit 5 (0x20) permits another transmit byte.
    while (!(read_register(5) & 0x20)) {
    }
  }
  write_register(0, static_cast<u8>(c));
}

inline void putc(char c) noexcept {
  TransmitGuard guard;
  putc_unlocked(c);
}

inline void enable_rx() noexcept {
  if (!platform::hardware.uart.valid) {
    return;
  }
  if (platform::hardware.uart.kind == platform::UartKind::Pl011) {
    // PL011 register map: CR=0x30, ICR=0x44, LCR_H=0x2c. Disable before
    // configuring 8-bit words (WLEN=3 at bit 5) and FIFO mode (bit 4), clear
    // all 11 interrupt causes (0x7ff), then enable UART/TX/RX (bits 0/8/9).
    write_register(0x30, 0);
    write_register(0x44, 0x7FF);
    write_register(0x2C, (3U << 5) | (1U << 4));
    write_register(0x30, (1U << 0) | (1U << 8) | (1U << 9));
  }
}

inline void enable_rx_interrupt() noexcept {
  if (!platform::hardware.uart.valid) {
    return;
  }
  if (platform::hardware.uart.kind == platform::UartKind::Pl011) {
    // IMSC=0x38: RXIM bit 4 unmasks FIFO receive interrupts.
    write_register(0x38, read_register(0x38) | (1U << 4));
  } else {
    // 16550 IER register 1 bit 0 enables RX; MCR register 4 bit 3 (OUT2)
    // opens the legacy PC interrupt output gate.
    write_register(1, read_register(1) | 1U);
    write_register(4, read_register(4) | 8U);
  }
}

inline void ack_rx_interrupt() noexcept {
  if (platform::hardware.uart.kind == platform::UartKind::Pl011) {
    // ICR=0x44 is write-one-to-clear; clear RX (bit 4) and RX timeout (bit 6).
    write_register(0x44, (1U << 4) | (1U << 6));
  }
}

inline int getc() noexcept {
  const auto &uart = platform::hardware.uart;
  if (!uart.valid) {
    return -1;
  }
  if (uart.kind == platform::UartKind::Pl011) {
    // FR=0x18 RXFE bit 4 marks empty; 16550 LSR register 5 uses DR bit 0.
    if (read_register(0x18) & (1U << 4)) {
      return -1;
    }
  } else if (!(read_register(5) & 1U)) {
    return -1;
  }
  // DR/register 0 also carries PL011 error status above the 8-bit character.
  return static_cast<int>(read_register(0) & 0xFFU);
}

// ============================================================================
// String output — with automatic \n → \r\n conversion
// ============================================================================

inline void puts(const char *str) noexcept {
  if (!str) {
    return;
  }
  TransmitGuard guard;
  while (*str) {
    if (*str == '\n') {
      putc_unlocked('\r');
    }
    putc_unlocked(*str++);
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

} // namespace moss::kernel::drivers::uart

// Note: The old `extern "C" void early_debug_print(...)` is kept in
// kernel_main.cpp for backward compatibility with assembly and test code.
// New code should `import moss.drivers.uart;` and call hal::uart::puts() directly.
