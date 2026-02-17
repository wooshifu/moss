// =============================================================================
// ARM64 System Call Entry Point
// =============================================================================
//
// This file implements the low-level system call entry/exit path for AArch64.
//
// Execution flow:
//   1. User-space executes `svc #0` → CPU takes Synchronous exception to EL1
//   2. Exception vector (in start_arm64.S) routes SVC to lower_el_sync_dispatch
//      which calls system_call_handler directly.  This file provides the LEGACY
//      entry point (syscall_entry_point) used when the kernel calls it from
//      the exception vector table's same-EL synchronous handler.
//   3. syscall_entry_point saves ALL general-purpose registers (x0-x29, LR),
//      remaps arguments from the user ABI to the C calling convention, and
//      calls `system_call_handler(nr, a0, a1, a2, a3, a4, a5)`.
//   4. On return, the syscall return value (in x0) is patched into the saved
//      register frame, all registers are restored, and `eret` returns to EL0.
//
// ARM64 system call ABI (Linux-compatible):
//   x8        = system call number
//   x0 - x5   = arguments 0 - 5
//   Return value delivered in x0
//
// C handler prototype (declared in moss.kernel module):
//   extern "C" int64_t system_call_handler(
//       uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
//       uint64_t a3, uint64_t a4, uint64_t a5);
//
// syscall_return:
//   Helper called from C++ to extract the return value from a SyscallContext
//   structure and place it in x0 for the caller.
//
// Stack frame layout (grows downward from SP on entry):
//   SP+0x00 : x0  (also receives return value after handler returns)
//   SP+0x08 : x1, x2
//   SP+0x18 : x3, x4
//   SP+0x28 : x5, x6
//   SP+0x38 : x7, x8  (x8 = syscall number)
//   SP+0x48 : x9, x10
//   ...
//   SP+0xE0 : x29 (FP), x30 (LR)
//
// NOTE: The primary syscall path in MOSS now goes through
//       lower_el_sync_dispatch (start_arm64.S), which provides a cleaner
//       trap frame.  This file is retained for compatibility and as the
//       implementation of the `syscall_entry_point` / `syscall_return`
//       symbols referenced by other kernel modules.
// =============================================================================

.section .text

.global syscall_entry_point
.global syscall_return

// =============================================================================
// syscall_entry_point — legacy same-EL system call handler
// =============================================================================
syscall_entry_point:
    // ---- Save frame pointer and link register (outermost pair) ----
    stp     x29, x30, [sp, #-16]!

    // ---- Early diagnostic: write "SYS!\n" to PL011 UART at 0x0900_0000 ----
    //      Useful during bringup to confirm the entry point is reached.
    mov     x10, #0x09000000            // QEMU virt PL011 UART base address
    mov     x11, #'S'
    str     w11, [x10]
    mov     x11, #'Y'
    str     w11, [x10]
    mov     x11, #'S'
    str     w11, [x10]
    mov     x11, #'!'
    str     w11, [x10]
    mov     x11, #'\n'
    str     w11, [x10]

    // ---- Save callee-saved and caller-saved general-purpose registers ----
    //      Registers are pushed in descending order so that x0 ends up at
    //      the lowest address (SP after all pushes).  Each `stp` decrements
    //      SP by 16.
    stp     x27, x28, [sp, #-16]!
    stp     x25, x26, [sp, #-16]!
    stp     x23, x24, [sp, #-16]!
    stp     x21, x22, [sp, #-16]!
    stp     x19, x20, [sp, #-16]!
    stp     x17, x18, [sp, #-16]!
    stp     x15, x16, [sp, #-16]!
    stp     x13, x14, [sp, #-16]!
    stp     x11, x12, [sp, #-16]!
    stp     x9, x10, [sp, #-16]!
    stp     x7, x8, [sp, #-16]!        // x8 = syscall number
    stp     x5, x6, [sp, #-16]!
    stp     x3, x4, [sp, #-16]!
    stp     x1, x2, [sp, #-16]!
    str     x0, [sp, #-8]!             // x0 = first arg / return value slot

    // ---- Data synchronization barrier ----
    //      Ensures all register stores above are visible before we read them
    //      back to remap arguments.
    dsb     sy

    // ---- Remap user-ABI registers → C calling convention ----
    //
    //  C prototype: system_call_handler(nr, a0, a1, a2, a3, a4, a5)
    //                                   x0  x1  x2  x3  x4  x5  x6
    //
    //  User ABI:    x8=nr, x0=a0, x1=a1, x2=a2, x3=a3, x4=a4, x5=a5
    //
    //  Stack offsets after the save sequence above:
    //    SP+0   = x0 (user a0)
    //    SP+8   = x1 (user a1)
    //    SP+16  = x2 (user a2)
    //    SP+24  = x3 (user a3)
    //    SP+32  = x4 (user a4)
    //    SP+40  = x5 (user a5)
    //    SP+64  = x8 (syscall number)
    ldr     x1, [sp]                    // C arg1 ← user x0 (a0)
    ldr     x2, [sp, #8]               // C arg2 ← user x1 (a1)
    ldr     x3, [sp, #16]              // C arg3 ← user x2 (a2)
    ldr     x7, [sp, #24]              // temp   ← user x3 (a3)
    ldr     x6, [sp, #32]              // C arg6 ← user x4 (a4)
    ldr     x5, [sp, #40]              // C arg5 ← user x5 (a5)
    mov     x4, x7                     // C arg4 ← user x3 (a3)
    ldr     x0, [sp, #64]              // C arg0 ← user x8 (syscall number)

    // ---- Call the C system call dispatcher ----
    bl      system_call_handler

    // ---- Patch return value into saved x0 slot on the stack ----
    //      When registers are restored below, x0 will contain the return value.
    str     x0, [sp]

    // ---- Data synchronization barrier before register restore ----
    dsb     sy

    // ---- Restore all general-purpose registers (reverse of save order) ----
    ldr     x0, [sp], #8               // x0 (contains return value)
    ldp     x1, x2, [sp], #16
    ldp     x3, x4, [sp], #16
    ldp     x5, x6, [sp], #16
    ldp     x7, x8, [sp], #16          // x8 = original syscall number
    ldp     x9, x10, [sp], #16
    ldp     x11, x12, [sp], #16
    ldp     x13, x14, [sp], #16
    ldp     x15, x16, [sp], #16
    ldp     x17, x18, [sp], #16
    ldp     x19, x20, [sp], #16
    ldp     x21, x22, [sp], #16
    ldp     x23, x24, [sp], #16
    ldp     x25, x26, [sp], #16
    ldp     x27, x28, [sp], #16
    ldp     x29, x30, [sp], #16        // FP, LR

    // ---- Return to user-space via exception return ----
    //      SPSR_EL1 = 0x0 → EL0t (user mode, all interrupts enabled)
    mov     x2, #0x0
    msr     spsr_el1, x2

    //      ELR_EL1 already points to the instruction after the `svc` that
    //      triggered this exception (CPU advances ELR automatically for SVC).
    mrs     x2, elr_el1
    msr     elr_el1, x2

    //      eret: atomically restores PSTATE from SPSR_EL1 and branches to
    //      the address in ELR_EL1, dropping back to EL0.
    eret

// =============================================================================
// syscall_return — extract return value from SyscallContext*
// =============================================================================
//
// Called from C++ when the kernel needs to return a value through a
// SyscallContext structure rather than the normal x0 path.
//
// Prototype: extern "C" void syscall_return(void* context) noexcept;
//
// SyscallContext layout (relevant field):
//   offset 56 (0x38): ret_value (uint64_t)
//
// Input:   x0 = pointer to SyscallContext (may be nullptr)
// Output:  x0 = context->ret_value, or -1 on nullptr
// =============================================================================
syscall_return:
    cbz     x0, .return_error           // nullptr guard

    ldr     x1, [x0, #56]              // x1 = context->ret_value
    mov     x0, x1                      // return value in x0
    ret

.return_error:
    mov     x0, #-1                     // error sentinel
    ret

// Prevent executable stack warning from the GNU linker
.section .note.GNU-stack,"",%progbits
