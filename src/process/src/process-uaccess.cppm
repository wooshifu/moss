export module moss.process:uaccess;
import moss.types;

export namespace moss::kernel::process {
// Select and retain the calling thread's AddressSpace version, independently
// of later Process replacement or CPU root changes. Check the complete range,
// then resolve and copy each page under its VM transaction. Return bytes NOT
// copied (zero on complete success); neither unmap nor fork can retire/share a
// frame in the middle of its physical-alias copy.
// Copies are not transactional: a prefix may have been copied. On input
// failure the uncopied kernel-buffer tail is zeroed. A stable kernel buffer
// of at least size bytes is required. These are short synchronous page leases,
// not long-lived DMA pins, atomic snapshots of concurrently written contents,
// or a substitute for active-root coordination and remote TLB shootdown.
[[nodiscard]] usize copy_from_user(void *destination, u64 source, usize size) noexcept;
[[nodiscard]] usize copy_to_user(u64 destination, const void *source, usize size) noexcept;
} // namespace moss::kernel::process
