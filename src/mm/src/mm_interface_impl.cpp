// MOSS内核统一内存管理器实现
// 提供UnifiedMemoryManager类的基础实现
// Module implementation unit

module;

module moss.mm;

namespace moss::kernel::mm {

// BuddyAllocatorV2 stub implementations
BuddyResult<moss::kernel::PhysAddr> BuddyAllocatorV2::allocate_pages(const PageAllocRequest &request) noexcept {
  // Delegates to PageFrameAllocator — full buddy with migration types,
  // per-CPU caches, and watermark management is a separate project.
  auto result = PageFrameAllocator::allocate_pages(request.order);
  if (!result) {
    return BuddyResult<moss::kernel::PhysAddr>{BuddyError::OutOfMemory};
  }
  return BuddyResult<moss::kernel::PhysAddr>{*result};
}

BuddyVoidResult BuddyAllocatorV2::initialize() noexcept { return BuddyVoidResult{}; }

BuddyVoidResult BuddyAllocatorV2::free_pages(moss::kernel::PhysAddr addr, moss::kernel::usize order) noexcept {
  auto result = PageFrameAllocator::free_pages(addr, order);
  if (!result) {
    return BuddyVoidResult{BuddyError::InvalidAddress};
  }
  return BuddyVoidResult{};
}

BuddyVoidResult BuddyAllocatorV2::compact_memory() noexcept { return BuddyVoidResult{}; }

BuddyAllocatorV2::WaterMark BuddyAllocatorV2::get_water_mark() noexcept { return WaterMark::HIGH; }

bool BuddyAllocatorV2::is_memory_pressure() noexcept { return false; }

BuddyAllocatorV2::FragmentationStats BuddyAllocatorV2::get_fragmentation_stats() noexcept {
  return FragmentationStats{};
}

BuddyAllocatorV2::MemoryStats BuddyAllocatorV2::get_memory_stats() noexcept { return MemoryStats{}; }

// 静态成员初始化
bool UnifiedMemoryManager::initialized_ = false;
UnifiedMemoryManager *UnifiedMemoryManager::instance_ = nullptr;

// 系统初始化 — atomic flag prevents double-init from concurrent CPUs.
// Uses __atomic builtins because containers::AtomicBool is not available
// here (mm is lower-level than containers in the module dependency graph).
MMVoidResult UnifiedMemoryManager::initialize_system(const SystemConfig &config) noexcept {
  // Atomic compare-and-swap: only one caller can transition false→true
  bool expected = false;
  if (!__atomic_compare_exchange_n(&initialized_, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    // Already initialized (or another CPU won the race)
    return MMVoidResult{};
  }

  // We won the race — create instance.
  // operator new panics on OOM in this kernel, so no null check needed.
  auto *raw = new char[sizeof(UnifiedMemoryManager)];
  auto *inst = new (raw) UnifiedMemoryManager(config);
  __atomic_store_n(&instance_, inst, __ATOMIC_RELEASE);
  return MMVoidResult{};
}

// 系统关闭 — properly destroy and free
void UnifiedMemoryManager::shutdown_system() noexcept {
  auto *inst = __atomic_exchange_n(&instance_, nullptr, __ATOMIC_ACQ_REL);
  __atomic_store_n(&initialized_, false, __ATOMIC_RELEASE);
  if (inst) {
    inst->~UnifiedMemoryManager();
    delete[] reinterpret_cast<char *>(inst);
  }
}

// 检查系统是否已初始化
bool UnifiedMemoryManager::is_system_initialized() noexcept { return __atomic_load_n(&initialized_, __ATOMIC_ACQUIRE); }

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
  // 简化实现
  MemoryInfo info{};
  info.virtual_address = address;
  return MMResult<MemoryInfo>{info};
}

MMResult<moss::kernel::usize>
UnifiedMemoryManager::get_allocated_size([[maybe_unused]] moss::kernel::VirtAddr address) noexcept {
  // 占位查询固定返回一个基页，未读取堆块头；不能把此值作为实际分配大小
  // 传给 sized free，后者会校验原请求长度。
  return MMResult<moss::kernel::usize>{moss::kernel::PAGE_SIZE};
}

// 高级内存操作
MMVoidResult UnifiedMemoryManager::reallocate([[maybe_unused]] moss::kernel::VirtAddr &address,
                                              [[maybe_unused]] moss::kernel::usize old_size,
                                              [[maybe_unused]] moss::kernel::usize new_size,
                                              [[maybe_unused]] AllocFlags flags) noexcept {
  // 简化实现
  return MMVoidResult{MMError::OperationFailed};
}

MMVoidResult UnifiedMemoryManager::prefault_memory([[maybe_unused]] moss::kernel::VirtAddr address,
                                                   [[maybe_unused]] moss::kernel::usize size) noexcept {
  return MMVoidResult{};
}

MMVoidResult UnifiedMemoryManager::advise_usage_pattern([[maybe_unused]] moss::kernel::VirtAddr address,
                                                        [[maybe_unused]] moss::kernel::usize size,
                                                        [[maybe_unused]] UsagePattern pattern) noexcept {
  return MMVoidResult{};
}

// 内存压力管理
MMVoidResult UnifiedMemoryManager::trigger_memory_reclaim() noexcept { return MMVoidResult{}; }

MMVoidResult UnifiedMemoryManager::trigger_memory_compaction() noexcept { return MMVoidResult{}; }

MemoryPressure UnifiedMemoryManager::get_memory_pressure() noexcept { return MemoryPressure::LOW; }

// 性能优化
MMVoidResult UnifiedMemoryManager::optimize_numa_placement([[maybe_unused]] moss::kernel::VirtAddr address,
                                                           [[maybe_unused]] moss::kernel::usize size) noexcept {
  return MMVoidResult{};
}

MMVoidResult UnifiedMemoryManager::promote_to_huge_pages([[maybe_unused]] moss::kernel::VirtAddr address,
                                                         [[maybe_unused]] moss::kernel::usize size) noexcept {
  return MMVoidResult{};
}

MMVoidResult UnifiedMemoryManager::compact_memory_region([[maybe_unused]] moss::kernel::VirtAddr start,
                                                         [[maybe_unused]] moss::kernel::usize size) noexcept {
  return MMVoidResult{};
}

// 系统监控和统计
UnifiedMemoryManager::SystemPerformanceStats UnifiedMemoryManager::get_performance_stats() noexcept {
  // 直接构造并返回，避免静态变量导致的C++运行时依赖
  SystemPerformanceStats stats{};

  // 尚未接入实际采样：100.0 是百分比字段的占位“全部成功/本地/命中”，
  // 0 表示未收集的计数和比例；这些值不构成运行时性能或健康证据。
  stats.allocation_perf.total_allocations = 0;
  stats.allocation_perf.failed_allocations = 0;
  stats.allocation_perf.avg_allocation_latency_us = 0;
  stats.allocation_perf.max_allocation_latency_us = 0;
  stats.allocation_perf.total_allocated_bytes = 0;
  stats.allocation_perf.allocation_success_rate = 100.0;

  stats.system_efficiency.memory_utilization = 0.0;
  stats.system_efficiency.fragmentation_ratio = 0.0;
  stats.system_efficiency.numa_locality_ratio = 100.0;
  stats.system_efficiency.huge_page_ratio = 0.0;
  stats.system_efficiency.cache_hit_ratio = 100.0;

  stats.overall_pressure = MemoryPressure::LOW;
  stats.report_timestamp = 0;

  return stats;
}

bool UnifiedMemoryManager::is_system_healthy() noexcept { return __atomic_load_n(&initialized_, __ATOMIC_ACQUIRE); }

void UnifiedMemoryManager::reset_performance_counters() noexcept {
  // Performance counters (allocation_count_, steal_count_, etc.) live in
  // BuddyAllocatorV2 which currently delegates to PageFrameAllocator.
  // The PageFrameAllocator tracks used_pages/free_pages atomically but
  // has no separate "counter reset" — values reflect cumulative state.
  // Nothing to reset until real per-interval counters are added.
}

// 调试和诊断
void UnifiedMemoryManager::dump_memory_layout() noexcept {
  namespace log = moss::kernel::logging;
  auto stats = PageFrameAllocator::get_memory_stats();
  // 固定 4 KiB 基页与 mm::PAGE_SIZE 一致，除以 1024 将字节转为 KiB。
  constexpr moss::kernel::usize PAGE_SIZE = 4096;
  log::klog::info("=== Memory Layout ===");
  log::klog::info("  total:  {} pages ({} KB)", stats.total_pages, stats.total_pages * PAGE_SIZE / 1024);
  log::klog::info("  free:   {} pages ({} KB)", stats.free_pages, stats.free_pages * PAGE_SIZE / 1024);
  log::klog::info("  used:   {} pages ({} KB)", stats.used_pages, stats.used_pages * PAGE_SIZE / 1024);
  log::klog::info("  kernel: {} pages ({} KB)", stats.kernel_pages, stats.kernel_pages * PAGE_SIZE / 1024);
}

void UnifiedMemoryManager::dump_allocation_history() noexcept {
  // Allocation history tracking requires a ring buffer to record each
  // allocate/free call with timestamp, size, and caller.  No such
  // infrastructure exists yet — this is a no-op until then.
}

MMResult<MemoryLeakDetector::LeakReport> UnifiedMemoryManager::generate_leak_report() noexcept {
  MemoryLeakDetector::LeakReport report{};
  report.total_leaked_bytes = 0;
  report.leak_count = 0;
  return MMResult<MemoryLeakDetector::LeakReport>{report};
}

// 单例访问 — caller must check is_system_initialized() first;
// crash immediately on null dereference is preferable to silent corruption.
UnifiedMemoryManager &UnifiedMemoryManager::get_instance() noexcept {
  auto *inst = __atomic_load_n(&instance_, __ATOMIC_ACQUIRE);
  if (!inst) {
    moss::kernel::arch::kernel_panic("UnifiedMemoryManager::get_instance() called before initialize_system()");
  }
  return *inst;
}

// 构造函数
UnifiedMemoryManager::UnifiedMemoryManager(const SystemConfig &config) noexcept : config_(config) {
  system_initialized_.store(false);
  background_thread_active_.store(false);
  last_background_run_.store(0);
}

} // namespace moss::kernel::mm
