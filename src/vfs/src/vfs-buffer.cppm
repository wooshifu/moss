export module moss.vfs:buffer;
import moss.types;

export namespace moss::kernel::vfs {
// Borrowed, bounded I/O views. Kernel callers supply stable kernel storage;
// syscall callers supply the shared user-copy policy. A policy returns bytes
// NOT copied, whereas these views return bytes copied. No raw pointer escapes
// to a file operation. Views do not pin memory or synchronize file state.
class InputBuffer {
public:
  using UserCopy = usize (*)(void *, u64, usize) noexcept;
  static InputBuffer kernel(const void *source, usize size) noexcept {
    return {reinterpret_cast<u64>(source), size, nullptr};
  }
  // A missing user-copy policy must invalidate the view, never enable the
  // raw kernel-pointer path and bypass fault containment.
  static InputBuffer user(u64 source, usize size, UserCopy copy) noexcept { return {copy ? source : 0, size, copy}; }
  usize size() const noexcept { return size_; }
  bool valid() const noexcept { return address_ && size_ <= ~u64{0} - address_; }
  usize copy_to(usize offset, void *destination, usize count) const noexcept {
    if (!valid() || offset > size_ || count > size_ - offset)
      return 0;
    if (copy_)
      return count - copy_(destination, address_ + offset, count);
    const auto *source = reinterpret_cast<const u8 *>(address_ + offset);
    auto *target = static_cast<u8 *>(destination);
    for (usize i = 0; i < count; ++i)
      target[i] = source[i];
    return count;
  }

private:
  InputBuffer(u64 address, usize size, UserCopy copy) noexcept : address_(address), size_(size), copy_(copy) {}
  u64 address_;
  usize size_;
  UserCopy copy_;
};

class OutputBuffer {
public:
  using UserCopy = usize (*)(u64, const void *, usize) noexcept;
  static OutputBuffer kernel(void *destination, usize size) noexcept {
    return {reinterpret_cast<u64>(destination), size, nullptr};
  }
  // As with InputBuffer, address zero makes a missing policy fail closed.
  static OutputBuffer user(u64 destination, usize size, UserCopy copy) noexcept {
    return {copy ? destination : 0, size, copy};
  }
  usize size() const noexcept { return size_; }
  bool valid() const noexcept { return address_ && size_ <= ~u64{0} - address_; }
  usize copy_from(usize offset, const void *source, usize count) const noexcept {
    if (!valid() || offset > size_ || count > size_ - offset)
      return 0;
    if (copy_)
      return count - copy_(address_ + offset, source, count);
    auto *target = reinterpret_cast<u8 *>(address_ + offset);
    const auto *data = static_cast<const u8 *>(source);
    for (usize i = 0; i < count; ++i)
      target[i] = data[i];
    return count;
  }

private:
  OutputBuffer(u64 address, usize size, UserCopy copy) noexcept : address_(address), size_(size), copy_(copy) {}
  u64 address_;
  usize size_;
  UserCopy copy_;
};
} // namespace moss::kernel::vfs
