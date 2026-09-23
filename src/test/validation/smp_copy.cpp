import moss.std;
import moss.types;
import moss.arch;
import moss.abi;
import moss.hal.timer;
import moss.boot;
import moss.fdt;
import moss.mm;
import moss.vfs;
import moss.process;
import moss.timer;
import moss.containers;
import moss.smart_ptr;
import moss.hal.uart;
import moss.hal.mmu;
import moss.logging;
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "queue_regression.hpp"
#include "scheduler_regression.hpp"
#include "validation/memory_internal.hpp"
#include "validation/smp_cases.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;

namespace moss::test::validation {
struct UserCopyVersion {
  shared_ptr<process::Process> owner;
  shared_ptr<process::AddressSpace> original, replacement;
  VirtAddr address = 0;
  bool armed = false, switched = false;

  static void activate(const shared_ptr<process::AddressSpace> &space) {
    process::CfsScheduler::use_address_space(space);
  }

  void replace(PhysAddr root, VirtAddr user_address) {
    if (!armed || root != original->pgd_phys || user_address != address) {
      return;
    }
    armed = false;
    // Model resuming a previously admitted copy after image replacement. This
    // is not a complete exec/scheduler test: only the production copy's binding
    // to its selected version is under test, and both roots are kept owned.
    activate(replacement);
    switched = owner->set_address_space(replacement).has_value();
  }

  void restore() const {
    activate(original);
    smp_require(owner->set_address_space(original));
  }
};
UserCopyVersion *user_copy_version = nullptr;

void user_copy_version_binding() {
  using Tables = mm::PageTableManager;
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
  {
    UserCopyVersion probe;
    probe.owner = process::current_process();
    probe.original = probe.owner ? probe.owner->address_space() : shared_ptr<process::AddressSpace>{};
    auto replacement = process::user_space::create_user_address_space();
    smp_require(probe.original && replacement);
    probe.replacement = moss::move(*replacement);
    // The first unused mmap cursor keeps the test outside the worker's code,
    // stack and heap. No userspace thread runs while this transient VMA exists.
    probe.address = probe.original->mmap_next;
    smp_require(!probe.original->find_vma(probe.address));
    constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE;
    auto old_page = mm::allocate_pages(0), new_page = mm::allocate_pages(0);
    smp_require(old_page && new_page);
    {
      auto transaction = probe.original->lock_vm();
      smp_require(probe.original->add_vma(probe.address, probe.address + page_size, flags, process::VmaType::MMAP));
      smp_require(Tables::map_user_page(probe.original->pgd_phys, probe.address, *old_page, mm::page_perms::USER_RW));
      auto *unobserved = Tables::get_user_pte(probe.original->pgd_phys, probe.address);
      // This fresh mapping has never been accessed through a user translation.
      // Clear the eager defaults so the test observes actual copy accounting.
      u64 observations = mm::page_attr::AF;
#if defined(MOSS_ARCH_X64) || defined(MOSS_ARCH_RISCV64)
      observations |= mm::page_attr::DIRTY;
#endif
      unobserved->raw &= ~observations;
    }
    smp_require(probe.replacement->add_vma(probe.address, probe.address + page_size, flags, process::VmaType::MMAP));
    smp_require(Tables::map_user_page(probe.replacement->pgd_phys, probe.address, *new_page, mm::page_perms::USER_RW));
    auto *old_data = reinterpret_cast<u8 *>(phys_to_virt(*old_page));
    auto *new_data = reinterpret_cast<u8 *>(phys_to_virt(*new_page));
    // Distinct arbitrary bytes distinguish the admitted image, replacement
    // image and copied payload without relying on allocator contents.
    constexpr u8 admitted = 0x5a, replaced = 0xa5, payload = 0x3c;
    old_data[0] = admitted;
    new_data[0] = replaced;
    user_copy_version = &probe;
    const bool interrupts = arch::interrupts_enabled();
    arch::disable_interrupts();
    u8 received = 0;
    probe.armed = true;
    const auto unread = process::copy_from_user(&received, probe.address, sizeof(received));
    probe.restore();
    ut::expect(probe.switched && unread == 0);
    ut::expect(received == admitted);
    const auto *read = Tables::get_user_pte(probe.original->pgd_phys, probe.address);
    ut::expect(read && (read->raw & mm::page_attr::AF) != 0);
#if defined(MOSS_ARCH_X64) || defined(MOSS_ARCH_RISCV64)
    ut::expect(read && (read->raw & mm::page_attr::DIRTY) == 0);
#endif
    probe.armed = true;
    probe.switched = false;
    const auto unwritten = process::copy_to_user(probe.address, &payload, sizeof(payload));
    probe.restore();
    user_copy_version = nullptr;
    ut::expect(probe.switched && unwritten == 0);
    ut::expect(old_data[0] == payload && new_data[0] == replaced);
    const auto *written = Tables::get_user_pte(probe.original->pgd_phys, probe.address);
    ut::expect(written && (written->raw & mm::page_attr::AF) != 0);
#if defined(MOSS_ARCH_X64) || defined(MOSS_ARCH_RISCV64)
    // A kernel-alias write must record dirtiness on the user leaf, not merely
    // on the alias used for copying, so later reclaim observes the write.
    ut::expect(written && (written->raw & mm::page_attr::DIRTY) != 0);
#endif
    {
      auto transaction = probe.original->lock_vm();
      Tables::unmap_user_page(probe.original->pgd_phys, probe.address);
      ut::expect(probe.original->remove_vma(probe.address, probe.address + page_size));
    }
    if (interrupts) {
      arch::enable_interrupts();
    }
  }
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == pages);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
}

void raw_user_copy_fixup() {
  using Tables = mm::PageTableManager;
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
  {
    auto owner = process::current_process();
    auto as = owner ? owner->address_space() : shared_ptr<process::AddressSpace>{};
    smp_require(static_cast<bool>(as));
    const VirtAddr address = as->mmap_next;
    smp_require(!as->find_vma(address) && !as->find_vma(address + page_size));
    auto allocated = mm::allocate_pages(0);
    smp_require(allocated.has_value());
    {
      auto transaction = as->lock_vm();
      constexpr u32 flags = process::vma_flags::READ | process::vma_flags::WRITE;
      smp_require(as->add_vma(address, address + page_size, flags));
      smp_require(Tables::map_user_page(as->pgd_phys, address, *allocated, mm::page_perms::USER_RW));
    }
    const auto *absent = Tables::get_user_pte(as->pgd_phys, address + page_size);
    smp_require(!absent || !absent->is_valid());
    auto *bytes = reinterpret_cast<u8 *>(phys_to_virt(*allocated));
    constexpr u8 original = 0x5a, sentinel = 0xa5; // Distinguish copied and untouched bytes.
    bytes[page_size - 1] = original;
    // Two bytes starting at the final resident byte force exactly one success
    // followed by an unresolvable fault in each real assembly primitive.
    u8 output[]{sentinel, sentinel};
    const u8 input[]{sentinel, original};
    const VirtAddr last = address + page_size - 1;
    const bool interrupts = arch::interrupts_enabled();
    arch::disable_interrupts();
    // Do not hold a VM transaction: native exception entry acquires it. This
    // single-threaded fixture retains the active root until both faults return.
    ut::expect(
        moss::abi::uaccess::moss_raw_copy_from_user(output, reinterpret_cast<const void *>(last), sizeof(output)) == 1);
    ut::expect(output[0] == original && output[1] == sentinel);
    ut::expect(moss::abi::uaccess::moss_raw_copy_to_user(reinterpret_cast<void *>(last), input, sizeof(input)) == 1);
    ut::expect(bytes[page_size - 1] == sentinel);
    {
      auto transaction = as->lock_vm();
      Tables::unmap_user_page(as->pgd_phys, address);
      ut::expect(as->remove_vma(address, address + page_size));
    }
    if (interrupts) {
      arch::enable_interrupts();
    }
  }
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == pages);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
}

extern "C" void moss_validation_user_copy(PhysAddr root, VirtAddr address) noexcept {
  if (user_copy_version) {
    user_copy_version->replace(root, address);
  }
}

} // namespace moss::test::validation
