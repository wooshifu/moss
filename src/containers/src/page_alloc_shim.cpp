// page_alloc_shim.cpp - C-linkage wrappers for PageFrameAllocator
// These bridge the module boundary: moss.containers (C++ module) calls
// PageFrameAllocator (non-module TU) via extern "C" functions,
// avoiding name-mangling mismatches between module and external linkage.

#include "mm/page_frame_allocator.hpp"

extern "C" {

// Returns 0 on failure, otherwise the physical address.
// We flatten Result<PhysAddr, PageAllocError> into a simple integer:
// 0 means error (PhysAddr 0 is never a valid allocation).
unsigned long long moss_slab_alloc_pages(unsigned long long order) noexcept {
  auto result =
      moss::kernel::mm::PageFrameAllocator::allocate_pages(
          static_cast<moss::kernel::usize>(order));
  if (!result) {
    return 0;
  }
  return static_cast<unsigned long long>(*result);
}

// Returns 0 on success, non-zero on failure.
int moss_slab_free_pages(unsigned long long addr,
                         unsigned long long order) noexcept {
  auto result = moss::kernel::mm::PageFrameAllocator::free_pages(
      static_cast<moss::kernel::PhysAddr>(addr),
      static_cast<moss::kernel::usize>(order));
  return result.has_value() ? 0 : 1;
}

} // extern "C"
