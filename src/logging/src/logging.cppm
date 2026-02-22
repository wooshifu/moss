// MOSS Kernel Logging Module — Linux-style printk architecture
//
// Three-layer design modeled on Linux's printk:
//
//   1. FORMAT LAYER: Callers format messages into stack-local LogBuffer
//      (concurrent, no lock needed — each CPU has its own stack).
//
//   2. RING BUFFER LAYER: Formatted text is copied into a global 32KB
//      lockless ring buffer as complete log records (LogRecordHeader +
//      text). A brief IrqSpinLock (write_lock_) serializes the copy
//      (~sub-microsecond hold time).
//
//   3. CONSOLE LAYER: After writing a record, the writer tries to acquire
//      console_lock_ (IrqSpinLock, non-blocking try_lock). If acquired,
//      it drains all pending records to UART via uart::puts(). Other CPUs
//      return immediately — only one CPU does the slow UART output.
//
// Emergency path: klog::panic() bypasses the ring buffer entirely and
// writes directly to UART with interrupts disabled, ensuring output
// even if the ring buffer or locks are corrupted.
//
// API unchanged:
//   klog::info("tick={} cpu={}", count, cpu_id);       // fmt-style
//   klog::info("data: ").hex(addr).str(" sz=").u64(n); // chaining
//
// Backend: hal::uart (architecture-independent UART/serial output)

export module moss.logging;

import moss.std;
import moss.types;
import moss.hal.uart;
import moss.arch;
import moss.containers;

export namespace moss::kernel::logging {

using moss::i32;
using moss::i64;
using moss::u16;
using moss::u32;
using moss::u64;
using moss::u8;
namespace uart = moss::kernel::hal::uart;
namespace arch = moss::kernel::arch;
using containers::AtomicU32;
using containers::AtomicU64;
using containers::IrqSpinLock;
using containers::LockGuard;

// ============================================================================
// FmtStr — format string wrapper that captures source location at call site
// ============================================================================

#if __has_builtin(__builtin_FILE_NAME)
#define MOSS_LOG_FILE_BUILTIN __builtin_FILE_NAME()
inline constexpr bool kFileBuiltinIsBareNameOnly = true;
#else
#define MOSS_LOG_FILE_BUILTIN __builtin_FILE()
inline constexpr bool kFileBuiltinIsBareNameOnly = false;
#endif

struct FmtStr {
  const char *value;
  const char *file;
  unsigned line;

  constexpr FmtStr(const char *s, const char *f = MOSS_LOG_FILE_BUILTIN, unsigned l = __builtin_LINE()) noexcept
      : value(s), file(f), line(l) {}
};

// ============================================================================
// Log levels
// ============================================================================

enum class LogLevel : u8 {
  Debug = 0,
  Info = 1,
  Warn = 2,
  Error = 3,
  Panic = 4,
};

inline LogLevel g_log_level = LogLevel::Debug;

inline void set_log_level(LogLevel level) noexcept { g_log_level = level; }
inline auto get_log_level() noexcept -> LogLevel { return g_log_level; }

// ============================================================================
// LogBuffer — fixed-size stack buffer with formatting primitives
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
    if (!s) {
      return;
    }
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

  void append_bool(bool v) noexcept { append_str(v ? "true" : "false"); }

  void append_source_loc(const char *file, unsigned line) noexcept {
    if (!file) {
      return;
    }
    const char *name = file;
    if constexpr (!kFileBuiltinIsBareNameOnly) {
      for (const char *p = file; *p; p++) {
        if (*p == '/' || *p == '\\') {
          name = p + 1;
        }
      }
    }
    append_str(name);
    append_char(':');
    append_dec(static_cast<u64>(line));
    append_char(' ');
  }

  void append_level_tag(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Debug:
      append_str("[D] ");
      break;
    case LogLevel::Info:
      append_str("[I] ");
      break;
    case LogLevel::Warn:
      append_str("[W] ");
      break;
    case LogLevel::Error:
      append_str("[E] ");
      break;
    case LogLevel::Panic:
      append_str("[P] ");
      break;
    default:
      append_str("[?] ");
      break;
    }
  }

  // Direct UART flush — used only by the emergency (panic) path.
  void flush_line_direct() noexcept {
    if (pos_ == 0) {
      return;
    }
    if (pos_ < BUFFER_SIZE - 1) {
      buf_[pos_++] = '\n';
    }
    buf_[pos_] = '\0';
    uart::puts(buf_);
    pos_ = 0;
  }

  [[nodiscard]] auto pos() const noexcept -> u32 { return pos_; }
  [[nodiscard]] auto data() const noexcept -> const char * { return buf_; }

private:
  char buf_[BUFFER_SIZE]{};
  u32 pos_{0};
};

// ============================================================================
// Format spec parsing
// ============================================================================

enum class FmtSpec : u8 {
  Auto,
  Hex,
  Bool,
};

inline auto parse_fmt_spec(const char *&p) noexcept -> FmtSpec {
  FmtSpec spec = FmtSpec::Auto;

  if (*p == '}') {
    p++;
    return spec;
  }

  if (*p == ':') {
    p++;
    if (*p == '#') {
      p++;
    }

    if (*p == 'x') {
      spec = FmtSpec::Hex;
      p++;
    } else if (*p == 'b') {
      spec = FmtSpec::Bool;
      p++;
    }
  }

  while (*p && *p != '}') {
    p++;
  }
  if (*p == '}') {
    p++;
  }

  return spec;
}

// ============================================================================
// Type-safe argument formatting via overloaded format_arg
// ============================================================================

inline void format_arg(LogBuffer &buf, FmtSpec spec, u64 value) noexcept {
  if (spec == FmtSpec::Hex) {
    buf.append_hex(value);
  } else if (spec == FmtSpec::Bool) {
    buf.append_bool(value != 0);
  } else {
    buf.append_dec(value);
  }
}

inline void format_arg(LogBuffer &buf, FmtSpec spec, i64 value) noexcept {
  if (spec == FmtSpec::Hex) {
    buf.append_hex(static_cast<u64>(value));
  } else {
    buf.append_signed(value);
  }
}

inline void format_arg(LogBuffer &buf, FmtSpec spec, u32 value) noexcept {
  format_arg(buf, spec, static_cast<u64>(value));
}

inline void format_arg(LogBuffer &buf, FmtSpec spec, i32 value) noexcept {
  format_arg(buf, spec, static_cast<i64>(value));
}

inline void format_arg(LogBuffer &buf, FmtSpec spec, long value) noexcept {
  format_arg(buf, spec, static_cast<i64>(value));
}

inline void format_arg(LogBuffer &buf, FmtSpec spec, unsigned long value) noexcept {
  format_arg(buf, spec, static_cast<u64>(value));
}

inline void format_arg(LogBuffer &buf, FmtSpec /*spec*/, const char *value) noexcept {
  buf.append_str(value ? value : "(null)");
}

inline void format_arg(LogBuffer &buf, FmtSpec /*spec*/, char value) noexcept { buf.append_char(value); }

inline void format_arg(LogBuffer &buf, FmtSpec /*spec*/, bool value) noexcept { buf.append_bool(value); }

inline void format_arg(LogBuffer &buf, FmtSpec spec, const void *value) noexcept {
  (void)spec;
  buf.append_hex(reinterpret_cast<u64>(value));
}

// ============================================================================
// Core format engine
// ============================================================================

inline void format_into(LogBuffer &buf, const char *fmt) noexcept {
  while (*fmt) {
    if (*fmt == '{' && *(fmt + 1)) {
      fmt++;
      if (*fmt == '{') {
        buf.append_char('{');
        fmt++;
        continue;
      }
      buf.append_str("<?>");
      while (*fmt && *fmt != '}') {
        fmt++;
      }
      if (*fmt == '}') {
        fmt++;
      }
    } else if (*fmt == '}' && *(fmt + 1) == '}') {
      buf.append_char('}');
      fmt += 2;
    } else {
      buf.append_char(*fmt++);
    }
  }
}

template <typename T, typename... Rest>
inline void format_into(LogBuffer &buf, const char *fmt, T value, Rest... rest) noexcept {
  while (*fmt) {
    if (*fmt == '{' && *(fmt + 1)) {
      fmt++;
      if (*fmt == '{') {
        buf.append_char('{');
        fmt++;
        continue;
      }
      FmtSpec spec = parse_fmt_spec(fmt);
      format_arg(buf, spec, value);
      format_into(buf, fmt, rest...);
      return;
    }
    if (*fmt == '}' && *(fmt + 1) == '}') {
      buf.append_char('}');
      fmt += 2;
    } else {
      buf.append_char(*fmt++);
    }
  }
}

// ============================================================================
// PrintkRingBuffer — Linux-style log record ring buffer
//
// Records are stored as LogRecordHeader (16 bytes) + text + padding.
// write_lock_ serializes writes (held ~sub-μs for memcpy).
// console_lock_ serializes UART drain (held for uart::puts duration).
// emergency_mode_ bypasses everything for panic output.
// ============================================================================

struct LogRecordHeader {
  u16 total_len; // header + text + '\0' + padding (8-byte aligned)
  u8 level;      // LogLevel as u8; 0xFF = skip-marker (padding)
  u8 cpu_id;     // writer CPU
  u32 seq;       // monotonic sequence number
  u64 timestamp; // arch::get_timestamp_counter()
};

inline constexpr u32 RING_BUFFER_SIZE = 32768; // 32KB, power of 2
inline constexpr u32 RING_BUFFER_MASK = RING_BUFFER_SIZE - 1;
inline constexpr u8 SKIP_MARKER = 0xFF;
inline constexpr u16 MAX_RECORD_TEXT = 496;

class PrintkRingBuffer {
public:
  // Write a formatted log line into the ring buffer, then try to drain.
  void emit(LogLevel level, const char *text, u32 text_len) noexcept {
    if (text_len > MAX_RECORD_TEXT) {
      text_len = MAX_RECORD_TEXT;
    }

    // Record size: header + text + '\n' + '\0', padded to 8 bytes
    u32 record_len = (static_cast<u32>(sizeof(LogRecordHeader)) + text_len + 2 + 7) & ~7U;

    u8 cpu = static_cast<u8>(arch::get_current_cpu_id() % moss::kernel::MAX_CPUS);
    u64 ts = arch::get_timestamp_counter();

    {
      LockGuard<IrqSpinLock> guard(write_lock_);

      u32 seq = next_seq_++;

      u64 wp = write_pos_.load(containers::MemoryOrder::Relaxed);
      u32 offset = static_cast<u32>(wp) & RING_BUFFER_MASK;

      // If record would wrap around buffer end, insert skip-marker padding
      if (offset + record_len > RING_BUFFER_SIZE) {
        u32 skip_len = RING_BUFFER_SIZE - offset;
        auto *skip = reinterpret_cast<LogRecordHeader *>(&buf_[offset]);
        skip->total_len = static_cast<u16>(skip_len);
        skip->level = SKIP_MARKER;
        skip->cpu_id = 0;
        skip->seq = seq;
        skip->timestamp = 0;
        wp += skip_len;
        write_pos_.store(wp, containers::MemoryOrder::Relaxed);
        offset = 0;
        seq = next_seq_++;
      }

      // Write record header
      auto *hdr = reinterpret_cast<LogRecordHeader *>(&buf_[offset]);
      hdr->total_len = static_cast<u16>(record_len);
      hdr->level = static_cast<u8>(level);
      hdr->cpu_id = cpu;
      hdr->seq = seq;
      hdr->timestamp = ts;

      // Copy text after header, append newline + null-terminate
      char *dst = &buf_[offset + sizeof(LogRecordHeader)];
      for (u32 i = 0; i < text_len; ++i) {
        dst[i] = text[i];
      }
      dst[text_len] = '\n';
      dst[text_len + 1] = '\0';

      write_pos_.store(wp + record_len, containers::MemoryOrder::Release);
    }

    // Try to drain buffered records to UART console
    try_drain();
  }

  // Attempt to drain all pending records to UART.
  // Non-blocking: returns immediately if another CPU is already draining.
  void try_drain() noexcept {
    if (!console_lock_.try_lock()) {
      return;
    }

    // Read write_pos_ snapshot — Acquire pairs with Release in emit().
    u64 wp = write_pos_.load(containers::MemoryOrder::Acquire);

    while (read_pos_ < wp) {
      u32 offset = static_cast<u32>(read_pos_) & RING_BUFFER_MASK;

      // Handle buffer overflow: if read_pos_ is too far behind,
      // skip ahead to avoid reading overwritten data.
      if (wp - read_pos_ > RING_BUFFER_SIZE) {
        read_pos_ = wp - RING_BUFFER_SIZE;
        offset = static_cast<u32>(read_pos_) & RING_BUFFER_MASK;
      }

      auto *hdr = reinterpret_cast<LogRecordHeader *>(&buf_[offset]);

      if (hdr->level == SKIP_MARKER) {
        // Skip padding record
        read_pos_ += hdr->total_len;
        continue;
      }

      // Output the text (already null-terminated by emit)
      const char *text = &buf_[offset + sizeof(LogRecordHeader)];
      uart::puts(text);

      read_pos_ += hdr->total_len;
    }

    console_lock_.unlock();
  }

  // Set emergency mode — all subsequent output goes direct to UART
  void set_emergency() noexcept { emergency_mode_.store(1, containers::MemoryOrder::Release); }

  [[nodiscard]] bool is_emergency() const noexcept {
    return emergency_mode_.load(containers::MemoryOrder::Relaxed) != 0;
  }

private:
  alignas(64) char buf_[RING_BUFFER_SIZE]{};

  // Cursor positions (monotonically increasing, masked for buffer access).
  // write_pos_: updated under write_lock_, read (unsynchronized) in try_drain()
  //   → atomic to avoid data race between writer and drainer.
  // read_pos_: updated only under console_lock_ → plain u64 is fine.
  alignas(64) AtomicU64 write_pos_; // updated under write_lock_
  alignas(64) u64 read_pos_{0};     // protected by console_lock_
  u32 next_seq_{0};                 // protected by write_lock_

  alignas(64) IrqSpinLock write_lock_{};
  alignas(64) IrqSpinLock console_lock_{};
  AtomicU32 emergency_mode_{0};
};

// Global ring buffer instance — static storage, zero-initialized
inline PrintkRingBuffer g_printk_rb;

// ============================================================================
// LogEntry — RAII log line builder (routes through ring buffer)
// ============================================================================

class LogEntry {
public:
  LogEntry(LogLevel level, bool active) noexcept : level_(level), active_(active) {
    if (!active_) {
      return;
    }
    buf_.append_level_tag(level);
  }

  LogEntry(const LogEntry &) = delete;
  auto operator=(const LogEntry &) -> LogEntry & = delete;

  LogEntry(LogEntry &&other) noexcept : buf_(other.buf_), level_(other.level_), active_(other.active_) {
    other.active_ = false;
  }

  ~LogEntry() noexcept { flush(); }

  // -- Chaining API --

  auto str(const char *s) noexcept -> LogEntry & {
    if (active_) {
      buf_.append_str(s);
    }
    return *this;
  }

  auto chr(char c) noexcept -> LogEntry & {
    if (active_) {
      buf_.append_char(c);
    }
    return *this;
  }

  auto u64(moss::u64 value) noexcept -> LogEntry & {
    if (active_) {
      buf_.append_dec(value);
    }
    return *this;
  }

  auto i64(moss::i64 value) noexcept -> LogEntry & {
    if (active_) {
      buf_.append_signed(value);
    }
    return *this;
  }

  auto u32(moss::u32 value) noexcept -> LogEntry & {
    if (active_) {
      buf_.append_dec(static_cast<moss::u64>(value));
    }
    return *this;
  }

  auto i32(moss::i32 value) noexcept -> LogEntry & {
    if (active_) {
      buf_.append_signed(static_cast<moss::i64>(value));
    }
    return *this;
  }

  auto hex(moss::u64 value) noexcept -> LogEntry & {
    if (active_) {
      buf_.append_hex(value);
    }
    return *this;
  }

  auto ptr(const void *p) noexcept -> LogEntry & {
    if (active_) {
      buf_.append_hex(reinterpret_cast<moss::u64>(p));
    }
    return *this;
  }

  auto boolean(bool v) noexcept -> LogEntry & {
    if (active_) {
      buf_.append_bool(v);
    }
    return *this;
  }

  void endl() noexcept { flush(); }

private:
  friend struct klog;

  LogBuffer buf_{};
  LogLevel level_;
  bool active_;

  void flush() noexcept {
    if (!active_) {
      return;
    }
    if (level_ == LogLevel::Panic || g_printk_rb.is_emergency()) {
      // Emergency: bypass ring buffer, write directly to UART
      buf_.flush_line_direct();
    } else {
      // Normal: write to ring buffer, then try to drain
      if (buf_.pos() > 0) {
        g_printk_rb.emit(level_, buf_.data(), buf_.pos());
      }
    }
    active_ = false;
  }
};

// ============================================================================
// klog — primary logging interface
// ============================================================================

struct klog {
  template <typename... Args> static void debug(FmtStr fmt, Args... args) noexcept {
    log_fmt(LogLevel::Debug, fmt, args...);
  }

  template <typename... Args> static void info(FmtStr fmt, Args... args) noexcept {
    log_fmt(LogLevel::Info, fmt, args...);
  }

  template <typename... Args> static void warn(FmtStr fmt, Args... args) noexcept {
    log_fmt(LogLevel::Warn, fmt, args...);
  }

  template <typename... Args> static void error(FmtStr fmt, Args... args) noexcept {
    log_fmt(LogLevel::Error, fmt, args...);
  }

  template <typename... Args> static void panic(FmtStr fmt, Args... args) noexcept {
    log_fmt(LogLevel::Panic, fmt, args...);
  }

  // -- Chaining API --

  static auto debug_chain(const char *prefix = "") noexcept -> LogEntry {
    LogEntry e(LogLevel::Debug, LogLevel::Debug >= g_log_level);
    if (LogLevel::Debug >= g_log_level) {
      e.buf_.append_str(prefix);
    }
    return e;
  }

  static auto info_chain(const char *prefix = "") noexcept -> LogEntry {
    LogEntry e(LogLevel::Info, LogLevel::Info >= g_log_level);
    if (LogLevel::Info >= g_log_level) {
      e.buf_.append_str(prefix);
    }
    return e;
  }

  static auto warn_chain(const char *prefix = "") noexcept -> LogEntry {
    LogEntry e(LogLevel::Warn, LogLevel::Warn >= g_log_level);
    if (LogLevel::Warn >= g_log_level) {
      e.buf_.append_str(prefix);
    }
    return e;
  }

  static auto error_chain(const char *prefix = "") noexcept -> LogEntry {
    LogEntry e(LogLevel::Error, LogLevel::Error >= g_log_level);
    if (LogLevel::Error >= g_log_level) {
      e.buf_.append_str(prefix);
    }
    return e;
  }

  static auto panic_chain(const char *prefix = "") noexcept -> LogEntry {
    LogEntry e(LogLevel::Panic, LogLevel::Panic >= g_log_level);
    if (LogLevel::Panic >= g_log_level) {
      e.buf_.append_str(prefix);
    }
    return e;
  }

private:
  template <typename... Args> static void log_fmt(LogLevel level, FmtStr fmt, Args... args) noexcept {
    if (level < g_log_level) {
      return;
    }

    LogBuffer buf;
    buf.append_level_tag(level);
    buf.append_source_loc(fmt.file, fmt.line);
    format_into(buf, fmt.value, args...);

    if (level == LogLevel::Panic || g_printk_rb.is_emergency()) {
      // Emergency: set flag + direct UART output
      g_printk_rb.set_emergency();
      arch::disable_all_interrupts();
      buf.flush_line_direct();
      return;
    }

    // Normal path: ring buffer + try_drain
    if (buf.pos() > 0) {
      g_printk_rb.emit(level, buf.data(), buf.pos());
    }
  }
};

} // namespace moss::kernel::logging
