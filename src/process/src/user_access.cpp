module moss.process;

// Validation observes the real admission and per-page ownership boundaries.
// Production retains the identical policy, resolution and byte-copy path.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_user_copy(moss::kernel::PhysAddr /*root*/,
                                                                       moss::kernel::VirtAddr /*address*/) noexcept {}
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_user_page(moss::kernel::PhysAddr /*root*/,
                                                                       moss::kernel::VirtAddr /*address*/,
                                                                       moss::kernel::PhysAddr /*page*/,
                                                                       bool /*write*/) noexcept {}

namespace moss::kernel::process {
namespace {
shared_ptr<Process> current_owner() noexcept {
  auto *thread = CfsScheduler::get_current_task();
  return thread && g_process_manager ? g_process_manager->find_process(thread->owner_pid) : shared_ptr<Process>{};
}

void clear_uncopied(void *destination, usize size, usize remaining) noexcept {
  auto *tail = static_cast<u8 *>(destination) + (size - remaining);
  // Preserve the shared input-copy contract: failed bytes never expose stale
  // kernel storage, even when earlier pages were copied successfully.
  for (usize index = 0; index < remaining; ++index) {
    tail[index] = 0;
  }
}
} // namespace

usize AddressSpace::transfer_user_pages(VirtAddr address, void *kernel_output, const void *kernel_input, usize size,
                                        mm::UserFaultAccess access) noexcept {
  if (!size) {
    return 0;
  }
  const auto required = static_cast<u32>(access);
  {
    auto transaction = lock_vm();
    // Preserve all-range admission: an initially unauthorized suffix must not
    // cause a prefix to be copied. Later unmap/OOM can still cause a short copy.
    if (!allows_user_access(address, size, required)) {
      return size;
    }
  }
  moss_validation_user_copy(pgd_phys, address);
  usize copied = 0;
  const bool write = access == mm::UserFaultAccess::Write;
  while (copied < size) {
    auto transaction = lock_vm();
    const VirtAddr cursor = address + copied; // Whole-range admission checked overflow.
    const usize offset = cursor & (PAGE_SIZE - 1);
    const usize available = PAGE_SIZE - offset;
    const usize chunk = size - copied < available ? size - copied : available;
    if (!allows_user_access(cursor, chunk, required)) {
      break;
    }
    auto *pte = mm::PageTableManager::get_user_pte(pgd_phys, cursor);
    const bool present = pte && pte->is_valid();
    if (!present || (write && pte->is_cow())) {
      // Resolve through the same MM algorithm as native fault entry while
      // already holding the VM transaction; never recursively acquire it.
      if (!resolve_fault_locked(cursor, access, present, nullptr)) {
        break;
      }
      pte = mm::PageTableManager::get_user_pte(pgd_phys, cursor);
    }
    if (!pte || !pte->is_valid() || (pte->raw & mm::page_attr::USER) == 0 ||
        (write && (!pte->is_writable() || pte->is_cow()))) {
      break; // A kernel physical alias must not bypass actual leaf permissions.
    }
#if defined(MOSS_ARCH_RISCV64)
    if (!write && (pte->raw & mm::page_attr::READ) == 0) {
      break;
    }
#endif
    const PhysAddr page = pte->get_phys_addr();
    // The mapping owns the frame for this whole lease: unmap and fork use the
    // same lock, and the caller retains this AddressSpace. No user VA is touched
    // while locked, so a CPU fault cannot reenter the VM transaction.
    auto *bytes = reinterpret_cast<u8 *>(phys_to_virt(page)) + offset;
    moss_validation_user_page(pgd_phys, cursor, page, write);
    if (write) {
      const auto *input = static_cast<const u8 *>(kernel_input) + copied;
      for (usize index = 0; index < chunk; ++index) {
        bytes[index] = input[index];
      }
    } else {
      auto *output = static_cast<u8 *>(kernel_output) + copied;
      for (usize index = 0; index < chunk; ++index) {
        output[index] = bytes[index];
      }
    }
    // Hardware observes the kernel alias, not this user leaf. Preserve the
    // user mapping's access/dirty accounting without losing concurrent CPU
    // A/D updates; the VM lease keeps its frame and permissions unchanged.
    u64 observed = mm::page_attr::AF;
#if defined(MOSS_ARCH_X64) || defined(MOSS_ARCH_RISCV64)
    if (write) {
      observed |= mm::page_attr::DIRTY;
    }
#endif
    (void)__atomic_fetch_or(&pte->raw, observed, __ATOMIC_RELAXED);
    copied += chunk;
  }
  return size - copied;
}

usize AddressSpace::copy_from_user(void *destination, VirtAddr source, usize size) noexcept {
  const auto remaining = transfer_user_pages(source, destination, nullptr, size, mm::UserFaultAccess::Read);
  if (remaining != 0) {
    clear_uncopied(destination, size, remaining);
  }
  return remaining;
}

usize AddressSpace::copy_to_user(VirtAddr destination, const void *source, usize size) noexcept {
  return transfer_user_pages(destination, nullptr, source, size, mm::UserFaultAccess::Write);
}

usize copy_from_user(void *destination, u64 source, usize size) noexcept {
  if (size == 0) {
    return 0;
  }
  auto owner = current_owner();
  auto as = owner ? owner->address_space() : shared_ptr<AddressSpace>{};
  if (as) {
    return as->copy_from_user(destination, source, size);
  }
  clear_uncopied(destination, size, size);
  return size;
}

usize copy_to_user(u64 destination, const void *source, usize size) noexcept {
  if (!size) {
    return 0;
  }
  auto owner = current_owner();
  auto as = owner ? owner->address_space() : shared_ptr<AddressSpace>{};
  return as ? as->copy_to_user(destination, source, size) : size;
}
} // namespace moss::kernel::process
