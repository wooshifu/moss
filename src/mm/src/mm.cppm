// MOSS Memory Management Module - Primary interface (re-export hub)
// Consolidates: page_frame_allocator, buddy_allocator_v2, runtime_heap_allocator,
// page_table, numa_policy, vmalloc_allocator, memory_reclaim, memory_compaction,
// huge_pages, memory_stats, mm_interface, kernel_memory

export module moss.mm;

import moss.std;
import moss.types;
import moss.result;
import moss.fdt;
import moss.containers;
import moss.arch;
import moss.platform;
import moss.hal.mmu;
import moss.logging;

export import :core;
export import :page_table;
export import :policy;
export import :reclaim;
export import :interface;

// === Page allocator shim definitions (extern "C") ===
extern "C" {

unsigned long long moss_slab_alloc_pages(unsigned long long order) noexcept {
  auto result = moss::kernel::mm::PageFrameAllocator::allocate_pages(static_cast<moss::kernel::usize>(order));
  if (!result) {
    return 0;
  }
  return static_cast<unsigned long long>(*result);
}

int moss_slab_free_pages(unsigned long long addr, unsigned long long order) noexcept {
  auto result = moss::kernel::mm::PageFrameAllocator::free_pages(static_cast<moss::kernel::PhysAddr>(addr),
                                                                 static_cast<moss::kernel::usize>(order));
  return result.has_value() ? 0 : 1;
}

} // extern "C"
