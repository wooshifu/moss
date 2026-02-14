// containers_defs.cpp - Module implementation unit for variable definitions
// thread_local and global variable definitions must be in implementation units,
// not interface units, to avoid duplicate symbols when multiple TUs import the module.

module moss.containers;

namespace moss::kernel::containers {

// RcuReadLock thread_local definition
thread_local moss::u32 RcuReadLock::read_depth_ = 0;

// Global slab allocator instance
SlabAllocator *g_slab_allocator = nullptr;

} // namespace moss::kernel::containers
