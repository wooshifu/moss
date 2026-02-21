// MOSS内核系统调用表实现
// 提供完整的系统调用处理和分发机制

module;

// Architecture detection
#include "arch_detect.h"

// extern "C" declarations in global module fragment
#if defined(__aarch64__) || defined(MOSS_ARCH_ARM64)
extern "C" void context_switch(void* prev_context, void* next_context);
extern "C" void switch_to_user(void* context, unsigned long user_sp);
#endif

module moss.kernel;

import moss.vfs;

namespace moss::kernel::syscall {

// 全局系统调用统计
SyscallStats g_syscall_stats = {0, 0, 0, 0, 0};

// 系统调用处理函数实现
namespace handlers {
    namespace log = moss::kernel::logging;

    // 基础系统调用处理函数
    long sys_debug_print(long arg0, long, long, long, long, long) noexcept {
        if (arg0 != 0) {
            moss::kernel::hal::uart::puts(reinterpret_cast<const char*>(arg0));
            return 0;
        }
        return -Errno::EINVAL;
    }

    long sys_exit(long exit_code, long, long, long, long, long) noexcept {
        using namespace moss::kernel::process;
        namespace log = moss::kernel::logging;

        log::klog::info("sys_exit: exit_code={}", exit_code);

        Thread *cur = CfsScheduler::get_current_task();
        if (!cur) {
            log::klog::error("sys_exit: no current thread");
            while (true) { ::moss::kernel::arch::cpu_yield(); }
        }

        ProcessId pid = cur->owner_pid;
        log::klog::info("sys_exit: PID={} TID={}", pid, static_cast<u32>(cur->tid));

        Process *proc = g_process_manager
            ? g_process_manager->find_process(pid) : nullptr;
        if (!proc) {
            log::klog::error("sys_exit: process not found PID={}", pid);
            while (true) { ::moss::kernel::arch::cpu_yield(); }
        }

        // Delegate to shared Zombie transition (never returns)
        do_exit(cur, proc, static_cast<i32>(exit_code));
    }

    long sys_getpid(long, long, long, long, long, long) noexcept {
        using namespace moss::kernel::process;
        Thread *cur = CfsScheduler::get_current_task();
        if (!cur) return -Errno::ESRCH;
        return static_cast<long>(cur->owner_pid);
    }

    long sys_getppid(long, long, long, long, long, long) noexcept {
        using namespace moss::kernel::process;
        Thread *cur = CfsScheduler::get_current_task();
        if (!cur) return -Errno::ESRCH;
        Process *proc = g_process_manager
            ? g_process_manager->find_process(cur->owner_pid)
            : nullptr;
        if (!proc) return -Errno::ESRCH;
        return static_cast<long>(proc->parent_pid());
    }

    long sys_getuid(long, long, long, long, long, long) noexcept {
        // TODO: 从安全子系统获取用户ID
        // 临时返回 root 用户 (0)
        return 0;
    }

    long sys_getgid(long, long, long, long, long, long) noexcept {
        // TODO: 从安全子系统获取组ID
        // 临时返回 root 组 (0)
        return 0;
    }

    // fork() — create a child process with COW-shared address space.
    // Child returns 0, parent returns child PID.
    long sys_fork(long, long, long, long, long, long) noexcept {
        using namespace moss::kernel::process;
        namespace log = moss::kernel::logging;

        // 1. Get current thread and process
        Thread *parent_thread = CfsScheduler::get_current_task();
        if (!parent_thread) {
            log::klog::error("sys_fork: no current thread");
            return -Errno::EAGAIN;
        }

        Process *parent_proc = g_process_manager
            ? g_process_manager->find_process(parent_thread->owner_pid)
            : nullptr;
        if (!parent_proc || !parent_proc->address_space()) {
            log::klog::error("sys_fork: no parent process or address space");
            return -Errno::EAGAIN;
        }

        AddressSpace *parent_as = parent_proc->address_space();

        // 2. Capture user-mode PC and SP (preserved in EL1 system registers)
        u64 user_pc = 0;
        u64 user_sp = 0;
#if defined(MOSS_ARCH_ARM64)
        asm volatile("mrs %0, elr_el1" : "=r"(user_pc));
        asm volatile("mrs %0, sp_el0"  : "=r"(user_sp));
#endif

        // 3. Create child process
        auto child_proc_result = g_process_manager->create_process(parent_proc->pid());
        if (!child_proc_result) {
            log::klog::error("sys_fork: create_process failed");
            return -Errno::ENOMEM;
        }
        Process *child_proc = *child_proc_result;

        // 4. Create child address space (new PGD + ASID)
        auto child_as_result = user_space::create_user_address_space();
        if (!child_as_result) {
            log::klog::error("sys_fork: create_user_address_space failed");
            return -Errno::ENOMEM;
        }
        auto child_as = moss::move(*child_as_result);

        // 5. Clone page tables with COW
        mm::PageTableManager::clone_user_page_tables(
            parent_as->pgd_phys, child_as->pgd_phys);

        // 6. Flush parent TLB (PTEs changed to readonly/COW)
#if defined(MOSS_ARCH_ARM64)
        {
            u64 asid_val = static_cast<u64>(parent_as->asid) << 48;
            asm volatile("tlbi aside1is, %0" :: "r"(asid_val));
            asm volatile("dsb ish" ::: "memory");
            asm volatile("isb" ::: "memory");
        }
#endif

        // 7. Copy VMAs from parent to child via RcuList iteration
        parent_as->vmas.for_each([&child_as](const process::VmaRegion& vma) {
            child_as->vmas.push_front(vma);
        });

        // 8. Bind address space to child process
        auto set_result = child_proc->set_address_space(moss::move(child_as));
        if (!set_result) {
            log::klog::error("sys_fork: set_address_space failed");
            return -Errno::ENOMEM;
        }

        // 9. Create child thread
        ThreadId child_tid = Process::allocate_thread_id();
        auto *child_thread = new Thread(child_tid, child_proc->pid());
        if (!child_thread) {
            log::klog::error("sys_fork: thread allocation failed");
            return -Errno::ENOMEM;
        }

        // 10. Copy parent context → child, set return register = 0 for child
        child_thread->context = parent_thread->context;
#if defined(MOSS_ARCH_ARM64)
        child_thread->context.x[0] = 0;        // ARM64: x0 = fork return
#elif defined(MOSS_ARCH_X86_64)
        child_thread->context.rax = 0;          // x86_64: rax = fork return
#elif defined(MOSS_ARCH_RISCV)
        child_thread->context.x[10] = 0;        // RISC-V: a0 (x10) = fork return
#endif
        child_thread->context.pc = user_pc;     // return to instruction after SVC
        child_thread->context.sp = user_sp;     // same user stack
        child_thread->context.pstate = 0;       // EL0t, all interrupts enabled

        child_thread->stack_base = parent_thread->stack_base;
        child_thread->stack_size = parent_thread->stack_size;
        child_thread->needs_initial_eret = true;
        child_thread->is_user_task = true;
        child_thread->sched_class = SchedClass::Normal;
        child_thread->se.nice = parent_thread->se.nice;
        child_thread->se.weight = parent_thread->se.weight;
        child_thread->se.vruntime = parent_thread->se.vruntime;
        child_thread->state = ProcessState::Ready;

        // 11. Allocate per-thread kernel stack (16KB)
        constexpr usize KERNEL_STACK_ORDER = 2;  // 4 pages = 16KB
        constexpr usize KERNEL_STACK_SIZE = PAGE_SIZE << KERNEL_STACK_ORDER;
        auto kstack_result = mm::allocate_pages(KERNEL_STACK_ORDER);
        if (!kstack_result) {
            log::klog::error("sys_fork: kernel stack alloc failed");
            delete child_thread;
            return -Errno::ENOMEM;
        }
        PhysAddr kstack_phys = *kstack_result;
        child_thread->kernel_stack_base = static_cast<VirtAddr>(kstack_phys);
        child_thread->kernel_stack_size = KERNEL_STACK_SIZE;

        // 12. Register child thread in child process's thread list
        child_proc->register_thread(child_thread);

        // 12b. Clone VFS fd table from parent to child
        if (parent_proc->fd_table() != nullptr) {
            auto* parent_fdt = static_cast<moss::kernel::vfs::FdTable*>(
                parent_proc->fd_table());
            auto* child_fdt = parent_fdt->clone();
            child_proc->set_fd_table(child_fdt);
        }

        // 13. Register child in parent's children list (for waitpid)
        parent_proc->add_child(child_proc->pid());

        // 14. Enqueue child into scheduler (scatter across CPUs via load balancer)
        child_proc->set_state(ProcessState::Running);
        if (g_scheduler) {
            u32 target_cpu = arch::get_current_cpu_id();
            if (g_load_balancer) {
                target_cpu = g_load_balancer->select_cpu_for_task(child_thread, *g_scheduler);
            }
            g_scheduler->enqueue_task(child_thread, target_cpu);
        }

        // 15. Parent returns child PID
        return static_cast<long>(child_proc->pid());
    }

    long sys_execve(long pathname_addr, long /* argv */, long /* envp */,
                    long, long, long) noexcept {
        using namespace moss::kernel::process;
        using namespace moss::kernel::elf;

        // 1. Get current thread and process
        Thread *cur = g_scheduler ? CfsScheduler::get_current_task() : nullptr;
        if (!cur) {
            log::klog::error("execve: no current task");
            return -Errno::ESRCH;
        }
        Process *proc = g_process_manager
            ? g_process_manager->find_process(cur->owner_pid)
            : nullptr;
        if (!proc || !proc->address_space()) {
            log::klog::error("execve: no process or address space");
            return -Errno::ESRCH;
        }

        // 2. Copy pathname from user memory into kernel buffer.
        //    After step 5 switches TTBR0 to the kernel PGD, user addresses
        //    are no longer accessible, so we must capture the string now.
        constexpr usize PATH_MAX = 256;
        char pathname_buf[PATH_MAX];
        {
            const char *user_path = reinterpret_cast<const char *>(
                static_cast<usize>(pathname_addr));
            if (!user_path) {
                return -Errno::EFAULT;
            }
            usize len = 0;
            while (len < PATH_MAX - 1 && user_path[len] != '\0') {
                pathname_buf[len] = user_path[len];
                len++;
            }
            pathname_buf[len] = '\0';
        }
        const char *pathname = pathname_buf;

        // 3. Resolve file via VFS path resolution (replaces direct initramfs access)
        auto* dentry = moss::kernel::vfs::resolve_path(pathname);
        if (!dentry || !dentry->inode) {
            log::klog::error("execve: '{}' not found via VFS", pathname);
            return -Errno::ENOENT;
        }
        auto* file_inode = dentry->inode;
        if (file_inode->type != moss::kernel::vfs::FileType::Regular) {
            log::klog::error("execve: '{}' is not a regular file", pathname);
            return -Errno::EACCES;
        }
        if (file_inode->data == nullptr || file_inode->size == 0) {
            log::klog::error("execve: '{}' has no data", pathname);
            return -Errno::ENOEXEC;
        }

        // 4. Validate ELF header (inode->data = zero-copy ELF backing)
        auto *elf_hdr = reinterpret_cast<const ElfHeader *>(file_inode->data);
        if (!validate_elf_header(elf_hdr, file_inode->size)) {
            log::klog::error("execve: '{}' is not a valid ELF", pathname);
            return -Errno::ENOEXEC;
        }

        VirtAddr elf_entry = elf_hdr->e_entry;
        const auto *phdrs = get_program_headers(elf_hdr);
        u16 phnum = elf_hdr->e_phnum;

        // ===== Point of no return =====
        // From here, errors terminate the process (old address space is gone).

        // 5. Switch TTBR0 to kernel PGD (safe teardown)
        AddressSpace *old_as = proc->address_space();
        PhysAddr old_pgd = old_as->pgd_phys;

#if defined(MOSS_ARCH_ARM64)
        {
            auto *kpgd = mm::PageTableManager::get_kernel_pgd();
            if (kpgd) {
                u64 kpgd_phys = mm::PageTableManager::get_physical_address(kpgd);
                asm volatile("msr ttbr0_el1, %0" :: "r"(kpgd_phys));
                asm volatile("dsb ish" ::: "memory");
                asm volatile("isb" ::: "memory");
            }
        }
#endif

        // 6. Free old user page tables
        if (old_pgd != 0) {
            mm::PageTableManager::free_user_page_tables(old_pgd);
            old_as->pgd_phys = 0; // prevent double-free
        }

        // 7. Create new address space
        auto new_as_result = user_space::create_user_address_space();
        if (!new_as_result) {
            log::klog::error("execve: failed to create new address space");
            // Unrecoverable — process has no address space
            cur->state = ProcessState::Terminated;
            if (g_scheduler) {
                g_scheduler->dequeue_task(cur);
                g_scheduler->schedule_after_exit();
            }
            while (true) { ::moss::kernel::arch::cpu_halt(); }
        }
        auto new_as = moss::move(*new_as_result);

        // 8. Load PT_LOAD segments as VMAs
        //
        // Multiple PT_LOAD segments may fall within the same page (e.g.
        // .text at 0x400000 and .rodata at 0x400048 both within page
        // 0x400000-0x401000).  The demand-paging handler maps one page
        // per fault using a single VMA's backing data, so overlapping
        // VMAs would cause data loss.
        //
        // Solution: two-pass approach.
        //   Pass 1 — compute the overall VA range and file-offset range
        //            across all PT_LOAD segments.
        //   Pass 2 — create one merged VMA if all segments fit in the
        //            same page range, otherwise fall back to per-segment
        //            VMAs (safe when segments are page-separated).
        {
            constexpr u16 MAX_LOADS = 8;
            u16 load_count = 0;

            // Collect PT_LOAD segments
            VirtAddr overall_start = ~0ULL;
            VirtAddr overall_end = 0;
            u64 file_offset_min = ~0ULL;
            u64 file_offset_max = 0;  // offset + filesz
            u32 merged_flags = 0;

            for (u16 i = 0; i < phnum && load_count < MAX_LOADS; ++i) {
                const auto &ph = phdrs[i];
                if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
                ++load_count;

                if (ph.p_vaddr < overall_start) overall_start = ph.p_vaddr;
                VirtAddr seg_end = ph.p_vaddr + ph.p_memsz;
                if (seg_end > overall_end) overall_end = seg_end;

                if (ph.p_filesz > 0) {
                    if (ph.p_offset < file_offset_min) file_offset_min = ph.p_offset;
                    u64 fo_end = ph.p_offset + ph.p_filesz;
                    if (fo_end > file_offset_max) file_offset_max = fo_end;
                }

                if (ph.p_flags & PF_R) merged_flags |= VmaFlags::READ;
                if (ph.p_flags & PF_W) merged_flags |= VmaFlags::WRITE;
                if (ph.p_flags & PF_X) merged_flags |= VmaFlags::EXEC;
            }

            // Page-align the overall range
            VirtAddr page_start = overall_start & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1);
            VirtAddr page_end = (overall_end + PAGE_SIZE - 1)
                                & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1);

            // Determine if all segments share the same page range
            // (common for small programs like hello.elf)
            bool same_page_range = (page_end - page_start) <= PAGE_SIZE
                                   || load_count <= 1;

            if (same_page_range && load_count > 0) {
                // Merged VMA: one VMA covering all PT_LOAD segments.
                // backing_offset accounts for the gap between page_start and
                // the first byte of file data in the ELF.
                VmaType vma_type = (merged_flags & VmaFlags::EXEC)
                    ? VmaType::CODE : VmaType::DATA;

                const u8 *backing = nullptr;
                usize backing_size = 0;
                u64 backing_offset = 0;
                if (file_offset_max > file_offset_min) {
                    backing = file_inode->data + file_offset_min;
                    backing_size = static_cast<usize>(file_offset_max - file_offset_min);
                    // backing_offset = how far into the page the data starts
                    backing_offset = overall_start - page_start;
                }

                if (backing_size == 0) merged_flags |= VmaFlags::DEMAND_ZERO;

                new_as->add_vma(page_start, page_end, merged_flags, vma_type,
                               backing, backing_offset, backing_size);

            } else {
                // Separate VMAs for page-separated segments (general case)
                for (u16 i = 0; i < phnum; ++i) {
                    const auto &ph = phdrs[i];
                    if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

                    u32 vma_flags = 0;
                    if (ph.p_flags & PF_R) vma_flags |= VmaFlags::READ;
                    if (ph.p_flags & PF_W) vma_flags |= VmaFlags::WRITE;
                    if (ph.p_flags & PF_X) vma_flags |= VmaFlags::EXEC;

                    VmaType vma_type = VmaType::DATA;
                    if ((ph.p_flags & PF_X) && !(ph.p_flags & PF_W))
                        vma_type = VmaType::CODE;

                    VirtAddr seg_start = ph.p_vaddr;
                    VirtAddr seg_end = (seg_start + ph.p_memsz + PAGE_SIZE - 1)
                                       & ~(static_cast<VirtAddr>(PAGE_SIZE) - 1);

                    const u8 *backing = (ph.p_filesz > 0)
                        ? (file_inode->data + ph.p_offset) : nullptr;
                    usize b_size = static_cast<usize>(ph.p_filesz);

                    if (ph.p_filesz == 0) vma_flags |= VmaFlags::DEMAND_ZERO;

                    new_as->add_vma(seg_start, seg_end, vma_flags, vma_type,
                                   backing, 0, b_size);

                    log::klog::info("  PT_LOAD: {:#x}-{:#x} filesz={} memsz={}",
                                   seg_start, seg_end,
                                   static_cast<u64>(ph.p_filesz),
                                   static_cast<u64>(ph.p_memsz));
                }
            }
        }

        // 9. Add stack VMA (demand-zero)
        constexpr VirtAddr STACK_BOTTOM = UserLayout::STACK_TOP - UserLayout::STACK_SIZE;
        new_as->add_vma(STACK_BOTTOM, UserLayout::STACK_TOP,
                       VmaFlags::READ | VmaFlags::WRITE | VmaFlags::DEMAND_ZERO,
                       VmaType::STACK);

        // 10. Add heap VMA (demand-zero)
        new_as->add_vma(UserLayout::HEAP_START,
                       UserLayout::HEAP_START + UserLayout::HEAP_INIT,
                       VmaFlags::READ | VmaFlags::WRITE | VmaFlags::DEMAND_ZERO,
                       VmaType::HEAP);

        // 11. Bind new address space to process
        auto set_result = proc->set_address_space(moss::move(new_as));
        if (!set_result) {
            log::klog::error("execve: set_address_space failed");
            cur->state = ProcessState::Terminated;
            if (g_scheduler) {
                g_scheduler->dequeue_task(cur);
                g_scheduler->schedule_after_exit();
            }
            while (true) { ::moss::kernel::arch::cpu_halt(); }
        }

        // 12. Reset thread context
        cur->context = CpuContext{};    // zero all registers
        cur->context.pc = elf_entry;
        cur->context.sp = UserLayout::STACK_TOP - 16;  // 16-byte aligned
        cur->context.pstate = 0;  // EL0t
        cur->needs_initial_eret = true;  // next dispatch does switch_to_user + eret
        cur->stack_base = STACK_BOTTOM;
        cur->stack_size = UserLayout::STACK_SIZE;

        // 13. Direct eret to new program image.
        //
        // execve is called from a syscall handler (EL1), so we can eret
        // directly to the new ELF entry point.  This is safe because:
        //   - We already rebuilt the address space and page tables
        //   - switch_to_user sets up ELR_EL1/SPSR_EL1/SP_EL0 and does eret
        //   - When the new program is later preempted by timer IRQ,
        //     irq_trampoline saves its state via context_switch, and
        //     bootstrap_contexts_[cpu] is already valid (saved by the
        //     user_eret_trampoline path that initially dispatched this task).
#if defined(MOSS_ARCH_ARM64)
        {
            cur->needs_initial_eret = false;
            cur->state = ProcessState::Running;

            arch::disable_interrupts();

            // Switch TTBR0 to new address space and invalidate TLB.
            // The old address space's mappings (e.g. 0x200000000) are cached
            // in the TLB.  Without invalidation, the CPU may use stale TLB
            // entries after the TTBR0 switch, causing spurious faults.
            if (proc->address_space() && proc->address_space()->pgd_phys != 0) {
                u64 ttbr0_val = proc->address_space()->pgd_phys
                              | (static_cast<u64>(proc->address_space()->asid) << 48);
                asm volatile("msr ttbr0_el1, %0" :: "r"(ttbr0_val));
                // Invalidate all TLB entries for this ASID
                asm volatile("tlbi aside1, %0" :: "r"(static_cast<u64>(proc->address_space()->asid) << 48));
                asm volatile("dsb sy" ::: "memory");
                asm volatile("isb" ::: "memory");
            }

            // Set TPIDR_EL1 for per-thread kernel stack
            if (cur->kernel_stack_base != 0) {
                u64 kstack_top = cur->kernel_stack_top();
                asm volatile("msr tpidr_el1, %0" :: "r"(kstack_top));
            }

            // eret to new program — never returns
            switch_to_user(&cur->context, cur->context.sp);
        }
#endif

        // Should not reach here (switch_to_user does eret)
        while (true) { ::moss::kernel::arch::cpu_halt(); }
    }

    // wait4(pid, wstatus, options, rusage) — wait for child process state change
    // pid > 0: wait for specific child
    // pid == -1: wait for any child
    // options: WNOHANG (1) = return immediately if no child has exited
    long sys_wait4(long wait_pid, long wstatus_addr, long options, long, long, long) noexcept {
        using namespace moss::kernel::process;

        constexpr long WNOHANG = 1;

        Thread *cur = CfsScheduler::get_current_task();
        if (!cur) return -Errno::EINVAL;

        Process *proc = g_process_manager
            ? g_process_manager->find_process(cur->owner_pid)
            : nullptr;
        if (!proc) return -Errno::EINVAL;

        // Must have children
        if (!proc->has_children()) {
            return -Errno::ECHILD;
        }

        while (true) {
            // Scan for matching zombie child
            ProcessId zombie_pid = proc->find_zombie_child(wait_pid);

            if (zombie_pid != INVALID_PROCESS_ID) {
                // Found a zombie — reap it
                Process *zombie = g_process_manager->find_process(zombie_pid);
                if (!zombie) {
                    // Race: already reaped by another thread, retry
                    continue;
                }

                i32 child_exit_code = zombie->exit_code();
                ProcessId result_pid = zombie->pid();

                // Remove from parent's children list
                proc->remove_child(zombie_pid);

                // Remove from process table and free Process object
                // terminate_process sets Terminated + removes from table + release()
                (void)g_process_manager->terminate_process(zombie_pid, child_exit_code);

                // Write status to user space if pointer is non-null
                // Linux WEXITSTATUS encoding: (exit_code & 0xFF) << 8
                if (wstatus_addr != 0) {
                    auto *wstatus_ptr = reinterpret_cast<int*>(
                        static_cast<unsigned long long>(wstatus_addr));
                    *wstatus_ptr = (static_cast<int>(child_exit_code) & 0xFF) << 8;
                }

                return static_cast<long>(result_pid);
            }

            // No zombie found
            // Check if specified PID is actually a child
            if (wait_pid > 0 && !proc->is_child(static_cast<ProcessId>(wait_pid))) {
                return -Errno::ECHILD;
            }

            // WNOHANG: non-blocking, return 0
            if (options & WNOHANG) {
                return 0;
            }

            // Block: add self to wait queue, set Blocked, dequeue from scheduler.
            // When a child calls sys_exit, it wakes all waiters on parent's WQ,
            // setting them back to Ready and re-enqueueing them.  The thread
            // then resumes here (after being re-dispatched by scheduler_tick's
            // context_switch) and loops back to rescan for zombies.
            proc->child_exit_wait_queue().add_waiter(static_cast<void*>(cur));
            cur->state = ProcessState::Blocked;
            if (g_scheduler) {
                g_scheduler->dequeue_task(cur);
            }

            // Yield CPU: switch to bootstrap context, let scheduler pick next task.
            // When this thread is woken (state=Ready, re-enqueued), scheduler_tick
            // will context_switch back and we resume after this point.
#if defined(MOSS_ARCH_ARM64)
            {
                u32 cpu = arch::get_current_cpu_id();
                CpuContext *my_ctx = &cur->context;
                CpuContext *bootstrap = &CfsScheduler::bootstrap_context(cpu);

                CfsScheduler::set_current_task(nullptr);
                arch::disable_interrupts();
                context_switch(my_ctx, bootstrap);
                arch::enable_interrupts();
            }
#endif
            // Resumed — remove self from wait queue and rescan
            proc->child_exit_wait_queue().remove_waiter(static_cast<void*>(cur));

            // Check we still have children (might have been reaped by another thread)
            if (!proc->has_children()) {
                return -Errno::ECHILD;
            }
        }
    }

    // waitpid(pid, wstatus, options) — thin wrapper over wait4
    long sys_waitpid(long pid, long wstatus, long options, long, long, long) noexcept {
        return sys_wait4(pid, wstatus, options, 0, 0, 0);
    }

    long sys_kill(long, long, long, long, long, long) noexcept {
        log::klog::warn("syscall: kill() not implemented");
        return -Errno::ENOSYS;
    }

    // ── VFS-backed file system calls ─────────────────────────────────

    /// Helper: get the calling process's VFS fd_table (void*).
    static void* get_current_fd_table() noexcept {
        using namespace moss::kernel::process;
        Thread *cur = CfsScheduler::get_current_task();
        if (!cur) return nullptr;
        Process *proc = g_process_manager
            ? g_process_manager->find_process(cur->owner_pid)
            : nullptr;
        return proc ? proc->fd_table() : nullptr;
    }

    long sys_open(long pathname_addr, long flags, long mode, long, long, long) noexcept {
        void* fdt = get_current_fd_table();
        if (!fdt) return -Errno::EBADF;

        const char* path = reinterpret_cast<const char*>(
            static_cast<unsigned long long>(pathname_addr));
        if (!path) return -Errno::EFAULT;

        return moss::kernel::vfs::syscall::do_open(
            fdt, path, static_cast<u32>(flags), static_cast<u32>(mode));
    }

    long sys_close(long fd, long, long, long, long, long) noexcept {
        void* fdt = get_current_fd_table();
        if (!fdt) return -Errno::EBADF;

        return moss::kernel::vfs::syscall::do_close(fdt, static_cast<int>(fd));
    }

    long sys_read(long fd, long buf_addr, long count, long, long, long) noexcept {
        void* fdt = get_current_fd_table();
        if (!fdt) return -Errno::EBADF;

        if (buf_addr == 0 || count <= 0) return -Errno::EINVAL;

        auto* buf = reinterpret_cast<u8*>(
            static_cast<unsigned long long>(buf_addr));

        return moss::kernel::vfs::syscall::do_read(
            fdt, static_cast<int>(fd), buf, static_cast<usize>(count));
    }

    long sys_write(long fd, long buf_addr, long count, long, long, long) noexcept {
        void* fdt = get_current_fd_table();
        if (!fdt) return -Errno::EBADF;

        if (buf_addr == 0 || count <= 0) return -Errno::EINVAL;

        const auto* buf = reinterpret_cast<const u8*>(
            static_cast<unsigned long long>(buf_addr));

        return moss::kernel::vfs::syscall::do_write(
            fdt, static_cast<int>(fd), buf, static_cast<usize>(count));
    }

    // ── Additional VFS syscalls (dup, dup2, pipe, lseek, fstat) ────

    long sys_lseek(long fd, long offset, long whence, long, long, long) noexcept {
        void* fdt = get_current_fd_table();
        if (!fdt) return -Errno::EBADF;
        return moss::kernel::vfs::syscall::do_lseek(
            fdt, fd, static_cast<i64>(offset), static_cast<u32>(whence));
    }

    long sys_fstat(long fd, long stat_buf_addr, long, long, long, long) noexcept {
        void* fdt = get_current_fd_table();
        if (!fdt) return -Errno::EBADF;
        if (stat_buf_addr == 0) return -Errno::EFAULT;
        auto* stat_buf = reinterpret_cast<void*>(
            static_cast<unsigned long long>(stat_buf_addr));
        return moss::kernel::vfs::syscall::do_fstat(fdt, fd, stat_buf);
    }

    long sys_dup(long oldfd, long, long, long, long, long) noexcept {
        void* fdt = get_current_fd_table();
        if (!fdt) return -Errno::EBADF;
        return moss::kernel::vfs::syscall::do_dup(fdt, oldfd);
    }

    long sys_dup2(long oldfd, long newfd, long, long, long, long) noexcept {
        void* fdt = get_current_fd_table();
        if (!fdt) return -Errno::EBADF;
        return moss::kernel::vfs::syscall::do_dup2(fdt, oldfd, newfd);
    }

    long sys_pipe(long pipefd_addr, long, long, long, long, long) noexcept {
        void* fdt = get_current_fd_table();
        if (!fdt) return -Errno::EBADF;
        if (pipefd_addr == 0) return -Errno::EFAULT;
        auto* pipefd = reinterpret_cast<long*>(
            static_cast<unsigned long long>(pipefd_addr));
        return moss::kernel::vfs::syscall::do_pipe(fdt, pipefd);
    }

    // 内存管理系统调用
    long sys_mmap(long, long, long, long, long, long) noexcept {
        log::klog::warn("syscall: mmap() not implemented");
        return -Errno::ENOSYS;
    }

    long sys_munmap(long, long, long, long, long, long) noexcept {
        log::klog::warn("syscall: munmap() not implemented");
        return -Errno::ENOSYS;
    }

    long sys_mprotect(long, long, long, long, long, long) noexcept {
        log::klog::warn("syscall: mprotect() not implemented");
        return -Errno::ENOSYS;
    }

    long sys_brk(long, long, long, long, long, long) noexcept {
        log::klog::warn("syscall: brk() not implemented");
        return -Errno::ENOSYS;
    }

    // 网络通信系统调用 - 框架实现
    long sys_socket(long, long, long, long, long, long) noexcept {
        log::klog::warn("syscall: socket() not implemented");
        return -Errno::ENOSYS;
    }

    long sys_bind(long, long, long, long, long, long) noexcept {
        log::klog::warn("syscall: bind() not implemented");
        return -Errno::ENOSYS;
    }

    long sys_listen(long, long, long, long, long, long) noexcept {
        log::klog::warn("syscall: listen() not implemented");
        return -Errno::ENOSYS;
    }

    long sys_accept(long, long, long, long, long, long) noexcept {
        log::klog::warn("syscall: accept() not implemented");
        return -Errno::ENOSYS;
    }

    // 未实现系统调用的默认处理器
    long sys_not_implemented(long, long, long, long, long, long) noexcept {
        log::klog::warn("syscall: unknown/unimplemented");
        return -Errno::ENOSYS;
    }
}

// 全局系统调用表定义
const SyscallDescriptor SYSCALL_TABLE[static_cast<int>(SyscallNumber::MAX_SYSCALL)] = {
    // === 基础系统调用 (0-9) ===
    {"debug_print", handlers::sys_debug_print, 1, true, "调试输出"},
    {"exit", handlers::sys_exit, 1, true, "进程退出"},
    {"getpid", handlers::sys_getpid, 0, true, "获取进程ID"},
    {"getppid", handlers::sys_getppid, 0, true, "获取父进程ID"},
    {"getuid", handlers::sys_getuid, 0, true, "获取用户ID"},
    {"getgid", handlers::sys_getgid, 0, true, "获取组ID"},
    {"geteuid", handlers::sys_not_implemented, 0, false, "获取有效用户ID"},
    {"getegid", handlers::sys_not_implemented, 0, false, "获取有效组ID"},
    {"setsid", handlers::sys_not_implemented, 0, false, "设置会话ID"},
    {"getpgid", handlers::sys_not_implemented, 1, false, "获取进程组ID"},

    // === 进程管理 (10-29) ===
    {"fork", handlers::sys_fork, 0, true, "创建子进程"},
    {"execve", handlers::sys_execve, 3, true, "执行程序"},
    {"wait4", handlers::sys_wait4, 4, true, "等待子进程"},
    {"waitpid", handlers::sys_waitpid, 3, true, "等待指定进程"},
    {"kill", handlers::sys_kill, 2, false, "发送信号"},
    {"sigaction", handlers::sys_not_implemented, 3, false, "信号处理设置"},
    {"sigprocmask", handlers::sys_not_implemented, 3, false, "信号掩码操作"},
    {"sigreturn", handlers::sys_not_implemented, 0, false, "信号返回"},
    {"pause", handlers::sys_not_implemented, 0, false, "等待信号"},
    {"alarm", handlers::sys_not_implemented, 1, false, "设置闹钟"},
    {"setpgid", handlers::sys_not_implemented, 2, false, "设置进程组"},
    {"setuid", handlers::sys_not_implemented, 1, false, "设置用户ID"},
    {"setgid", handlers::sys_not_implemented, 1, false, "设置组ID"},
    {"seteuid", handlers::sys_not_implemented, 1, false, "设置有效用户ID"},
    {"setegid", handlers::sys_not_implemented, 1, false, "设置有效组ID"},
    {"getpgrp", handlers::sys_not_implemented, 0, false, "获取进程组"},
    {"setpgrp", handlers::sys_not_implemented, 0, false, "设置进程组"},
    {"getsid", handlers::sys_not_implemented, 1, false, "获取会话ID"},
    {"nice", handlers::sys_not_implemented, 1, false, "设置进程优先级"},
    {"getpriority", handlers::sys_not_implemented, 2, false, "获取进程优先级"},

    // === 文件系统操作 (30-59) ===
    {"open", handlers::sys_open, 3, true, "打开文件"},
    {"close", handlers::sys_close, 1, true, "关闭文件"},
    {"read", handlers::sys_read, 3, true, "读取文件"},
    {"write", handlers::sys_write, 3, true, "写入文件"},
    {"lseek", handlers::sys_lseek, 3, true, "文件定位"},
    {"stat", handlers::sys_not_implemented, 2, false, "获取文件状态"},
    {"fstat", handlers::sys_fstat, 2, true, "获取文件描述符状态"},
    {"lstat", handlers::sys_not_implemented, 2, false, "获取链接文件状态"},
    {"access", handlers::sys_not_implemented, 2, false, "检查文件权限"},
    {"chmod", handlers::sys_not_implemented, 2, false, "修改文件权限"},
    {"chown", handlers::sys_not_implemented, 3, false, "修改文件所有者"},
    {"umask", handlers::sys_not_implemented, 1, false, "设置文件创建掩码"},
    {"dup", handlers::sys_dup, 1, true, "复制文件描述符"},
    {"dup2", handlers::sys_dup2, 2, true, "复制文件描述符到指定位置"},
    {"pipe", handlers::sys_pipe, 1, true, "创建管道"},
    {"mkdir", handlers::sys_not_implemented, 2, false, "创建目录"},
    {"rmdir", handlers::sys_not_implemented, 1, false, "删除目录"},
    {"link", handlers::sys_not_implemented, 2, false, "创建硬链接"},
    {"unlink", handlers::sys_not_implemented, 1, false, "删除文件"},
    {"symlink", handlers::sys_not_implemented, 2, false, "创建符号链接"},
    {"readlink", handlers::sys_not_implemented, 3, false, "读取符号链接"},
    {"chdir", handlers::sys_not_implemented, 1, false, "改变工作目录"},
    {"getcwd", handlers::sys_not_implemented, 2, false, "获取当前目录"},
    {"rename", handlers::sys_not_implemented, 2, false, "重命名文件"},
    {"truncate", handlers::sys_not_implemented, 2, false, "截断文件"},
    {"ftruncate", handlers::sys_not_implemented, 2, false, "截断文件(通过fd)"},
    {"fsync", handlers::sys_not_implemented, 1, false, "同步文件"},
    {"fdatasync", handlers::sys_not_implemented, 1, false, "同步文件数据"},
    {"sync", handlers::sys_not_implemented, 0, false, "同步所有文件"},
    {"mount", handlers::sys_not_implemented, 5, false, "挂载文件系统"},

    // === 内存管理 (60-79) ===
    {"mmap", handlers::sys_mmap, 6, false, "内存映射"},
    {"munmap", handlers::sys_munmap, 2, false, "取消内存映射"},
    {"mprotect", handlers::sys_mprotect, 3, false, "修改内存保护"},
    {"mlock", handlers::sys_not_implemented, 2, false, "锁定内存页"},
    {"munlock", handlers::sys_not_implemented, 2, false, "解锁内存页"},
    {"mlockall", handlers::sys_not_implemented, 1, false, "锁定所有内存页"},
    {"munlockall", handlers::sys_not_implemented, 0, false, "解锁所有内存页"},
    {"madvise", handlers::sys_not_implemented, 3, false, "内存使用建议"},
    {"msync", handlers::sys_not_implemented, 3, false, "同步内存映射"},
    {"brk", handlers::sys_brk, 1, false, "设置数据段大小"},
    {"sbrk", handlers::sys_not_implemented, 1, false, "调整数据段大小"},
    {"mremap", handlers::sys_not_implemented, 5, false, "重新映射内存"},
    {"mincore", handlers::sys_not_implemented, 3, false, "检查页面是否在内存中"},
    {"mmap2", handlers::sys_not_implemented, 6, false, "内存映射(扩展版)"},
    {"remap_file_pages", handlers::sys_not_implemented, 5, false, "重新映射文件页"},
    {"mbind", handlers::sys_not_implemented, 6, false, "NUMA内存绑定"},
    {"get_mempolicy", handlers::sys_not_implemented, 5, false, "获取内存策略"},
    {"set_mempolicy", handlers::sys_not_implemented, 3, false, "设置内存策略"},
    {"migrate_pages", handlers::sys_not_implemented, 4, false, "迁移内存页"},
    {"move_pages", handlers::sys_not_implemented, 6, false, "移动内存页"},

    // === 时间和定时器 (80-89) ===
    {"time", handlers::sys_not_implemented, 1, false, "获取时间"},
    {"gettimeofday", handlers::sys_not_implemented, 2, false, "获取时间(微秒精度)"},
    {"settimeofday", handlers::sys_not_implemented, 2, false, "设置时间"},
    {"clock_gettime", handlers::sys_not_implemented, 2, false, "获取时钟时间"},
    {"clock_settime", handlers::sys_not_implemented, 2, false, "设置时钟时间"},
    {"clock_getres", handlers::sys_not_implemented, 2, false, "获取时钟分辨率"},
    {"nanosleep", handlers::sys_not_implemented, 2, false, "纳秒级睡眠"},
    {"timer_create", handlers::sys_not_implemented, 3, false, "创建定时器"},
    {"timer_settime", handlers::sys_not_implemented, 4, false, "设置定时器"},
    {"timer_gettime", handlers::sys_not_implemented, 2, false, "获取定时器状态"},

    // === 网络通信 (90-109) ===
    {"socket", handlers::sys_socket, 3, false, "创建socket"},
    {"bind", handlers::sys_bind, 3, false, "绑定地址"},
    {"listen", handlers::sys_listen, 2, false, "监听连接"},
    {"accept", handlers::sys_accept, 3, false, "接受连接"},
    {"connect", handlers::sys_not_implemented, 3, false, "建立连接"},
    {"send", handlers::sys_not_implemented, 4, false, "发送数据"},
    {"recv", handlers::sys_not_implemented, 4, false, "接收数据"},
    {"sendto", handlers::sys_not_implemented, 6, false, "发送数据到指定地址"},
    {"recvfrom", handlers::sys_not_implemented, 6, false, "从指定地址接收数据"},
    {"shutdown", handlers::sys_not_implemented, 2, false, "关闭socket"},
    {"setsockopt", handlers::sys_not_implemented, 5, false, "设置socket选项"},
    {"getsockopt", handlers::sys_not_implemented, 5, false, "获取socket选项"},
    {"getsockname", handlers::sys_not_implemented, 3, false, "获取socket名称"},
    {"getpeername", handlers::sys_not_implemented, 3, false, "获取对端名称"},
    {"socketpair", handlers::sys_not_implemented, 4, false, "创建socket对"},
    {"sendmsg", handlers::sys_not_implemented, 3, false, "发送消息"},
    {"recvmsg", handlers::sys_not_implemented, 3, false, "接收消息"},
    {"select", handlers::sys_not_implemented, 5, false, "I/O多路复用"},
    {"poll", handlers::sys_not_implemented, 3, false, "轮询I/O事件"},
    {"epoll_create", handlers::sys_not_implemented, 1, false, "创建epoll实例"},

    // === 系统信息和控制 (110-129) ===
    {"uname", handlers::sys_not_implemented, 1, false, "获取系统信息"},
    {"sysinfo", handlers::sys_not_implemented, 1, false, "获取系统统计信息"},
    {"getrlimit", handlers::sys_not_implemented, 2, false, "获取资源限制"},
    {"setrlimit", handlers::sys_not_implemented, 2, false, "设置资源限制"},
    {"getrusage", handlers::sys_not_implemented, 2, false, "获取资源使用情况"},
    {"times", handlers::sys_not_implemented, 1, false, "获取进程时间"},
    {"ptrace", handlers::sys_not_implemented, 4, false, "进程跟踪"},
    {"syslog", handlers::sys_not_implemented, 3, false, "系统日志"},
    {"reboot", handlers::sys_not_implemented, 4, false, "系统重启"},
    {"sethostname", handlers::sys_not_implemented, 2, false, "设置主机名"},
    {"gethostname", handlers::sys_not_implemented, 2, false, "获取主机名"},
    {"setdomainname", handlers::sys_not_implemented, 2, false, "设置域名"},
    {"getdomainname", handlers::sys_not_implemented, 2, false, "获取域名"},
    {"iopl", handlers::sys_not_implemented, 1, false, "I/O权限级别"},
    {"ioperm", handlers::sys_not_implemented, 3, false, "I/O端口权限"},
    {"sysctl", handlers::sys_not_implemented, 1, false, "系统控制"},
    {"arch_prctl", handlers::sys_not_implemented, 2, false, "架构特定控制"},
    {"prctl", handlers::sys_not_implemented, 5, false, "进程控制"},
    {"capget", handlers::sys_not_implemented, 2, false, "获取能力"},
    {"capset", handlers::sys_not_implemented, 2, false, "设置能力"}
};

// 系统调用分发器实现
long SyscallDispatcher::dispatch(long syscall_number, long arg0, long arg1,
                                long arg2, long arg3, long arg4, long arg5) noexcept {
    // 更新统计信息
    ++g_syscall_stats.total_syscalls;

    // 检查系统调用号有效性
    if (!is_valid_syscall(syscall_number)) {
        ++g_syscall_stats.invalid_syscalls;
        return -Errno::EINVAL;
    }

    const SyscallDescriptor* desc = &SYSCALL_TABLE[syscall_number];

    // 检查是否已实现
    if (!desc->implemented) {
        ++g_syscall_stats.unimplemented_syscalls;
        return desc->handler(arg0, arg1, arg2, arg3, arg4, arg5);
    }

    // 调用系统调用处理函数
    long result = desc->handler(arg0, arg1, arg2, arg3, arg4, arg5);

    // 更新统计信息
    if (result >= 0) {
        ++g_syscall_stats.successful_syscalls;
    } else {
        ++g_syscall_stats.failed_syscalls;
    }

    return result;
}

const SyscallDescriptor* SyscallDispatcher::get_syscall_info(long syscall_number) noexcept {
    if (!is_valid_syscall(syscall_number)) {
        return nullptr;
    }
    return &SYSCALL_TABLE[syscall_number];
}

bool SyscallDispatcher::is_implemented(long syscall_number) noexcept {
    if (!is_valid_syscall(syscall_number)) {
        return false;
    }
    return SYSCALL_TABLE[syscall_number].implemented;
}

bool SyscallDispatcher::is_valid_syscall(long syscall_number) noexcept {
    return syscall_number >= 0 &&
           syscall_number < static_cast<long>(SyscallNumber::MAX_SYSCALL);
}

void SyscallDispatcher::get_syscall_stats(u64* total_calls, u64* implemented_calls) noexcept {
    if (total_calls) {
        *total_calls = g_syscall_stats.total_syscalls;
    }

    if (implemented_calls) {
        u64 count = 0;
        for (int i = 0; i < static_cast<int>(SyscallNumber::MAX_SYSCALL); ++i) {
            if (SYSCALL_TABLE[i].implemented) {
                ++count;
            }
        }
        *implemented_calls = count;
    }
}

void SyscallDispatcher::print_implemented_syscalls() noexcept {
    namespace log = moss::kernel::logging;

    log::klog::info("=== implemented syscalls ===");
    for (int i = 0; i < static_cast<int>(SyscallNumber::MAX_SYSCALL); ++i) {
        const auto& desc = SYSCALL_TABLE[i];
        if (desc.implemented) {
            log::klog::info("  [{}] {}", i, desc.name);
        }
    }
    log::klog::info("============================");
}

} // namespace moss::kernel::syscall
