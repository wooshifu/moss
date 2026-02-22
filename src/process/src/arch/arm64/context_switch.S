// =============================================================================
// ARM64 Context Switch Implementation
// =============================================================================
//
// This file provides the core process/thread switching primitives for AArch64:
//
//   context_switch(prev, next)  — save prev's CPU state, restore next's
//   switch_to_user(ctx, usp)   — enter EL0 user mode for the first time
//   kernel_thread_entry()      — trampoline for newly created kernel threads
//   syscall_fast_path()        — optimized path for lightweight syscalls
//   flush_tlb_single(va)       — invalidate a single TLB entry by VA
//   flush_tlb_all()            — invalidate all EL1 TLB entries
//
// CpuContext structure layout (defined in moss.process module):
//   Offset  Size   Field
//   ------  ----   -----
//   0x000   256    x[32]     — general-purpose registers x0-x31 (8 bytes each)
//   0x100   8      sp        — stack pointer (offset 248 in x[] is LR=x30)
//   0x108   8      pc        — program counter (saved/restored as LR)
//   0x110   8      pstate    — DAIF mask (from mrs/msr daif)
//   0x118   16     fpsr/fpcr — floating-point status/control registers
//   0x120   512    q[32]     — NEON/FP registers v0-v31 (128-bit each)
//   0x320   8      tpidr_el0 — thread-local storage pointer (user TLS)
//
// IMPORTANT — DAIF handling during context_switch:
//   The DAIF (interrupt mask) register is NOT restored during context_switch.
//   This is intentional:
//   - context_switch is always called from an interrupt handler or with IRQs
//     masked.  Restoring an unmasked DAIF while SP still points to the old
//     task's stack would allow a nested IRQ to corrupt that stack.
//   - The correct unmask happens via:
//     * irq_trampoline's eret (restores SPSR_EL1 which has pre-IRQ PSTATE)
//     * test_task_entry (fresh tasks run with caller's IRQ state naturally)
// =============================================================================

.section .text

.extern current_thread_ptr
.extern next_thread_ptr

// =============================================================================
// context_switch — save current thread, restore next thread
// =============================================================================
//
// Prototype: extern "C" void context_switch(CpuContext* prev, CpuContext* next);
//
// Parameters:
//   x0 = prev — pointer to current thread's CpuContext (save destination)
//   x1 = next — pointer to next thread's CpuContext (restore source)
//
// After return, execution continues in the context of `next`.
// If `next` was previously suspended by context_switch, it resumes at the
// instruction following its own `bl context_switch`.
// =============================================================================
.global context_switch
.type context_switch, @function
context_switch:
    // ---- Phase 1: Save current (prev) context ----

    // Save all 31 general-purpose registers into prev->x[0..30]
    stp     x0, x1, [x0, #0]           // x0, x1
    stp     x2, x3, [x0, #16]          // x2, x3
    stp     x4, x5, [x0, #32]          // x4, x5
    stp     x6, x7, [x0, #48]          // x6, x7
    stp     x8, x9, [x0, #64]          // x8, x9
    stp     x10, x11, [x0, #80]        // x10, x11
    stp     x12, x13, [x0, #96]        // x12, x13
    stp     x14, x15, [x0, #112]       // x14, x15
    stp     x16, x17, [x0, #128]       // x16, x17
    stp     x18, x19, [x0, #144]       // x18, x19
    stp     x20, x21, [x0, #160]       // x20, x21
    stp     x22, x23, [x0, #176]       // x22, x23
    stp     x24, x25, [x0, #192]       // x24, x25
    stp     x26, x27, [x0, #208]       // x26, x27
    stp     x28, x29, [x0, #224]       // x28, x29 (FP)
    str     x30, [x0, #240]            // x30 (LR)

    // Save stack pointer → prev->sp
    mov     x2, sp
    str     x2, [x0, #248]

    // Save return address as PC → prev->pc
    // (When this thread is resumed, it will return to the caller of
    //  context_switch, so LR is the correct "program counter" to save.)
    str     x30, [x0, #256]

    // Save DAIF interrupt mask → prev->pstate
    mrs     x2, daif
    str     x2, [x0, #264]

    // Save floating-point status registers → prev->fpsr, prev->fpcr
    mrs     x2, fpsr
    mrs     x3, fpcr
    stp     x2, x3, [x0, #272]

    // Save all 32 NEON/FP 128-bit registers (v0-v31) → prev->q[]
    add     x2, x0, #288               // base address of q[] array
    stp     q0, q1, [x2, #0]
    stp     q2, q3, [x2, #32]
    stp     q4, q5, [x2, #64]
    stp     q6, q7, [x2, #96]
    stp     q8, q9, [x2, #128]
    stp     q10, q11, [x2, #160]
    stp     q12, q13, [x2, #192]
    stp     q14, q15, [x2, #224]
    stp     q16, q17, [x2, #256]
    stp     q18, q19, [x2, #288]
    stp     q20, q21, [x2, #320]
    stp     q22, q23, [x2, #352]
    stp     q24, q25, [x2, #384]
    stp     q26, q27, [x2, #416]
    stp     q28, q29, [x2, #448]
    stp     q30, q31, [x2, #480]

    // Save user-space thread pointer → prev->tpidr_el0
    mrs     x2, tpidr_el0
    str     x2, [x0, #800]

    // ---- Phase 2: Restore next context ----
    //
    // IMPORTANT: DAIF is intentionally NOT restored here.
    // See file header comment for the rationale.

    // Restore user-space thread pointer
    ldr     x2, [x1, #800]
    msr     tpidr_el0, x2

    // Restore NEON/FP registers
    add     x2, x1, #288
    ldp     q0, q1, [x2, #0]
    ldp     q2, q3, [x2, #32]
    ldp     q4, q5, [x2, #64]
    ldp     q6, q7, [x2, #96]
    ldp     q8, q9, [x2, #128]
    ldp     q10, q11, [x2, #160]
    ldp     q12, q13, [x2, #192]
    ldp     q14, q15, [x2, #224]
    ldp     q16, q17, [x2, #256]
    ldp     q18, q19, [x2, #288]
    ldp     q20, q21, [x2, #320]
    ldp     q22, q23, [x2, #352]
    ldp     q24, q25, [x2, #384]
    ldp     q26, q27, [x2, #416]
    ldp     q28, q29, [x2, #448]
    ldp     q30, q31, [x2, #480]

    // Restore floating-point status registers
    ldp     x2, x3, [x1, #272]
    msr     fpsr, x2
    msr     fpcr, x3

    // Restore stack pointer FIRST (critical for IRQ safety — after this
    // point, any IRQ will use the new task's stack, not the old one)
    ldr     x2, [x1, #248]
    mov     sp, x2

    // Restore general-purpose registers (x2-x29, x30)
    ldp     x2, x3, [x1, #16]
    ldp     x4, x5, [x1, #32]
    ldp     x6, x7, [x1, #48]
    ldp     x8, x9, [x1, #64]
    ldp     x10, x11, [x1, #80]
    ldp     x12, x13, [x1, #96]
    ldp     x14, x15, [x1, #112]
    ldp     x16, x17, [x1, #128]
    ldp     x18, x19, [x1, #144]
    ldp     x20, x21, [x1, #160]
    ldp     x22, x23, [x1, #176]
    ldp     x24, x25, [x1, #192]
    ldp     x26, x27, [x1, #208]
    ldp     x28, x29, [x1, #224]       // x29 = FP
    ldr     x30, [x1, #240]            // x30 = LR (return address)

    // Restore x0 and x1 last (x1 is our base pointer)
    ldp     x0, x1, [x1, #0]

    // Return to the new thread's saved LR
    ret

.size context_switch, .-context_switch

// =============================================================================
// switch_to_user — first-time entry into EL0 user mode
// =============================================================================
//
// Prototype: extern "C" void switch_to_user(CpuContext* ctx, uint64_t user_sp);
//
// Parameters:
//   x0 = ctx     — pointer to the user task's CpuContext
//   x1 = user_sp — user-space stack pointer to load into SP_EL0
//
// This function:
//   1. Sets SPSR_EL1 = 0x0 (EL0t with all interrupts enabled)
//   2. Loads the user entry point from ctx->pc into ELR_EL1
//   3. Restores all GP, FP/NEON, and TLS registers from ctx
//   4. Sets SP_EL1 to the per-thread kernel stack (from TPIDR_EL1) so that
//      future exceptions from EL0 use the correct kernel stack
//   5. Executes eret to atomically drop to EL0
// =============================================================================
.global switch_to_user
.type switch_to_user, @function
switch_to_user:
    // Configure exception return to EL0t (user mode, interrupts enabled)
    mov     x2, #0x0
    msr     spsr_el1, x2

    // Set user stack pointer (EL0 uses SP_EL0)
    msr     sp_el0, x1

    // Set user entry point → ELR_EL1
    ldr     x2, [x0, #256]             // ctx->pc
    msr     elr_el1, x2

    // Restore FP status registers
    ldp     x2, x3, [x0, #272]
    msr     fpsr, x2
    msr     fpcr, x3

    // Restore NEON/FP registers (v0-v31)
    add     x2, x0, #288
    ldp     q0, q1, [x2, #0]
    ldp     q2, q3, [x2, #32]
    ldp     q4, q5, [x2, #64]
    ldp     q6, q7, [x2, #96]
    ldp     q8, q9, [x2, #128]
    ldp     q10, q11, [x2, #160]
    ldp     q12, q13, [x2, #192]
    ldp     q14, q15, [x2, #224]
    ldp     q16, q17, [x2, #256]
    ldp     q18, q19, [x2, #288]
    ldp     q20, q21, [x2, #320]
    ldp     q22, q23, [x2, #352]
    ldp     q24, q25, [x2, #384]
    ldp     q26, q27, [x2, #416]
    ldp     q28, q29, [x2, #448]
    ldp     q30, q31, [x2, #480]

    // Restore user-space thread-local storage pointer
    ldr     x2, [x0, #800]
    msr     tpidr_el0, x2

    // Restore general-purpose registers (x2-x29, x30)
    ldp     x2, x3, [x0, #16]
    ldp     x4, x5, [x0, #32]
    ldp     x6, x7, [x0, #48]
    ldp     x8, x9, [x0, #64]
    ldp     x10, x11, [x0, #80]
    ldp     x12, x13, [x0, #96]
    ldp     x14, x15, [x0, #112]
    ldp     x16, x17, [x0, #128]
    ldp     x18, x19, [x0, #144]
    ldp     x20, x21, [x0, #160]
    ldp     x22, x23, [x0, #176]
    ldp     x24, x25, [x0, #192]
    ldp     x26, x27, [x0, #208]
    ldp     x28, x29, [x0, #224]
    ldr     x30, [x0, #240]

    // Set SP_EL1 to this thread's per-thread kernel stack.
    // After eret drops us to EL0, SP_EL1 is preserved.  The next exception
    // from EL0 will enter EL1 with SP already pointing to the correct
    // per-thread kernel stack — no assembly fixup needed.
    mrs     x1, tpidr_el1               // TPIDR_EL1 = kernel stack top
    mov     sp, x1                      // SP_EL1 = per-thread kernel stack

    // Restore x0 and x1 last (x0 was our context pointer)
    ldr     x1, [x0, #8]               // restore user x1
    ldr     x0, [x0, #0]               // restore user x0

    // Atomically restore PSTATE from SPSR_EL1 and branch to ELR_EL1 (EL0)
    eret

.size switch_to_user, .-switch_to_user

// =============================================================================
// kernel_thread_entry — trampoline for newly created kernel threads
// =============================================================================
//
// When a kernel thread is first scheduled via context_switch, its saved LR
// points here.  The thread function pointer and argument were stored in
// callee-saved registers x19 (arg) and x20 (func) by the thread creator.
//
// Flow:
//   1. Move x19 → x0 (arg), x20 → x1 (func)
//   2. Enable IRQs (msr daifclr, #2) — kernel threads start with IRQs masked
//      because context_switch preserves the caller's DAIF state
//   3. Call func(arg)
//   4. Spin forever if the function returns (should not happen — thread
//      functions call sys_exit or schedule_exit)
// =============================================================================
.global kernel_thread_entry
.type kernel_thread_entry, @function
kernel_thread_entry:
    mov     x0, x19                     // arg  (stashed by thread creator)
    mov     x1, x20                     // func (stashed by thread creator)

    // Enable IRQ — context_switch caller was IRQ-masked
    msr     daifclr, #2

    // Call the kernel thread function: func(arg)
    blr     x1

    // Should never reach here — infinite loop as safety net
    b       .

.size kernel_thread_entry, .-kernel_thread_entry

// =============================================================================
// user_eret_trampoline — first-time eret to EL0 via context_switch
// =============================================================================
//
// This trampoline is the target of `ret` after context_switch restores a
// freshly-prepared CpuContext for a user task that has never entered EL0
// (needs_initial_eret).
//
// By going through context_switch instead of calling switch_to_user directly,
// the caller's bootstrap context is properly saved.  This allows waitpid's
// context_switch(my_ctx, bootstrap) to correctly return to the idle loop.
//
// Callee-saved registers set by context_switch_to_task before context_switch:
//   x19 = (reserved — Thread*)
//   x20 = user entry point (ELR_EL1)
//   x21 = user stack pointer (SP_EL0)
//   x22 = user x0 value (fork: 0, execve: argc)
//   x23 = user x1 value (fork: 0, execve: argv pointer)
//   x24 = CpuContext pointer (fork: non-zero, execve: 0)
//
// Two modes:
//   x24 == 0 (execve): zero all user GP regs except x0/x1 (clean slate)
//   x24 != 0 (fork):   restore parent's full GP regs from CpuContext at x24
//
// Preconditions (set by context_switch_to_task's normal path):
//   - TTBR0_EL1 already points to this process's page tables
//   - TPIDR_EL1 already holds this thread's kernel stack top
//   - IRQs are masked (context_switch path masks them)
// =============================================================================
.global user_eret_trampoline
.type user_eret_trampoline, @function
user_eret_trampoline:
    // Set SPSR_EL1 = 0x0 (EL0t, all interrupts enabled upon eret)
    mov     x0, #0
    msr     spsr_el1, x0

    // Set user stack pointer
    msr     sp_el0, x21

    // Set user entry point
    msr     elr_el1, x20

    // Set SP_EL1 to per-thread kernel stack (from TPIDR_EL1).
    // Future exceptions from EL0 will use this stack.
    mrs     x0, tpidr_el1
    mov     sp, x0

    // Check x24: fork (non-zero) vs execve (zero)
    cbnz    x24, .Lfork_restore

    // ---- Execve path: clean register slate ----
    // Set user x0 and x1 from callee-saved registers.
    mov     x0, x22
    mov     x1, x23

    // Zero remaining user-visible GP registers for clean entry.
    mov     x2, #0
    mov     x3, #0
    mov     x4, #0
    mov     x5, #0
    mov     x6, #0
    mov     x7, #0
    mov     x8, #0
    mov     x9, #0
    mov     x10, #0
    mov     x11, #0
    mov     x12, #0
    mov     x13, #0
    mov     x14, #0
    mov     x15, #0
    mov     x16, #0
    mov     x17, #0
    mov     x18, #0
    mov     x19, #0
    mov     x20, #0
    mov     x21, #0
    mov     x22, #0
    mov     x23, #0
    mov     x24, #0
    mov     x25, #0
    mov     x26, #0
    mov     x27, #0
    mov     x28, #0
    mov     x29, #0
    mov     x30, #0
    eret

    // ---- Fork path: restore parent's full GP registers ----
    // x24 = pointer to CpuContext (in Thread.context)
    // CpuContext layout: x[0] at offset 0, x[1] at 8, ..., x[30] at 240
    //
    // Parent's original x[19]-x[24] were saved to kernel stack before
    // context_switch_to_task overwrote them with trampoline args.
    // Location: [SP - 48] (SP = kernel_stack_top after mov sp above)
.Lfork_restore:
    // x0 = fork return value (always 0 for child)
    mov     x0, #0

    // Restore x1 from parent's saved context
    ldr     x1, [x24, #8]              // parent x[1]

    // Restore x2-x18 from CpuContext (these were NOT overwritten)
    ldp     x2, x3, [x24, #16]
    ldp     x4, x5, [x24, #32]
    ldp     x6, x7, [x24, #48]
    ldp     x8, x9, [x24, #64]
    ldp     x10, x11, [x24, #80]
    ldp     x12, x13, [x24, #96]
    ldp     x14, x15, [x24, #112]
    ldp     x16, x17, [x24, #128]
    ldr     x18, [x24, #144]

    // Restore x25-x29, x30 from CpuContext (NOT overwritten by trampoline setup)
    ldr     x25, [x24, #200]           // parent x[25]
    ldp     x26, x27, [x24, #208]      // parent x[26], x[27]
    ldp     x28, x29, [x24, #224]      // parent x[28], x[29] (FP)
    ldr     x30, [x24, #240]           // parent x[30] (LR)

    // Restore x19-x24 from kernel stack save area.
    // These were saved by context_switch_to_task BEFORE overwriting with
    // trampoline args.  SP = kernel_stack_top, saved at [SP - 48].
    ldp     x19, x20, [sp, #-48]       // parent x[19], x[20]
    ldp     x21, x22, [sp, #-32]       // parent x[21], x[22]
    ldp     x23, x24, [sp, #-16]       // parent x[23], x[24]

    eret

.size user_eret_trampoline, .-user_eret_trampoline

// =============================================================================
// syscall_fast_path — optimized entry for lightweight system calls
// =============================================================================
//
// Intended for high-frequency syscalls (number < 0x100) that can be handled
// without a full register save/restore.  Currently a stub.
//
// Prototype: extern "C" void syscall_fast_path();
//
// On entry: x8 = syscall number, x0-x5 = args
// =============================================================================
.global syscall_fast_path
.type syscall_fast_path, @function
syscall_fast_path:
    // Minimal save: only FP and LR
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    // Check if this is a "fast" syscall (number < 0x100)
    cmp     x8, #0x100
    b.ge    syscall_slow_path

    // TODO: Handle fast system calls here (e.g., getpid, clock_gettime)

    // Restore and return
    ldp     x29, x30, [sp], #16
    ret

syscall_slow_path:
    // Fall through to full system call handling
    ldp     x29, x30, [sp], #16
    b       system_call_handler         // external: full syscall dispatcher

.size syscall_fast_path, .-syscall_fast_path

// =============================================================================
// TLB maintenance helpers
// =============================================================================

// flush_tlb_single — invalidate a single TLB entry
//
// Prototype: extern "C" void flush_tlb_single(uint64_t va);
// Parameter: x0 = virtual address (only the page-aligned bits matter)
//
// Uses TLBI VAE1 which invalidates by VA + ASID for EL1 regime.
// DSB + ISB ensure completion before any subsequent instruction or
// data access uses the updated translation.
.global flush_tlb_single
.type flush_tlb_single, @function
flush_tlb_single:
    tlbi    vae1, x0                    // TLB Invalidate by VA, EL1
    dsb     sy                          // ensure completion
    isb                                 // synchronize instruction stream
    ret
.size flush_tlb_single, .-flush_tlb_single

// flush_tlb_all — invalidate ALL TLB entries for EL1
//
// Prototype: extern "C" void flush_tlb_all();
//
// Uses TLBI VMALLE1 which invalidates all entries in the EL1&0
// translation regime.  Expensive — use sparingly (e.g., after
// changing TTBR0/TTBR1 or bulk page table updates).
.global flush_tlb_all
.type flush_tlb_all, @function
flush_tlb_all:
    tlbi    vmalle1                     // TLB Invalidate All, EL1
    dsb     sy                          // ensure completion
    isb                                 // synchronize instruction stream
    ret
.size flush_tlb_all, .-flush_tlb_all
