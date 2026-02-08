#pragma once

/**
 * @file memory_concepts.hpp
 * @brief MOSS 内存管理 C++23 concepts 定义（工作版本）
 * @author MOSS Kernel Team
 * @version C++23
 *
 * 为内存分配器、智能指针、页表管理器等定义类型约束。
 * 专注于内存安全和性能优化。避免std依赖以确保编译成功。
 */

#include "kernel_concepts.hpp"

namespace moss::concepts {

using namespace moss::kernel;

/**
 * @brief 内核分配器概念
 *
 * 基本的内存分配器要求，基于接口检查
 */
template<typename Alloc>
concept KernelAllocator = KernelSafe<Alloc> &&
                          requires(Alloc a, usize size, usize align, void* ptr) {
                              a.allocate(size, align);
                              a.deallocate(ptr, size);
                          };

/**
 * @brief UniquePtr兼容类型概念
 *
 * 基于大小和基本特性的UniquePtr兼容性检查，避免std依赖
 */
template<typename T>
concept UniquePtrCompatible = PtrCompatible<T> && KernelSafe<T>;

/**
 * @brief SharedPtr兼容类型概念
 *
 * SharedPtr的基本要求，基于大小检查
 */
template<typename T>
concept SharedPtrCompatible = UniquePtrCompatible<T> &&
                              (sizeof(T) > 1); // 避免void类型

/**
 * @brief 智能指针删除器概念
 *
 * 删除器的基本要求，基于调用检查
 */
template<typename Deleter, typename T>
concept SmartPtrDeleter = KernelSafe<Deleter> &&
                          requires(Deleter d, T* ptr) {
                              d(ptr);
                          };

/**
 * @brief 页表项类型概念
 *
 * 页表项的要求：64位整数类型
 */
template<typename PTE>
concept PageTableEntry = IntegerType<PTE> &&
                         AtomicCompatible<PTE> &&
                         (sizeof(PTE) == sizeof(u64));

/**
 * @brief 物理页面概念
 *
 * 物理页面描述符的基本要求，基于接口检查
 */
template<typename Page>
concept PhysicalPage = KernelSafe<Page> &&
                       requires(Page p) {
                           p.physical_addr();
                           p.ref_count();
                           p.is_free();
                           p.inc_ref();
                           p.dec_ref();
                       };

/**
 * @brief 虚拟内存区域概念
 *
 * VMA的基本要求，基于接口检查
 */
template<typename VMA>
concept VirtualMemoryArea = KernelSafe<VMA> &&
                            requires(VMA vma) {
                                vma.start_addr();
                                vma.end_addr();
                                vma.size();
                            };

/**
 * @brief 内存池概念
 *
 * 内存池的基本要求，基于接口检查
 */
template<typename Pool>
concept MemoryPool = KernelSafe<Pool> &&
                     requires(Pool p, void* ptr) {
                         p.allocate();
                         p.deallocate(ptr);
                         p.block_size();
                     };

/**
 * @brief DMA缓冲区概念
 *
 * DMA缓冲区的要求，基于接口检查
 */
template<typename DMABuffer>
concept DmaBuffer = KernelSafe<DMABuffer> &&
                    requires(DMABuffer buf) {
                        buf.virtual_addr();
                        buf.physical_addr();
                        buf.size();
                    };

/**
 * @brief 页面分配器概念
 *
 * 页面级分配器，基于接口检查
 */
template<typename PageAlloc>
concept PageAllocator = KernelAllocator<PageAlloc> &&
                        requires(PageAlloc a, usize page_count) {
                            a.allocate_pages(page_count);
                            a.deallocate_pages(PhysAddr{0}, page_count);
                        };

/**
 * @brief Slab分配器概念
 *
 * Slab分配器的特殊要求，基于接口检查
 */
template<typename SlabAlloc>
concept SlabAllocator = KernelAllocator<SlabAlloc> &&
                        requires(SlabAlloc a) {
                            a.object_size();
                            a.objects_per_slab();
                        };

/**
 * @brief 内存屏障概念
 *
 * 内存屏障操作，基于接口检查
 */
template<typename Barrier>
concept MemoryBarrier = KernelSafe<Barrier> &&
                        requires(Barrier b) {
                            b.full_barrier();
                            b.read_barrier();
                            b.write_barrier();
                        };

/**
 * @brief 缓存管理概念
 *
 * CPU缓存管理，基于接口检查
 */
template<typename Cache>
concept CacheManager = KernelSafe<Cache> &&
                       requires(Cache c, void* addr, usize size) {
                           c.flush_dcache(addr, size);
                           c.invalidate_dcache(addr, size);
                           c.flush_icache();
                       };

} // namespace moss::concepts