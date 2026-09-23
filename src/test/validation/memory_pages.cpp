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
#include "validation/memory_cases.hpp"
#include "validation/memory_internal.hpp"
#include "validation/memory_layout.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;
namespace bench = moss::bench;
using moss::test::validation::HeapPressure;

namespace moss::test::validation {
void vma_boundaries() {
  using namespace process;
  const auto before = mm::PageFrameAllocator::get_memory_stats().free_pages;
  {
    auto created = user_space::create_user_address_space();
    if (!ut::expect(created.has_value())) {
      return;
    }
    auto &space = **created;
    constexpr auto base = user_layout::MMAP_BASE;
    const VirtAddr invalid[][2] = {
        {0, page_size},
        {mm::PageTableManager::KERNEL_IDENTITY_END - page_size, mm::PageTableManager::KERNEL_IDENTITY_END},
        {USER_MAX, USER_MAX + page_size},
        {USER_MAX - page_size, USER_MAX + page_size},
        {~VirtAddr{0} - page_size + 1, page_size},
        {base, base},
        {base + page_size, base},
        {base + 1, base + page_size},
        {user_layout::SIGRETURN_PAGE, user_layout::SIGRETURN_PAGE + page_size},
    };
    for (const auto &range : invalid) {
      const bool added = space.add_vma(range[0], range[1], vma_flags::READ, VmaType::MMAP);
      ut::expect(!added);
      if (added) {
        ut::expect(space.remove_vma(range[0], range[1]));
      }
    }
    ut::expect(space.add_vma(base, base + page_size, vma_flags::READ, VmaType::MMAP));
    ut::expect(!space.add_vma(base, base + page_size, vma_flags::WRITE, VmaType::MMAP));
    ut::expect(space.remove_vma(base, base + page_size));
    ut::expect(!space.add_vma(base, base + page_size, ~u32{0}, VmaType::MMAP));
    ut::expect(
        !space.add_vma(base, base + page_size, vma_flags::READ | vma_flags::WRITE | vma_flags::EXEC, VmaType::MMAP));
    ut::expect(space.add_vma(base, base + page_size, vma_flags::READ | vma_flags::WRITE, VmaType::MMAP));
    ut::expect(space.add_vma(base + page_size, base + 2 * page_size, vma_flags::READ, VmaType::MMAP));
    ut::expect(space.allows_user_access(base + page_size - 8, 16, vma_flags::READ));
    ut::expect(!space.allows_user_access(base + page_size - 8, 16, vma_flags::WRITE));
    ut::expect(!space.allows_user_access(base + 2 * page_size - 8, 16, vma_flags::READ));
    ut::expect(!space.allows_user_access(0, 1, vma_flags::READ));
    ut::expect(!space.allows_user_access(USER_MAX - 1, 2, vma_flags::READ));
    ut::expect(space.allows_user_access(0, 0, vma_flags::READ));

    constexpr auto heap = user_layout::HEAP_START;
    constexpr u32 heap_flags = vma_flags::READ | vma_flags::WRITE | vma_flags::DEMAND_ZERO;
    ut::expect(!space.add_vma(heap + page_size, heap + 2 * page_size, heap_flags, VmaType::HEAP));
    ut::expect(space.add_vma(heap, heap, heap_flags, VmaType::HEAP));
    ut::expect(!space.add_vma(heap, heap, heap_flags, VmaType::HEAP));
    bool resized = false;
    ut::expect(space.resize_vma(heap, heap, heap + page_size, VmaType::HEAP,
                                [&](const VmaRegion &old) { resized = old.end_addr == heap; }));
    ut::expect(resized && space.find_vma(heap) && space.find_vma(heap)->end_addr == heap + page_size);
    const auto collision_start = heap + 2 * page_size;
    const auto collision_end = collision_start + page_size;
    ut::expect(space.add_vma(collision_start, collision_end, vma_flags::READ, VmaType::MMAP));
    resized = false;
    ut::expect(!space.resize_vma(heap, heap + page_size, collision_end, VmaType::HEAP,
                                 [&](const VmaRegion &) { resized = true; }));
    ut::expect(!resized && space.find_vma(heap) && space.find_vma(heap)->end_addr == heap + page_size);
    ut::expect(space.resize_vma(heap, heap + page_size, heap, VmaType::HEAP,
                                [&](const VmaRegion &old) { resized = old.end_addr == heap + page_size; }));
    ut::expect(resized && !space.find_vma(heap));
    ut::expect(space.remove_vma(heap, heap));
    ut::expect(space.remove_vma(collision_start, collision_end));

    const auto stub = user_layout::SIGRETURN_PAGE;
    ut::expect(!space.add_vma(stub, stub + page_size, vma_flags::WRITE, VmaType::SIGRETURN));
    ut::expect(space.add_vma(stub, stub + page_size, vma_flags::READ | vma_flags::EXEC, VmaType::SIGRETURN));
  }
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == before);
}

void pages() {
  // Orders 0..7 cover 1..128-page blocks held concurrently. An arbitrary
  // nonzero base pattern plus order distinguishes each block after allocation.
  auto before = mm::PageFrameAllocator::get_memory_stats().free_pages;
  PhysAddr live[8]{};
  for (usize order = 0; order < 8; ++order) {
    auto page = mm::PageFrameAllocator::allocate_pages(order);
    if (!ut::expect(static_cast<bool>(page))) {
      break;
    }
    live[order] = *page;
    ut::expect((*page & ((page_size << order) - 1)) == 0);
    for (usize other = 0; other < order; ++other) {
      ut::expect(live[other] + (page_size << other) <= *page || *page + (page_size << order) <= live[other]);
    }
    *reinterpret_cast<volatile u64 *>(*page) = 0x12345678 + order;
  }
  for (usize order = 0; order < 8; ++order) {
    if (live[order]) {
      ut::expect(*reinterpret_cast<volatile u64 *>(live[order]) == 0x12345678 + order);
      ut::expect(static_cast<bool>(mm::PageFrameAllocator::free_pages(live[order], order)));
    }
  }
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == before);
}

void reuse_pages() {
  // Cycle through orders 0..3 (1..8 pages) for 128 bounded reuse operations;
  // the count is a regression workload budget, not a production limit.
  auto before = mm::PageFrameAllocator::get_memory_stats().free_pages;
  for (usize i = 0; i < 128; ++i) {
    auto page = mm::PageFrameAllocator::allocate_pages(i % 4);
    if (!ut::expect(static_cast<bool>(page))) {
      return;
    }
    ut::expect(static_cast<bool>(mm::PageFrameAllocator::free_pages(*page, i % 4)));
  }
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == before);
  auto invalid_order = mm::PageFrameAllocator::allocate_pages(MAX_ORDER + 1);
  ut::expect(!invalid_order && invalid_order.error() == mm::PageAllocError::InvalidOrder);
  auto invalid_address = mm::PageFrameAllocator::free_pages(1, 0);
  ut::expect(!invalid_address && invalid_address.error() == mm::PageAllocError::InvalidAddress);
}

void page_release_contract() {
  using Pfa = mm::PageFrameAllocator;
  const auto before = Pfa::get_memory_stats();
  for (usize order = 0; order <= MAX_ORDER; ++order) {
    auto allocation = Pfa::allocate_pages(order);
    if (!ut::expect(static_cast<bool>(allocation))) {
      return;
    }
    const usize pages_owned = usize{1} << order;
    const auto live = Pfa::get_memory_stats();
    ut::expect((*allocation & ((page_size << order) - 1)) == 0);
    ut::expect(live.used_pages == before.used_pages + pages_owned);
    ut::expect(live.free_pages + pages_owned == before.free_pages);
    for (usize i = 0; i < pages_owned; ++i) {
      *reinterpret_cast<volatile u64 *>(*allocation + i * page_size) = *allocation ^ i;
      ut::expect(Pfa::page_ref_get(*allocation + i * page_size) == 1);
    }
    const usize wrong = order == 0 ? 1 : order - 1;
    auto wrong_order = Pfa::free_pages(*allocation, wrong);
    if (!ut::expect(!wrong_order)) {
      return; // A broken free may already have mutated part of the live block.
    }
    ut::expect(wrong_order.error() == mm::PageAllocError::InvalidOrder);
    ut::expect(!Pfa::free_pages(*allocation, MAX_ORDER + 1));
    ut::expect(!Pfa::free_pages(*allocation + 1, order));
    if (order != 0) {
      ut::expect(!Pfa::free_pages(*allocation + page_size, 0));
      // The last page being shared must reject the whole free, not partially
      // release preceding pages before discovering the outstanding reference.
      const auto last = *allocation + (pages_owned - 1) * page_size;
      Pfa::page_ref_inc(last);
      auto shared = Pfa::free_pages(*allocation, order);
      if (!ut::expect(!shared)) {
        return;
      }
      ut::expect(shared.error() == mm::PageAllocError::PageInUse);
      ut::expect(Pfa::page_ref_dec(last) == 1);
    }
    ut::expect(Pfa::get_memory_stats().free_pages == live.free_pages);
    ut::expect(Pfa::get_memory_stats().used_pages == live.used_pages);
    for (usize i = 0; i < pages_owned; ++i) {
      ut::expect(*reinterpret_cast<volatile u64 *>(*allocation + i * page_size) == (*allocation ^ i));
      ut::expect(Pfa::page_ref_get(*allocation + i * page_size) == 1);
    }
    ut::expect(static_cast<bool>(Pfa::free_pages(*allocation, order)));
    ut::expect(!Pfa::free_pages(*allocation, order));
    ut::expect(Pfa::page_ref_get(*allocation) == 0);
    ut::expect(Pfa::get_memory_stats().free_pages == before.free_pages);
    ut::expect(Pfa::get_memory_stats().used_pages == before.used_pages);
  }
  ut::expect(!Pfa::free_pages(moss::abi::linker::heap_start(), 0));
  // kernel_end is exclusive and may now be usable when metadata is elsewhere.
  ut::expect(!Pfa::free_pages(moss::abi::linker::kernel_end() - page_size, 0));
  ut::expect(!Pfa::free_pages(~PhysAddr{0} & ~(page_size - 1), 0));
  const auto &info = moss::fdt::get_platform_info();
  if (info.initrd_end > info.initrd_start) {
    ut::expect(!Pfa::free_pages(info.initrd_start & ~(page_size - 1), 0));
  }
  for (u32 i = 0; i < info.reserved_region_count; ++i) {
    ut::expect(!Pfa::free_pages(info.reserved_regions[i].base & ~(page_size - 1), 0));
  }
  ut::expect(Pfa::get_memory_stats().free_pages == before.free_pages);
  ut::expect(Pfa::get_memory_stats().used_pages == before.used_pages);
}

void page_exhaustion() {
  using Pfa = mm::PageFrameAllocator;
  LayoutSnapshot layout;
  if (!layout.capture()) {
    return;
  }
  struct Owned {
    PhysAddr address;
    usize order;
  };
  // Store block descriptors off the worker stack. The fixed 2048-entry budget
  // is not a PFA capacity: overflow fails this fixture after freeing the extra
  // block. Its sizing basis for fragmented memory maps is unrecorded.
  static Owned owned[2048]{};
  const auto &info = moss::fdt::get_platform_info();
  auto initrd_hash = [&] {
    // FNV-1a's 64-bit offset basis and prime detect accidental initrd writes;
    // this is a regression checksum, not an integrity/authentication check.
    // Constant definitions: https://www.rfc-editor.org/rfc/rfc9923.html#section-5
    u64 hash = 14695981039346656037ULL;
    for (PhysAddr address = info.initrd_start; address < info.initrd_end; ++address) {
      hash = (hash ^ *reinterpret_cast<volatile u8 *>(address)) * 1099511628211ULL;
    }
    return hash;
  };
  const auto before = Pfa::get_memory_stats();
  const u64 original_hash = initrd_hash();
  usize count = 0;
  usize pages_owned = 0;
  bool valid = true;
  for (usize order_plus_one = MAX_ORDER + 1; order_plus_one != 0 && valid; --order_plus_one) {
    const usize order = order_plus_one - 1;
    for (;;) {
      auto allocation = Pfa::allocate_pages(order);
      if (!allocation) {
        valid = allocation.error() == mm::PageAllocError::OutOfMemory;
        break;
      }
      if (count == 2048) {
        ut::expect(static_cast<bool>(Pfa::free_pages(*allocation, order)));
        valid = false;
        break;
      }
      const PhysAddr begin = *allocation;
      const PhysAddr end = begin + (page_size << order);
      valid = valid && (begin & ((page_size << order) - 1)) == 0 && begin >= moss::abi::linker::kernel_end();
      valid = valid && ram_contains(begin, end) && layout.excludes(begin, end) &&
              (end <= info.initrd_start || begin >= info.initrd_end);
      for (u32 i = 0; i < info.reserved_region_count; ++i) {
        const auto &region = info.reserved_regions[i];
        valid = valid && (end <= region.base || begin >= region.base + region.size);
      }
      for (usize i = 0; i < count; ++i) {
        valid = valid && (end <= owned[i].address || begin >= owned[i].address + (page_size << owned[i].order));
      }
      owned[count++] = {.address = begin, .order = order};
      if (!valid) {
        break; // Never write into a block found to overlap reserved or live memory.
      }
      pages_owned += usize{1} << order;
      // An arbitrary nonzero XOR mask and a complemented end word distinguish
      // each page's endpoints, exposing overlap or writes beyond owned storage.
      for (PhysAddr page = begin; page < end; page += page_size) {
        *reinterpret_cast<volatile u64 *>(page) = page ^ 0x5eed1234ULL;
        *reinterpret_cast<volatile u64 *>(page + page_size - sizeof(u64)) = ~page;
      }
    }
  }
  ut::expect(valid && pages_owned == before.free_pages);
  ut::expect(Pfa::get_memory_stats().free_pages == 0);
  ut::expect(Pfa::get_memory_stats().used_pages == before.used_pages + pages_owned);
  ut::expect(!Pfa::allocate_pages(0));
  ut::expect(initrd_hash() == original_hash);
  while (count) {
    const auto &block = owned[--count];
    if (valid) {
      for (PhysAddr page = block.address; page < block.address + (page_size << block.order); page += page_size) {
        valid = valid && *reinterpret_cast<volatile u64 *>(page) == (page ^ 0x5eed1234ULL) &&
                *reinterpret_cast<volatile u64 *>(page + page_size - sizeof(u64)) == ~page;
      }
    }
    ut::expect(static_cast<bool>(Pfa::free_pages(block.address, block.order)));
  }
  ut::expect(valid);
  ut::expect(Pfa::get_memory_stats().free_pages == before.free_pages);
  ut::expect(Pfa::get_memory_stats().used_pages == before.used_pages);
  auto merged = Pfa::allocate_pages(MAX_ORDER);
  if (ut::expect(static_cast<bool>(merged))) {
    ut::expect(static_cast<bool>(Pfa::free_pages(*merged, MAX_ORDER)));
  }
  ut::expect(Pfa::get_memory_stats().free_pages == before.free_pages);
  layout.verify();
}

} // namespace moss::test::validation
