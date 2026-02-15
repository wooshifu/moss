# MOSS Kernel - Feature Tracking

## Architecture Overview

MOSS is a C++26 freestanding hybrid kernel targeting QEMU virtual machines.
Primary architecture: ARM64 (aarch64-unknown-elf), with x86_64 and RISC-V stubs.
Build system: CMake + Clang C++26 modules, 6 presets (3 arch x debug/release).

---

## Completed Features

### Boot & Initialization

- [x] **ARM64 full boot sequence** — EL3→EL2→EL1 transition, BSS clear, stack setup (256KB), FPU/NEON enable, exception vector table installation
  - `src/boot/src/arch/arm64/start_arm64.S`
- [x] **SMP boot (ARM64)** — CPU topology detection via DTB, PSCI `CPU_ON` for secondary CPUs, per-CPU stack allocation (32KB each), secondary CPU parking/activation protocol, supports up to 8 CPUs
  - `src/boot/src/arch/arm64/boot_impl.cpp`
- [x] **Device Tree (FDT) parsing** — libfdt-based parser, discovers UART base, GIC distributor/CPU base, memory regions, CPU count, timer frequency at runtime; dual DTB discovery (bootloader pointer + RAM scan)
  - `src/fdt/src/fdt.cppm`
- [x] **x86_64 boot stub** — Multiboot2 header, BSS clear, stack setup, jump to `early_main`, UART init, then halts
  - `src/boot/src/arch/x86_64/start_x86_64.S`, `src/boot/src/arch/x86_64/boot_impl.cpp`
- [x] **RISC-V boot stub** — BSS clear, stack setup, jump to `early_main`, UART init, then halts
  - `src/boot/src/arch/riscv/start_riscv.S`, `src/boot/src/arch/riscv/boot_impl.cpp`

### Hardware Abstraction Layer (HAL)

- [x] **UART HAL** — 3-architecture support:
  - ARM64: PL011 at 0x09000000, flag register polling
  - x86_64: COM1 8250/16550 at I/O port 0x3F8
  - RISC-V: NS16550 at 0x10000000
  - API: `init()`, `putc()`, `getc()`, `puts()`, `is_readable()`, `is_writable()`
  - `src/hal/uart/src/uart_hal.cppm`
- [x] **Timer HAL (ARM64)** — Generic Timer: `cntvct_el0` counter read, `cntfrq_el0` frequency, `cntv_cval_el0` compare, `cntv_ctl_el0` control, `ack_interrupt()`, `set_compare()`, `enable()`/`disable()`
  - `src/hal/timer/src/timer_hal.cppm`
- [x] **Interrupt Controller HAL (ARM64)** — GICv2: distributor register offsets (GICD\_\*), CPU interface offsets (GICC\_\*), `init_distributor()`, `init_cpu_interface()`, `enable_irq()`, `disable_irq()`, `ack_irq()`, `eoi()`, `send_sgi()`
  - `src/hal/intc/src/intc_hal.cppm`
- [x] **MMU HAL** — 3-architecture PTE bit definitions:
  - ARM64: 4-level (L0-L3), 48-bit VA, `PageAttr::VALID|TABLE|AF|SH_INNER|NORMAL_MEMORY`, `PagePerms::KERNEL_RWX|USER_RWX|READ_ONLY|NO_EXEC`
  - x86_64: 4-level (PML4→PDPT→PD→PT), x86 PTE bits (PRESENT, WRITABLE, USER)
  - RISC-V: Sv48 4-level, PTE bits (V, R, W, X, U, G, A, D)
  - API: `VirtualAddressBreakdown`, `break_virtual_address()`, `AddressSpaceConfig`
  - `src/hal/mmu/src/mmu_hal.cppm`
- [x] **Platform defaults** — Compile-time hardware constants per platform (UART base, GIC base, RAM base/size, timer frequency) for ARM64-QEMU-virt, x86_64-QEMU, RISC-V-QEMU-virt
  - `src/platform/src/platform.cppm`

### Memory Management

- [x] **Buddy page frame allocator** — Orders 0-10 (4KB→4MB), free list per order, split on alloc, coalesce on free, watermark tracking (min/low/high), max pages 131072 (512MB), bitmap tracking for buddy coalescing
  - `src/mm/src/mm.cppm` (`PageFrameAllocator`), `src/mm/src/page_frame_allocator.cpp`
- [x] **Runtime kernel heap** — `operator new` / `operator delete` backed by buddy allocator, block header with size tracking, supports arbitrary allocation sizes
  - `src/kernel/src/runtime_support.cpp`
- [x] **Slab allocator** — `SlabCache` with partial/full/empty page lists, CAS lock-free allocation, `SlabAllocator` with multiple size classes, backed by buddy allocator via C shim (`moss_slab_alloc_pages`/`moss_slab_free_pages`)
  - `src/containers/src/containers.cppm` (lines 1292-1806), `src/mm/src/page_alloc_shim.cpp`
- [x] **4-level page tables (ARM64)** — PGD→PUD→PMD→PTE, 4KB granule:
  - Early boot: 64 statically-allocated page tables in BSS
  - Post-boot: Dynamic allocation from buddy allocator (`allocate_page_table_dynamic()`)
  - `setup_kernel_page_tables()`: identity map 4×1GB blocks (PUD[0]=Device, PUD[1]=Normal RAM, PUD[2-3]=Device)
  - `setup_kernel_high_half_tables()`: TTBR1 high-half kernel mapping
  - `src/mm/src/mm.cppm` (lines 557-658), `src/mm/src/page_table.cpp`
- [x] **MMU enable** — Full MMU setup: MAIR (AttrIndx 0=Device-nGnRnE, 1=Normal WB), TCR_EL1 (T0SZ=16, T1SZ=16, 4KB granule, inner/outer shareable WB-WA), TTBR0 + TTBR1 write, TLB flush, SCTLR_EL1 M-bit set
  - `src/mm/src/page_table.cpp`
- [x] **Per-process page tables** — PGD dynamically allocated from buddy, kernel PGD[0] copied into user PGD for shared identity mapping, ASID allocation (8-bit, wraps with global TLB flush at 256)
  - `src/process/src/process.cpp` (`create_user_address_space()`)
- [x] **TTBR0 switching** — Complete lifecycle:
  - First eret: set TTBR0 to process PGD in `needs_initial_eret` path (with IRQ disabled)
  - Re-dispatch after preemption: set TTBR0 in `context_switch_to_task()` else branch for `is_user_task` threads
  - Process exit: restore TTBR0 to kernel PGD in `sys_exit` and `terminate_current_user_process`
  - `src/process/src/process.cppm` (`context_switch_to_task`), `src/kernel/src/syscall_table.cpp`, `src/kernel/src/kernel_main.cpp`
- [x] **Demand paging** — Page fault handler for EL0 translation faults (DFSC 0x04-0x07):
  - VMA lookup via `demand_page_lookup()` bridge function (mm↔process module boundary)
  - Physical page allocation from buddy allocator (order-0)
  - Page content: copy from ELF backing data + zero-fill remainder, or full zero page
  - PTE construction: AP[1]=1 (EL0 access), AF=1, nG=1, SH=Inner Shareable, PXN=1, AttrIndx=1 (Normal), AP[2] for read-only, UXN for non-exec
  - TLB invalidate on faulting address after map
  - `src/mm/src/page_fault.cpp` (`try_demand_page`, `user_page_fault_handler`)
- [x] **map_user_page()** — 4-level walk on user PGD, allocates intermediate tables dynamically, sets L3 page descriptor (bits[1:0]=0b11)
  - `src/mm/src/page_table.cpp`
- [x] **free_user_page_tables()** — Recursive 4-level walk: frees leaf physical pages (demand-paged), then PTE/PMD/PUD table pages, then PGD page; skips PGD[0] (shared kernel identity map)
  - `src/mm/src/page_table.cpp`
- [x] **VMA management** — `AddressSpace` struct with 16 VMA slots, `add_vma()` with overlap check, `find_vma()` for fault lookup, VMA types: CODE/DATA/BSS/STACK/HEAP, VMA flags: READ/WRITE/EXEC
  - `src/process/src/process.cppm` (`AddressSpace`, `VmaRegion`)
- [x] **Page fault diagnostics** — EC-to-string, DFSC/IFSC-to-string, kernel fault handler with TLB invalidate retry for stale entries, permission fault logging
  - `src/mm/src/page_fault.cpp`
- [x] **Memory statistics** — `MemoryStats` class tracking allocation counts, free pages, watermarks, memory pressure levels (LOW/MEDIUM/HIGH/CRITICAL), `is_memory_system_healthy()`, `get_memory_pressure()`
  - `src/mm/src/mm.cppm`

### Process Management & Scheduling

- [x] **CFS scheduler** — Full Linux-style Completely Fair Scheduler:
  - Red-black tree runqueue per CPU (`CfsRunqueue`), pool-based node allocation (MAX_NODES=1024/CPU), free list recycling
  - Virtual runtime (vruntime) tracking, weighted fair scheduling using 40-entry nice-to-weight table (nice -20 to +19)
  - `rb_leftmost_` cache for O(1) `pick_next_task()`
  - Parameters: `SCHED_LATENCY_NS=6ms`, `SCHED_MIN_GRANULARITY_NS=0.75ms`, `SCHED_WAKEUP_GRANULARITY_NS=1ms`
  - `src/process/src/process.cppm` (lines 700-1185)
- [x] **Timer-driven preemption** — Timer PPI IRQ 27 fires periodically → `irq_handler_c()` reprograms next compare → CPU 0 dispatches via `TimerSubsystem::handle_interrupt()` → HrTimer callback → `scheduler_tick()`; secondary CPUs call `scheduler_tick()` directly
  - `src/kernel/src/kernel_main.cpp` (lines 266-293)
- [x] **scheduler_tick()** — Updates vruntime of current task, checks `should_preempt_current()` (vruntime comparison vs leftmost in RB tree), resets time-slice accounting (`prev_sum_exec_runtime`), records preemption stats, context-switches to next task
  - `src/process/src/process.cppm` (lines 1664-1706)
- [x] **context_switch (ARM64)** — Assembly: saves all 31 GP regs + SP + LR + DAIF + FPSR/FPCR + 32 NEON Q-regs + TPIDR_EL0; restores everything except DAIF (left to eret); uses `ret` to saved LR
  - `src/process/src/context_switch.S`
- [x] **switch_to_user (ARM64)** — Assembly: sets SPSR_EL1=0 (EL0t), SP_EL0, ELR_EL1 from context.pc, restores all GP/NEON/TPIDR, `eret` to user mode
  - `src/process/src/context_switch.S`
- [x] **Per-CPU bootstrap contexts** — Throwaway `CpuContext` per CPU for `context_switch()` when no previous task exists (e.g. `schedule_after_exit`, first dispatch)
  - `src/process/src/process.cppm` (`bootstrap_contexts_[]`), `src/process/src/process.cpp`
- [x] **Load balancer** — `LoadBalancer` class with idle balancing, periodic rebalancing, task migration between CPUs, CPU affinity support, configurable policies (Conservative/Aggressive/NUMA-Aware), per-CPU imbalance calculation, `try_idle_balance()` for work stealing
  - `src/process/src/process.cppm` (lines 2055-2260+)
- [x] **Reschedule IPI** — SGI 0 as reschedule IPI, `send_reschedule_ipi()` for cross-CPU task migration notification, handled in `irq_handler_c` with early return after EOI
  - `src/kernel/src/kernel_main.cpp` (lines 257-259)
- [x] **ProcessManager** — Process creation with PID allocation (atomic counter), process termination (mark + remove from table + release refcount), process table via `RcuHashMap`, `find_process()`, `process_exists()`
  - `src/process/src/process.cpp`
- [x] **Process destructor cleanup** — `~Process()` calls `cleanup_threads()` (delete all thread objects) + `free_user_page_tables()` (recursive page table/physical page release)
  - `src/process/src/process.cppm`
- [x] **Idle tasks** — `IdleTask` class with per-CPU idle process, `cpu_idle_once()` using WFI (ARM64) / HLT (x86_64) / WFI (RISC-V)
  - `src/process/src/idle_process.cpp`
- [x] **Secondary CPU scheduling loop** — `secondary_cpu_schedule_loop()`: per-CPU pick_next_task + context_switch loop, timer-driven via `irq_handler_c` recognizing per-CPU timer PPI, idle balance + WFI when no tasks
  - `src/process/src/process.cpp`

### ELF Loader & User-Space Execution

- [x] **ELF loader** — Validates ELF64 header (magic, class, endianness, type ET_EXEC), checks architecture compatibility (EM_AARCH64/EM_X86_64/EM_RISCV), parses PT_LOAD program headers, registers VMA regions with correct types (CODE/DATA/BSS) and permissions (R/W/X), stores backing data pointer for demand paging
  - `src/kernel/src/elf_loader.cpp`, `src/kernel/src/kernel.cppm`
- [x] **Embedded user program** — `hello.elf` compiled from `userspace/hello.c`, embedded via `.incbin` in `arm64_user_program.S`, linked with `userspace/userspace.ld` (base 0x400000)
  - `src/kernel/src/arch/arm64_user_program.S`, `userspace/hello.c`, `userspace/userspace.ld`
- [x] **Init process creation** — Allocates per-process address space (PGD + ASID), creates process + thread, sets entry point from ELF, registers stack/heap VMAs, sets `needs_initial_eret=true` + `is_user_task=true`, enqueues into CFS scheduler
  - `src/kernel/src/kernel.cppm` (lines ~1140-1220)
- [x] **User-space VMA layout** — CODE (0x400000, RX), DATA (after code, RW), BSS (after data, RW), STACK (0xFFFF0000-0x10000, 64KB, RW), HEAP (0x800000, 1MB, RW)
  - `src/kernel/src/kernel.cppm`

### Interrupt & Exception Handling

- [x] **GIC driver** — `GenericInterruptController` class: distributor init (group 0/1, target routing, priority), CPU interface init (priority mask, binary point), per-IRQ enable/disable, priority config, handler registration (function pointer + context), IPI via SGI
  - `src/interrupts/src/interrupts.cppm`
- [x] **IRQ trampoline (ARM64)** — 34-slot frame (x0-x30 + ELR_EL1 + SPSR_EL1 + pad = 272 bytes), full save, call `irq_handler_c`, full restore, `eret`; same frame used for both same-EL and lower-EL IRQs
  - `src/boot/src/arch/arm64/start_arm64.S` (lines 422-476)
- [x] **Exception vector table (ARM64)** — 4×4 vector table at `.balign 2048`: Current EL SP0 (sync/irq/fiq/serror), Current EL SPx (sync→exception_handler, irq→irq_trampoline), Lower EL AArch64 (sync→lower_el_sync_dispatch, irq→irq_trampoline), Lower EL AArch32
  - `src/boot/src/arch/arm64/start_arm64.S` (lines 373-417)
- [x] **Lower EL sync dispatch** — Routes by EC: EC=0x15→SVC handler (syscall), EC=0x24/0x20→user page fault handler, other→unhandled user exception handler; full register save/restore + eret
  - `src/boot/src/arch/arm64/start_arm64.S` (lines 578-678)
- [x] **EOI ordering** — GIC EOI sent BEFORE timer dispatch in `irq_handler_c`, ensuring GIC is ready for next interrupt even if `context_switch` suspends handler mid-execution
  - `src/kernel/src/kernel_main.cpp` (line 250)
- [x] **User fault termination** — `kill_user_process()` and `unhandled_user_exception_handler()` call `terminate_current_user_process()` bridge function → mark Terminated → dequeue → restore kernel TTBR0 → `schedule_after_exit()`, instead of WFI halt
  - `src/mm/src/page_fault.cpp`, `src/kernel/src/kernel_main.cpp`

### System Calls

- [x] **SVC dispatch (ARM64)** — `lower_el_sync_dispatch` in start_arm64.S extracts x8 as syscall number, x0-x5 as args, calls `system_call_handler()` in C, stores return value in saved x0 slot, eret back to EL0
  - `src/boot/src/arch/arm64/start_arm64.S` (lines 627-641)
- [x] **Syscall table** — 130 entries in `SYSCALL_TABLE[]`, each with name, handler function pointer, arg count, implemented flag, description
  - `src/kernel/src/syscall_table.cpp`
- [x] **sys_debug_print (0)** — Writes string at arg0 to UART via `early_debug_print()`
- [x] **sys_exit (1)** — Marks thread Terminated, dequeues from scheduler, terminates process in ProcessManager, restores TTBR0 to kernel PGD, calls `schedule_after_exit()` ([[noreturn]])
- [x] **sys_write (33)** — Writes `count` bytes from `buf` to UART for fd=1 (stdout) and fd=2 (stderr); returns byte count or -EBADF/-EINVAL
- [x] **sys_getpid/getppid/getuid/getgid (2-5)** — Marked implemented but return hardcoded values (1, 0, 0, 0)

### Timer Subsystem

- [x] **Clocksource** — Reads ARM64 generic timer (`cntvct_el0`), frequency from `cntfrq_el0` or DTB, nanosecond conversion via `ns_to_cycles()` / `cycles_to_ns()`
  - `src/timer/src/timer.cppm`
- [x] **HrTimer** — High-resolution timer: callback + context, expiry time (ns), one-shot/periodic modes, armed/disarmed state
  - `src/timer/src/timer.cppm`
- [x] **TimerSubsystem** — Singleton managing clocksource + timer queue, `handle_interrupt()` checks expired timers and fires callbacks, drives scheduler tick on CPU 0 via periodic HrTimer, calibration at init
  - `src/timer/src/timer.cppm`

### Logging

- [x] **klog** — Type-safe kernel logging with `{}` format placeholders, severity levels (DEBUG/INFO/WARN/ERROR/PANIC), outputs via UART HAL, supports u32/u64/i32/i64/const char\*/bool/pointer formatting, hex (`{:#x}`), binary (`{:#b}`) output
  - `src/logging/src/logging.cppm`

### Synchronization & Data Structures

- [x] **TicketSpinLock** — Fair FIFO spinlock using atomic ticket counter (next/now), `lock()`/`unlock()`/`try_lock()`
- [x] **IrqSpinLock** — Spinlock that saves and disables IRQs on `lock()`, restores on `unlock()` (uses DAIF on ARM64)
- [x] **LockGuard** — RAII lock guard template
- [x] **AtomicPtr<T>** — Atomic pointer operations via `__atomic_*` builtins: `load()`, `store()`, `compare_exchange_weak()`
- [x] **AtomicCounter<T>** — Atomic integer operations: `load()`, `store()`, `fetch_add()`, `fetch_sub()`, `compare_exchange_weak()`
- [x] **PerCpuData<T>** — Per-CPU data storage (array indexed by CPU ID, up to MAX_CPUS=8)
- [x] **PerCpuAtomicCounter<T>** — Per-CPU atomic counters with `aggregate()` to sum across CPUs
- [x] **PerCpuWorkQueue<T, N>** — Per-CPU bounded work queues (SPSC ring buffer)
- [x] **MpscQueue<T>** — Multi-producer single-consumer lock-free queue
- [x] **RcuList<T>** — RCU-protected singly-linked list: `push_front()`, `remove()`, `find_if()`, `for_each()`
- [x] **RcuHashMap<K, V>** — RCU-protected hash map: `insert_or_update()`, `find()`, `remove()`, `size()`
  - All in `src/containers/src/containers.cppm`

### Core Infrastructure

- [x] **C++26 module system** — 23 modules with clean dependency graph, module partition support
- [x] **Freestanding types** — `u8/u16/u32/u64`, `i8/i16/i32/i64`, `usize/isize`, `PhysAddr/VirtAddr`, `ProcessId/ThreadId`, `ErrorCode` enum (28 error codes)
  - `src/core/src/types.cppm`
- [x] **Result<T, E>** — Rust-style error handling: `has_value()`, `value()`, `error()`, `operator*`, implicit conversion from T, explicit from ErrorCode
  - `src/core/src/result.cppm`
- [x] **Smart pointers** — `unique_ptr<T>` with `make_unique<T>()` for freestanding environment, move semantics, `get()`, `release()`, `reset()`
  - `src/core/src/smart_ptr.cppm`
- [x] **Concepts** — C++20 concepts: `Integral`, `FloatingPoint`, `Unsigned`, `Signed`, `Pointer`, `Arithmetic`, `Comparable`, `Swappable`, `Trivial`, `TriviallyCopyable`
  - `src/core/src/concepts.cppm`
- [x] **Freestanding stdlib** — `memcpy`, `memset`, `memmove`, `strlen`, `strcmp`, `strncmp`, `strncpy`, `min/max`, `MemoryOrder` enum
  - `src/core/src/std.cppm`
- [x] **Architecture abstraction** — `cpu_halt()`, `cpu_yield()`, `cpu_idle_once()`, `disable_all_interrupts()`, `disable_interrupts()`, `enable_interrupts()`, `get_current_cpu_id()`, `get_current_el()`, `get_timestamp_counter()`, `flush_tlb()`, `send_sgi()`
  - `src/aal/src/arch.cppm`

### Build System & Testing

- [x] **CMake build** — Top-level project with Clang cross-compilation, `-Weverything -Werror`, freestanding C++26 (`-std=c++26 -fmodules -ffreestanding -fno-exceptions -fno-rtti`), per-architecture toolchain flags
  - `CMakeLists.txt`, `cmake/arch_support.cmake`
- [x] **6 build presets** — arm64-qemu-{debug,release}, x86_64-qemu-{debug,release}, riscv-qemu-{debug,release}, each with configure→build→test workflow
  - `CMakePresets.json`
- [x] **Python build orchestrator** — `build.py` using Typer/Rich, supports `--arch`, `--build-type`, `--all`, `--dry-run`, `list` subcommand, parallel build, summary table
  - `build.py`, `pyproject.toml`
- [x] **Test framework** — Kernel-optimized Boost.UT adaptation, standalone `moss.test.elf` with per-architecture `_start`, semihosting exit codes (ARM64 SYS_EXIT, x86_64 port 0x501, RISC-V HTIF)
  - `src/test/framework/ut_kernel.hpp`, `src/test/framework/moss_ut.hpp`, `src/test/test_main.cpp`
- [x] **QEMU run scripts** — Auto-generated per build preset, correct machine/CPU/memory flags
  - `build/<preset>/run_qemu.sh`
- [x] **Linker scripts** — Kernel: `.text.boot` at 0x40080000, sections: text/rodata/data/bss/stack(256KB)/heap(8MB)/page_tables(2MB). User: base at 0x400000
  - `linker.ld`, `userspace/userspace.ld`

---

## Pending Features

### P0: Multi-Process Support (Prerequisite for any real OS)

- [x] **Per-process kernel stack** — Each user-mode thread gets its own 16KB kernel stack (order 2 from buddy allocator). SP_EL1 is set to the per-thread kernel stack top before entering EL0 (via TPIDR_EL1 in `switch_to_user`). SP_EL1 is preserved across eret, so when exceptions from EL0 arrive, SP already points to the correct per-thread kernel stack.
  - Thread struct: `kernel_stack_base`, `kernel_stack_size`, `kernel_stack_top()` method
  - Allocation: buddy allocator order 2 (16KB = 4 pages) in `kernel.cppm` user process creation
  - TPIDR_EL1 set in `context_switch_to_task()` before `switch_to_user` and before `context_switch` for user task re-dispatch
  - `switch_to_user` assembly: reads TPIDR_EL1 → sets SP before eret
  - Files: `process.cppm` (Thread struct), `kernel.cppm`, `context_switch.S`, `start_arm64.S`

- [ ] **fork() system call** — Cannot create child processes. This is the fundamental process creation primitive.
  - Need: copy Process object (new PID, inherit parent_pid)
  - Need: clone address space — allocate new PGD, copy PTE structure, mark all writable pages as COW (read-only + COW flag)
  - Need: clone thread state (copy CpuContext, set child return value to 0)
  - Need: add child to parent's children list for wait()
  - Need: enqueue child thread into scheduler
  - Files: `syscall_table.cpp` (handler), `process.cppm`/`process.cpp` (clone logic), `page_table.cpp` (PGD copy + COW marking)
  - Dependency: COW support, per-process kernel stack
  - Complexity: High

- [ ] **Copy-on-Write (COW)** — Permission fault handler (DFSC 0x0C-0x0F) currently panics. Need to handle write faults on COW-marked pages.
  - Need: COW flag in PTE (use software-defined bit, e.g. bit 55 or 58)
  - Need: reference counting for physical pages (shared between parent/child)
  - Need: on write fault: if refcount > 1, allocate new page, copy content, map writable, decrement old refcount; if refcount == 1, just remap as writable
  - Files: `page_fault.cpp` (permission fault path), `mm.cppm` (page refcount), `page_table.cpp`
  - Dependency: Required by fork()
  - Complexity: High

- [ ] **execve() system call** — Cannot replace process image. Need to load a new ELF into an existing process.
  - Need: tear down old address space (free all user page tables + physical pages)
  - Need: create new address space with new PGD
  - Need: load new ELF segments as VMAs (reuse existing ELF loader)
  - Need: reset thread context (new entry point, new stack)
  - Need: close inherited file descriptors marked CLOEXEC (when FD table exists)
  - Files: `syscall_table.cpp`, `kernel.cppm` (ElfLoader), `process.cppm`, `page_table.cpp`
  - Complexity: Medium-High

- [ ] **wait()/waitpid() system calls** — Cannot wait for child process exit or collect exit status.
  - Need: parent-child relationship tracking (children list in Process)
  - Need: zombie state: terminated processes stay in process table until parent calls wait()
  - Need: blocking: if no child has exited, block parent (set to Blocked state, wake on child exit)
  - Need: WNOHANG option for non-blocking check
  - Files: `syscall_table.cpp`, `process.cppm` (Process class), `process.cpp`
  - Dependency: fork() must exist for there to be children
  - Complexity: Medium

### P1: Basic OS Functionality

- [ ] **VFS (Virtual File System)** — No file system layer at all. This blocks open/close/read/write/stat and everything that depends on file descriptors.
  - Need: `struct file`, `struct inode`, `struct dentry`, `struct superblock` abstractions
  - Need: file descriptor table per process (array of `struct file*`)
  - Need: VFS operations: `open()`, `close()`, `read()`, `write()`, `stat()`, `lseek()`
  - Need: at minimum a ramfs/tmpfs for in-memory file system
  - Need: devfs for device nodes (e.g. /dev/console → UART)
  - Complexity: Very High — this is a major subsystem

- [ ] **mmap() / munmap() system calls** — User-space cannot dynamically map memory. Current heap is a fixed 1MB VMA at 0x800000.
  - Need: find free VA range in process address space
  - Need: create VMA for the mapping
  - Need: anonymous mmap: demand-zero pages
  - Need: file-backed mmap: (requires VFS)
  - Need: munmap: remove VMA, unmap pages, free physical pages
  - Files: `syscall_table.cpp`, `process.cppm` (AddressSpace), `page_table.cpp`
  - Complexity: Medium

- [ ] **brk() system call** — User-space cannot expand the heap. Currently brk() returns ENOSYS.
  - Need: track program break per process (initialized to end of BSS)
  - Need: on brk(new_addr): extend/shrink heap VMA, map/unmap pages as needed
  - Files: `syscall_table.cpp`, `process.cppm` (AddressSpace)
  - Complexity: Low-Medium

- [ ] **Signal mechanism** — No signal delivery or handling. kill/sigaction/sigprocmask all return ENOSYS.
  - Need: signal pending bitmap per thread
  - Need: signal handler registration (sigaction)
  - Need: signal delivery on return to user-space (check pending signals before eret)
  - Need: signal handler execution (set up signal frame on user stack, redirect ELR to handler)
  - Need: sigreturn (restore original context from signal frame)
  - Need: default signal actions (SIGKILL→terminate, SIGSEGV→terminate+core, SIGSTOP→stop)
  - Files: `process.cppm` (Thread struct), `start_arm64.S` (check signals before eret), new `signal.cpp`
  - Complexity: High

- [ ] **Pipe IPC** — pipe() returns ENOSYS. No inter-process communication primitive.
  - Need: kernel buffer (e.g. 4KB ring buffer) shared between read and write ends
  - Need: file descriptor integration (read end fd, write end fd)
  - Need: blocking: reader blocks if pipe empty, writer blocks if pipe full
  - Need: EOF: close write end → reader gets EOF
  - Dependency: VFS / file descriptor table
  - Complexity: Medium

- [ ] **User pointer validation** — sys_write does raw `reinterpret_cast` on user-provided buffer address. No `copy_from_user()`/`copy_to_user()` safety.
  - Need: `copy_from_user(kernel_dst, user_src, len)` — validates user pointer falls within process VMA with correct permissions before copying
  - Need: `copy_to_user(user_dst, kernel_src, len)` — same for kernel→user direction
  - Need: use in all syscalls that access user memory
  - Files: new helper functions, `syscall_table.cpp` (all user-pointer syscalls)
  - Complexity: Low-Medium

### P2: System Robustness

- [ ] **KPTI (Kernel Page Table Isolation)** — Currently user PGD[0] contains complete kernel identity map via shared PGD entry. User-mode code can potentially read kernel memory via speculative execution (Meltdown).
  - Need: separate user and kernel page tables — user PGD should NOT contain kernel mappings
  - Need: on exception entry from EL0, switch to kernel page tables (modify exception vector to load kernel TTBR0 first)
  - Need: on eret to EL0, switch back to user page tables
  - Alternative: use TTBR1 for kernel (already done) and make TTBR0 user-only — but current kernel code runs at identity-mapped low addresses, which requires TTBR0
  - Complexity: High — requires rethinking kernel address space layout

- [ ] **Stack growth** — User stack is fixed at 64KB. Stack overflow causes page fault → process termination, no automatic growth.
  - Need: guard page below stack VMA (unmapped, triggers page fault on access)
  - Need: on page fault in guard region: extend stack VMA downward, map new page, move guard page
  - Need: configurable stack limit (e.g. 8MB max)
  - Files: `page_fault.cpp`, `process.cppm` (VMA management)
  - Complexity: Low-Medium

- [ ] **Proper getpid/getppid/getuid/getgid** — Currently return hardcoded values instead of querying current process.
  - Need: getpid → return current_process()->pid()
  - Need: getppid → return current_process()->parent_pid()
  - Need: getuid/getgid → implement credential structure in Process
  - Files: `syscall_table.cpp`
  - Complexity: Low

- [ ] **Red-black tree full rebalancing** — `rb_insert_fixup()` only sets root black (no rotations or uncle-based recoloring). `rb_delete_fixup()` is also simplified. Under adversarial insertion patterns, tree degrades to O(n).
  - Need: implement full left/right rotation
  - Need: implement 3-case insert fixup (uncle red → recolor, uncle black → rotate)
  - Need: implement 4-case delete fixup
  - Files: `process.cppm` (CfsRunqueue RB-tree methods)
  - Complexity: Medium

- [ ] **Timer system calls** — nanosleep, clock_gettime, clock_getres all return ENOSYS.
  - Need: clock_gettime(CLOCK_MONOTONIC) → read clocksource and return ns
  - Need: nanosleep → add HrTimer with wakeup callback, block thread until expiry
  - Need: clock_getres → return clocksource resolution
  - Files: `syscall_table.cpp`, `timer.cppm`
  - Complexity: Low-Medium

- [ ] **Process exit notification** — When a user process crashes (SIGSEGV equivalent via `terminate_current_user_process`), the exit code is set but no parent is notified. With wait() not implemented, zombie processes accumulate.
  - Dependency: wait() implementation, signal mechanism
  - Complexity: Low (once dependencies exist)

### P3: Platform Extension

- [ ] **x86_64 full kernel** — Only UART HAL and boot stub exist. Everything else is missing.
  - Need: MMU setup (CR3, 4-level paging, PAE)
  - Need: IDT (Interrupt Descriptor Table) setup
  - Need: APIC initialization (Local APIC + I/O APIC)
  - Need: Timer setup (APIC timer or HPET)
  - Need: context_switch.S (x86_64 calling convention, RSP/RIP save/restore)
  - Need: syscall entry (SYSCALL/SYSRET or INT 0x80)
  - Need: exception handlers (page fault via ISR 14, etc.)
  - Need: SMP boot (AP startup via SIPI)
  - Complexity: Very High — essentially a second kernel port

- [ ] **RISC-V full kernel** — Same situation as x86_64, only UART HAL and boot stub.
  - Need: MMU setup (satp register, Sv48 page tables)
  - Need: Trap handler setup (stvec, scause dispatch)
  - Need: PLIC initialization
  - Need: Timer setup (mtime/mtimecmp via SBI)
  - Need: context_switch.S (RISC-V calling convention)
  - Need: ecall entry for syscalls
  - Need: SMP boot (via SBI HSM extension)
  - Complexity: Very High

- [ ] **Block device driver** — No storage device support. Needed for any persistent file system.
  - Need: virtio-blk driver for QEMU
  - Need: block layer abstraction (read/write sector)
  - Complexity: High

- [ ] **Network device driver** — No networking. All 20 network syscalls are stubs.
  - Need: virtio-net driver for QEMU
  - Need: network stack (at minimum: Ethernet frame handling, ARP, IP, UDP)
  - Complexity: Very High

- [ ] **Device driver framework** — `DeviceManager` exists as a framework but has no actual drivers registered.
  - Need: device tree-driven probe (match compatible strings to driver init functions)
  - Need: interrupt routing from GIC to driver handlers
  - Need: DMA-capable memory allocation for device buffers
  - Complexity: Medium

### P4: Advanced Features

- [ ] **initramfs / root filesystem** — No storage layer at all. Kernel can only run the single embedded hello.elf.
  - Need: CPIO archive parser for initramfs (passed by bootloader in memory)
  - Need: ramfs to hold extracted files
  - Need: mount as root filesystem
  - Dependency: VFS
  - Complexity: Medium

- [ ] **User-space libc** — Current user programs use raw SVC inline assembly. No C library.
  - Need: minimal libc with syscall wrappers (write, exit, mmap, brk, etc.)
  - Need: printf/puts implementation using write()
  - Need: malloc/free using brk() or mmap()
  - Need: _start entry point that calls main() and then exit()
  - Complexity: Medium

- [ ] **NUMA actual support** — Framework exists (policy manager, distance matrix, node states) but no multi-node hardware detection or cross-node allocation.
  - Need: SRAT/SLIT ACPI table parsing (x86_64) or DTB NUMA node parsing (ARM64)
  - Need: per-node buddy allocator zones
  - Need: NUMA-aware page allocation policy
  - Complexity: High

- [ ] **Huge page support** — Interface defined for 2MB/1GB pages but not connected to allocation paths.
  - Need: PMD-level block mapping for 2MB pages
  - Need: PUD-level block mapping for 1GB pages
  - Need: THP (Transparent Huge Pages) or explicit hugetlbfs
  - Complexity: Medium

- [ ] **Memory reclaimer** — Class exists but no actual reclaim algorithm.
  - Need: LRU page list tracking
  - Need: page eviction under memory pressure
  - Need: reclaim watermark triggers
  - Dependency: Useful mainly after mmap/file-backed pages exist
  - Complexity: High

- [ ] **Memory compactor** — Class exists but no actual compaction.
  - Need: page migration (copy content, update PTE, flush TLB)
  - Need: free page scanner + movable page scanner
  - Need: compaction trigger (high-order allocation failure)
  - Complexity: High

- [ ] **Shared memory IPC** — `SharedMemoryManager` exists in `moss.ipc` module but is not connected to any syscall or user-facing API.
  - Need: shmget/shmat/shmdt syscalls or mmap(MAP_SHARED)
  - Need: physical page sharing between processes
  - Dependency: mmap, VFS
  - Complexity: Medium

- [ ] **Multi-user support** — No credential structure, no permission checking.
  - Need: uid/gid/euid/egid in Process
  - Need: capability-based permission checking
  - Need: setuid/setgid syscalls
  - Complexity: Medium

---

## Module Dependency Graph

```
moss.types ← moss.std ← moss.concepts ← moss.result ← moss.smart_ptr
                                              ↓
                                          moss.arch
                                              ↓
                               ┌──────────────┼──────────────┐
                               ↓              ↓              ↓
                          moss.platform   moss.logging   moss.fdt
                               ↓              ↓
                          moss.hal.*     moss.containers
                               ↓              ↓
                    ┌──────────┼──────────────┼──────────┐
                    ↓          ↓              ↓          ↓
               moss.mm   moss.interrupts  moss.timer  moss.ipc
                    ↓          ↓              ↓
                    └──────────┼──────────────┘
                               ↓
                          moss.process
                               ↓
                          moss.drivers
                               ↓
                          moss.kernel ← moss.boot
```

---

## File Statistics

- **Total source files**: ~60 (.cppm + .cpp + .S + .c)
- **Total lines of code**: ~15,000+ (estimated)
- **Modules**: 23 C++26 modules
- **Syscall table entries**: 130 (7 implemented, ~123 stubs)
- **Architectures**: 3 (ARM64 full, x86_64 stub, RISC-V stub)
- **Build presets**: 6 (3 arch × 2 build types)
