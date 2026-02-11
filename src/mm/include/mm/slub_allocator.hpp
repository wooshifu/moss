#pragma once

// SLUB内存分配器实现 - 基于Per-CPU快速路径的高性能分配器
// 结合Linux SLUB设计和多核优化，提供卓越的分配性能

#include "../../../include/result.hpp"
#include "../../../include/types.hpp"
#include "../../../include/moss_std.hpp"
#include "../../../include/arch/arch_abstraction.hpp"
#include "containers/atomic_types.hpp"
#include "containers/per_cpu_data.hpp"
#include "mm/page_frame_allocator.hpp"

// 内核环境的工具函数
template <typename T>
constexpr const T &kernel_max(const T &a, const T &b) noexcept {
  return (a < b) ? b : a;
}

namespace moss::kernel::mm {

// SLUB分配器错误代码
enum class SlubError : u32 {
  OutOfMemory = 1,
  InvalidSize = 2,
  DoubleFree = 3,
  CorruptedSlab = 4,
  CpuIndexOutOfRange = 5
};

// SLUB分配结果类型
template <typename T> using SlubResult = moss::kernel::Result<T, SlubError>;
using SlubVoidResult = moss::kernel::Result<void, SlubError>;

// 内存对齐工具
template <moss::kernel::usize Alignment>
constexpr moss::kernel::usize align_up(moss::kernel::usize value) noexcept {
  static_assert((Alignment & (Alignment - 1)) == 0, "Alignment must be power of 2");
  return (value + Alignment - 1) & ~(Alignment - 1);
}

// SLUB页面结构 - 优化的页面布局
struct SlubPage {
  void *memory;                         // 页面内存起始地址
  moss::kernel::usize object_size;      // 对象大小
  moss::kernel::usize objects_per_page; // 每页对象数量

  // 使用增强的原子操作
  moss::kernel::containers::AtomicCounter<moss::kernel::usize> free_count;
  moss::kernel::containers::AtomicPtr<u8> free_list;
  moss::kernel::containers::AtomicPtr<SlubPage> next;

  // 性能统计
  moss::kernel::containers::AtomicCounter<moss::kernel::u64> alloc_count;
  moss::kernel::containers::AtomicCounter<moss::kernel::u64> free_count_total;

  SlubPage(void *mem, moss::kernel::usize obj_size, moss::kernel::usize obj_per_page) noexcept
      : memory(mem), object_size(obj_size), objects_per_page(obj_per_page),
        free_count(obj_per_page), free_list(nullptr), next(nullptr),
        alloc_count(0), free_count_total(0) {
    initialize_free_list();
  }

private:
  void initialize_free_list() noexcept {
    char *current = static_cast<char *>(memory);
    void *last_free = nullptr;

    for (moss::kernel::usize i = 0; i < objects_per_page; ++i) {
      void **obj_ptr = reinterpret_cast<void **>(current);
      *obj_ptr = last_free;
      last_free = current;
      current += object_size;
    }

    free_list.store(static_cast<u8 *>(last_free), moss::MemoryOrder::Release);
  }
};

// Per-CPU SLUB缓存 - 核心性能优化
struct PerCpuSlubCache {
  // CPU本地页面 - 快速路径
  moss::kernel::containers::AtomicPtr<SlubPage> active_page;
  moss::kernel::containers::AtomicPtr<u8> free_ptr;  // 直接指向下一个可用对象
  moss::kernel::containers::AtomicCounter<moss::kernel::usize> free_objects;

  // CPU本地统计
  moss::kernel::containers::CacheAlignedAtomic<moss::kernel::u64> fast_allocs;
  moss::kernel::containers::CacheAlignedAtomic<moss::kernel::u64> fast_frees;
  moss::kernel::containers::CacheAlignedAtomic<moss::kernel::u64> slow_path_fallbacks;

  // 批量释放缓冲区 - 提升释放性能
  static constexpr moss::kernel::usize BATCH_FREE_SIZE = 16;
  void *free_batch[BATCH_FREE_SIZE];
  moss::kernel::containers::AtomicCounter<moss::kernel::usize> batch_count;

  PerCpuSlubCache() noexcept
      : active_page(nullptr), free_ptr(nullptr), free_objects(0),
        fast_allocs(0), fast_frees(0), slow_path_fallbacks(0),
        free_batch{}, batch_count(0) {}

  // Per-CPU快速分配 - 关键性能路径
  [[nodiscard]] void* fast_alloc() noexcept {
    // 使用内存预取优化缓存性能
    u8* current_ptr = free_ptr.load(moss::MemoryOrder::Relaxed);
    if (current_ptr != nullptr) {
      // 预取下一个对象，提升缓存命中率
      __builtin_prefetch(current_ptr, 0, 3); // 预取到L1缓存

      // 获取下一个空闲对象指针
      u8* next_ptr = *reinterpret_cast<u8**>(current_ptr);

      // 使用CAS原子更新 - 充分利用增强的原子操作
      if (free_ptr.compare_exchange_weak(current_ptr, next_ptr,
                                        moss::MemoryOrder::AcqRel,
                                        moss::MemoryOrder::Relaxed)) {
        free_objects.fetch_sub(1, moss::MemoryOrder::Relaxed);
        (void)fast_allocs.value.fetch_add(1, moss::MemoryOrder::Relaxed);
        return current_ptr;
      }
    }

    // 快速路径失败，记录统计
    (void)slow_path_fallbacks.value.fetch_add(1, moss::MemoryOrder::Relaxed);
    return nullptr;
  }

  // Per-CPU快速释放 - 批量处理优化
  [[nodiscard]] bool fast_free(void* ptr, [[maybe_unused]] moss::kernel::usize object_size) noexcept {
    // 检查是否属于当前CPU的活动页面
    SlubPage* current_page = active_page.load(moss::MemoryOrder::Acquire);
    if (current_page != nullptr && belongs_to_page(ptr, current_page)) {
      // 添加到批量释放缓冲区
      moss::kernel::usize current_batch = batch_count.fetch_add(1, moss::MemoryOrder::AcqRel);
      if (current_batch < BATCH_FREE_SIZE) {
        free_batch[current_batch] = ptr;
        (void)fast_frees.value.fetch_add(1, moss::MemoryOrder::Relaxed);

        // 批量释放缓冲区满时，执行批量释放
        if (current_batch + 1 == BATCH_FREE_SIZE) {
          flush_batch_free_impl(object_size);
        }
        return true;
      } else {
        // 缓冲区溢出，直接释放
        batch_count.fetch_sub(1, moss::MemoryOrder::AcqRel);
      }
    }

    return false;
  }

  // 公开的批量释放接口
  void flush_batch_free([[maybe_unused]] moss::kernel::usize object_size) noexcept {
    flush_batch_free_impl(object_size);
  }

private:
  // 检查指针是否属于指定页面
  [[nodiscard]] static bool belongs_to_page(void* ptr, SlubPage* page) noexcept {
    moss::kernel::usize ptr_addr = reinterpret_cast<moss::kernel::usize>(ptr);
    moss::kernel::usize page_start = reinterpret_cast<moss::kernel::usize>(page->memory);
    moss::kernel::usize page_end = page_start + moss::kernel::PAGE_SIZE;
    return ptr_addr >= page_start && ptr_addr < page_end;
  }

  // 批量释放处理
  void flush_batch_free_impl(moss::kernel::usize object_size) noexcept {
    // 将批量缓冲区中的对象链接到空闲链表
    if (batch_count.load(moss::MemoryOrder::Acquire) == 0) return;

    // 构建本地链表
    void* batch_head = nullptr;
    moss::kernel::usize count = 0;

    for (moss::kernel::usize i = 0; i < BATCH_FREE_SIZE && i < batch_count.load(moss::MemoryOrder::Relaxed); ++i) {
      void** obj_ptr = static_cast<void**>(free_batch[i]);
      *obj_ptr = batch_head;
      batch_head = free_batch[i];
      count++;
    }

    // 原子地将批量链表添加到页面空闲链表
    if (batch_head != nullptr) {
      u8* current_free = free_ptr.load(moss::MemoryOrder::Acquire);
      void** batch_tail = static_cast<void**>(batch_head);

      do {
        // 找到批量链表的尾部，连接到当前空闲链表
        void** tail_ptr = batch_tail;
        for (moss::kernel::usize i = 1; i < count; ++i) {
          tail_ptr = static_cast<void**>(*tail_ptr);
        }
        *tail_ptr = current_free;
      } while (!free_ptr.compare_exchange_weak(current_free, static_cast<u8*>(batch_head),
                                              moss::MemoryOrder::AcqRel,
                                              moss::MemoryOrder::Acquire));

      free_objects.fetch_add(count, moss::MemoryOrder::Relaxed);
    }

    batch_count.store(0, moss::MemoryOrder::Release);
  }
};

// 高性能SLUB缓存
class SlubCache {
private:
  const moss::kernel::usize object_size_;
  const moss::kernel::usize object_alignment_;
  const moss::kernel::usize aligned_object_size_;
  const moss::kernel::usize objects_per_page_;

  // Per-CPU缓存数组 - 核心优化
  moss::kernel::containers::PerCpuData<PerCpuSlubCache> per_cpu_caches_;

  // 全局页面链表（慢速路径）
  moss::kernel::containers::AtomicPtr<SlubPage> partial_pages_;
  moss::kernel::containers::AtomicPtr<SlubPage> empty_pages_;

  // 全局统计信息
  moss::kernel::containers::AtomicCounter<moss::kernel::usize> total_objects_;
  moss::kernel::containers::AtomicCounter<moss::kernel::usize> allocated_objects_;
  moss::kernel::containers::AtomicCounter<moss::kernel::u64> global_alloc_count_;
  moss::kernel::containers::AtomicCounter<moss::kernel::u64> global_free_count_;

public:
  explicit SlubCache(moss::kernel::usize object_size,
                    moss::kernel::usize alignment = alignof(void*)) noexcept
      : object_size_(object_size), object_alignment_(alignment),
        aligned_object_size_(align_up<alignof(void*)>(
            kernel_max(object_size, sizeof(void*)))),
        objects_per_page_(moss::kernel::PAGE_SIZE / aligned_object_size_),
        per_cpu_caches_{}, partial_pages_(nullptr), empty_pages_(nullptr),
        total_objects_(0), allocated_objects_(0),
        global_alloc_count_(0), global_free_count_(0) {}

  ~SlubCache() noexcept {
    // 清理所有CPU缓存
    for (moss::kernel::u32 cpu = 0; cpu < moss::kernel::MAX_CPUS; ++cpu) {
      auto& cpu_cache = per_cpu_caches_.get_cpu(cpu);
      cpu_cache.flush_batch_free(aligned_object_size_);
    }

    // 释放全局页面
    free_page_list(partial_pages_.load(moss::MemoryOrder::Relaxed));
    free_page_list(empty_pages_.load(moss::MemoryOrder::Relaxed));
  }

  NON_COPYABLE_NON_MOVABLE(SlubCache)

  // 主分配接口 - 优化的双路径设计
  [[nodiscard]] SlubResult<void*> allocate() noexcept {
    // 快速路径：尝试从当前CPU缓存分配
    moss::kernel::u32 cpu_id = moss::kernel::arch::get_current_cpu_id();
    if (cpu_id < moss::kernel::MAX_CPUS) {
      auto& cpu_cache = per_cpu_caches_.get_cpu(cpu_id);

      if (void* ptr = cpu_cache.fast_alloc()) {
        allocated_objects_.fetch_add(1, moss::MemoryOrder::Relaxed);
        global_alloc_count_.fetch_add(1, moss::MemoryOrder::Relaxed);
        return SlubResult<void*>{ptr};
      }
    }

    // 慢速路径：从全局页面分配或分配新页面
    return slow_path_allocate(cpu_id);
  }

  // 主释放接口
  [[nodiscard]] SlubVoidResult deallocate(void* ptr) noexcept {
    if (ptr == nullptr) {
      return SlubVoidResult{};
    }

    // 快速路径：尝试释放到当前CPU缓存
    moss::kernel::u32 cpu_id = moss::kernel::arch::get_current_cpu_id();
    if (cpu_id < moss::kernel::MAX_CPUS) {
      auto& cpu_cache = per_cpu_caches_.get_cpu(cpu_id);

      if (cpu_cache.fast_free(ptr, aligned_object_size_)) {
        allocated_objects_.fetch_sub(1, moss::MemoryOrder::Relaxed);
        global_free_count_.fetch_add(1, moss::MemoryOrder::Relaxed);
        return SlubVoidResult{};
      }
    }

    // 慢速路径：释放到全局页面
    return slow_path_deallocate(ptr);
  }

  // 性能统计接口
  [[nodiscard]] moss::kernel::usize total_objects() const noexcept {
    return total_objects_.load(moss::MemoryOrder::Relaxed);
  }

  [[nodiscard]] moss::kernel::usize allocated_objects() const noexcept {
    return allocated_objects_.load(moss::MemoryOrder::Relaxed);
  }

  [[nodiscard]] moss::kernel::u64 total_allocations() const noexcept {
    return global_alloc_count_.load(moss::MemoryOrder::Relaxed);
  }

  [[nodiscard]] moss::kernel::u64 total_frees() const noexcept {
    return global_free_count_.load(moss::MemoryOrder::Relaxed);
  }

  // Per-CPU统计聚合
  [[nodiscard]] moss::kernel::u64 fast_path_allocations() const noexcept {
    moss::kernel::u64 total = 0;
    for (moss::kernel::u32 cpu = 0; cpu < moss::kernel::MAX_CPUS; ++cpu) {
      const auto& cpu_cache = per_cpu_caches_.get_cpu(cpu);
      total += cpu_cache.fast_allocs.load(moss::MemoryOrder::Relaxed);
    }
    return total;
  }

  [[nodiscard]] moss::kernel::u64 slow_path_fallbacks() const noexcept {
    moss::kernel::u64 total = 0;
    for (moss::kernel::u32 cpu = 0; cpu < moss::kernel::MAX_CPUS; ++cpu) {
      const auto& cpu_cache = per_cpu_caches_.get_cpu(cpu);
      total += cpu_cache.slow_path_fallbacks.load(moss::MemoryOrder::Relaxed);
    }
    return total;
  }

  [[nodiscard]] double fast_path_ratio() const noexcept {
    moss::kernel::u64 fast = fast_path_allocations();
    moss::kernel::u64 total = total_allocations();
    return total > 0 ? static_cast<double>(fast) / static_cast<double>(total) : 0.0;
  }

private:
  // 慢速路径分配实现
  [[nodiscard]] SlubResult<void*> slow_path_allocate(moss::kernel::u32 cpu_id) noexcept {
    // 尝试从部分页面分配
    SlubPage* page = partial_pages_.load(moss::MemoryOrder::Acquire);
    if (page != nullptr) {
      if (auto result = allocate_from_page(page)) {
        // 为CPU缓存设置新的活动页面
        if (cpu_id < moss::kernel::MAX_CPUS) {
          auto& cpu_cache = per_cpu_caches_.get_cpu(cpu_id);
          cpu_cache.active_page.store(page, moss::MemoryOrder::Release);

          // 预填充CPU缓存
          prefill_cpu_cache(cpu_cache, page);
        }

        return result;
      }
    }

    // 分配新页面
    return allocate_new_page(cpu_id);
  }

  // 慢速路径释放实现
  [[nodiscard]] SlubVoidResult slow_path_deallocate(void* ptr) noexcept {
    // 查找对象所属页面并释放
    SlubPage* page = find_page_for_object(ptr);
    if (page == nullptr) {
      return SlubVoidResult{SlubError::CorruptedSlab};
    }

    return free_to_page(page, ptr);
  }

  // 预填充CPU缓存
  void prefill_cpu_cache(PerCpuSlubCache& cpu_cache, SlubPage* page) noexcept {
    // 从页面预取若干对象到CPU缓存，提升后续分配性能
    constexpr moss::kernel::usize PREFILL_COUNT = 8;

    u8* free_ptr = page->free_list.load(moss::MemoryOrder::Acquire);
    moss::kernel::usize prefilled = 0;

    while (free_ptr != nullptr && prefilled < PREFILL_COUNT) {
      u8* next_ptr = *reinterpret_cast<u8**>(free_ptr);

      if (page->free_list.compare_exchange_weak(free_ptr, next_ptr,
                                               moss::MemoryOrder::AcqRel,
                                               moss::MemoryOrder::Acquire)) {
        // 成功获取对象，设置为CPU缓存的free_ptr
        if (prefilled == 0) {
          cpu_cache.free_ptr.store(free_ptr, moss::MemoryOrder::Release);
          cpu_cache.free_objects.store(1, moss::MemoryOrder::Release);
        }
        prefilled++;
      }
    }
  }

  // 页面分配和管理辅助函数
  [[nodiscard]] SlubResult<void*> allocate_from_page(SlubPage* page) noexcept {
    u8* current_free = page->free_list.load(moss::MemoryOrder::Acquire);
    if (current_free == nullptr) {
      return SlubResult<void*>{SlubError::OutOfMemory};
    }

    u8* next_free;
    do {
      if (current_free == nullptr) {
        return SlubResult<void*>{SlubError::OutOfMemory};
      }
      next_free = *reinterpret_cast<u8**>(current_free);
    } while (!page->free_list.compare_exchange_weak(current_free, next_free,
                                                   moss::MemoryOrder::AcqRel,
                                                   moss::MemoryOrder::Acquire));

    page->free_count.fetch_sub(1, moss::MemoryOrder::AcqRel);
    page->alloc_count.fetch_add(1, moss::MemoryOrder::Relaxed);
    allocated_objects_.fetch_add(1, moss::MemoryOrder::Relaxed);
    global_alloc_count_.fetch_add(1, moss::MemoryOrder::Relaxed);

    return SlubResult<void*>{current_free};
  }

  [[nodiscard]] SlubResult<void*> allocate_new_page(moss::kernel::u32 cpu_id) noexcept {
    void* page_memory = allocate_page();
    if (page_memory == nullptr) {
      return SlubResult<void*>{SlubError::OutOfMemory};
    }

    SlubPage* new_page = new SlubPage(page_memory, aligned_object_size_, objects_per_page_);
    total_objects_.fetch_add(objects_per_page_, moss::MemoryOrder::Relaxed);

    // 添加到部分页面链表
    add_to_partial_list(new_page);

    // 为CPU缓存设置活动页面
    if (cpu_id < moss::kernel::MAX_CPUS) {
      auto& cpu_cache = per_cpu_caches_.get_cpu(cpu_id);
      cpu_cache.active_page.store(new_page, moss::MemoryOrder::Release);
      prefill_cpu_cache(cpu_cache, new_page);
    }

    return allocate_from_page(new_page);
  }

  [[nodiscard]] SlubVoidResult free_to_page(SlubPage* page, void* ptr) noexcept {
    void** obj_ptr = static_cast<void**>(ptr);
    u8* current_free = page->free_list.load(moss::MemoryOrder::Relaxed);

    do {
      *obj_ptr = current_free;
    } while (!page->free_list.compare_exchange_weak(current_free, static_cast<u8*>(ptr),
                                                   moss::MemoryOrder::Release,
                                                   moss::MemoryOrder::Relaxed));

    page->free_count.fetch_add(1, moss::MemoryOrder::AcqRel);
    page->free_count_total.fetch_add(1, moss::MemoryOrder::Relaxed);
    allocated_objects_.fetch_sub(1, moss::MemoryOrder::Relaxed);
    global_free_count_.fetch_add(1, moss::MemoryOrder::Relaxed);

    return SlubVoidResult{};
  }

  // 页面查找和链表管理
  [[nodiscard]] SlubPage* find_page_for_object(void* ptr) const noexcept {
    moss::kernel::usize ptr_addr = reinterpret_cast<moss::kernel::usize>(ptr);
    moss::kernel::usize page_addr = ptr_addr & ~(moss::kernel::PAGE_SIZE - 1);

    // 在部分页面链表中搜索
    SlubPage* current = partial_pages_.load(moss::MemoryOrder::Acquire);
    while (current != nullptr) {
      moss::kernel::usize current_page_addr =
          reinterpret_cast<moss::kernel::usize>(current->memory) & ~(moss::kernel::PAGE_SIZE - 1);
      if (current_page_addr == page_addr) {
        return current;
      }
      current = current->next.load(moss::MemoryOrder::Acquire);
    }

    return nullptr;
  }

  void add_to_partial_list(SlubPage* page) noexcept {
    SlubPage* old_head = partial_pages_.load(moss::MemoryOrder::Relaxed);
    do {
      page->next.store(old_head, moss::MemoryOrder::Relaxed);
    } while (!partial_pages_.compare_exchange_weak(old_head, page,
                                                  moss::MemoryOrder::Release,
                                                  moss::MemoryOrder::Relaxed));
  }

  [[nodiscard]] void* allocate_page() noexcept {
    auto result = moss::kernel::mm::PageFrameAllocator::allocate_pages(0);
    return result ? reinterpret_cast<void*>(result.value()) : nullptr;
  }

  void free_page_list(SlubPage* head) noexcept {
    while (head != nullptr) {
      SlubPage* next = head->next.load(moss::MemoryOrder::Relaxed);
      moss::kernel::PhysAddr phys_addr = reinterpret_cast<moss::kernel::PhysAddr>(head->memory);
      [[maybe_unused]] auto result = moss::kernel::mm::PageFrameAllocator::free_pages(phys_addr, 0);
      delete head;
      head = next;
    }
  }
};

// 多大小SLUB分配器 - 完整的内存管理接口
class SlubAllocator {
private:
  static constexpr moss::kernel::usize NUM_CACHES = 32;
  static constexpr moss::kernel::usize MIN_OBJECT_SIZE = 8;
  static constexpr moss::kernel::usize MAX_OBJECT_SIZE = 4096;

  // 预定义的对象大小缓存
  SlubCache* caches_[NUM_CACHES];
  moss::kernel::usize cache_sizes_[NUM_CACHES];

  // 全局统计信息
  moss::kernel::containers::AtomicCounter<moss::kernel::u64> total_allocations_;
  moss::kernel::containers::AtomicCounter<moss::kernel::u64> total_frees_;
  moss::kernel::containers::AtomicCounter<moss::kernel::u64> active_objects_;

public:
  SlubAllocator() noexcept
      : caches_{}, cache_sizes_{},
        total_allocations_(0), total_frees_(0), active_objects_(0) {
    // 初始化不同大小的缓存
    moss::kernel::usize size = MIN_OBJECT_SIZE;
    for (moss::kernel::usize i = 0; i < NUM_CACHES; ++i) {
      cache_sizes_[i] = size;
      caches_[i] = new SlubCache(size);
      size *= 2;
      if (size > MAX_OBJECT_SIZE) {
        size = MAX_OBJECT_SIZE;
      }
    }
  }

  ~SlubAllocator() noexcept {
    for (moss::kernel::usize i = 0; i < NUM_CACHES; ++i) {
      delete caches_[i];
    }
  }

  NON_COPYABLE_NON_MOVABLE(SlubAllocator)

  // 高性能分配接口
  [[nodiscard]] SlubResult<void*> allocate(moss::kernel::usize size) noexcept {
    moss::kernel::usize cache_index = find_cache_index(size);
    if (cache_index >= NUM_CACHES) {
      return SlubResult<void*>{SlubError::InvalidSize};
    }

    auto result = caches_[cache_index]->allocate();
    if (result) {
      (void)total_allocations_.fetch_add(1, moss::MemoryOrder::Relaxed);
      (void)active_objects_.fetch_add(1, moss::MemoryOrder::Relaxed);
    }
    return result;
  }

  // 类型化分配
  template <typename T>
  [[nodiscard]] SlubResult<T*> allocate() noexcept {
    auto result = allocate(sizeof(T));
    if (!result) {
      return SlubResult<T*>{result.error()};
    }
    return SlubResult<T*>{static_cast<T*>(*result)};
  }

  // 高性能释放接口
  [[nodiscard]] SlubVoidResult deallocate(void* ptr, moss::kernel::usize size) noexcept {
    if (ptr == nullptr) {
      return SlubVoidResult{};
    }

    moss::kernel::usize cache_index = find_cache_index(size);
    if (cache_index >= NUM_CACHES) {
      return SlubVoidResult{SlubError::InvalidSize};
    }

    auto result = caches_[cache_index]->deallocate(ptr);
    if (result) {
      (void)total_frees_.fetch_add(1, moss::MemoryOrder::Relaxed);
      (void)active_objects_.fetch_sub(1, moss::MemoryOrder::Relaxed);
    }
    return result;
  }

  // 类型化释放
  template <typename T>
  [[nodiscard]] SlubVoidResult deallocate(T* ptr) noexcept {
    return deallocate(ptr, sizeof(T));
  }

  // 性能统计接口
  [[nodiscard]] moss::kernel::u64 get_total_allocations() const noexcept {
    return total_allocations_.load(moss::MemoryOrder::Relaxed);
  }

  [[nodiscard]] moss::kernel::u64 get_total_frees() const noexcept {
    return total_frees_.load(moss::MemoryOrder::Relaxed);
  }

  [[nodiscard]] moss::kernel::u64 get_active_objects() const noexcept {
    return active_objects_.load(moss::MemoryOrder::Relaxed);
  }

  // 获取Per-CPU性能统计
  [[nodiscard]] moss::kernel::u64 get_fast_path_allocations() const noexcept {
    moss::kernel::u64 total = 0;
    for (moss::kernel::usize i = 0; i < NUM_CACHES; ++i) {
      total += caches_[i]->fast_path_allocations();
    }
    return total;
  }

  [[nodiscard]] double get_fast_path_ratio() const noexcept {
    moss::kernel::u64 fast = get_fast_path_allocations();
    moss::kernel::u64 total = get_total_allocations();
    return total > 0 ? static_cast<double>(fast) / static_cast<double>(total) : 0.0;
  }

  // 内存压力时的缓存清理
  void shrink_caches() noexcept {
    for (moss::kernel::usize i = 0; i < NUM_CACHES; ++i) {
      // 清理所有CPU缓存中的批量释放缓冲区
      for (moss::kernel::u32 cpu = 0; cpu < moss::kernel::MAX_CPUS; ++cpu) {
        // 注意：这里需要访问SlubCache的内部Per-CPU缓存
        // 在实际实现中可能需要添加公开接口
      }
    }
  }

  // 获取详细统计信息
  void dump_statistics() const noexcept {
    // 输出总体统计
    // 在实际实现中，这里会输出到内核日志系统

    // 输出每个缓存的详细信息
    for (moss::kernel::usize i = 0; i < NUM_CACHES; ++i) {
      if (caches_[i]->total_objects() > 0) {
        // 缓存大小、总对象数、已分配对象数、快速路径比例等
      }
    }
  }

private:
  [[nodiscard]] moss::kernel::usize find_cache_index(moss::kernel::usize size) const noexcept {
    for (moss::kernel::usize i = 0; i < NUM_CACHES; ++i) {
      if (cache_sizes_[i] >= size) {
        return i;
      }
    }
    return NUM_CACHES; // 超出最大大小
  }
};

// 全局SLUB分配器实例
extern SlubAllocator* g_slub_allocator;

// 便利函数 - 使用SLUB分配器的高性能接口
template <typename T, typename... Args>
[[nodiscard]] SlubResult<T*> slub_new(Args&&... args) noexcept {
  auto ptr_result = g_slub_allocator->allocate<T>();
  if (!ptr_result) {
    return SlubResult<T*>{ptr_result.error()};
  }

  T* ptr = *ptr_result;
  try {
    new (ptr) T(moss::forward<Args>(args)...);
    return SlubResult<T*>{ptr};
  } catch (...) {
    [[maybe_unused]] auto cleanup_result = g_slub_allocator->deallocate(ptr);
    return SlubResult<T*>{SlubError::OutOfMemory};
  }
}

template <typename T>
[[nodiscard]] SlubVoidResult slub_delete(T* ptr) noexcept {
  if (ptr != nullptr) {
    ptr->~T();
    return g_slub_allocator->deallocate(ptr);
  }
  return SlubVoidResult{};
}

} // namespace moss::kernel::mm
