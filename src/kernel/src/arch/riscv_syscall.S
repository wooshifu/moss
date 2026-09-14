// RV64 native exception frame, shared by syscall, IRQ and fault paths.
// sscratch is the entry-stack pointer in U-mode and zero in S-mode.
#include "moss/trap_frame_offsets.h"

.section .text
.balign 4
.global syscall_entry_point
.type syscall_entry_point, @function
syscall_entry_point:
    csrrw sp, sscratch, sp
    bnez sp, .Lstack_ready
    csrrw sp, sscratch, sp
.Lstack_ready:
    addi sp, sp, -MOSS_TF_SIZE
    sd ra, MOSS_TF_RA(sp)
    sd t0, MOSS_TF_T0(sp)
    sd t1, MOSS_TF_T1(sp)
    sd t2, MOSS_TF_T2(sp)
    sd t3, MOSS_TF_T3(sp)
    sd t4, MOSS_TF_T4(sp)
    sd t5, MOSS_TF_T5(sp)
    sd t6, MOSS_TF_T6(sp)
    sd a0, MOSS_TF_A0(sp)
    sd a1, MOSS_TF_A1(sp)
    sd a2, MOSS_TF_A2(sp)
    sd a3, MOSS_TF_A3(sp)
    sd a4, MOSS_TF_A4(sp)
    sd a5, MOSS_TF_A5(sp)
    sd a6, MOSS_TF_A6(sp)
    sd a7, MOSS_TF_A7(sp)
    sd s0, MOSS_TF_S0(sp)
    sd s1, MOSS_TF_S1(sp)
    sd s2, MOSS_TF_S2(sp)
    sd s3, MOSS_TF_S3(sp)
    sd s4, MOSS_TF_S4(sp)
    sd s5, MOSS_TF_S5(sp)
    sd s6, MOSS_TF_S6(sp)
    sd s7, MOSS_TF_S7(sp)
    sd s8, MOSS_TF_S8(sp)
    sd s9, MOSS_TF_S9(sp)
    sd s10, MOSS_TF_S10(sp)
    sd s11, MOSS_TF_S11(sp)
    sd tp, MOSS_TF_TP(sp)
    sd gp, MOSS_TF_GP(sp)
    csrr t0, sepc
    sd t0, MOSS_TF_PC(sp)
    csrr t0, sstatus
    sd t0, MOSS_TF_STATUS(sp)
    // Use the architectural origin, not the untrusted user SP (which can be 0).
    andi t1, t0, 256
    bnez t1, .Lkernel_identity
    ld tp, MOSS_TF_SIZE(sp)
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
.Lkernel_identity:
    csrr t0, sscratch
    beqz t1, .Lsave_sp
    addi t0, sp, MOSS_TF_SIZE
.Lsave_sp:
    sd t0, MOSS_TF_SP(sp)
    csrw sscratch, zero
    csrr t0, scause
    sd t0, MOSS_TF_CAUSE(sp)
    sd zero, MOSS_TF_RESERVED0(sp)
    sd zero, MOSS_TF_RESERVED1(sp)
    // C fault/syscall/return handlers may access validated user memory.
    li t1, (1 << 18)
    csrs sstatus, t1
    bltz t0, .Linterrupt
    li t1, 8
    beq t0, t1, .Lsyscall
    li t1, 12
    beq t0, t1, .Lfault
    li t1, 13
    beq t0, t1, .Lfault
    li t1, 15
    beq t0, t1, .Lfault
    mv a0, t0
    ld a1, MOSS_TF_PC(sp)
    csrr a2, stval
    call riscv_exception_handler
.Lhalt:
    wfi
    j .Lhalt
.Lfault:
    mv a0, t0
    csrr a1, stval
    ld a2, MOSS_TF_PC(sp)
    call riscv_page_fault_handler
    j .Lreturn
.Lsyscall:
    ld t0, MOSS_TF_PC(sp)
    addi t0, t0, 4
    sd t0, MOSS_TF_PC(sp)
    mv a0, sp
    call system_call_handler
    j .Lreturn
.Linterrupt:
    slli t0, t0, 1
    srli t0, t0, 1
    li t1, 5
    beq t0, t1, .Ltimer
    li t1, 9
    beq t0, t1, .Lexternal
    li t1, 1
    bne t0, t1, .Lreturn
    csrci sip, 2
    j .Lreturn
.Ltimer:
    call riscv_timer_handler
    j .Lreturn
.Lexternal:
    call riscv_external_handler
.Lreturn:
    csrci sstatus, 2
    mv a0, sp
    call user_return_handler
    ld t0, MOSS_TF_PC(sp)
    csrw sepc, t0
    ld t0, MOSS_TF_STATUS(sp)
    csrw sstatus, t0
    andi t0, t0, 256
    bnez t0, .Lreturn_kernel
    ld t0, MOSS_TF_SP(sp)
    csrw sscratch, t0
    ld ra, MOSS_TF_RA(sp)
    ld t0, MOSS_TF_T0(sp)
    ld t1, MOSS_TF_T1(sp)
    ld t2, MOSS_TF_T2(sp)
    ld t3, MOSS_TF_T3(sp)
    ld t4, MOSS_TF_T4(sp)
    ld t5, MOSS_TF_T5(sp)
    ld t6, MOSS_TF_T6(sp)
    ld a0, MOSS_TF_A0(sp)
    ld a1, MOSS_TF_A1(sp)
    ld a2, MOSS_TF_A2(sp)
    ld a3, MOSS_TF_A3(sp)
    ld a4, MOSS_TF_A4(sp)
    ld a5, MOSS_TF_A5(sp)
    ld a6, MOSS_TF_A6(sp)
    ld a7, MOSS_TF_A7(sp)
    ld s0, MOSS_TF_S0(sp)
    ld s1, MOSS_TF_S1(sp)
    ld s2, MOSS_TF_S2(sp)
    ld s3, MOSS_TF_S3(sp)
    ld s4, MOSS_TF_S4(sp)
    ld s5, MOSS_TF_S5(sp)
    ld s6, MOSS_TF_S6(sp)
    ld s7, MOSS_TF_S7(sp)
    ld s8, MOSS_TF_S8(sp)
    ld s9, MOSS_TF_S9(sp)
    ld s10, MOSS_TF_S10(sp)
    ld s11, MOSS_TF_S11(sp)
    ld tp, MOSS_TF_TP(sp)
    ld gp, MOSS_TF_GP(sp)
    addi sp, sp, MOSS_TF_SIZE
    csrrw sp, sscratch, sp
    sret
.Lreturn_kernel:
    ld ra, MOSS_TF_RA(sp)
    ld t0, MOSS_TF_T0(sp)
    ld t1, MOSS_TF_T1(sp)
    ld t2, MOSS_TF_T2(sp)
    ld t3, MOSS_TF_T3(sp)
    ld t4, MOSS_TF_T4(sp)
    ld t5, MOSS_TF_T5(sp)
    ld t6, MOSS_TF_T6(sp)
    ld a0, MOSS_TF_A0(sp)
    ld a1, MOSS_TF_A1(sp)
    ld a2, MOSS_TF_A2(sp)
    ld a3, MOSS_TF_A3(sp)
    ld a4, MOSS_TF_A4(sp)
    ld a5, MOSS_TF_A5(sp)
    ld a6, MOSS_TF_A6(sp)
    ld a7, MOSS_TF_A7(sp)
    ld s0, MOSS_TF_S0(sp)
    ld s1, MOSS_TF_S1(sp)
    ld s2, MOSS_TF_S2(sp)
    ld s3, MOSS_TF_S3(sp)
    ld s4, MOSS_TF_S4(sp)
    ld s5, MOSS_TF_S5(sp)
    ld s6, MOSS_TF_S6(sp)
    ld s7, MOSS_TF_S7(sp)
    ld s8, MOSS_TF_S8(sp)
    ld s9, MOSS_TF_S9(sp)
    ld s10, MOSS_TF_S10(sp)
    ld s11, MOSS_TF_S11(sp)
    ld tp, MOSS_TF_TP(sp)
    ld gp, MOSS_TF_GP(sp)
    addi sp, sp, MOSS_TF_SIZE
    sret
.size syscall_entry_point, . - syscall_entry_point

.global syscall_return
.type syscall_return, @function
syscall_return:
    beqz a0, .Lreturn_error
    ld a0, 56(a0)
    ret
.Lreturn_error:
    li a0, -1
    ret
.size syscall_return, . - syscall_return
.section .note.GNU-stack,"",@progbits
