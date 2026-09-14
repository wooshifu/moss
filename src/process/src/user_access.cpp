module moss.process;
import moss.abi;

namespace moss::kernel::process {
namespace {
shared_ptr<Process> current_owner() noexcept {
  auto *thread = CfsScheduler::get_current_task();
  return thread && g_process_manager ? g_process_manager->find_process(thread->owner_pid) : shared_ptr<Process>{};
}
} // namespace

usize copy_from_user(void *destination, u64 source, usize size) noexcept {
  if (!size)
    return 0;
  auto owner = current_owner();
  auto *as = owner ? owner->address_space() : nullptr;
  usize remaining = size;
  if (as && as->allows_user_access(source, size, vma_flags::READ)) {
    remaining = moss::abi::uaccess::moss_raw_copy_from_user(destination, reinterpret_cast<const void *>(source), size);
  }
  auto *tail = static_cast<u8 *>(destination) + (size - remaining);
  for (usize i = 0; i < remaining; ++i)
    tail[i] = 0;
  return remaining;
}

usize copy_to_user(u64 destination, const void *source, usize size) noexcept {
  if (!size)
    return 0;
  auto owner = current_owner();
  auto *as = owner ? owner->address_space() : nullptr;
  if (!as || !as->allows_user_access(destination, size, vma_flags::WRITE))
    return size;
  return moss::abi::uaccess::moss_raw_copy_to_user(reinterpret_cast<void *>(destination), source, size);
}
} // namespace moss::kernel::process
