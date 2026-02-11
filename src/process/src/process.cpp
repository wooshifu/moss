// MOSS进程管理器实现
// 支持用户地址空间管理和ELF程序加载

#include "process.hpp"
#include "cfs_scheduler.hpp"
#include "mm/kernel_memory.hpp"
#include "mm/page_table.hpp"
#include "kernel/elf_loader.hpp"

// 用于调试输出
extern "C" void early_debug_print(const char* message) noexcept;

// 获取当前时间的辅助函数
static u64 get_current_time() noexcept {
    u64 count;
#if defined(MOSS_ARCH_ARM64)
    asm volatile("mrs %0, cntvct_el0" : "=r"(count));
#elif defined(MOSS_ARCH_X86_64)
    asm volatile("rdtsc" : "=A"(count));
#elif defined(MOSS_ARCH_RISCV)
    asm volatile("rdcycle %0" : "=r"(count));
#else
    count = 0; // 回退实现
#endif
    return count;
}

namespace moss::kernel::process {

// 进程映射项
struct ProcessEntry {
    ProcessId pid;
    Process* process;

    ProcessEntry(ProcessId id, Process* proc) : pid(id), process(proc) {}

    bool operator==(const ProcessEntry& other) const {
        return pid == other.pid;
    }
};

// 全局进程管理器实例
ProcessManager* g_process_manager = nullptr;

// 全局调度器实例
CfsScheduler* g_scheduler = nullptr;

// Process类方法实现
KernelResult<ThreadId> Process::create_thread(VirtAddr entry_point,
                                             VirtAddr stack_base,
                                             usize stack_size) noexcept {
    if (!address_space_) {
        return KernelResult<ThreadId>{ErrorCode::InvalidState};
    }

    ThreadId tid = allocate_thread_id();
    if (tid == INVALID_THREAD_ID) {
        return KernelResult<ThreadId>{ErrorCode::ResourceExhausted};
    }

    // 创建线程对象
    auto* thread = new Thread(tid, pid_);
    if (!thread) {
        return KernelResult<ThreadId>{ErrorCode::OutOfMemory};
    }

    // 初始化CPU上下文
    thread->context.pc = entry_point;
    thread->context.sp = stack_base + stack_size; // 栈向下增长
    thread->context.pstate = 0x0; // 用户模式

    // 设置栈信息
    thread->stack_base = stack_base;
    thread->stack_size = stack_size;

    // 设置线程状态
    thread->state = ProcessState::Ready;
    thread->start_time = get_current_time();

    // 添加到线程列表 (使用RcuList的push_front)
    ThreadEntry entry(tid, thread);
    threads_.push_front(entry);

    // 如果是第一个线程，设置为主线程
    if (thread_count_.load(containers::MemoryOrder::Relaxed) == 0) {
        main_thread_id_ = tid;
    }

    (void)thread_count_.fetch_add(1, containers::MemoryOrder::Relaxed);

    // 🔧 关键修复：将新线程加入调度器运行队列
    if (g_scheduler != nullptr) {
        // 获取当前CPU ID
        u32 current_cpu = 0; // 简化实现：使用CPU 0
        g_scheduler->enqueue_task(thread, current_cpu);

        early_debug_print("✅ 线程已加入调度器运行队列 TID=");
        // 简化的数字输出
        char tid_str[10];
        u32 temp_tid = static_cast<u32>(tid);
        int pos = 0;
        do {
            tid_str[pos++] = '0' + (temp_tid % 10);
            temp_tid /= 10;
        } while (temp_tid > 0 && pos < 9);
        tid_str[pos] = '\0';
        // 反转字符串
        for (int i = 0; i < pos / 2; i++) {
            char temp = tid_str[i];
            tid_str[i] = tid_str[pos - 1 - i];
            tid_str[pos - 1 - i] = temp;
        }
        early_debug_print(tid_str);
        early_debug_print("\n");
    }

    return KernelResult<ThreadId>{tid};
}

Thread* Process::get_thread(ThreadId tid) const noexcept {
    const ThreadEntry* entry = threads_.find_if([tid](const ThreadEntry& e) {
        return e.tid == tid;
    });
    return entry ? entry->thread : nullptr;
}

Thread* Process::get_main_thread() const noexcept {
    return get_thread(main_thread_id_);
}

VoidResult Process::set_address_space(unique_ptr<AddressSpace> as) noexcept {
    if (!as) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    address_space_ = moss::move(as);
    return VoidResult{};
}

void Process::set_state(ProcessState new_state) noexcept {
    state_ = new_state;
}

void Process::update_cpu_time(u64 user_time, u64 kernel_time) noexcept {
    stats_.cpu_time += user_time + kernel_time;
}

void Process::record_context_switch(bool voluntary) noexcept {
    if (voluntary) {
        ++stats_.voluntary_ctxt_switches;
    } else {
        ++stats_.nonvoluntary_ctxt_switches;
    }
}

void Process::record_page_fault(bool major) noexcept {
    if (major) {
        ++stats_.major_faults;
    } else {
        ++stats_.minor_faults;
    }
}

void Process::cleanup_threads() noexcept {
    threads_.for_each([](const ThreadEntry& entry) {
        delete entry.thread;
    });
    threads_.clear();
}

ThreadId Process::allocate_thread_id() noexcept {
    // 简化实现：从当前线程数量+1开始分配
    static containers::AtomicU32 next_tid{1000};
    return next_tid.fetch_add(1, containers::MemoryOrder::Relaxed);
}


// ProcessManager类方法实现
KernelResult<Process*> ProcessManager::create_process(ProcessId parent_pid) noexcept {
    ProcessId new_pid = allocate_pid();
    if (new_pid == INVALID_PROCESS_ID) {
        return KernelResult<Process*>{ErrorCode::ResourceExhausted};
    }

    auto* process = new Process(new_pid, parent_pid);
    if (!process) {
        return KernelResult<Process*>{ErrorCode::OutOfMemory};
    }

    processes_.insert_or_update(new_pid, process);

    record_fork();
    return KernelResult<Process*>{process};
}

VoidResult ProcessManager::terminate_process(ProcessId pid, i32 exit_code) noexcept {
    Process* const* entry = processes_.find(pid);
    if (!entry) {
        return VoidResult{ErrorCode::NotFound};
    }

    Process* process = *entry;
    process->set_state(ProcessState::Terminated);
    process->set_exit_code(exit_code);

    // 从进程表中移除
    (void)processes_.remove(pid);

    // 释放引用（可能会删除进程对象）
    process->release();

    record_exit();
    return VoidResult{};
}

Process* ProcessManager::find_process(ProcessId pid) const noexcept {
    Process* const* entry = processes_.find(pid);
    return entry ? *entry : nullptr;
}

bool ProcessManager::process_exists(ProcessId pid) const noexcept {
    return find_process(pid) != nullptr;
}

KernelResult<ProcessId> ProcessManager::sys_fork() noexcept {
    // TODO: 实现进程复制
    return KernelResult<ProcessId>{ErrorCode::NotSupported};
}

VoidResult ProcessManager::sys_exit(i32 exit_code) noexcept {
    // TODO: 实现进程退出
    (void)exit_code;
    return VoidResult{ErrorCode::NotSupported};
}

KernelResult<ProcessId> ProcessManager::sys_wait(ProcessId pid) noexcept {
    // TODO: 实现进程等待
    (void)pid;
    return KernelResult<ProcessId>{ErrorCode::NotSupported};
}

u64 ProcessManager::total_processes() const noexcept {
    return processes_.size();
}

ProcessId ProcessManager::allocate_pid() noexcept {
    return next_pid_.fetch_add(1, containers::MemoryOrder::Relaxed);
}

// 用户地址空间管理扩展功能
namespace user_space {

// 创建用户地址空间
KernelResult<unique_ptr<AddressSpace>> create_user_address_space() noexcept {
    early_debug_print("🏗️ 创建用户地址空间...\n");

    // 简化实现：创建一个基本的地址空间
    // TODO: 实现实际的页表分配和管理

    // 分配ASID（地址空间ID）
    static containers::AtomicU32 next_asid{1};
    u16 asid = static_cast<u16>(next_asid.fetch_add(1, containers::MemoryOrder::Relaxed));

    // 创建地址空间对象（使用占位符物理地址）
    PhysAddr pgd_phys = 0x0; // 占位符
    auto address_space = make_unique<AddressSpace>(pgd_phys, asid);
    if (!address_space) {
        return KernelResult<unique_ptr<AddressSpace>>{ErrorCode::OutOfMemory};
    }

    early_debug_print("✅ 用户地址空间创建成功（简化版本）\n");
    return KernelResult<unique_ptr<AddressSpace>>{moss::move(address_space)};
}

// 从ELF程序加载创建进程
KernelResult<Process*> create_process_from_elf(const u8* elf_data, usize elf_size) noexcept {
    early_debug_print("🚀 从ELF数据创建进程...\n");

    if (!g_process_manager) {
        return KernelResult<Process*>{ErrorCode::InternalError};
    }

    // 1. 使用ELF加载器加载程序
    auto load_result = elf::ElfLoader::load_elf_from_memory(elf_data, elf_size);
    if (!load_result) {
        early_debug_print("❌ ELF程序加载失败\n");
        return KernelResult<Process*>{load_result.error()};
    }

    const auto& loaded_program = *load_result;
    early_debug_print("✅ ELF程序加载成功\n");

    // 2. 创建进程
    auto process_result = g_process_manager->create_process();
    if (!process_result) {
        early_debug_print("❌ 进程创建失败\n");
        return process_result;
    }

    Process* process = *process_result;
    early_debug_print("✅ 进程创建成功\n");

    // 3. 创建用户地址空间
    auto as_result = create_user_address_space();
    if (!as_result) {
        early_debug_print("❌ 用户地址空间创建失败\n");
        (void)g_process_manager->terminate_process(process->pid(), -1);
        return KernelResult<Process*>{as_result.error()};
    }

    unique_ptr<AddressSpace> address_space = moss::move(*as_result);
    early_debug_print("✅ 用户地址空间创建成功\n");

    // 4. 设置地址空间到进程
    auto set_as_result = process->set_address_space(moss::move(address_space));
    if (!set_as_result) {
        early_debug_print("❌ 地址空间设置失败\n");
        (void)g_process_manager->terminate_process(process->pid(), -1);
        return KernelResult<Process*>{set_as_result.error()};
    }

    // 5. 创建主线程
    auto thread_result = process->create_thread(
        loaded_program.entry_point,
        loaded_program.stack_top - 8 * 1024 * 1024, // 栈基址（8MB栈）
        8 * 1024 * 1024 // 栈大小
    );

    if (!thread_result) {
        early_debug_print("❌ 主线程创建失败\n");
        (void)g_process_manager->terminate_process(process->pid(), -1);
        return KernelResult<Process*>{thread_result.error()};
    }

    (void)*thread_result; // 抑制未使用变量警告
    early_debug_print("✅ 主线程创建成功\n");

    // 6. 设置进程为就绪状态
    process->set_state(ProcessState::Ready);

    early_debug_print("🎉 从ELF创建进程完成\n");
    return KernelResult<Process*>{process};
}

// 映射内存区域到用户地址空间
VoidResult map_user_memory(AddressSpace* as, VirtAddr vaddr, PhysAddr paddr,
                          usize size, u32 flags) noexcept {
    if (!as || vaddr == 0 || size == 0) {
        return VoidResult{ErrorCode::InvalidArgument};
    }

    // 创建VMA区域
    VmaRegion region(vaddr, vaddr + size, flags, paddr);

    // 添加到地址空间的VMA列表 (使用push_front)
    as->vma_list.push_front(region);

    // 更新统计信息
    usize pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    (void)as->total_pages.fetch_add(pages, containers::MemoryOrder::Relaxed);

    return VoidResult{};
}

// 分配用户堆内存
KernelResult<VirtAddr> allocate_user_heap(Process* process, usize size) noexcept {
    if (!process || !process->address_space() || size == 0) {
        return KernelResult<VirtAddr>{ErrorCode::InvalidArgument};
    }

    AddressSpace* as = process->address_space();

    // 简化实现：固定分配在4GB处
    VirtAddr heap_addr = 0x100000000ULL; // 4GB

    // TODO: 实现真正的内存分配和映射
    // 现在只是创建VMA区域
    u32 flags = 0x3; // 可读写
    auto map_result = map_user_memory(as, heap_addr, 0, size, flags);
    if (!map_result) {
        return KernelResult<VirtAddr>{map_result.error()};
    }

    return KernelResult<VirtAddr>{heap_addr};
}

} // namespace user_space

} // namespace moss::kernel::process
