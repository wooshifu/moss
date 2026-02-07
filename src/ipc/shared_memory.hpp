#pragma once

// 高性能共享内存管理器
// 支持零拷贝IPC和大页面优化

#include "types.hpp"
#include "result.hpp"
#include "smart_ptr.hpp"
#include "containers/containers.hpp"
#include "mm/page_table.hpp"

namespace moss::kernel::ipc {

// 使用内核智能指针
using moss::kernel::unique_ptr;
using moss::kernel::make_unique;

// 共享内存权限
enum class ShmPermission : u8 {
    Read = 1,
    Write = 2,
    Execute = 4,
    ReadWrite = Read | Write,
    ReadExecute = Read | Execute,
    All = Read | Write | Execute
};

constexpr ShmPermission operator|(ShmPermission a, ShmPermission b) noexcept {
    return static_cast<ShmPermission>(static_cast<u8>(a) | static_cast<u8>(b));
}

constexpr ShmPermission operator&(ShmPermission a, ShmPermission b) noexcept {
    return static_cast<ShmPermission>(static_cast<u8>(a) & static_cast<u8>(b));
}

// 共享内存管理器统计信息
struct SharedMemoryStats {
    u64 total_regions;
    usize total_memory_usage;
    usize large_pages_used;
    usize huge_pages_used;
};

// 共享内存区域类型
enum class ShmType : u8 {
    Normal = 0,         // 普通共享内存
    DeviceMemory = 1,   // 设备内存映射
    DMA_Coherent = 2,   // DMA一致性内存
    LargePage = 3       // 大页面内存
};

// 共享内存区域描述符
struct ShmRegion {
    ShmId id;                           // 共享内存ID
    PhysAddr phys_base;                 // 物理基址
    VirtAddr virt_base;                 // 虚拟基址
    usize size;                         // 大小
    ShmType type;                       // 类型
    ShmPermission permission;           // 权限
    u32 ref_count;                      // 引用计数
    ProcessId owner_pid;                // 创建者进程ID
    u64 creation_time;                  // 创建时间

    // 页面属性
    mm::MemoryAttributes attributes;    // 内存属性
    usize page_size;                    // 页面大小（4KB, 2MB, 1GB）

    ShmRegion(ShmId region_id, PhysAddr phys, VirtAddr virt, usize sz,
              ShmType t, ShmPermission perm, ProcessId pid) noexcept
        : id(region_id), phys_base(phys), virt_base(virt), size(sz),
          type(t), permission(perm), ref_count(1), owner_pid(pid),
          creation_time(0), attributes{}, page_size(PAGE_SIZE) {}
};

// 进程的共享内存映射
struct ShmMapping {
    ShmId region_id;                    // 共享内存区域ID
    VirtAddr virt_addr;                 // 在进程地址空间中的虚拟地址
    usize size;                         // 映射大小
    ShmPermission permission;           // 映射权限
    u64 map_time;                       // 映射时间

    ShmMapping(ShmId id, VirtAddr addr, usize sz, ShmPermission perm) noexcept
        : region_id(id), virt_addr(addr), size(sz), permission(perm), map_time(0) {}

    // 相等比较运算符（用于RcuList::remove）
    [[nodiscard]] bool operator==(const ShmMapping& other) const noexcept {
        return region_id == other.region_id &&
               virt_addr == other.virt_addr &&
               size == other.size;
    }
};

// 共享内存管理器
class SharedMemoryManager {
private:
    // 共享内存区域注册表
    containers::RcuHashMap<ShmId, ShmRegion*> regions_;

    // Per-进程的共享内存映射
    containers::RcuHashMap<ProcessId, containers::RcuList<ShmMapping>*> process_mappings_;

    // ID分配器
    containers::AtomicCounter<ShmId> next_shm_id_;

    // 统计信息
    containers::AtomicCounter<u64> total_regions_;
    containers::AtomicCounter<usize> total_memory_usage_;

    // 大页面支持
    containers::AtomicCounter<usize> large_pages_used_;
    containers::AtomicCounter<usize> huge_pages_used_;

public:
    SharedMemoryManager() noexcept
        : next_shm_id_(1), total_regions_(0), total_memory_usage_(0),
          large_pages_used_(0), huge_pages_used_(0) {}

    ~SharedMemoryManager() noexcept {
        cleanup_all_regions();
    }

    // 禁用拷贝和移动
    NON_COPYABLE_NON_MOVABLE(SharedMemoryManager)

    // 创建共享内存区域
    [[nodiscard]] KernelResult<ShmId> create_region(ProcessId creator_pid,
                                                    usize size,
                                                    ShmType type = ShmType::Normal,
                                                    ShmPermission permission = ShmPermission::ReadWrite) noexcept {

        if (size == 0 || size > MAX_SHM_SIZE) {
            return KernelResult<ShmId>{KernelError::InvalidArgument};
        }

        // 分配共享内存ID
        ShmId region_id = next_shm_id_.fetch_add(1, containers::MemoryOrder::Relaxed);

        // 选择页面大小
        usize page_size = select_page_size(size, type);
        usize aligned_size = align_up_to_page(size, page_size);

        // 分配物理内存
        auto phys_result = allocate_physical_memory(aligned_size, page_size);
        if (!phys_result) {
            return KernelResult<ShmId>{phys_result.error()};
        }

        PhysAddr phys_addr = *phys_result;

        // 分配虚拟地址（内核空间）
        VirtAddr virt_addr = allocate_kernel_virtual_address(aligned_size);
        if (virt_addr == 0) {
            free_physical_memory(phys_addr, aligned_size);
            return KernelResult<ShmId>{KernelError::OutOfMemory};
        }

        // 创建共享内存区域
        ShmRegion* region = new ShmRegion(region_id, phys_addr, virt_addr,
                                         aligned_size, type, permission, creator_pid);
        region->page_size = page_size;
        region->attributes = get_memory_attributes(type);

        // 建立内核映射
        auto map_result = map_kernel_memory(virt_addr, phys_addr, aligned_size,
                                           region->attributes, page_size);
        if (!map_result) {
            delete region;
            free_physical_memory(phys_addr, aligned_size);
            return KernelResult<ShmId>{map_result.error()};
        }

        // 注册共享内存区域
        regions_.insert_or_update(region_id, region);
        (void)total_regions_.fetch_add(1, containers::MemoryOrder::Relaxed);
        (void)total_memory_usage_.fetch_add(aligned_size, containers::MemoryOrder::Relaxed);

        // 更新大页面统计
        update_page_statistics(page_size, 1);

        return KernelResult<ShmId>{region_id};
    }

    // 将共享内存映射到进程地址空间
    [[nodiscard]] KernelResult<VirtAddr> map_to_process(ProcessId pid,
                                                        ShmId region_id,
                                                        VirtAddr hint_addr = 0,
                                                        ShmPermission map_permission = ShmPermission::ReadWrite) noexcept {

        // 查找共享内存区域
        auto region_ptr = regions_.find(region_id);
        if (region_ptr == nullptr) {
            return KernelResult<VirtAddr>{KernelError::InvalidArgument};
        }
        const ShmRegion* region = *region_ptr;
        if (region == nullptr) {
            return KernelResult<VirtAddr>{KernelError::InvalidArgument};
        }

        // 检查权限
        if ((region->permission & map_permission) != map_permission) {
            return KernelResult<VirtAddr>{KernelError::PermissionDenied};
        }

        // 分配进程虚拟地址
        VirtAddr user_virt_addr = allocate_user_virtual_address(pid, region->size, hint_addr);
        if (user_virt_addr == 0) {
            return KernelResult<VirtAddr>{KernelError::OutOfMemory};
        }

        // 建立用户空间映射
        auto map_result = map_user_memory(pid, user_virt_addr, region->phys_base,
                                         region->size, region->attributes, region->page_size);
        if (!map_result) {
            free_user_virtual_address(pid, user_virt_addr, region->size);
            return KernelResult<VirtAddr>{map_result.error()};
        }

        // 增加引用计数
        const_cast<ShmRegion*>(region)->ref_count++;

        // 记录进程映射
        record_process_mapping(pid, region_id, user_virt_addr, region->size, map_permission);

        return KernelResult<VirtAddr>{user_virt_addr};
    }

    // 从进程地址空间取消映射
    [[nodiscard]] VoidResult unmap_from_process(ProcessId pid, ShmId region_id) noexcept {

        // 查找共享内存区域
        auto region_ptr = regions_.find(region_id);
        if (region_ptr == nullptr) {
            return VoidResult{KernelError::InvalidArgument};
        }
        ShmRegion* region = *region_ptr;
        if (region == nullptr) {
            return VoidResult{KernelError::InvalidArgument};
        }

        // 查找进程映射
        auto mapping = find_process_mapping(pid, region_id);
        if (!mapping) {
            return VoidResult{KernelError::NotFound};
        }

        // 取消用户空间映射
        unmap_user_memory(pid, mapping->virt_addr, mapping->size);

        // 释放虚拟地址
        free_user_virtual_address(pid, mapping->virt_addr, mapping->size);

        // 减少引用计数
        region->ref_count--;

        // 移除进程映射记录
        remove_process_mapping(pid, region_id);

        // 如果没有进程使用，考虑释放共享内存区域
        if (region->ref_count == 0) {
            (void)destroy_region(region_id);
        }

        return VoidResult{};
    }

    // 销毁共享内存区域
    [[nodiscard]] VoidResult destroy_region(ShmId region_id) noexcept {

        auto region_ptr = regions_.find(region_id);
        if (region_ptr == nullptr) {
            return VoidResult{KernelError::InvalidArgument};
        }
        ShmRegion* region = *region_ptr;
        if (region == nullptr) {
            return VoidResult{KernelError::InvalidArgument};
        }

        // 确保没有进程在使用
        if (region->ref_count > 0) {
            return VoidResult{KernelError::Busy};
        }

        // 取消内核映射
        unmap_kernel_memory(region->virt_base, region->size);

        // 释放物理内存
        free_physical_memory(region->phys_base, region->size);

        // 释放虚拟地址
        free_kernel_virtual_address(region->virt_base, region->size);

        // 更新统计
        (void)total_regions_.fetch_sub(1, containers::MemoryOrder::Relaxed);
        (void)total_memory_usage_.fetch_sub(region->size, containers::MemoryOrder::Relaxed);
        update_page_statistics(region->page_size, -1);

        // 从注册表中移除
        regions_.remove(region_id);

        delete region;
        return VoidResult{};
    }

    // 获取共享内存区域信息
    [[nodiscard]] const ShmRegion* get_region_info(ShmId region_id) const noexcept {
        auto region_ptr = regions_.find(region_id);
        if (region_ptr == nullptr) {
            return nullptr;
        }
        return *region_ptr;
    }

    // 同步共享内存（刷新缓存）
    void sync_region(ShmId region_id) noexcept {
        auto region_ptr = regions_.find(region_id);
        if (region_ptr == nullptr) {
            return;
        }
        const ShmRegion* region = *region_ptr;
        if (region == nullptr) {
            return;
        }

        // 执行缓存刷新操作
        flush_cache_range(region->virt_base, region->size);
    }

    // 获取统计信息
    [[nodiscard]] SharedMemoryStats get_statistics() const noexcept {
        return {
            total_regions_.load(containers::MemoryOrder::Relaxed),
            total_memory_usage_.load(containers::MemoryOrder::Relaxed),
            large_pages_used_.load(containers::MemoryOrder::Relaxed),
            huge_pages_used_.load(containers::MemoryOrder::Relaxed)
        };
    }

    // 清理进程的所有共享内存映射
    void cleanup_process_mappings(ProcessId pid) noexcept {
        auto mappings_ptr = process_mappings_.find(pid);
        if (mappings_ptr == nullptr) {
            return;
        }
        auto* mappings = *mappings_ptr;
        if (mappings == nullptr) {
            return;
        }

        // 取消所有映射
        mappings->for_each([this, pid](const ShmMapping& mapping) {
            (void)unmap_from_process(pid, mapping.region_id);
        });

        // 清理映射列表
        process_mappings_.remove(pid);
        delete mappings;
    }

private:
    // 选择最优页面大小
    [[nodiscard]] usize select_page_size(usize size, ShmType type) const noexcept {
        if (type == ShmType::LargePage || size >= HUGE_PAGE_SIZE) {
            return HUGE_PAGE_SIZE;  // 1GB
        } else if (size >= LARGE_PAGE_SIZE) {
            return LARGE_PAGE_SIZE; // 2MB
        } else {
            return PAGE_SIZE;       // 4KB
        }
    }

    // 页面对齐
    [[nodiscard]] static usize align_up_to_page(usize size, usize page_size) noexcept {
        return (size + page_size - 1) & ~(page_size - 1);
    }

    // 获取内存属性
    [[nodiscard]] static mm::MemoryAttributes get_memory_attributes(ShmType type) noexcept {
        switch (type) {
            case ShmType::Normal:
                return mm::MemoryAttributes::NORMAL_CACHEABLE;
            case ShmType::DeviceMemory:
                return mm::MemoryAttributes::DEVICE_nGnRnE;
            case ShmType::DMA_Coherent:
                return mm::MemoryAttributes::NORMAL_NON_CACHEABLE;
            case ShmType::LargePage:
                return mm::MemoryAttributes::NORMAL_CACHEABLE;
            default:
                return mm::MemoryAttributes::NORMAL_CACHEABLE;
        }
    }

    // 物理内存分配（简化实现）
    [[nodiscard]] KernelResult<PhysAddr> allocate_physical_memory([[maybe_unused]] usize size,
                                                                  [[maybe_unused]] usize page_size) noexcept {
        // 实际实现中需要从页面分配器分配
        // 这里返回模拟地址
        return KernelResult<PhysAddr>{0x80000000};
    }

    void free_physical_memory([[maybe_unused]] PhysAddr addr, [[maybe_unused]] usize size) noexcept {
        // 实际实现中需要释放到页面分配器
    }

    // 虚拟地址分配（简化实现）
    [[nodiscard]] VirtAddr allocate_kernel_virtual_address([[maybe_unused]] usize size) noexcept {
        // 实际实现中需要从内核虚拟地址空间分配
        return 0xFFFF800000000000ULL;
    }

    void free_kernel_virtual_address([[maybe_unused]] VirtAddr addr, [[maybe_unused]] usize size) noexcept {
        // 实际实现中需要释放虚拟地址
    }

    [[nodiscard]] VirtAddr allocate_user_virtual_address([[maybe_unused]] ProcessId pid,
                                                          [[maybe_unused]] usize size,
                                                          [[maybe_unused]] VirtAddr hint) noexcept {
        // 实际实现中需要从用户虚拟地址空间分配
        return 0x400000;
    }

    void free_user_virtual_address([[maybe_unused]] ProcessId pid,
                                   [[maybe_unused]] VirtAddr addr,
                                   [[maybe_unused]] usize size) noexcept {
        // 实际实现中需要释放用户虚拟地址
    }

    // 内存映射（简化实现）
    [[nodiscard]] VoidResult map_kernel_memory([[maybe_unused]] VirtAddr virt,
                                               [[maybe_unused]] PhysAddr phys,
                                               [[maybe_unused]] usize size,
                                               [[maybe_unused]] mm::MemoryAttributes attr,
                                               [[maybe_unused]] usize page_size) noexcept {
        // 实际实现中需要设置页表
        return VoidResult{};
    }

    void unmap_kernel_memory([[maybe_unused]] VirtAddr virt, [[maybe_unused]] usize size) noexcept {
        // 实际实现中需要清除页表项
    }

    [[nodiscard]] VoidResult map_user_memory([[maybe_unused]] ProcessId pid,
                                             [[maybe_unused]] VirtAddr virt,
                                             [[maybe_unused]] PhysAddr phys,
                                             [[maybe_unused]] usize size,
                                             [[maybe_unused]] mm::MemoryAttributes attr,
                                             [[maybe_unused]] usize page_size) noexcept {
        // 实际实现中需要设置用户页表
        return VoidResult{};
    }

    void unmap_user_memory([[maybe_unused]] ProcessId pid,
                           [[maybe_unused]] VirtAddr virt,
                           [[maybe_unused]] usize size) noexcept {
        // 实际实现中需要清除用户页表项
    }

    // 缓存管理
    void flush_cache_range(VirtAddr addr, usize size) noexcept {
        // ARM64缓存刷新操作
        VirtAddr end = addr + size;
        for (VirtAddr va = addr; va < end; va += CACHE_LINE_SIZE) {
            asm volatile("dc civac, %0" :: "r"(va) : "memory");
        }
        asm volatile("dsb sy" ::: "memory");
    }

    // 进程映射管理
    void record_process_mapping(ProcessId pid, ShmId region_id, VirtAddr virt_addr,
                               usize size, ShmPermission permission) noexcept {
        auto mappings_ptr = process_mappings_.find(pid);
        containers::RcuList<ShmMapping>* mappings = nullptr;
        if (mappings_ptr != nullptr) {
            mappings = *mappings_ptr;
        }
        if (mappings == nullptr) {
            mappings = new containers::RcuList<ShmMapping>();
            process_mappings_.insert_or_update(pid, mappings);
        }

        mappings->push_front(ShmMapping(region_id, virt_addr, size, permission));
    }

    [[nodiscard]] containers::Optional<ShmMapping> find_process_mapping(ProcessId pid, ShmId region_id) noexcept {
        auto mappings_ptr = process_mappings_.find(pid);
        if (mappings_ptr == nullptr) {
            return containers::Optional<ShmMapping>{};
        }
        auto* mappings = *mappings_ptr;
        if (mappings == nullptr) {
            return containers::Optional<ShmMapping>{};
        }

        const ShmMapping* found = mappings->find_if([region_id](const ShmMapping& mapping) {
            return mapping.region_id == region_id;
        });

        return found ? containers::Optional<ShmMapping>{*found} : containers::Optional<ShmMapping>{};
    }

    void remove_process_mapping(ProcessId pid, ShmId region_id) noexcept {
        auto mappings_ptr = process_mappings_.find(pid);
        if (mappings_ptr == nullptr) {
            return;
        }
        auto* mappings = *mappings_ptr;
        if (mappings == nullptr) {
            return;
        }

        // 首先找到要删除的映射
        const ShmMapping* found = mappings->find_if([region_id](const ShmMapping& mapping) {
            return mapping.region_id == region_id;
        });

        if (found != nullptr) {
            mappings->remove(*found);
        }
    }

    // 统计更新
    void update_page_statistics(usize page_size, i32 delta) noexcept {
        if (page_size == LARGE_PAGE_SIZE) {
            if (delta > 0) {
                (void)large_pages_used_.fetch_add(delta, containers::MemoryOrder::Relaxed);
            } else {
                (void)large_pages_used_.fetch_sub(-delta, containers::MemoryOrder::Relaxed);
            }
        } else if (page_size == HUGE_PAGE_SIZE) {
            if (delta > 0) {
                (void)huge_pages_used_.fetch_add(delta, containers::MemoryOrder::Relaxed);
            } else {
                (void)huge_pages_used_.fetch_sub(-delta, containers::MemoryOrder::Relaxed);
            }
        }
    }

    // 清理所有区域
    void cleanup_all_regions() noexcept {
        regions_.for_each([this](const auto& entry) {
            (void)destroy_region(entry.key);
        });
    }

    // 常量定义
    static constexpr usize MAX_SHM_SIZE = 1ULL << 32;  // 4GB最大共享内存
    static constexpr usize LARGE_PAGE_SIZE = 2 * 1024 * 1024;    // 2MB
    static constexpr usize HUGE_PAGE_SIZE = 1024 * 1024 * 1024;  // 1GB
};

// 全局共享内存管理器实例
extern SharedMemoryManager* g_shared_memory_manager;

} // namespace moss::kernel::ipc