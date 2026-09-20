// MOSS内核统一内存管理器实现
// 提供UnifiedMemoryManager类的基础实现
// Module implementation unit

module;

module moss.mm;

// Only the validation image observes the publication boundary. Production
// initialization does not wait here or replace any allocation/state change.
extern "C" [[gnu::weak, gnu::noinline]] void moss_validation_mm_before_ready(const void *published_instance
                                                                             [[maybe_unused]]) noexcept {}

namespace moss::kernel::mm {

// BuddyAllocatorV2 is currently only a compatibility allocation facade over
// PageFrameAllocator. V2 metadata-dependent operations must report that their
// backend is absent instead of synthesizing a healthy allocator state.
BuddyResult<moss::kernel::PhysAddr> BuddyAllocatorV2::allocate_pages(const PageAllocRequest &request) noexcept {
  // Delegates to PageFrameAllocator — full buddy with migration types,
  // per-CPU caches, and watermark management is a separate project.
  auto result = PageFrameAllocator::allocate_pages(request.order);
  if (!result) {
    return BuddyResult<moss::kernel::PhysAddr>{BuddyError::OutOfMemory};
  }
  return BuddyResult<moss::kernel::PhysAddr>{*result};
}

BuddyVoidResult BuddyAllocatorV2::initialize() noexcept { return BuddyVoidResult{BuddyError::NotSupported}; }

BuddyVoidResult BuddyAllocatorV2::free_pages(moss::kernel::PhysAddr addr, moss::kernel::usize order) noexcept {
  auto result = PageFrameAllocator::free_pages(addr, order);
  if (!result) {
    return BuddyVoidResult{BuddyError::InvalidAddress};
  }
  return BuddyVoidResult{};
}

BuddyVoidResult BuddyAllocatorV2::compact_memory() noexcept { return BuddyVoidResult{BuddyError::NotSupported}; }

BuddyResult<BuddyAllocatorV2::WaterMark> BuddyAllocatorV2::get_water_mark() noexcept {
  return BuddyResult<WaterMark>{BuddyError::NotSupported};
}

BuddyResult<bool> BuddyAllocatorV2::is_memory_pressure() noexcept {
  return BuddyResult<bool>{BuddyError::NotSupported};
}

BuddyResult<BuddyAllocatorV2::FragmentationStats> BuddyAllocatorV2::get_fragmentation_stats() noexcept {
  return BuddyResult<FragmentationStats>{BuddyError::NotSupported};
}

BuddyResult<BuddyAllocatorV2::MemoryStats> BuddyAllocatorV2::get_memory_stats() noexcept {
  return BuddyResult<MemoryStats>{BuddyError::NotSupported};
}

// 静态成员初始化
u32 UnifiedMemoryManager::initialization_state_ = static_cast<u32>(InitializationState::Uninitialized);
UnifiedMemoryManager *UnifiedMemoryManager::instance_ = nullptr;

// System initialization uses an explicit publication state. The constructing
// CPU publishes instance_ before Ready; acquire readers that observe Ready
// therefore cannot return success while the singleton is still absent.
MMVoidResult UnifiedMemoryManager::initialize_system(const SystemConfig &config) noexcept {
  constexpr u32 uninitialized = static_cast<u32>(InitializationState::Uninitialized);
  constexpr u32 initializing = static_cast<u32>(InitializationState::Initializing);
  constexpr u32 ready = static_cast<u32>(InitializationState::Ready);
  constexpr u32 shutting_down = static_cast<u32>(InitializationState::ShuttingDown);

  u32 observed = uninitialized;
  for (;;) {
    if (__atomic_compare_exchange_n(&initialization_state_, &observed, initializing, true, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
      break;
    }
    if (observed == ready) {
      return MMVoidResult{};
    }
    if (observed == shutting_down) {
      return MMVoidResult{MMError::ResourceBusy};
    }
    // The winning CPU must finish construction and publish Ready before a
    // concurrent initializer may report success.
    while (observed == initializing) {
      moss::kernel::arch::cpu_yield();
      observed = __atomic_load_n(&initialization_state_, __ATOMIC_ACQUIRE);
    }
    if (observed == ready) {
      return MMVoidResult{};
    }
    if (observed == shutting_down) {
      return MMVoidResult{MMError::ResourceBusy};
    }
    observed = uninitialized;
  }

  // operator new panics on OOM in this kernel, so no null check needed.
  auto *raw = new char[sizeof(UnifiedMemoryManager)];
  auto *inst = new (raw) UnifiedMemoryManager(config);
  __atomic_store_n(&instance_, inst, __ATOMIC_RELEASE);
  moss_validation_mm_before_ready(inst);
  __atomic_store_n(&initialization_state_, ready, __ATOMIC_RELEASE);
  return MMVoidResult{};
}

// Shutdown is serialized with initialization. Runtime users must already be
// quiesced; this state machine protects publication, not arbitrary live borrows.
void UnifiedMemoryManager::shutdown_system() noexcept {
  constexpr u32 uninitialized = static_cast<u32>(InitializationState::Uninitialized);
  constexpr u32 initializing = static_cast<u32>(InitializationState::Initializing);
  constexpr u32 ready = static_cast<u32>(InitializationState::Ready);
  constexpr u32 shutting_down = static_cast<u32>(InitializationState::ShuttingDown);

  u32 observed = ready;
  for (;;) {
    if (__atomic_compare_exchange_n(&initialization_state_, &observed, shutting_down, true, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
      break;
    }
    if (observed == uninitialized) {
      return;
    }
    while (observed == initializing || observed == shutting_down) {
      moss::kernel::arch::cpu_yield();
      observed = __atomic_load_n(&initialization_state_, __ATOMIC_ACQUIRE);
    }
    if (observed == uninitialized) {
      return;
    }
    observed = ready;
  }

  auto *inst = __atomic_exchange_n(&instance_, nullptr, __ATOMIC_ACQ_REL);
  if (inst) {
    inst->~UnifiedMemoryManager();
    delete[] reinterpret_cast<char *>(inst);
  }
  __atomic_store_n(&initialization_state_, uninitialized, __ATOMIC_RELEASE);
}

// 检查系统是否已初始化
bool UnifiedMemoryManager::is_system_initialized() noexcept {
  constexpr u32 ready = static_cast<u32>(InitializationState::Ready);
  return __atomic_load_n(&initialization_state_, __ATOMIC_ACQUIRE) == ready;
}

// 主要内存分配接口 — delegates to RuntimeHeapAllocator
MMResult<moss::kernel::VirtAddr> UnifiedMemoryManager::allocate(const MemoryRequest &request) noexcept {
  if (request.size == 0) {
    return MMResult<moss::kernel::VirtAddr>{MMError::OperationFailed};
  }

  auto result = RuntimeHeapAllocator::allocate_aligned(request.size, request.alignment);
  if (!result) {
    return MMResult<moss::kernel::VirtAddr>{MMError::OperationFailed};
  }

  return MMResult<moss::kernel::VirtAddr>{reinterpret_cast<moss::kernel::VirtAddr>(*result)};
}

// 内存释放 — delegates to RuntimeHeapAllocator
MMVoidResult UnifiedMemoryManager::free(moss::kernel::VirtAddr address) noexcept {
  if (address == 0) {
    return MMVoidResult{};
  }
  auto result = RuntimeHeapAllocator::deallocate(reinterpret_cast<void *>(address), 0);
  if (!result) {
    return MMVoidResult{MMError::OperationFailed};
  }
  return MMVoidResult{};
}

MMVoidResult UnifiedMemoryManager::free(moss::kernel::VirtAddr address, moss::kernel::usize size) noexcept {
  if (address == 0) {
    return MMVoidResult{};
  }
  auto result = RuntimeHeapAllocator::deallocate(reinterpret_cast<void *>(address), size);
  if (!result) {
    return MMVoidResult{MMError::OperationFailed};
  }
  return MMVoidResult{};
}

// 内存信息查询
MMResult<MemoryInfo> UnifiedMemoryManager::query_memory_info([[maybe_unused]] moss::kernel::VirtAddr address) noexcept {
  // RuntimeHeapAllocator does not expose allocation metadata. Returning a
  // mostly-zero record would let callers treat an unknown address as valid.
  return MMResult<MemoryInfo>{MMError::NotSupported};
}

MMResult<moss::kernel::usize>
UnifiedMemoryManager::get_allocated_size([[maybe_unused]] moss::kernel::VirtAddr address) noexcept {
  // RuntimeHeapAllocator does not retain a public size lookup. A base-page
  // placeholder can corrupt a later sized free, so reject the query instead.
  return MMResult<moss::kernel::usize>{MMError::NotSupported};
}

// 高级内存操作
MMVoidResult UnifiedMemoryManager::reallocate([[maybe_unused]] moss::kernel::VirtAddr &address,
                                              [[maybe_unused]] moss::kernel::usize old_size,
                                              [[maybe_unused]] moss::kernel::usize new_size,
                                              [[maybe_unused]] AllocFlags flags) noexcept {
  // RuntimeHeapAllocator has no in-place resize or allocation-size lookup, so
  // this operation cannot preserve the old allocation on every failure yet.
  return MMVoidResult{MMError::NotSupported};
}

MMVoidResult UnifiedMemoryManager::prefault_memory([[maybe_unused]] moss::kernel::VirtAddr address,
                                                   [[maybe_unused]] moss::kernel::usize size) noexcept {
  return MMVoidResult{MMError::NotSupported};
}

MMVoidResult UnifiedMemoryManager::advise_usage_pattern([[maybe_unused]] moss::kernel::VirtAddr address,
                                                        [[maybe_unused]] moss::kernel::usize size,
                                                        [[maybe_unused]] UsagePattern pattern) noexcept {
  return MMVoidResult{MMError::NotSupported};
}

// 内存压力管理
MMVoidResult UnifiedMemoryManager::trigger_memory_reclaim() noexcept { return MMVoidResult{MMError::NotSupported}; }

MMVoidResult UnifiedMemoryManager::trigger_memory_compaction() noexcept { return MMVoidResult{MMError::NotSupported}; }

MMResult<MemoryPressure> UnifiedMemoryManager::get_memory_pressure() noexcept {
  // Neither the PFA nor the facade maintains validated pressure thresholds.
  // LOW would therefore be a fabricated health signal rather than a sample.
  return MMResult<MemoryPressure>{MMError::NotSupported};
}

// 性能优化
MMVoidResult UnifiedMemoryManager::optimize_numa_placement([[maybe_unused]] moss::kernel::VirtAddr address,
                                                           [[maybe_unused]] moss::kernel::usize size) noexcept {
  return MMVoidResult{MMError::NotSupported};
}

MMVoidResult UnifiedMemoryManager::promote_to_huge_pages([[maybe_unused]] moss::kernel::VirtAddr address,
                                                         [[maybe_unused]] moss::kernel::usize size) noexcept {
  return MMVoidResult{MMError::NotSupported};
}

MMVoidResult UnifiedMemoryManager::compact_memory_region([[maybe_unused]] moss::kernel::VirtAddr start,
                                                         [[maybe_unused]] moss::kernel::usize size) noexcept {
  return MMVoidResult{MMError::NotSupported};
}

// 系统监控和统计
MMResult<UnifiedMemoryManager::SystemPerformanceStats> UnifiedMemoryManager::get_performance_stats() noexcept {
  // Allocation counts, latency, NUMA locality, huge-page use, and cache hits
  // are not sampled. Returning zeroes or 100% would make absence look measured.
  return MMResult<SystemPerformanceStats>{MMError::NotSupported};
}

// "Healthy" currently means only that singleton publication completed. It is
// not a claim that unsupported pressure or performance monitors are healthy.
bool UnifiedMemoryManager::is_system_healthy() noexcept { return is_system_initialized(); }

MMVoidResult UnifiedMemoryManager::reset_performance_counters() noexcept {
  // No resettable performance sampler exists; PFA usage is live state and must
  // never be zeroed merely to emulate a counter reset.
  return MMVoidResult{MMError::NotSupported};
}

// 调试和诊断
void UnifiedMemoryManager::dump_memory_layout() noexcept {
  namespace log = moss::kernel::logging;
  auto stats = PageFrameAllocator::get_memory_stats();
  // KiB is defined as 1024 bytes; use the architecture's declared page size
  // so this diagnostic remains correct if the supported granule changes.
  constexpr moss::kernel::usize bytes_per_kib = 1024;
  log::klog::info("=== Memory Layout ===");
  log::klog::info("  total:  {} pages ({} KB)", stats.total_pages,
                  stats.total_pages * moss::kernel::PAGE_SIZE / bytes_per_kib);
  log::klog::info("  free:   {} pages ({} KB)", stats.free_pages,
                  stats.free_pages * moss::kernel::PAGE_SIZE / bytes_per_kib);
  log::klog::info("  used:   {} pages ({} KB)", stats.used_pages,
                  stats.used_pages * moss::kernel::PAGE_SIZE / bytes_per_kib);
  log::klog::info("  kernel: {} pages ({} KB)", stats.kernel_pages,
                  stats.kernel_pages * moss::kernel::PAGE_SIZE / bytes_per_kib);
}

MMVoidResult UnifiedMemoryManager::dump_allocation_history() noexcept {
  // No allocation-history ring exists, so an empty dump cannot be presented as
  // a successful diagnostic collection.
  return MMVoidResult{MMError::NotSupported};
}

MMResult<MemoryLeakDetector::LeakReport> UnifiedMemoryManager::generate_leak_report() noexcept {
  // No allocation ownership tracker feeds MemoryLeakDetector. A zero-leak
  // report would mean "not measured", not evidence that no leaks exist.
  return MMResult<MemoryLeakDetector::LeakReport>{MMError::NotSupported};
}

// 构造函数
UnifiedMemoryManager::UnifiedMemoryManager(const SystemConfig &config) noexcept : config_(config) {
  system_initialized_.store(false);
  background_thread_active_.store(false);
  last_background_run_.store(0);
}

} // namespace moss::kernel::mm
