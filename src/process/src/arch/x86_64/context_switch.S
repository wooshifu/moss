// =============================================================================
// x86_64 Context Switch Implementation
// =============================================================================
//
// This file provides the core process/thread switching primitives for x86_64:
//
//   context_switch(prev, next) — save prev's CPU state, restore next's
//   switch_to_user(ctx, usp)  — enter ring 3 user mode via IRETQ
//   kernel_thread_entry()     — trampoline for newly created kernel threads
//
// CpuContext structure layout (defined in moss.process module):
//   Offset  Size   Field
//   ------  ----   -----
//   0x00    8      rax       (also used for syscall return value)
//   0x08    8      rbx
//   0x10    8      rcx
//   0x18    8      rdx
//   0x20    8      rsi
//   0x28    8      rdi
//   0x30    8      rbp       (frame pointer)
//   0x38    8      sp        (stack pointer)
//   0x40    8      r8
//   0x48    8      r9
//   0x50    8      r10
//   0x58    8      r11
//   0x60    8      r12       (callee-saved)
//   0x68    8      r13       (callee-saved)
//   0x70    8      r14       (callee-saved)
//   0x78    8      r15       (callee-saved)
//   0x80    8      pstate    (RFLAGS)
//   0x88    8      pc        (RIP / return address)
//
// x86_64 calling convention (System V AMD64 ABI):
//   Arguments: RDI, RSI, RDX, RCX, R8, R9, [stack]
//   Callee-saved: RBX, RBP, R12-R15
//   Return value: RAX
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
//   RDI = prev — pointer to current thread's CpuContext (save destination)
//   RSI = next — pointer to next thread's CpuContext (restore source)
// =============================================================================
.global context_switch
.type context_switch, @function
context_switch:
    // ---- Phase 1: Save current (prev) context ----

    // Save all general-purpose registers into prev CpuContext
    movq    %rax, 0(%rdi)              // prev->rax
    movq    %rbx, 8(%rdi)              // prev->rbx
    movq    %rcx, 16(%rdi)             // prev->rcx
    movq    %rdx, 24(%rdi)             // prev->rdx
    movq    %rsi, 32(%rdi)             // prev->rsi
    movq    %rdi, 40(%rdi)             // prev->rdi
    movq    %rbp, 48(%rdi)             // prev->rbp
    movq    %rsp, 56(%rdi)             // prev->sp
    movq    %r8, 64(%rdi)              // prev->r8
    movq    %r9, 72(%rdi)              // prev->r9
    movq    %r10, 80(%rdi)             // prev->r10
    movq    %r11, 88(%rdi)             // prev->r11
    movq    %r12, 96(%rdi)             // prev->r12
    movq    %r13, 104(%rdi)            // prev->r13
    movq    %r14, 112(%rdi)            // prev->r14
    movq    %r15, 120(%rdi)            // prev->r15

    // Save RFLAGS → prev->pstate
    pushfq
    popq    %rax
    movq    %rax, 128(%rdi)

    // Save return address (from the top of the stack) → prev->pc
    movq    (%rsp), %rax
    movq    %rax, 136(%rdi)

    // ---- Phase 2: Restore next context ----

    // Restore RFLAGS from next->pstate
    movq    128(%rsi), %rax
    pushq   %rax
    popfq

    // Restore general-purpose registers from next CpuContext
    movq    0(%rsi), %rax              // next->rax
    movq    8(%rsi), %rbx              // next->rbx
    movq    16(%rsi), %rcx             // next->rcx
    movq    24(%rsi), %rdx             // next->rdx
    movq    48(%rsi), %rbp             // next->rbp
    movq    56(%rsi), %rsp             // next->sp (stack switch happens here)
    movq    64(%rsi), %r8              // next->r8
    movq    72(%rsi), %r9              // next->r9
    movq    80(%rsi), %r10             // next->r10
    movq    88(%rsi), %r11             // next->r11
    movq    96(%rsi), %r12             // next->r12
    movq    104(%rsi), %r13            // next->r13
    movq    112(%rsi), %r14            // next->r14
    movq    120(%rsi), %r15            // next->r15

    // Push saved PC as return address so `ret` will jump there
    movq    136(%rsi), %r10            // next->pc
    pushq   %r10

    // Restore RDI and RSI last (they are our base pointers)
    movq    40(%rsi), %rdi             // next->rdi
    movq    32(%rsi), %rsi             // next->rsi

    // Return to new thread's saved PC
    ret

.size context_switch, .-context_switch

// =============================================================================
// switch_to_user — enter ring 3 user mode via IRETQ
// =============================================================================
//
// Prototype: extern "C" void switch_to_user(CpuContext* ctx, uint64_t user_sp);
//
// Parameters:
//   RDI = ctx     — pointer to user task's CpuContext
//   RSI = user_sp — user-space stack pointer
//
// IRETQ expects the following stack layout (pushed in order, top = lowest addr):
//   [RSP+0]  RIP     — user entry point
//   [RSP+8]  CS      — user code segment selector (0x1B = ring 3, GDT index 3)
//   [RSP+16] RFLAGS  — 0x202 = IF (interrupt enable) set
//   [RSP+24] RSP     — user stack pointer
//   [RSP+32] SS      — user stack segment selector (0x23 = ring 3, GDT index 4)
//
// GDT segment selectors:
//   0x1B = (3 << 3) | 3  — user code segment, RPL=3
//   0x23 = (4 << 3) | 3  — user data segment, RPL=3
// =============================================================================
.global switch_to_user
.type switch_to_user, @function
switch_to_user:
    // Build IRETQ frame on the kernel stack
    pushq   $0x23                       // SS  (user data segment)
    pushq   %rsi                        // RSP (user stack pointer)
    pushq   $0x202                      // RFLAGS (IF=1, reserved bit 1 = 1)
    pushq   $0x1B                       // CS  (user code segment)
    movq    136(%rdi), %rax             // ctx->pc = user entry point
    pushq   %rax                        // RIP

    // Set all data segment registers to user data segment
    movw    $0x23, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs

    // Restore general-purpose registers from ctx
    movq    0(%rdi), %rax
    movq    8(%rdi), %rbx
    movq    16(%rdi), %rcx
    movq    24(%rdi), %rdx
    movq    32(%rdi), %rsi
    movq    48(%rdi), %rbp
    movq    64(%rdi), %r8
    movq    72(%rdi), %r9
    movq    80(%rdi), %r10
    movq    88(%rdi), %r11
    movq    96(%rdi), %r12
    movq    104(%rdi), %r13
    movq    112(%rdi), %r14
    movq    120(%rdi), %r15
    movq    40(%rdi), %rdi              // restore RDI last (was base pointer)

    // Atomically switch to ring 3: pops RIP, CS, RFLAGS, RSP, SS
    iretq

.size switch_to_user, .-switch_to_user

// =============================================================================
// kernel_thread_entry — trampoline for newly created kernel threads
// =============================================================================
//
// The thread creator sets up the context so that when context_switch returns,
// it "returns" to this trampoline with:
//   RDI = function pointer
//   RSI = argument
//
// Flow:
//   1. Save func in R11, move arg to RDI (first C argument)
//   2. Call func(arg)
//   3. If func returns, invoke sys_exit(0) via SYSCALL
// =============================================================================
.global kernel_thread_entry
.type kernel_thread_entry, @function
kernel_thread_entry:
    movq    %rdi, %r11                  // save function pointer
    movq    %rsi, %rdi                  // arg → first C argument
    callq   *%r11                       // call func(arg)

    // Thread returned — invoke exit (should not normally reach here)
    movq    $60, %rax                   // x86_64 exit syscall number
    movq    $0, %rdi                    // exit code = 0
    syscall

.size kernel_thread_entry, .-kernel_thread_entry
