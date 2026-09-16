// =============================================================================
// x64 Interrupt Service Routine Stubs
// =============================================================================
//
// Provides 256 ISR entry points for the IDT.  Each stub pushes a uniform
// frame (vector number + error code) then jumps to isr_common, which saves
// all GPRs and calls the C++ handler:
//
//   extern "C" void x64_interrupt_handler(
//       uint64_t vector, uint64_t error_code, void* frame);
//
// CPU exceptions that push an error code: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30.
// All other vectors get a dummy zero error code pushed for uniform layout.
//
// Stack frame after isr_common saves (RSP-relative, 15 GPRs x 8 = 120 bytes):
//   [RSP +   0] = R15
//   [RSP +   8] = R14  ...
//   [RSP + 112] = RAX
//   [RSP + 120] = vector number  (pushed by stub)
//   [RSP + 128] = error code     (pushed by CPU or stub)
//   [RSP + 136] = RIP            (pushed by CPU on interrupt)
//   [RSP + 144] = CS
//   [RSP + 152] = RFLAGS
//   [RSP + 160] = RSP            (if privilege change)
//   [RSP + 168] = SS             (if privilege change)
// =============================================================================

#include "moss/trap_frame_offsets.h"
.section .text

// Enable alternate macro mode for .rept variable expansion
.altmacro

// ---------------------------------------------------------------------------
// Macros: ISR stubs without / with hardware error code
// ---------------------------------------------------------------------------
.macro ISR_NOERR vector
.global isr_stub_\vector
.type isr_stub_\vector, @function
isr_stub_\vector:
    pushq   $0                          // dummy error code
    pushq   $\vector                    // vector number
    jmp     isr_common
.size isr_stub_\vector, . - isr_stub_\vector
.endm

.macro ISR_ERR vector
.global isr_stub_\vector
.type isr_stub_\vector, @function
isr_stub_\vector:
    // CPU already pushed the error code
    pushq   $\vector                    // vector number
    jmp     isr_common
.size isr_stub_\vector, . - isr_stub_\vector
.endm

// Helper macro for generating ISR stubs in .rept loops (altmacro mode)
.macro MAKE_ISR_NOERR n
    ISR_NOERR %n
.endm

// ---------------------------------------------------------------------------
// CPU Exceptions (vectors 0-31)
// ---------------------------------------------------------------------------

// Exceptions without error code
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7

// Exception 8: Double Fault (error code)
ISR_ERR 8

// Exception 9: Coprocessor Segment Overrun (legacy, no error code)
ISR_NOERR 9

// Exceptions 10-14: all push error code
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14

// Exceptions 15-16: no error code
ISR_NOERR 15
ISR_NOERR 16

// Exception 17: Alignment Check (error code)
ISR_ERR 17

// Exceptions 18-20: no error code
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20

// Exception 21: Control Protection (error code)
ISR_ERR 21

// Exceptions 22-28: reserved, no error code
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28

// Exception 29: VMX exception (error code)
ISR_ERR 29

// Exception 30: Security Exception (error code)
ISR_ERR 30

// Exception 31: reserved
ISR_NOERR 31

// ---------------------------------------------------------------------------
// IRQs and software vectors (32-255): no error code
// ---------------------------------------------------------------------------
.set _isr_i, 32
.rept 224                         // 256 IDT vectors - 32 reserved CPU exception vectors
    MAKE_ISR_NOERR _isr_i
    .set _isr_i, _isr_i + 1
.endr

// ---------------------------------------------------------------------------
// isr_common — shared ISR body
// ---------------------------------------------------------------------------
.type isr_common, @function
isr_common:
    // Save all GPRs (15 x 8 = 120 bytes)
    pushq   %rax
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    // Call C++ handler: x64_interrupt_handler(vector, error_code, frame)
    cld
    movq    MOSS_TF_VECTOR(%rsp), %rdi
    movq    MOSS_TF_ERROR(%rsp), %rsi
    movq    %rsp, %rdx                  // arg2: pointer to saved register frame
    // The interrupted kernel stack need not have call-site alignment.
    movq    %rsp, %rbx
    andq    $-16, %rsp
    call    x64_interrupt_handler
    cli
    movq    %rbx, %rdi
    call    user_return_handler
    movq    %rbx, %rsp

    // Restore all GPRs (reverse order)
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbx
    popq    %rax

    // Drop two software/CPU metadata slots (2 * 8=16 bytes), leaving exactly
    // the hardware return frame for IRETQ. Keep this paired with ISR_* pushes.
    addq    $16, %rsp

    // Return from interrupt
    iretq
.size isr_common, . - isr_common

// ---------------------------------------------------------------------------
// isr_stub_table — address table for C++ IDT setup
// ---------------------------------------------------------------------------
// Array of 256 function pointers, used by setup_idt() in boot_impl.cpp.
// ---------------------------------------------------------------------------
.macro STUB_ENTRY n
    .quad isr_stub_\n
.endm

.section ".rodata", "a"
.align 8
.global isr_stub_table
isr_stub_table:
.set _tbl_i, 0
.rept 256
    STUB_ENTRY %_tbl_i
    .set _tbl_i, _tbl_i + 1
.endr

// Prevent executable stack warning
.section .note.GNU-stack,"",@progbits
