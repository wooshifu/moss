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
import moss.ipc;
import moss.drivers;
import moss.result;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;

#include "framework/benchmark.hpp"
#include "framework/ut_kernel.hpp"
#include "hardware_regression.hpp"
#include "ipc_regression.hpp"
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
void address_space_heap_rollback() {
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
  {
    HeapPressure pressure;
    if (!ut::expect(pressure.acquire(sizeof(process::AddressSpace)))) {
      return;
    }
    // More attempts than the ASID capacity: even a one-lease-per-failure leak
    // must fail here, before pressure is removed and normal allocation resumes.
    for (unsigned attempt = 0; attempt < 256; ++attempt) {
      auto space = process::user_space::create_user_address_space();
      if (!ut::expect(!space && space.error() == ErrorCode::OutOfMemory)) {
        break;
      }
      ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == pages);
    }
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  // Reuse the existing public-boundary capacity check to require all 254 free
  // leases, not merely one successful retry after the failures.
  asid_leases();
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == pages);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
}

HeapPressure *address_space_control_pressure = nullptr;
bool address_space_control_exhausted = false;

void address_space_control_rollback() {
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
  HeapPressure pressure;
  // Repeated control-block failures must preserve the exact resource baseline.
  // Eight rounds exercise reuse without repeating the expensive full heap fill
  // for every ASID; the capacity check below independently detects one lost tag.
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    address_space_control_exhausted = false;
    address_space_control_pressure = &pressure;
    auto created = process::user_space::create_user_address_space();
    address_space_control_pressure = nullptr;
    ut::expect(address_space_control_exhausted && !created && created.error() == ErrorCode::OutOfMemory);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
    ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == pages);
  }
  asid_leases();
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == pages);
}

void vma_heap_rollback() {
  using namespace process;
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  const auto pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
  {
    auto created = user_space::create_user_address_space();
    if (!ut::expect(created.has_value())) {
      return;
    }
    auto &space = **created;
    constexpr auto base = user_layout::MMAP_BASE;
    if (!ut::expect(space.add_vma(base, base + page_size, vma_flags::READ, VmaType::MMAP))) {
      return;
    }
    const auto populated = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
    {
      HeapPressure pressure;
      if (!ut::expect(pressure.acquire(sizeof(VmaRegion)))) {
        return;
      }
      ut::expect(!space.add_vma(base + page_size, base + 2 * page_size, vma_flags::WRITE, VmaType::MMAP));
      ut::expect(!space.find_vma(base + page_size) && space.vmas.size() == 1);
      ut::expect(space.allows_user_access(base, page_size, vma_flags::READ) &&
                 !space.allows_user_access(base, page_size, vma_flags::WRITE));
    }
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated);
    ut::expect(space.add_vma(base + page_size, base + 2 * page_size, vma_flags::WRITE, VmaType::MMAP));
    ut::expect(space.allows_user_access(base + page_size, page_size, vma_flags::WRITE));
    ut::expect(space.remove_vma(base + page_size, base + 2 * page_size) && space.vmas.size() == 1);
    ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == populated);
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == pages);
}

void heap_alignment() {
  // Power-of-two alignments cover byte through page granularity. A 73-byte
  // request is deliberately unaligned; offsets 0/72 touch its exact endpoints,
  // and distinct arbitrary sentinels reveal aliasing or truncated usable storage.
  const usize alignments[] = {1, 2, 4, 8, 16, 32, 64, 256, 4096};
  const auto before = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  for (usize alignment : alignments) {
    auto allocation = mm::RuntimeHeapAllocator::allocate_aligned(73, alignment);
    if (!ut::expect(static_cast<bool>(allocation))) {
      return;
    }
    auto address = reinterpret_cast<usize>(*allocation);
    const bool aligned = ut::expect(address % alignment == 0);
    auto *bytes = static_cast<volatile u8 *>(*allocation);
    bytes[0] = 0x35;
    bytes[72] = 0x79;
    ut::expect(bytes[0] == 0x35 && bytes[72] == 0x79);
    ut::expect(static_cast<bool>(mm::RuntimeHeapAllocator::deallocate(*allocation, 73)));
    if (!aligned) {
      return;
    }
  }
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == before);
}
void heap_invalid_requests() {
  using Heap = mm::RuntimeHeapAllocator;
  const auto before = Heap::get_heap_stats();
  const usize maximum = ~usize{0};
  ut::expect(!Heap::allocate(0));
  ut::expect(!Heap::allocate_aligned(16, 0));
  ut::expect(!Heap::allocate_aligned(16, 3));
  ut::expect(!Heap::allocate(maximum));
  ut::expect(!Heap::allocate(maximum - 31));
  ut::expect(!Heap::allocate_aligned(16, maximum));
  ut::expect(!Heap::allocate_aligned(16, usize{1} << 63));
  ut::expect(!Heap::expand_heap(0));
  ut::expect(!Heap::expand_heap(maximum));
  ut::expect(!Heap::expand_heap(maximum - page_size));
  const auto after = Heap::get_heap_stats();
  ut::expect(after.total_heap_size == before.total_heap_size);
  ut::expect(after.allocated_bytes == before.allocated_bytes);
  ut::expect(after.largest_free_block == before.largest_free_block);
}
void heap_release_contract() {
  // Allocate 73 bytes and reject a sized free of 72; endpoint sentinels must
  // survive that failure. The values distinguish mismatch from a valid release.
  using Heap = mm::RuntimeHeapAllocator;
  const auto before = Heap::get_heap_stats().allocated_bytes;
  auto allocation = Heap::allocate_aligned(73, page_size);
  if (!ut::expect(static_cast<bool>(allocation))) {
    return;
  }
  auto *bytes = static_cast<volatile u8 *>(*allocation);
  bytes[0] = 0x39;
  bytes[72] = 0x81;
  const auto live = Heap::get_heap_stats().allocated_bytes;
  ut::expect(static_cast<bool>(Heap::deallocate(nullptr, 0)));
  ut::expect(!Heap::deallocate(reinterpret_cast<void *>(Heap::get_heap_start()), 0));
  ut::expect(!Heap::deallocate(reinterpret_cast<void *>(Heap::get_heap_end()), 0));
  ut::expect(!Heap::deallocate(static_cast<u8 *>(*allocation) + 1, 0));
  ut::expect(!Heap::deallocate(static_cast<u8 *>(*allocation) + 16, 0));
  if (!ut::expect(!Heap::deallocate(*allocation, 72))) {
    return; // A broken sized free may already have released the allocation.
  }
  ut::expect(Heap::get_heap_stats().allocated_bytes == live);
  ut::expect(bytes[0] == 0x39 && bytes[72] == 0x81);
  ut::expect(static_cast<bool>(Heap::deallocate(*allocation, 73)));
  ut::expect(!Heap::deallocate(*allocation, 0));
  ut::expect(Heap::get_heap_stats().allocated_bytes == before);
}
void heap_reuse() {
  using Heap = mm::RuntimeHeapAllocator;
  struct Slot {
    void *pointer = nullptr;
    usize size = 0;
    u8 pattern = 0;
  };
  Slot slots[32]{};
  // Fixed seed and a modulo-2^32 linear-congruential recurrence make this
  // workload repeatable; these multiplier/increment values define the fixture,
  // not a source of cryptographic randomness or measured allocator tuning.
  const auto before = Heap::get_heap_stats().allocated_bytes;
  u32 seed = 0x5eed;
  bool valid = true;
  for (usize step = 0; step < 4096 && valid; ++step) {
    // Mix 32 live slots over 4096 bounded steps, with 1..1024-byte requests and
    // power-of-two alignments 2^3..2^12 (8 bytes..4 KiB). Bit slices vary choices
    // independently of the recurrence's weakest low bits.
    seed = seed * 1664525U + 1013904223U;
    auto &slot = slots[(seed >> 16) % 32];
    if (slot.pointer) {
      auto *bytes = static_cast<volatile u8 *>(slot.pointer);
      for (usize i = 0; i < slot.size; ++i) {
        valid = valid && bytes[i] == slot.pattern;
      }
      valid = valid && static_cast<bool>(Heap::deallocate(slot.pointer, slot.size));
      slot.pointer = nullptr;
    } else {
      slot.size = 1 + (seed >> 8) % 1024;
      const usize alignment = usize{1} << (3 + (seed >> 24) % 10);
      auto allocation = Heap::allocate_aligned(slot.size, alignment);
      if (!ut::expect(static_cast<bool>(allocation))) {
        valid = false;
        break;
      }
      slot.pointer = *allocation;
      slot.pattern = static_cast<u8>(step);
      const auto address = reinterpret_cast<usize>(slot.pointer);
      valid = valid && address % alignment == 0;
      for (const auto &other : slots) {
        if (other.pointer && &other != &slot) {
          const auto other_address = reinterpret_cast<usize>(other.pointer);
          valid = valid && (address + slot.size <= other_address || other_address + other.size <= address);
        }
      }
      auto *bytes = static_cast<volatile u8 *>(slot.pointer);
      for (usize i = 0; i < slot.size; ++i) {
        bytes[i] = slot.pattern;
      }
    }
  }
  for (const auto &slot : slots) {
    if (slot.pointer) {
      auto *bytes = static_cast<volatile u8 *>(slot.pointer);
      for (usize i = 0; i < slot.size; ++i) {
        valid = valid && bytes[i] == slot.pattern;
      }
      ut::expect(static_cast<bool>(Heap::deallocate(slot.pointer, slot.size)));
    }
  }
  ut::expect(valid);
  ut::expect(Heap::get_heap_stats().allocated_bytes == before);
}
void heap_exhaustion() {
  // 128 pointers track up to 8 MiB in 64 KiB chunks, then the fixture tests
  // smaller remainder allocations. These bound bookkeeping and stack usage.
  using Heap = mm::RuntimeHeapAllocator;
  constexpr usize chunk_size = static_cast<const usize>(64 * 1024);
  void *owned[128]{};
  auto sentinel = mm::PageFrameAllocator::allocate_pages(0);
  if (!ut::expect(static_cast<bool>(sentinel))) {
    return;
  }
  auto *page = reinterpret_cast<volatile u64 *>(*sentinel);
  // Arbitrary nonzero poison, varied by word index, detects allocator writes
  // escaping the heap into a separately owned physical page.
  for (usize i = 0; i < page_size / sizeof(u64); ++i) {
    page[i] = 0x123456789abcdef0ULL ^ i;
  }
  LayoutSnapshot layout;
  if (!layout.capture()) {
    ut::expect(static_cast<bool>(mm::PageFrameAllocator::free_pages(*sentinel, 0)));
    return;
  }
  const auto before = Heap::get_heap_stats().allocated_bytes;
  const auto free_pages = mm::PageFrameAllocator::get_memory_stats().free_pages;
  // Hold these together to write beyond the old 4/64/256-KiB boundaries.
  constexpr usize probes[] = {static_cast<const usize>(4 * 1024), static_cast<const usize>(64 * 1024),
                              static_cast<const usize>(256 * 1024)};
  usize probe_count = 0;
  for (usize size : probes) {
    auto allocation = Heap::allocate(size);
    if (!ut::expect(static_cast<bool>(allocation))) {
      break;
    }
    owned[probe_count++] = *allocation;
    auto *bytes = static_cast<volatile u8 *>(*allocation);
    for (usize i = 0; i < size; ++i) {
      bytes[i] = 0xa5;
    }
    layout.verify();
  }
  while (probe_count) {
    --probe_count;
    ut::expect(static_cast<bool>(Heap::deallocate(owned[probe_count], probes[probe_count])));
  }
  usize count = 0;
  bool exhausted = false;
  bool valid = true;
  while (count < 128) {
    auto allocation = Heap::allocate(chunk_size);
    if (!allocation) {
      exhausted = allocation.error() == mm::HeapAllocError::OutOfMemory;
      break;
    }
    owned[count++] = *allocation;
    const auto address = reinterpret_cast<usize>(*allocation);
    valid =
        valid && address >= moss::abi::linker::heap_start() && address + chunk_size <= moss::abi::linker::heap_end();
    auto *bytes = static_cast<volatile u8 *>(*allocation);
    for (usize i = 0; i < chunk_size; ++i) {
      bytes[i] = static_cast<u8>(count);
    }
  }
  ut::expect(exhausted && count > 0);
  ut::expect(Heap::get_heap_end() == moss::abi::linker::heap_end());
  ut::expect(!Heap::expand_heap(page_size));
  layout.verify();
  while (count) {
    auto *bytes = static_cast<volatile u8 *>(owned[count - 1]);
    for (usize i = 0; i < chunk_size; ++i) {
      valid = valid && bytes[i] == static_cast<u8>(count);
    }
    ut::expect(static_cast<bool>(Heap::deallocate(owned[--count], chunk_size)));
  }
  for (usize i = 0; i < page_size / sizeof(u64); ++i) {
    valid = valid && page[i] == (0x123456789abcdef0ULL ^ i);
  }
  ut::expect(valid);
  ut::expect(Heap::get_heap_stats().allocated_bytes == before);
  ut::expect(mm::PageFrameAllocator::get_memory_stats().free_pages == free_pages);
  layout.verify();
  ut::expect(static_cast<bool>(mm::PageFrameAllocator::free_pages(*sentinel, 0)));
  // A large allocation after release requires the split blocks to coalesce.
  auto reused = Heap::allocate(static_cast<usize>(1024 * 1024));
  if (ut::expect(static_cast<bool>(reused))) {
    ut::expect(static_cast<bool>(Heap::deallocate(*reused, 0)));
  }
  ut::expect(Heap::get_heap_stats().allocated_bytes == before);
}
} // namespace moss::test::validation
