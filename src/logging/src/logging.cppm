// MOSS Kernel Logging Module — Unified logging with levels and formatting
//
// Replaces scattered early_debug_print(), sched_log(), kernel_print(),
// debug_print() calls with a single type-safe API.
//
// Two formatting styles:
//
//   1. fmt-style with {} placeholders (PREFERRED):
//      klog::info("tick={} cpu={} addr={:#x}", count, cpu_id, addr);
//
//   2. Chaining style:
//      klog::info("tick=").u64(count).str(" cpu=").u32(cpu_id);
//
// Supported {} format specs:
//   {}     — auto (decimal for integers, string for const char*)
//   {:#x}  — hexadecimal with 0x prefix
//   {:#b}  — boolean as "true"/"false"
//
// Output format:
//   [L] filename:line message
//   e.g. [I] kernel_main.cpp:53 === MOSS kernel main starting ===
//
// Source location is captured automatically via __builtin_FILE()/__builtin_LINE()
// using FmtStr's implicit constructor — callers need NO syntax changes.
//
// Backend: hal::uart (architecture-independent UART/serial output)
//
// Boot-phase functions (early_print, boot_print) remain independent
// because they execute before the C++26 module system is available.

module;

#include "arch_detect.h"

export module moss.logging;

import moss.std;
import moss.types;
import moss.hal.uart;

export namespace moss::kernel::logging {

using moss::i32;
using moss::i64;
using moss::u8;
using moss::u32;
using moss::u64;
namespace uart = moss::kernel::hal::uart;

// ============================================================================
// FmtStr — format string wrapper that captures source location at call site
//
// Uses __builtin_FILE() / __builtin_LINE() as default arguments so that
// the compiler evaluates them at the CALLER's location, not here.
// The implicit constructor from const char* means callers write:
//   klog::info("msg");         // file:line captured automatically
//   klog::info("x={}", val);   // same — no syntax change needed
// ============================================================================

struct FmtStr {
  const char* value;
  const char* file;
  unsigned    line;

  // Implicit conversion from string literal — captures source location
  constexpr FmtStr(const char* s,
                   const char* f = __builtin_FILE(),
                   unsigned    l = __builtin_LINE()) noexcept
      : value(s), file(f), line(l) {}
};

// ============================================================================
// Log levels
// ============================================================================

enum class LogLevel : u8 {
  Debug = 0,
  Info  = 1,
  Warn  = 2,
  Error = 3,
  Panic = 4,
};

// ============================================================================
// Runtime log level filter — only messages >= this level are output
// ============================================================================

inline LogLevel g_log_level = LogLevel::Debug;

inline void set_log_level(LogLevel level) noexcept { g_log_level = level; }
inline auto get_log_level() noexcept -> LogLevel { return g_log_level; }

// ============================================================================
// LogBuffer — fixed-size stack buffer with formatting primitives
//
// Shared by both the {} formatter and the chaining API. All formatting
// is done into this buffer; it is flushed to UART as a single write.
// ============================================================================

class LogBuffer {
public:
  static constexpr u32 BUFFER_SIZE = 512;

  void append_char(char c) noexcept {
    if (pos_ < BUFFER_SIZE - 1) {
      buf_[pos_++] = c;
    }
  }

  void append_str(const char *s) noexcept {
    if (!s) return;
    while (*s && pos_ < BUFFER_SIZE - 1) {
      buf_[pos_++] = *s++;
    }
  }

  void append_dec(u64 value) noexcept {
    if (value == 0) {
      append_char('0');
      return;
    }
    char tmp[21];
    u32 len = 0;
    while (value > 0) {
      tmp[len++] = static_cast<char>('0' + (value % 10));
      value /= 10;
    }
    for (u32 j = len; j > 0; j--) {
      append_char(tmp[j - 1]);
    }
  }

  void append_signed(i64 value) noexcept {
    if (value < 0) {
      append_char('-');
      if (value == (-9223372036854775807LL - 1)) {
        append_str("9223372036854775808");
        return;
      }
      append_dec(static_cast<u64>(-value));
    } else {
      append_dec(static_cast<u64>(value));
    }
  }

  void append_hex(u64 value) noexcept {
    constexpr char hex_chars[] = "0123456789abcdef";
    append_char('0');
    append_char('x');

    if (value == 0) {
      append_char('0');
      return;
    }

    int shift = 60;
    while (shift > 0 && ((value >> shift) & 0xF) == 0) {
      shift -= 4;
    }
    while (shift >= 0) {
      append_char(hex_chars[(value >> shift) & 0xF]);
      shift -= 4;
    }
  }

  void append_bool(bool v) noexcept {
    append_str(v ? "true" : "false");
  }

  // Extract basename from full path and format as "file:line "
  void append_source_loc(const char *file, unsigned line) noexcept {
    if (!file) return;
    // Find last '/' to extract basename
    const char *basename = file;
    for (const char *p = file; *p; p++) {
      if (*p == '/') basename = p + 1;
    }
    append_str(basename);
    append_char(':');
    append_dec(static_cast<u64>(line));
    append_char(' ');
  }

  void append_level_tag(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Debug: append_str("[D] "); break;
    case LogLevel::Info:  append_str("[I] "); break;
    case LogLevel::Warn:  append_str("[W] "); break;
    case LogLevel::Error: append_str("[E] "); break;
    case LogLevel::Panic: append_str("[P] "); break;
    default: append_str("[?] "); break;
    }
  }

  void flush_line() noexcept {
    if (pos_ == 0) return;
    if (pos_ < BUFFER_SIZE - 1) {
      buf_[pos_++] = '\n';
    }
    buf_[pos_] = '\0';
    uart::puts(buf_);
    pos_ = 0;
  }

  [[nodiscard]] auto pos() const noexcept -> u32 { return pos_; }

private:
  char buf_[BUFFER_SIZE]{};
  u32  pos_{0};
};

// ============================================================================
// Format spec parsing — detect {}, {:#x}, {:#b} in format strings
// ============================================================================

enum class FmtSpec : u8 {
  Auto,   // {} — decimal for numbers, string for const char*
  Hex,    // {:#x} or {:x} — hexadecimal
  Bool,   // {:#b} — boolean
};

// Parse a format spec starting after '{'. Returns the spec and advances
// the pointer past the closing '}'.
inline auto parse_fmt_spec(const char *&p) noexcept -> FmtSpec {
  FmtSpec spec = FmtSpec::Auto;

  if (*p == '}') {
    p++; // skip '}'
    return spec;
  }

  // Skip ':'
  if (*p == ':') {
    p++;
    // Skip optional '#'
    if (*p == '#') p++;

    if (*p == 'x') {
      spec = FmtSpec::Hex;
      p++;
    } else if (*p == 'b') {
      spec = FmtSpec::Bool;
      p++;
    }
  }

  // Skip to closing '}'
  while (*p && *p != '}') p++;
  if (*p == '}') p++;

  return spec;
}

// ============================================================================
// Type-safe argument formatting via overloaded format_arg
// ============================================================================

inline void format_arg(LogBuffer &buf, FmtSpec spec, u64 value) noexcept {
  if (spec == FmtSpec::Hex) { buf.append_hex(value); }
  else if (spec == FmtSpec::Bool) { buf.append_bool(value != 0); }
  else { buf.append_dec(value); }
}

inline void format_arg(LogBuffer &buf, FmtSpec spec, i64 value) noexcept {
  if (spec == FmtSpec::Hex) { buf.append_hex(static_cast<u64>(value)); }
  else { buf.append_signed(value); }
}

inline void format_arg(LogBuffer &buf, FmtSpec spec, u32 value) noexcept {
  format_arg(buf, spec, static_cast<u64>(value));
}

inline void format_arg(LogBuffer &buf, FmtSpec spec, i32 value) noexcept {
  format_arg(buf, spec, static_cast<i64>(value));
}

// Handle 'long' which may differ from i32/i64 on some platforms
inline void format_arg(LogBuffer &buf, FmtSpec spec, long value) noexcept {
  format_arg(buf, spec, static_cast<i64>(value));
}

inline void format_arg(LogBuffer &buf, FmtSpec spec, unsigned long value) noexcept {
  format_arg(buf, spec, static_cast<u64>(value));
}

inline void format_arg(LogBuffer &buf, FmtSpec /*spec*/, const char *value) noexcept {
  buf.append_str(value ? value : "(null)");
}

inline void format_arg(LogBuffer &buf, FmtSpec /*spec*/, char value) noexcept {
  buf.append_char(value);
}

inline void format_arg(LogBuffer &buf, FmtSpec /*spec*/, bool value) noexcept {
  buf.append_bool(value);
}

inline void format_arg(LogBuffer &buf, FmtSpec spec, const void *value) noexcept {
  (void)spec;
  buf.append_hex(reinterpret_cast<u64>(value));
}

// ============================================================================
// Core format engine — walks format string, substitutes {} with args
// ============================================================================

// Base case: no more arguments — just append remaining format string
inline void format_into(LogBuffer &buf, const char *fmt) noexcept {
  while (*fmt) {
    if (*fmt == '{' && *(fmt + 1)) {
      fmt++; // skip '{'
      // Literal {{ → '{'
      if (*fmt == '{') {
        buf.append_char('{');
        fmt++;
        continue;
      }
      // No more args — output {} placeholder literally
      buf.append_str("<?>");
      while (*fmt && *fmt != '}') fmt++;
      if (*fmt == '}') fmt++;
    } else if (*fmt == '}' && *(fmt + 1) == '}') {
      // Literal }} → '}'
      buf.append_char('}');
      fmt += 2;
    } else {
      buf.append_char(*fmt++);
    }
  }
}

// Recursive case: format the first arg, then recurse for the rest
template <typename T, typename... Rest>
inline void format_into(LogBuffer &buf, const char *fmt, T value,
                        Rest... rest) noexcept {
  while (*fmt) {
    if (*fmt == '{' && *(fmt + 1)) {
      fmt++; // skip '{'
      // Literal {{ → '{'
      if (*fmt == '{') {
        buf.append_char('{');
        fmt++;
        continue;
      }
      // Parse format spec and format this argument
      FmtSpec spec = parse_fmt_spec(fmt);
      format_arg(buf, spec, value);
      // Recurse for remaining args
      format_into(buf, fmt, rest...);
      return;
    } else if (*fmt == '}' && *(fmt + 1) == '}') {
      buf.append_char('}');
      fmt += 2;
    } else {
      buf.append_char(*fmt++);
    }
  }
  // Format string ended but we still have args — silently ignore
}

// ============================================================================
// LogEntry — RAII log line builder
//
// Supports both styles:
//   auto-flush:    klog::info("tick={} cpu={}", count, cpu_id);
//   chaining:      klog::info("tick=").u64(count).str(" cpu=").u32(cpu_id);
// ============================================================================

class LogEntry {
public:
  LogEntry(LogLevel level, bool active) noexcept
      : active_(active) {
    if (!active_) return;
    buf_.append_level_tag(level);
  }

  LogEntry(const LogEntry &) = delete;
  auto operator=(const LogEntry &) -> LogEntry & = delete;

  // Move constructor needed for factory return
  LogEntry(LogEntry &&other) noexcept
      : buf_(other.buf_), active_(other.active_) {
    other.active_ = false; // Prevent double-flush
  }

  ~LogEntry() noexcept { flush(); }

  // -- Chaining API --

  auto str(const char *s) noexcept -> LogEntry & {
    if (active_) buf_.append_str(s);
    return *this;
  }

  auto chr(char c) noexcept -> LogEntry & {
    if (active_) buf_.append_char(c);
    return *this;
  }

  auto u64(moss::u64 value) noexcept -> LogEntry & {
    if (active_) buf_.append_dec(value);
    return *this;
  }

  auto i64(moss::i64 value) noexcept -> LogEntry & {
    if (active_) buf_.append_signed(value);
    return *this;
  }

  auto u32(moss::u32 value) noexcept -> LogEntry & {
    if (active_) buf_.append_dec(static_cast<moss::u64>(value));
    return *this;
  }

  auto i32(moss::i32 value) noexcept -> LogEntry & {
    if (active_) buf_.append_signed(static_cast<moss::i64>(value));
    return *this;
  }

  auto hex(moss::u64 value) noexcept -> LogEntry & {
    if (active_) buf_.append_hex(value);
    return *this;
  }

  auto ptr(const void *p) noexcept -> LogEntry & {
    if (active_) buf_.append_hex(reinterpret_cast<moss::u64>(p));
    return *this;
  }

  auto boolean(bool v) noexcept -> LogEntry & {
    if (active_) buf_.append_bool(v);
    return *this;
  }

  void endl() noexcept { flush(); }

private:
  friend struct klog;

  LogBuffer buf_{};
  bool active_;

  void flush() noexcept {
    if (!active_) return;
    buf_.flush_line();
    active_ = false;
  }
};

// ============================================================================
// klog — primary logging interface
//
// fmt-style (preferred):
//   klog::info("count={} addr={:#x}", count, addr);
//
// Simple message:
//   klog::info("kernel started");
//
// Chaining (for complex output):
//   klog::info("data: ").hex(addr).str(" size=").u64(size);
// ============================================================================

struct klog {
  // -- fmt-style API with {} placeholders --
  // FmtStr's implicit constructor captures __builtin_FILE()/__builtin_LINE()
  // at the call site, so callers need no syntax change.

  template <typename... Args>
  static void debug(FmtStr fmt, Args... args) noexcept {
    log_fmt(LogLevel::Debug, fmt, args...);
  }

  template <typename... Args>
  static void info(FmtStr fmt, Args... args) noexcept {
    log_fmt(LogLevel::Info, fmt, args...);
  }

  template <typename... Args>
  static void warn(FmtStr fmt, Args... args) noexcept {
    log_fmt(LogLevel::Warn, fmt, args...);
  }

  template <typename... Args>
  static void error(FmtStr fmt, Args... args) noexcept {
    log_fmt(LogLevel::Error, fmt, args...);
  }

  template <typename... Args>
  static void panic(FmtStr fmt, Args... args) noexcept {
    log_fmt(LogLevel::Panic, fmt, args...);
  }

  // -- Chaining API (returns LogEntry for .str()/.u64()/.hex() etc.) --

  static auto debug_chain(const char *prefix = "") noexcept -> LogEntry {
    LogEntry e(LogLevel::Debug, LogLevel::Debug >= g_log_level);
    if (LogLevel::Debug >= g_log_level) e.buf_.append_str(prefix);
    return e;
  }

  static auto info_chain(const char *prefix = "") noexcept -> LogEntry {
    LogEntry e(LogLevel::Info, LogLevel::Info >= g_log_level);
    if (LogLevel::Info >= g_log_level) e.buf_.append_str(prefix);
    return e;
  }

  static auto warn_chain(const char *prefix = "") noexcept -> LogEntry {
    LogEntry e(LogLevel::Warn, LogLevel::Warn >= g_log_level);
    if (LogLevel::Warn >= g_log_level) e.buf_.append_str(prefix);
    return e;
  }

  static auto error_chain(const char *prefix = "") noexcept -> LogEntry {
    LogEntry e(LogLevel::Error, LogLevel::Error >= g_log_level);
    if (LogLevel::Error >= g_log_level) e.buf_.append_str(prefix);
    return e;
  }

  static auto panic_chain(const char *prefix = "") noexcept -> LogEntry {
    LogEntry e(LogLevel::Panic, LogLevel::Panic >= g_log_level);
    if (LogLevel::Panic >= g_log_level) e.buf_.append_str(prefix);
    return e;
  }

private:
  template <typename... Args>
  static void log_fmt(LogLevel level, FmtStr fmt, Args... args) noexcept {
    if (level < g_log_level) return;
    LogBuffer buf;
    buf.append_level_tag(level);
    buf.append_source_loc(fmt.file, fmt.line);
    format_into(buf, fmt.value, args...);
    buf.flush_line();
  }
};

} // namespace moss::kernel::logging
