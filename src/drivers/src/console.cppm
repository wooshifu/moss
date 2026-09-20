export module moss.drivers.console;

import moss.types;
import moss.result;
import moss.arch;
import moss.abi;
import moss.containers;
import moss.platform;
import moss.drivers.uart;
import moss.interrupts;

export namespace moss::kernel::drivers::console {

class RxRing {
  // One slot distinguishes full from empty. Preserve the existing 255-byte capacity.
  static constexpr usize capacity = 256;
  u8 bytes_[capacity]{};
  usize head_{0}, tail_{0};

public:
  [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }
  bool put(u8 value) noexcept {
    const usize next = (head_ + 1) & (capacity - 1);
    if (next == tail_)
      return false;
    bytes_[head_] = value;
    head_ = next;
    return true;
  }
  int get() noexcept {
    if (empty())
      return -1;
    const int value = bytes_[tail_];
    tail_ = (tail_ + 1) & (capacity - 1);
    return value;
  }
};

[[nodiscard]] VoidResult initialize() noexcept;
[[nodiscard]] bool is_initialized() noexcept;
int try_getc() noexcept;
int getc_blocking() noexcept;
} // namespace moss::kernel::drivers::console
