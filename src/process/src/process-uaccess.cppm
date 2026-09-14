export module moss.process:uaccess;
import moss.types;

export namespace moss::kernel::process {
// Access the calling thread's active address space. Check the complete user
// range and VMA permissions; demand/COW faults may resolve normally. Return
// false on an unresolvable access instead of terminating the caller.
// Copies are not transactional: a prefix may have been copied. On input
// failure the uncopied kernel-buffer tail is zeroed. A stable kernel buffer
// of at least size bytes is required. This does not pin pages across a
// concurrent VMA/PTE change or replace the address space's mutation protocol.
[[nodiscard]] bool copy_from_user(void *destination, u64 source, usize size) noexcept;
[[nodiscard]] bool copy_to_user(u64 destination, const void *source, usize size) noexcept;
} // namespace moss::kernel::process
