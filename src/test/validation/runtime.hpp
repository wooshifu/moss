#pragma once

#include "validation_internal.hpp"

namespace moss::test::validation {
bool is_lifecycle();
const char *selected_suite();
const char *running_case();
bool boot_option(const char *key, char *out, usize capacity);
u64 numeric_boot_option(const char *key, u64 fallback);
bool affinity_valid();
void start_case(const char *name);
void end_case();
[[noreturn]] void finish(const char *reason = "complete");
[[noreturn]] void invalid_control();

// All strings come from the bounded catalog or fixed diagnostic identifiers.
// One complete record per UART write avoids allocating a formatting buffer.
class Event {
  // Fixed 1 KiB serial-record budget avoids heap allocation. Overflow marks the
  // run failed instead of emitting a truncated record that could parse as success.
  char buffer_[1024]{};
  usize used_ = 0;
  bool overflow_ = false;
  void append(char c) {
    if (used_ + 1 >= sizeof(buffer_)) {
      overflow_ = true;
      return;
    }
    buffer_[used_++] = c;
    buffer_[used_] = 0;
  }
  void append(const char *s) {
    while (*s) {
      append(*s++);
    }
  }

public:
  explicit Event(const char *type) {
    append("@@MOSS {\"v\":1");
    str("event", type);
    str("workload", selected_suite());
  }
  Event &str(const char *key, const char *value) {
    append(",\"");
    append(key);
    append("\":\"");
    for (; *value; ++value) {
      if (*value == '\\' || *value == '"') {
        append('\\');
      }
      append(*value);
    }
    append('"');
    return *this;
  }
  Event &number(const char *key, u64 value) {
    append(",\"");
    append(key);
    append("\":");
    // A u64 needs at most 20 decimal digits; append adds them individually, so
    // this reverse-order scratch array needs no null terminator.
    char digits[20];
    unsigned n = 0;
    do {
      digits[n++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value);
    while (n) {
      append(digits[--n]);
    }
    return *this;
  }
  void send() {
    append("}\n");
    if (overflow_) {
      kernel_test_exit(2);
    }
    hal::uart::puts(buffer_);
  }
};

} // namespace moss::test::validation
