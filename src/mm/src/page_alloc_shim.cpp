// page_alloc_shim.cpp - C-linkage wrappers for PageFrameAllocator
// Module implementation unit of moss.mm
// These bridge the module boundary: external code calls
// PageFrameAllocator via extern "C" functions.

module moss.mm;

extern "C" {

// Returns 0 on failure, otherwise the physical address.
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
