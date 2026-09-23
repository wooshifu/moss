#pragma once

#include "framework/ut_kernel.hpp"

namespace moss::test::validation {
using namespace moss::kernel;
namespace ut = boost::ut;

void register_vfs_cases();

// Store ownership in the real allocations themselves: exhausting the heap
// must not need another allocation to remember how to release it.
struct HeapPressure {
  void *head = nullptr;

  bool acquire(usize size) {
    if (size < sizeof(void *)) {
      return false;
    }
    auto exhaust = [&](usize request_size) {
      for (;;) {
        auto allocation = mm::RuntimeHeapAllocator::allocate(request_size);
        if (!allocation) {
          return allocation.error() == mm::HeapAllocError::OutOfMemory;
        }
        *static_cast<void **>(*allocation) = head;
        head = *allocation;
      }
    };
    // The current heap arena is 8 MiB: 64 KiB bulk requests bound its first
    // pass to roughly 128 records. Halving then consumes every smaller free
    // fragment before the exact target-size request proves real exhaustion.
    constexpr usize BULK_PRESSURE_BYTES = usize{64} * 1024;
    for (usize request_size = BULK_PRESSURE_BYTES; request_size > size; request_size /= 2) {
      if (!exhaust(request_size)) {
        return false;
      }
    }
    return exhaust(size);
  }
  bool release_one() {
    if (!head) {
      return false;
    }
    void *block = head;
    head = *static_cast<void **>(block);
    return ut::expect(mm::RuntimeHeapAllocator::deallocate(block, 0).has_value());
  }
  void release() {
    while (head) {
      release_one();
    }
  }
  ~HeapPressure() { release(); }
};

void register_driver_cases();
} // namespace moss::test::validation
