#pragma once

// 内存回收引擎核心实现 - 展示关键回收算法
// LRU页面管理、智能扫描和工作集检测的具体实现

#include "memory_reclaim.hpp"
#include "../../../include/moss_std.hpp"

namespace moss::kernel::mm {

// LRU链表管理实现
class LRUListImpl {
public:
    // 高效的LRU页面添加 - O(1)时间复杂度
    static void add_to_active_impl(LRUList* lru, ReclaimPageInfo* page) noexcept {
        // 1. 设置页面状态
        page->state = PageState::ACTIVE;
        page->last_access_time = get_current_time_us();

        // 2. 插入到活跃链表头部 (最近使用)
        insert_to_list_head(&lru->active_head_, &lru->active_tail_, page);

        // 3. 更新计数器
        lru->active_count_.fetch_add(1, moss::MemoryOrder::Relaxed);

        // 4. 设置页面标志
        moss::kernel::u32 flags = page->flags.load(moss::MemoryOrder::Relaxed);
        flags |= ReclaimPageInfo::Flags::REFERENCED;
        page->flags.store(flags, moss::MemoryOrder::Release);
    }

    // 活跃页面降级到非活跃 - 二次机会算法
    static void move_to_inactive_impl(LRUList* lru, ReclaimPageInfo* page) noexcept {
        // 1. 从活跃链表移除
        remove_from_list(&lru->active_head_, &lru->active_tail_, page);
        lru->active_count_.fetch_sub(1, moss::MemoryOrder::Relaxed);

        // 2. 检查是否最近被访问 (二次机会)
        moss::kernel::u32 flags = page->flags.load(moss::MemoryOrder::Acquire);
        if (flags & ReclaimPageInfo::Flags::REFERENCED) {
            // 清除引用位，给予第二次机会
            flags &= ~ReclaimPageInfo::Flags::REFERENCED;
            page->flags.store(flags, moss::MemoryOrder::Release);

            // 重新插入到活跃链表头部
            insert_to_list_head(&lru->active_head_, &lru->active_tail_, page);
            lru->active_count_.fetch_add(1, moss::MemoryOrder::Relaxed);
            return;
        }

        // 3. 移动到非活跃链表
        page->state = PageState::INACTIVE;
        insert_to_list_head(&lru->inactive_head_, &lru->inactive_tail_, page);
        lru->inactive_count_.fetch_add(1, moss::MemoryOrder::Relaxed);
    }

    // 智能链表平衡 - 保持合理的活跃/非活跃比例
    static void balance_lists_impl(LRUList* lru) noexcept {
        moss::kernel::usize active_count = lru->active_count_.load(moss::MemoryOrder::Relaxed);
        moss::kernel::usize inactive_count = lru->inactive_count_.load(moss::MemoryOrder::Relaxed);

        if (active_count == 0 && inactive_count == 0) return;

        // 目标比例: 活跃页面占60%，非活跃页面占40%
        constexpr double TARGET_ACTIVE_RATIO = 0.6;

        moss::kernel::usize total_pages = active_count + inactive_count;
        moss::kernel::usize target_active = static_cast<moss::kernel::usize>(total_pages * TARGET_ACTIVE_RATIO);

        // 活跃页面过多，需要降级一些到非活跃
        if (active_count > target_active) {
            moss::kernel::usize pages_to_deactivate = active_count - target_active;
            ReclaimPageInfo* current = lru->active_tail_;

            for (moss::kernel::usize i = 0; i < pages_to_deactivate && current; ++i) {
                ReclaimPageInfo* next = current->prev_lru;
                move_to_inactive_impl(lru, current);
                current = next;
            }
        }
        // 非活跃页面过多，提升一些最近访问的页面
        else if (active_count < target_active && inactive_count > 0) {
            moss::kernel::usize pages_to_activate = moss::min(target_active - active_count, inactive_count / 4);
            ReclaimPageInfo* current = lru->inactive_head_;

            for (moss::kernel::usize i = 0; i < pages_to_activate && current; ++i) {
                ReclaimPageInfo* next = current->next_lru;
                moss::kernel::u32 flags = current->flags.load(moss::MemoryOrder::Acquire);

                // 只提升最近被引用的页面
                if (flags & ReclaimPageInfo::Flags::REFERENCED) {
                    // 移动到活跃链表
                    remove_from_list(&lru->inactive_head_, &lru->inactive_tail_, current);
                    lru->inactive_count_.fetch_sub(1, moss::MemoryOrder::Relaxed);

                    current->state = PageState::ACTIVE;
                    insert_to_list_head(&lru->active_head_, &lru->active_tail_, current);
                    lru->active_count_.fetch_add(1, moss::MemoryOrder::Relaxed);
                }
                current = next;
            }
        }
    }

private:
    // 链表操作辅助函数
    static void insert_to_list_head(ReclaimPageInfo** head, ReclaimPageInfo** tail, ReclaimPageInfo* page) noexcept {
        page->next_lru = *head;
        page->prev_lru = nullptr;

        if (*head) {
            (*head)->prev_lru = page;
        } else {
            *tail = page;
        }
        *head = page;
    }

    static void remove_from_list(ReclaimPageInfo** head, ReclaimPageInfo** tail, ReclaimPageInfo* page) noexcept {
        if (page->prev_lru) {
            page->prev_lru->next_lru = page->next_lru;
        } else {
            *head = page->next_lru;
        }

        if (page->next_lru) {
            page->next_lru->prev_lru = page->prev_lru;
        } else {
            *tail = page->prev_lru;
        }

        page->prev_lru = page->next_lru = nullptr;
    }

    static moss::kernel::u64 get_current_time_us() noexcept {
        // 简化实现 - 实际应使用系统时钟
        static moss::kernel::containers::AtomicU64 fake_time{0};
        return fake_time.fetch_add(1000, moss::MemoryOrder::Relaxed); // 1ms递增
    }
};

// 工作集检测器实现
class WorkingSetDetectorImpl {
public:
    // 智能工作集大小计算 - 基于访问模式和颠簸检测
    static void update_working_set_size_impl(WorkingSetDetector* detector) noexcept {
        moss::kernel::u64 current_time = get_current_time_us();
        moss::kernel::u64 time_delta = current_time - detector->last_sample_time_;

        // 每秒更新一次
        if (time_delta < 1000000) return; // 1秒 = 1,000,000微秒

        moss::kernel::u64 refault_count = detector->refault_count_.load(moss::MemoryOrder::Acquire);
        moss::kernel::u64 access_count = detector->access_count_.load(moss::MemoryOrder::Acquire);

        // 计算重新访问率 (refault rate)
        double refault_rate = static_cast<double>(refault_count) / moss::max(access_count, 1UL);

        moss::kernel::usize current_ws_size = detector->current_working_set_size_.load(moss::MemoryOrder::Relaxed);
        moss::kernel::usize new_ws_size = current_ws_size;

        // 工作集调整算法
        if (refault_rate > detector->config_.thrashing_threshold) {
            // 重新访问率过高，增大工作集
            new_ws_size = moss::min(current_ws_size + (current_ws_size / 8),
                                   detector->config_.max_working_set_size);
        } else if (refault_rate < detector->config_.thrashing_threshold / 2) {
            // 重新访问率较低，可以适当减小工作集
            new_ws_size = moss::max(current_ws_size - (current_ws_size / 16),
                                   detector->config_.min_working_set_size);
        }

        if (new_ws_size != current_ws_size) {
            detector->current_working_set_size_.store(new_ws_size, moss::MemoryOrder::Release);
        }

        // 重置计数器
        detector->refault_count_.store(0, moss::MemoryOrder::Relaxed);
        detector->access_count_.store(0, moss::MemoryOrder::Relaxed);
        detector->last_sample_time_ = current_time;
    }

    // 颠簸检测算法 - 基于访问历史分析
    static bool is_thrashing_detected_impl(const WorkingSetDetector* detector) noexcept {
        moss::kernel::u64 refault_count = detector->refault_count_.load(moss::MemoryOrder::Acquire);
        moss::kernel::u64 access_count = detector->access_count_.load(moss::MemoryOrder::Acquire);

        if (access_count < 100) return false; // 数据不足

        double current_refault_rate = static_cast<double>(refault_count) / access_count;

        // 当重新访问率超过阈值时认为发生颠簸
        return current_refault_rate > detector->config_.thrashing_threshold;
    }

    // 页面访问记录 - 环形缓冲区实现
    static void record_page_access_impl(WorkingSetDetector* detector, moss::kernel::PhysAddr addr) noexcept {
        moss::kernel::u32 index = detector->history_index_.fetch_add(1, moss::MemoryOrder::AcqRel) % WorkingSetDetector::HISTORY_SIZE;
        detector->access_history_[index] = addr;
        detector->access_count_.fetch_add(1, moss::MemoryOrder::Relaxed);
    }

private:
    static moss::kernel::u64 get_current_time_us() noexcept {
        // 与LRU实现保持一致
        static moss::kernel::containers::AtomicU64 fake_time{0};
        return fake_time.fetch_add(1000, moss::MemoryOrder::Relaxed);
    }
};

// 页面扫描器实现
class PageScannerImpl {
public:
    // 智能页面扫描算法 - 结合年龄和访问模式
    static ReclaimResult<moss::kernel::usize> scan_inactive_list_impl(PageScanner* scanner,
                                                                     moss::kernel::usize target_pages) noexcept {
        if (scanner->scanning_active_.load(moss::MemoryOrder::Acquire)) {
            return ReclaimResult<moss::kernel::usize>{ReclaimError::ScannerBusy};
        }

        scanner->scanning_active_.store(true, moss::MemoryOrder::Release);

        moss::kernel::usize pages_scanned = 0;
        moss::kernel::usize pages_reclaimed = 0;
        moss::kernel::u64 start_time = get_current_time_us();

        ReclaimPageInfo* current = scanner->lru_list_->get_inactive_tail();

        // 扫描非活跃链表，从尾部开始（最老的页面）
        while (current && pages_reclaimed < target_pages && pages_scanned < scanner->config_.scan_batch_size) {
            ReclaimPageInfo* next = current->prev_lru;
            pages_scanned++;

            // 检查页面是否可回收
            if (should_reclaim_page_impl(scanner, current)) {
                if (reclaim_page_impl(scanner, current).is_ok()) {
                    pages_reclaimed++;
                }
            } else if (should_activate_page_impl(scanner, current)) {
                // 页面被重新引用，提升到活跃链表
                LRUListImpl::move_to_active_impl(scanner->lru_list_, current);
                scanner->current_stats_.pages_activated++;
            }

            current = next;
        }

        // 更新统计信息
        moss::kernel::u64 scan_time = get_current_time_us() - start_time;
        scanner->current_stats_.pages_scanned += pages_scanned;
        scanner->current_stats_.pages_reclaimed += pages_reclaimed;
        scanner->current_stats_.scan_time_us += scan_time;

        if (pages_scanned > 0) {
            scanner->current_stats_.efficiency_ratio =
                static_cast<double>(pages_reclaimed) / pages_scanned;
        }

        scanner->scanning_active_.store(false, moss::MemoryOrder::Release);
        scanner->last_scan_time_ = get_current_time_us();

        return ReclaimResult<moss::kernel::usize>{pages_reclaimed};
    }

    // 活跃页面扫描 - 二次机会算法
    static ReclaimResult<moss::kernel::usize> scan_active_list_impl(PageScanner* scanner,
                                                                   moss::kernel::usize target_pages) noexcept {
        moss::kernel::usize pages_scanned = 0;
        moss::kernel::usize pages_deactivated = 0;

        ReclaimPageInfo* current = scanner->lru_list_->get_active_tail();

        // 扫描活跃链表，寻找可以降级的页面
        while (current && pages_deactivated < target_pages && pages_scanned < scanner->config_.scan_batch_size) {
            ReclaimPageInfo* next = current->prev_lru;
            pages_scanned++;

            moss::kernel::u32 flags = current->flags.load(moss::MemoryOrder::Acquire);

            // 检查页面是否最近被访问
            if (flags & ReclaimPageInfo::Flags::REFERENCED) {
                // 清除引用位，给予第二次机会
                flags &= ~ReclaimPageInfo::Flags::REFERENCED;
                current->flags.store(flags, moss::MemoryOrder::Release);
            } else {
                // 页面未被引用，降级到非活跃
                LRUListImpl::move_to_inactive_impl(scanner->lru_list_, current);
                pages_deactivated++;
                scanner->current_stats_.pages_deactivated++;
            }

            current = next;
        }

        return ReclaimResult<moss::kernel::usize>{pages_deactivated};
    }

    // 页面回收评估算法
    static bool should_reclaim_page_impl(const PageScanner* scanner, ReclaimPageInfo* page) noexcept {
        moss::kernel::u32 flags = page->flags.load(moss::MemoryOrder::Acquire);

        // 锁定页面不能回收
        if (flags & (ReclaimPageInfo::Flags::LOCKED | ReclaimPageInfo::Flags::UNEVICTABLE)) {
            return false;
        }

        // 工作集页面保护
        if ((flags & ReclaimPageInfo::Flags::WORKINGSET) &&
            !scanner->working_set_detector_->is_thrashing_detected()) {
            return false;
        }

        // 检查页面年龄
        moss::kernel::u32 page_age = calculate_page_age_impl(page);
        moss::kernel::u32 min_age_threshold = scanner->config_.max_scan_priority * 1000; // 毫秒

        if (page_age < min_age_threshold) {
            return false;
        }

        // 检查是否最近被访问
        if (flags & ReclaimPageInfo::Flags::REFERENCED) {
            return false;
        }

        // 脏页面需要特殊处理
        if (flags & ReclaimPageInfo::Flags::DIRTY) {
            // 在高内存压力下也回收脏页面
            return scanner->scan_priority_.load(moss::MemoryOrder::Relaxed) >= 3;
        }

        return true;
    }

    // 页面激活评估
    static bool should_activate_page_impl(const PageScanner* scanner, ReclaimPageInfo* page) noexcept {
        (void)scanner; // 避免未使用警告

        moss::kernel::u32 flags = page->flags.load(moss::MemoryOrder::Acquire);

        // 最近被引用的页面应该被激活
        if (flags & ReclaimPageInfo::Flags::REFERENCED) {
            return true;
        }

        // 工作集页面倾向于被激活
        if (flags & ReclaimPageInfo::Flags::WORKINGSET) {
            return true;
        }

        return false;
    }

    // 实际回收页面
    static ReclaimVoidResult reclaim_page_impl(PageScanner* scanner, ReclaimPageInfo* page) noexcept {
        moss::kernel::u32 flags = page->flags.load(moss::MemoryOrder::Acquire);

        // 处理脏页面
        if (flags & ReclaimPageInfo::Flags::DIRTY) {
            // 简化实现：标记为非脏
            flags &= ~ReclaimPageInfo::Flags::DIRTY;
            page->flags.store(flags, moss::MemoryOrder::Release);
        }

        // 从LRU链表移除
        scanner->lru_list_->remove_from_lru(page);

        // 释放物理页面
        [[maybe_unused]] auto result = BuddyAllocatorV2::free_pages(page->phys_addr, 0);

        // 释放页面元数据
        delete page;

        return ReclaimVoidResult{};
    }

private:
    static moss::kernel::u32 calculate_page_age_impl(ReclaimPageInfo* page) noexcept {
        moss::kernel::u64 current_time = get_current_time_us();
        return static_cast<moss::kernel::u32>((current_time - page->last_access_time) / 1000); // 转换为毫秒
    }

    static moss::kernel::u64 get_current_time_us() noexcept {
        static moss::kernel::containers::AtomicU64 fake_time{0};
        return fake_time.fetch_add(1000, moss::MemoryOrder::Relaxed);
    }
};

// 内存压力监控实现
class MemoryPressureMonitorImpl {
public:
    // 智能压力等级计算
    static void update_pressure_level_impl(MemoryPressureMonitor* monitor) noexcept {
        moss::kernel::usize free_memory = get_free_memory_impl();
        moss::kernel::usize available_memory = get_available_memory_impl();

        moss::kernel::usize low_threshold = monitor->free_threshold_low_.load(moss::MemoryOrder::Relaxed);
        [[maybe_unused]] moss::kernel::usize high_threshold = monitor->free_threshold_high_.load(moss::MemoryOrder::Relaxed);
        moss::kernel::usize critical_threshold = monitor->critical_threshold_.load(moss::MemoryOrder::Relaxed);

        MemoryPressureMonitor::PressureLevel new_level;

        if (free_memory <= critical_threshold) {
            new_level = MemoryPressureMonitor::PressureLevel::CRITICAL;
        } else if (free_memory <= low_threshold) {
            new_level = MemoryPressureMonitor::PressureLevel::HIGH;
        } else if (available_memory <= low_threshold * 2) {
            new_level = MemoryPressureMonitor::PressureLevel::MEDIUM;
        } else {
            new_level = MemoryPressureMonitor::PressureLevel::LOW;
        }

        // 平滑压力等级变化 - 避免频繁波动
        MemoryPressureMonitor::PressureLevel current_level =
            static_cast<MemoryPressureMonitor::PressureLevel>(
                monitor->current_pressure_level_.load(moss::MemoryOrder::Acquire));

        if (new_level != current_level) {
            monitor->current_pressure_level_.store(static_cast<moss::kernel::u32>(new_level),
                                                  moss::MemoryOrder::Release);
            monitor->record_pressure_level(new_level);
        }
    }

    // 基于历史数据的压力趋势分析
    static bool is_pressure_increasing_impl(const MemoryPressureMonitor* monitor) noexcept {
        moss::kernel::u32 history_idx = monitor->history_index_.load(moss::MemoryOrder::Acquire);

        if (history_idx < 4) return false; // 数据不足

        // 检查最近4个数据点的趋势
        moss::kernel::u32 increasing_count = 0;
        for (moss::kernel::usize i = 1; i < 4; ++i) {
            moss::kernel::u32 prev_idx = (history_idx - i - 1) % MemoryPressureMonitor::PRESSURE_HISTORY_SIZE;
            moss::kernel::u32 curr_idx = (history_idx - i) % MemoryPressureMonitor::PRESSURE_HISTORY_SIZE;

            if (static_cast<moss::kernel::u32>(monitor->pressure_history_[curr_idx]) >
                static_cast<moss::kernel::u32>(monitor->pressure_history_[prev_idx])) {
                increasing_count++;
            }
        }

        return increasing_count >= 2; // 至少2个增长点
    }

private:
    static moss::kernel::usize get_free_memory_impl() noexcept {
        // 简化实现 - 从Buddy分配器获取空闲内存
        auto stats = BuddyAllocatorV2::get_memory_stats();
        return stats.free_pages * PAGE_SIZE;
    }

    static moss::kernel::usize get_available_memory_impl() noexcept {
        // 简化实现 - 空闲内存 + 可回收内存
        moss::kernel::usize free_memory = get_free_memory_impl();
        // 假设有25%的内存是可回收的缓存
        return free_memory + (free_memory / 4);
    }
};

} // namespace moss::kernel::mm
