// =============================================================================
// x64 Context Switch Implementation
// =============================================================================
//
// CpuContext structure layout (defined in moss.process module):
//   Offset  Size   Field
//   ------  ----   -----
//   0x00    8      rax
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
//   0xA0    512    fp        (16-byte-aligned FXSAVE64 image)
//   0x2A0   8      fs_base   (userspace thread pointer)
// Numeric memory operands are byte offsets checked by CpuContext's C++
// static assertions. The 512-byte FP image starts at aligned offset 160;
// its end at 672 is the FS-base field, so layout edits must update assembly.
//
// x64 calling convention (System V AMD64 ABI):
//   Arguments: RDI, RSI, RDX, RCX, R8, R9, [stack]
//   Callee-saved: RBX, RBP, R12-R15
//   Return value: RAX
// =============================================================================

#include "moss/trap_frame_offsets.h"
.section .text

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
    fxsave64 160(%rdi)

    // Save all general-purpose registers into prev CpuContext
    movq    %rax, 0(%rdi)              // prev->rax
    movq    %rbx, 8(%rdi)              // prev->rbx
    movq    %rcx, 16(%rdi)             // prev->rcx
    movq    %rdx, 24(%rdi)             // prev->rdx
    movq    %rsi, 32(%rdi)             // prev->rsi
    movq    %rdi, 40(%rdi)             // prev->rdi
    movq    %rbp, 48(%rdi)             // prev->rbp
    // CALL pushed one 8-byte return address. Save SP above it because resume
    // pushes prev->pc again; retaining the old slot would shift the stack.
    leaq    8(%rsp), %rax              // caller SP after this call returns
    movq    %rax, 56(%rdi)
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

    // Preserve the userspace thread pointer across every scheduler transition.
    // Architectural MSR identifier IA32_FS_BASE; RDMSR/WRMSR split a 64-bit
    // base between EDX:EAX, hence the 32-bit shifts in all three paths.
    movl    $0xC0000100, %ecx           // IA32_FS_BASE
    rdmsr
    shlq    $32, %rdx
    orq     %rdx, %rax
    movq    %rax, 672(%rdi)

    // ---- Phase 2: Restore next context ----
    movq    672(%rsi), %rax
    movq    %rax, %rdx
    shrq    $32, %rdx
    movl    $0xC0000100, %ecx
    wrmsr
    fxrstor64 160(%rsi)

    // Restore RFLAGS from next->pstate
    movq    128(%rsi), %rax
    // -513 is ~(1<<9): clear RFLAGS.IF until both stack and registers belong
    // to next. Only the completed scheduler/user-return path may restore IF.
    andq    $-513, %rax                // caller reenables interrupts after the switch
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
    pushq   136(%rsi)

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
// GDT layout: [3]=User Data(0x1B), [4]=User Code(0x23).
// Selectors encode (GDT index<<3)|RPL: 3*8+3 and 4*8+3 request ring 3;
// these constants must agree with the boot GDT, not with a process address.
// =============================================================================
.global switch_to_user
.type switch_to_user, @function
switch_to_user:
    // First entry / exec must install this process's state, not the last owner.
    fxrstor64 160(%rdi)
    movq    672(%rdi), %rax             // fresh exec clears FS; fork installs its inherited value
    movq    %rax, %rdx
    shrq    $32, %rdx
    movl    $0xC0000100, %ecx
    wrmsr
    // Build IRETQ frame on the kernel stack
    pushq   $0x1B                       // SS  (user data segment, RPL=3)
    pushq   %rsi                        // RSP (user stack pointer)
    movq    128(%rdi), %rax
    andq    $MOSS_USER_STATUS_MASK, %rax
    orq     $MOSS_USER_STATUS_FIXED, %rax
    pushq   %rax                        // preserve user condition codes across fork
    pushq   $0x23                       // CS  (user code segment, RPL=3)
    movq    136(%rdi), %rax             // ctx->pc = user entry point
    pushq   %rax                        // RIP

    // Set all data segment registers to user data segment
    movw    $0x1B, %ax
    movw    %ax, %ds
    movw    %ax, %es

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
.global kernel_thread_entry
.type kernel_thread_entry, @function
kernel_thread_entry:
    movq    %rdi, %r11                  // save function pointer
    movq    %rsi, %rdi                  // arg → first C argument
    callq   *%r11                       // call func(arg)

    // Thread returned — invoke exit
    movq    $60, %rax                   // x64 exit syscall number
    movq    $0, %rdi                    // exit code = 0
    syscall

.size kernel_thread_entry, .-kernel_thread_entry

// Prevent executable stack warning from the GNU linker
.section .note.GNU-stack,"",@progbits
