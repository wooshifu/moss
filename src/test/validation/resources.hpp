#pragma once

#include "validation/runtime.hpp"

namespace moss::test::validation {
struct LifecycleResources {
  u64 heap_bytes = 0, free_pages = 0, processes = 0, threads = 0, descriptors = 0, file_refs = 0;
  u64 user_pages = 0, stack_pages = 0;
  vfs::PoolUsage vfs_pools;

  static LifecycleResources capture();
  bool operator==(const LifecycleResources &) const = default;
};

void *fd_table();
void resources();
void process_heap_rollback();
} // namespace moss::test::validation
