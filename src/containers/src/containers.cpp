// containers.cpp - module implementation unit
// Provides definitions for module-level symbols (thread_local, extern)

module moss.containers;

namespace moss::kernel::containers {

// RcuReadLock thread_local definition
thread_local u32 RcuReadLock::read_depth_ = 0;

// Global slab allocator instance
SlabAllocator *g_slab_allocator = nullptr;

} // namespace moss::kernel::containers
