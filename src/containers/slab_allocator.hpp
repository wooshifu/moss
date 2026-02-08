#pragma once

// Slab内存分配器实现
// 专为内核对象分配优化，减少碎片和提高性能

#include "atomic_types.hpp"
#include "../include/types.hpp"
#include "../include/result.hpp"

// 包含统一的内核标准库支持
// Removed kernel_std.hpp include to avoid conflicts

namespace moss::kernel::containers {

// 内核环境的工具函数
template<typename T>
constexpr const T& kernel_max(const T& a, const T& b) noexcept {
    return (a < b) ? b : a;
}

// Slab分配器错误代码
enum class SlabError : u32 {
    OutOfMemory = 1,
    InvalidSize = 2,
    DoubleFree = 3,
    CorruptedSlab = 4
};

// Slab分配结果类型
template<typename T>
using SlabResult = moss::kernel::Result<T, SlabError>;

// void特化
using SlabVoidResult = moss::kernel::Result<void, SlabError>;

// 内存对齐工具
template<moss::kernel::usize Alignment>
constexpr moss::kernel::usize align_up(moss::kernel::usize value) noexcept {
    static_assert((Alignment & (Alignment - 1)) == 0, "Alignment must be power of 2");
    return (value + Alignment - 1) & ~(Alignment - 1);
}

constexpr bool is_aligned(moss::kernel::usize value, moss::kernel::usize alignment) noexcept {
    return (value & (alignment - 1)) == 0;
}

// Slab页面结构
struct SlabPage {
    void* memory;           // 页面内存起始地址
    moss::kernel::usize object_size;      // 对象大小
    moss::kernel::usize objects_per_page; // 每页对象数量
    moss::kernel::containers::AtomicCounter<moss::kernel::usize> free_count;    // 空闲对象数量
    moss::kernel::containers::AtomicPtr<u8> free_list;           // 空闲对象链表
    moss::kernel::containers::AtomicPtr<SlabPage> next;          // 下一个slab页面

    SlabPage(void* mem, moss::kernel::usize obj_size, moss::kernel::usize obj_per_page) noexcept
        : memory(mem), object_size(obj_size), objects_per_page(obj_per_page),
          free_count(obj_per_page), free_list(nullptr), next(nullptr) {
        initialize_free_list();
    }

private:
    void initialize_free_list() noexcept {
        // 将页面内存组织成空闲对象链表
        char* current = static_cast<char*>(memory);
        void* last_free = nullptr;

        for (moss::kernel::usize i = 0; i < objects_per_page; ++i) {
            void** obj_ptr = reinterpret_cast<void**>(current);
            *obj_ptr = last_free;
            last_free = current;
            current += object_size;
        }

        free_list.store(static_cast<u8*>(last_free), moss::MemoryOrder::Release);
    }
};

// 单个对象大小的Slab缓存
class SlabCache {
private:
    const moss::kernel::usize object_size_;
    [[maybe_unused]] const moss::kernel::usize object_alignment_;
    const moss::kernel::usize aligned_object_size_;
    const moss::kernel::usize objects_per_page_;

    // 页面链表
    moss::kernel::containers::AtomicPtr<SlabPage> full_pages_;    // 已满的页面
    moss::kernel::containers::AtomicPtr<SlabPage> partial_pages_; // 部分使用的页面
    moss::kernel::containers::AtomicPtr<SlabPage> empty_pages_;   // 空的页面

    // 统计信息
    moss::kernel::containers::AtomicCounter<moss::kernel::usize> total_objects_;
    moss::kernel::containers::AtomicCounter<moss::kernel::usize> allocated_objects_;

public:
    SlabCache(moss::kernel::usize object_size, moss::kernel::usize alignment = alignof(void*)) noexcept
        : object_size_(object_size),
          object_alignment_(alignment),
          aligned_object_size_(align_up<alignof(void*)>(kernel_max<moss::kernel::usize>(object_size, sizeof(void*)))),
          objects_per_page_(moss::kernel::PAGE_SIZE / aligned_object_size_),
          full_pages_(nullptr), partial_pages_(nullptr), empty_pages_(nullptr),
          total_objects_(0), allocated_objects_(0) {}

    ~SlabCache() noexcept {
        // 释放所有页面
        free_page_list(full_pages_.load(moss::MemoryOrder::Relaxed));
        free_page_list(partial_pages_.load(moss::MemoryOrder::Relaxed));
        free_page_list(empty_pages_.load(moss::MemoryOrder::Relaxed));
    }

    // 禁用拷贝和移动
    NON_COPYABLE_NON_MOVABLE(SlabCache)

    // 分配对象
    [[nodiscard]] SlabResult<void*> allocate() noexcept {
        // 首先尝试从部分使用的页面分配
        if (auto result = allocate_from_partial()) {
            return result;
        }

        // 尝试从空页面分配
        if (auto result = allocate_from_empty()) {
            return result;
        }

        // 需要分配新页面
        return allocate_new_page();
    }

    // 释放对象
    [[nodiscard]] SlabVoidResult deallocate(void* ptr) noexcept {
        if (ptr == nullptr) {
            return SlabVoidResult{};
        }

        // 查找对象所属的页面
        SlabPage* page = find_page_for_object(ptr);
        if (page == nullptr) {
            return SlabVoidResult{SlabError::CorruptedSlab};
        }

        // 将对象添加到页面的空闲链表中
        u8* current_free = page->free_list.load(moss::MemoryOrder::Relaxed);
        void** obj_ptr = static_cast<void**>(ptr);

        do {
            *obj_ptr = current_free;
        } while (!page->free_list.compare_exchange_weak(current_free, static_cast<u8*>(ptr),
                                                        moss::MemoryOrder::Release,
                                                        moss::MemoryOrder::Relaxed));

        moss::kernel::usize new_free_count = page->free_count.fetch_add(1, moss::MemoryOrder::AcqRel) + 1;
        (void)allocated_objects_.fetch_sub(1, moss::MemoryOrder::Relaxed);

        // 根据页面状态移动到相应的链表
        if (new_free_count == page->objects_per_page) {
            // 页面完全空闲，移动到空页面链表
            move_page_to_empty(page);
        } else if (new_free_count == 1) {
            // 从满页面变为部分页面
            move_page_from_full_to_partial(page);
        }

        return SlabVoidResult{};
    }

    // 获取统计信息
    [[nodiscard]] moss::kernel::usize total_objects() const noexcept {
        return total_objects_.load(moss::MemoryOrder::Relaxed);
    }

    [[nodiscard]] moss::kernel::usize allocated_objects() const noexcept {
        return allocated_objects_.load(moss::MemoryOrder::Relaxed);
    }

    [[nodiscard]] moss::kernel::usize object_size() const noexcept {
        return object_size_;
    }

    [[nodiscard]] double utilization() const noexcept {
        moss::kernel::usize total = total_objects();
        if (total == 0) return 0.0;
        return static_cast<double>(allocated_objects()) / static_cast<double>(total);
    }

private:
    [[nodiscard]] SlabResult<void*> allocate_from_partial() noexcept {
        SlabPage* page = partial_pages_.load(moss::MemoryOrder::Acquire);
        if (page == nullptr) {
            return SlabResult<void*>{SlabError::OutOfMemory};
        }

        return allocate_from_page(page);
    }

    [[nodiscard]] SlabResult<void*> allocate_from_empty() noexcept {
        SlabPage* page = empty_pages_.exchange(nullptr, moss::MemoryOrder::AcqRel);
        if (page == nullptr) {
            return SlabResult<void*>{SlabError::OutOfMemory};
        }

        // 将页面移动到部分使用链表
        move_page_to_partial(page);
        return allocate_from_page(page);
    }

    [[nodiscard]] SlabResult<void*> allocate_new_page() noexcept {
        // 分配新的页面内存
        void* page_memory = allocate_page();
        if (page_memory == nullptr) {
            return SlabResult<void*>{SlabError::OutOfMemory};
        }

        // 创建新的slab页面
        SlabPage* new_page = new SlabPage(page_memory, aligned_object_size_, objects_per_page_);
        (void)total_objects_.fetch_add(objects_per_page_, moss::MemoryOrder::Relaxed);

        // 添加到部分使用链表
        move_page_to_partial(new_page);
        return allocate_from_page(new_page);
    }

    [[nodiscard]] SlabResult<void*> allocate_from_page(SlabPage* page) noexcept {
        u8* current_free = page->free_list.load(moss::MemoryOrder::Acquire);
        if (current_free == nullptr) {
            return SlabResult<void*>{SlabError::OutOfMemory};
        }

        // 从空闲链表中取出一个对象
        u8* next_free;
        do {
            if (current_free == nullptr) {
                return SlabResult<void*>{SlabError::OutOfMemory};
            }
            next_free = *reinterpret_cast<u8**>(current_free);
        } while (!page->free_list.compare_exchange_weak(current_free, next_free,
                                                        moss::MemoryOrder::AcqRel,
                                                        moss::MemoryOrder::Acquire));

        moss::kernel::usize new_free_count = page->free_count.fetch_sub(1, moss::MemoryOrder::AcqRel) - 1;
        (void)allocated_objects_.fetch_add(1, moss::MemoryOrder::Relaxed);

        // 如果页面已满，移动到满页面链表
        if (new_free_count == 0) {
            move_page_from_partial_to_full(page);
        }

        return SlabResult<void*>{current_free};
    }

    // 查找对象所属的页面
    [[nodiscard]] SlabPage* find_page_for_object(void* ptr) const noexcept {
        moss::kernel::usize ptr_addr = reinterpret_cast<moss::kernel::usize>(ptr);
        moss::kernel::usize page_addr = ptr_addr & ~(moss::kernel::PAGE_SIZE - 1);

        // 在所有页面链表中搜索
        if (auto page = find_in_page_list(full_pages_.load(moss::MemoryOrder::Acquire), page_addr)) {
            return page;
        }
        if (auto page = find_in_page_list(partial_pages_.load(moss::MemoryOrder::Acquire), page_addr)) {
            return page;
        }
        return find_in_page_list(empty_pages_.load(moss::MemoryOrder::Acquire), page_addr);
    }

    [[nodiscard]] SlabPage* find_in_page_list(SlabPage* head, moss::kernel::usize page_addr) const noexcept {
        SlabPage* current = head;
        while (current != nullptr) {
            moss::kernel::usize current_page_addr = reinterpret_cast<moss::kernel::usize>(current->memory) & ~(moss::kernel::PAGE_SIZE - 1);
            if (current_page_addr == page_addr) {
                return current;
            }
            current = current->next.load(moss::MemoryOrder::Acquire);
        }
        return nullptr;
    }

    // 页面链表管理
    void move_page_to_partial(SlabPage* page) noexcept {
        SlabPage* old_head = partial_pages_.load(moss::MemoryOrder::Relaxed);
        do {
            page->next.store(old_head, moss::MemoryOrder::Relaxed);
        } while (!partial_pages_.compare_exchange_weak(old_head, page,
                                                      moss::MemoryOrder::Release,
                                                      moss::MemoryOrder::Relaxed));
    }

    void move_page_to_full(SlabPage* page) noexcept {
        SlabPage* old_head = full_pages_.load(moss::MemoryOrder::Relaxed);
        do {
            page->next.store(old_head, moss::MemoryOrder::Relaxed);
        } while (!full_pages_.compare_exchange_weak(old_head, page,
                                                   moss::MemoryOrder::Release,
                                                   moss::MemoryOrder::Relaxed));
    }

    void move_page_to_empty(SlabPage* page) noexcept {
        SlabPage* old_head = empty_pages_.load(moss::MemoryOrder::Relaxed);
        do {
            page->next.store(old_head, moss::MemoryOrder::Relaxed);
        } while (!empty_pages_.compare_exchange_weak(old_head, page,
                                                    moss::MemoryOrder::Release,
                                                    moss::MemoryOrder::Relaxed));
    }

    void move_page_from_partial_to_full(SlabPage* page) noexcept {
        remove_page_from_list(partial_pages_, page);
        move_page_to_full(page);
    }

    void move_page_from_full_to_partial(SlabPage* page) noexcept {
        remove_page_from_list(full_pages_, page);
        move_page_to_partial(page);
    }

    void remove_page_from_list([[maybe_unused]] moss::kernel::containers::AtomicPtr<SlabPage>& head,
                               [[maybe_unused]] SlabPage* page) noexcept {
        // 简化实现：重建链表（实际实现应该更高效）
        // 这里需要更复杂的无锁链表删除算法
    }

    // 页面内存分配（简化实现）
    [[nodiscard]] void* allocate_page() noexcept {
        // 在实际实现中，这里应该从内核页分配器分配
        // 目前使用简化的内存分配（在实际内核中需要替换）

        // 在真正的内核中，这里会调用页面分配器
        // 例如：alloc_pages(GFP_KERNEL, 0)

        // 临时使用原始内存分配（这在真正的内核中不可用）
        // 这只是为了让代码编译通过，实际使用时需要替换
        return nullptr;  // 暂时返回nullptr，表示分配失败
    }

    void free_page(void* ptr) noexcept {
        // 在真正的内核中，这里会调用页面释放器
        // 例如：free_pages(ptr, 0)

        // 目前什么都不做
        (void)ptr;
    }

    void free_page_list(SlabPage* head) noexcept {
        while (head != nullptr) {
            SlabPage* next = head->next.load(moss::MemoryOrder::Relaxed);
            free_page(head->memory);
            delete head;
            head = next;
        }
    }
};

// 多大小Slab分配器
class SlabAllocator {
private:
    static constexpr moss::kernel::usize NUM_CACHES = 32;
    static constexpr moss::kernel::usize MIN_OBJECT_SIZE = 8;
    static constexpr moss::kernel::usize MAX_OBJECT_SIZE = 4096;

    // 预定义的对象大小（2的幂）
    SlabCache* caches_[NUM_CACHES];
    moss::kernel::usize cache_sizes_[NUM_CACHES];

public:
    SlabAllocator() noexcept {
        // 初始化不同大小的缓存
        moss::kernel::usize size = MIN_OBJECT_SIZE;
        for (moss::kernel::usize i = 0; i < NUM_CACHES; ++i) {
            cache_sizes_[i] = size;
            caches_[i] = new SlabCache(size);
            size *= 2;
            if (size > MAX_OBJECT_SIZE) {
                size = MAX_OBJECT_SIZE;
            }
        }
    }

    ~SlabAllocator() noexcept {
        for (moss::kernel::usize i = 0; i < NUM_CACHES; ++i) {
            delete caches_[i];
        }
    }

    // 禁用拷贝和移动
    NON_COPYABLE_NON_MOVABLE(SlabAllocator)

    // 分配指定大小的内存
    [[nodiscard]] SlabResult<void*> allocate(moss::kernel::usize size) noexcept {
        moss::kernel::usize cache_index = find_cache_index(size);
        if (cache_index >= NUM_CACHES) {
            return SlabResult<void*>{SlabError::InvalidSize};
        }

        return caches_[cache_index]->allocate();
    }

    // 分配类型化对象
    template<typename T>
    [[nodiscard]] SlabResult<T*> allocate() noexcept {
        auto result = allocate(sizeof(T));
        if (!result) {
            return SlabResult<T*>{result.error()};
        }
        return SlabResult<T*>{static_cast<T*>(*result)};
    }

    // 释放内存
    [[nodiscard]] SlabVoidResult deallocate(void* ptr, moss::kernel::usize size) noexcept {
        if (ptr == nullptr) {
            return SlabVoidResult{};
        }

        moss::kernel::usize cache_index = find_cache_index(size);
        if (cache_index >= NUM_CACHES) {
            return SlabVoidResult{SlabError::InvalidSize};
        }

        return caches_[cache_index]->deallocate(ptr);
    }

    // 释放类型化对象
    template<typename T>
    [[nodiscard]] SlabVoidResult deallocate(T* ptr) noexcept {
        return deallocate(ptr, sizeof(T));
    }

    // 获取统计信息
    void get_statistics() const noexcept {
        // 输出所有缓存的统计信息（用于调试）
        for (moss::kernel::usize i = 0; i < NUM_CACHES; ++i) {
            if (caches_[i]->total_objects() > 0) {
                // 在实际实现中，这里会输出到内核日志
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
        return NUM_CACHES;  // 超出最大大小
    }
};

// 全局slab分配器实例
extern SlabAllocator* g_slab_allocator;

// 便利函数
template<typename T, typename... Args>
[[nodiscard]] SlabResult<T*> slab_new(Args&&... args) noexcept {
    auto ptr_result = g_slab_allocator->allocate<T>();
    if (!ptr_result) {
        return SlabResult<T*>{ptr_result.error()};
    }

    T* ptr = *ptr_result;
    try {
        new (ptr) T(moss::forward<Args>(args)...);
        return SlabResult<T*>{ptr};
    } catch (...) {
        g_slab_allocator->deallocate(ptr);
        return SlabResult<T*>{SlabError::OutOfMemory};
    }
}

template<typename T>
[[nodiscard]] SlabVoidResult slab_delete(T* ptr) noexcept {
    if (ptr != nullptr) {
        ptr->~T();
        return g_slab_allocator->deallocate(ptr);
    }
    return SlabVoidResult{};
}

} // namespace moss::kernel::containers