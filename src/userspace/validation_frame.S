// Execute real syscalls with every preservable GP register seeded. The
// function restores its C caller's registers and returns the syscall result,
// or -(1000 + register index) on corruption. Fork executes the same checks in
// both processes, before any exec or userspace C code can hide the difference.
.text
.global frame_register_probe
.type frame_register_probe, @function
frame_register_probe:
#if defined(__aarch64__)
    sub sp, sp, #272
    str x0, [sp, #0]
    str x1, [sp, #8]
    str x2, [sp, #16]
    str x3, [sp, #24]
    str x4, [sp, #32]
    str x5, [sp, #40]
    str x6, [sp, #48]
    str x7, [sp, #56]
    str x8, [sp, #64]
    str x9, [sp, #72]
    str x10, [sp, #80]
    str x11, [sp, #88]
    str x12, [sp, #96]
    str x13, [sp, #104]
    str x14, [sp, #112]
    str x15, [sp, #120]
    str x16, [sp, #128]
    str x17, [sp, #136]
    str x18, [sp, #144]
    str x19, [sp, #152]
    str x20, [sp, #160]
    str x21, [sp, #168]
    str x22, [sp, #176]
    str x23, [sp, #184]
    str x24, [sp, #192]
    str x25, [sp, #200]
    str x26, [sp, #208]
    str x27, [sp, #216]
    str x28, [sp, #224]
    str x29, [sp, #232]
    str x30, [sp, #240]
    mov x0, #256
    mov x1, #257
    mov x2, #258
    mov x3, #259
    mov x4, #260
    mov x5, #261
    mov x6, #262
    mov x7, #263
    mov x8, #264
    mov x9, #265
    mov x10, #266
    mov x11, #267
    mov x12, #268
    mov x13, #269
    mov x14, #270
    mov x15, #271
    mov x16, #272
    mov x17, #273
    mov x18, #274
    mov x19, #275
    mov x20, #276
    mov x21, #277
    mov x22, #278
    mov x23, #279
    mov x24, #280
    mov x25, #281
    mov x26, #282
    mov x27, #283
    mov x28, #284
    mov x29, #285
    mov x30, #286
    ldr x8, [sp, #0]
    cmp x8, #14
    b.ne .Larm_no_signal
    mov x1, #10
    mov x0, #1
    b .Larm_set_flags
.Larm_no_signal:
    mov x0, #256
.Larm_set_flags:
    str x0, [sp, #256]
    mov x0, #0xa0000000
    msr nzcv, x0
    ldr x0, [sp, #256]
    svc #0
    str x0, [sp, #248]
    mrs x0, nzcv
    lsr x0, x0, #28
    cmp x0, #10
    b.ne .Lbad_arm_flags
    ldr x0, [sp, #0]
    cmp x0, #14
    b.eq .Larm_signal_arg
    mov x0, #257
    b .Larm_check_arg
.Larm_signal_arg:
    mov x0, #10
.Larm_check_arg:
    cmp x1, x0
    b.ne .Lbad_arm_1
    mov x0, #258
    cmp x2, x0
    b.ne .Lbad_arm_2
    mov x0, #259
    cmp x3, x0
    b.ne .Lbad_arm_3
    mov x0, #260
    cmp x4, x0
    b.ne .Lbad_arm_4
    mov x0, #261
    cmp x5, x0
    b.ne .Lbad_arm_5
    mov x0, #262
    cmp x6, x0
    b.ne .Lbad_arm_6
    mov x0, #263
    cmp x7, x0
    b.ne .Lbad_arm_7
    ldr x0, [sp, #0]
    cmp x8, x0
    b.ne .Lbad_arm_8
    mov x0, #265
    cmp x9, x0
    b.ne .Lbad_arm_9
    mov x0, #266
    cmp x10, x0
    b.ne .Lbad_arm_10
    mov x0, #267
    cmp x11, x0
    b.ne .Lbad_arm_11
    mov x0, #268
    cmp x12, x0
    b.ne .Lbad_arm_12
    mov x0, #269
    cmp x13, x0
    b.ne .Lbad_arm_13
    mov x0, #270
    cmp x14, x0
    b.ne .Lbad_arm_14
    mov x0, #271
    cmp x15, x0
    b.ne .Lbad_arm_15
    mov x0, #272
    cmp x16, x0
    b.ne .Lbad_arm_16
    mov x0, #273
    cmp x17, x0
    b.ne .Lbad_arm_17
    mov x0, #274
    cmp x18, x0
    b.ne .Lbad_arm_18
    mov x0, #275
    cmp x19, x0
    b.ne .Lbad_arm_19
    mov x0, #276
    cmp x20, x0
    b.ne .Lbad_arm_20
    mov x0, #277
    cmp x21, x0
    b.ne .Lbad_arm_21
    mov x0, #278
    cmp x22, x0
    b.ne .Lbad_arm_22
    mov x0, #279
    cmp x23, x0
    b.ne .Lbad_arm_23
    mov x0, #280
    cmp x24, x0
    b.ne .Lbad_arm_24
    mov x0, #281
    cmp x25, x0
    b.ne .Lbad_arm_25
    mov x0, #282
    cmp x26, x0
    b.ne .Lbad_arm_26
    mov x0, #283
    cmp x27, x0
    b.ne .Lbad_arm_27
    mov x0, #284
    cmp x28, x0
    b.ne .Lbad_arm_28
    mov x0, #285
    cmp x29, x0
    b.ne .Lbad_arm_29
    mov x0, #286
    cmp x30, x0
    b.ne .Lbad_arm_30
    b .Ldone_arm
.Lbad_arm_1:
    mov x0, #-1001
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_2:
    mov x0, #-1002
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_3:
    mov x0, #-1003
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_4:
    mov x0, #-1004
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_5:
    mov x0, #-1005
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_6:
    mov x0, #-1006
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_7:
    mov x0, #-1007
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_8:
    mov x0, #-1008
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_9:
    mov x0, #-1009
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_10:
    mov x0, #-1010
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_11:
    mov x0, #-1011
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_12:
    mov x0, #-1012
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_13:
    mov x0, #-1013
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_14:
    mov x0, #-1014
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_15:
    mov x0, #-1015
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_16:
    mov x0, #-1016
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_17:
    mov x0, #-1017
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_18:
    mov x0, #-1018
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_19:
    mov x0, #-1019
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_20:
    mov x0, #-1020
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_21:
    mov x0, #-1021
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_22:
    mov x0, #-1022
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_23:
    mov x0, #-1023
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_24:
    mov x0, #-1024
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_25:
    mov x0, #-1025
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_26:
    mov x0, #-1026
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_27:
    mov x0, #-1027
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_28:
    mov x0, #-1028
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_29:
    mov x0, #-1029
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_30:
    mov x0, #-1030
    str x0, [sp, #248]
    b .Ldone_arm
.Lbad_arm_flags:
    mov x0, #-1099
    str x0, [sp, #248]
.Ldone_arm:
    ldr x1, [sp, #8]
    ldr x2, [sp, #16]
    ldr x3, [sp, #24]
    ldr x4, [sp, #32]
    ldr x5, [sp, #40]
    ldr x6, [sp, #48]
    ldr x7, [sp, #56]
    ldr x8, [sp, #64]
    ldr x9, [sp, #72]
    ldr x10, [sp, #80]
    ldr x11, [sp, #88]
    ldr x12, [sp, #96]
    ldr x13, [sp, #104]
    ldr x14, [sp, #112]
    ldr x15, [sp, #120]
    ldr x16, [sp, #128]
    ldr x17, [sp, #136]
    ldr x18, [sp, #144]
    ldr x19, [sp, #152]
    ldr x20, [sp, #160]
    ldr x21, [sp, #168]
    ldr x22, [sp, #176]
    ldr x23, [sp, #184]
    ldr x24, [sp, #192]
    ldr x25, [sp, #200]
    ldr x26, [sp, #208]
    ldr x27, [sp, #216]
    ldr x28, [sp, #224]
    ldr x29, [sp, #232]
    ldr x30, [sp, #240]
    ldr x0, [sp, #248]
    add sp, sp, #272
    ret
#elif defined(__riscv)
    addi sp, sp, -272
    sd x1, 8(sp)
    sd x3, 24(sp)
    sd x4, 32(sp)
    sd x5, 40(sp)
    sd x6, 48(sp)
    sd x7, 56(sp)
    sd x8, 64(sp)
    sd x9, 72(sp)
    sd x10, 80(sp)
    sd x11, 88(sp)
    sd x12, 96(sp)
    sd x13, 104(sp)
    sd x14, 112(sp)
    sd x15, 120(sp)
    sd x16, 128(sp)
    sd x17, 136(sp)
    sd x18, 144(sp)
    sd x19, 152(sp)
    sd x20, 160(sp)
    sd x21, 168(sp)
    sd x22, 176(sp)
    sd x23, 184(sp)
    sd x24, 192(sp)
    sd x25, 200(sp)
    sd x26, 208(sp)
    sd x27, 216(sp)
    sd x28, 224(sp)
    sd x29, 232(sp)
    sd x30, 240(sp)
    sd x31, 248(sp)
    li x1, 257
    li x3, 259
    li x4, 260
    li x5, 261
    li x6, 262
    li x7, 263
    li x8, 264
    li x9, 265
    li x10, 266
    li x11, 267
    li x12, 268
    li x13, 269
    li x14, 270
    li x15, 271
    li x16, 272
    li x17, 273
    li x18, 274
    li x19, 275
    li x20, 276
    li x21, 277
    li x22, 278
    li x23, 279
    li x24, 280
    li x25, 281
    li x26, 282
    li x27, 283
    li x28, 284
    li x29, 285
    li x30, 286
    li x31, 287
    ld a7, 80(sp)
    li a0, 14
    bne a7, a0, .Lrv_no_signal
    li a0, 1
    li a1, 10
    j .Lrv_call
.Lrv_no_signal:
    li a0, 266
.Lrv_call:
    ecall
    sd a0, 256(sp)
    li a0, 257
    bne x1, a0, .Lbad_rv_1
    li a0, 259
    bne x3, a0, .Lbad_rv_3
    li a0, 260
    bne x4, a0, .Lbad_rv_4
    li a0, 261
    bne x5, a0, .Lbad_rv_5
    li a0, 262
    bne x6, a0, .Lbad_rv_6
    li a0, 263
    bne x7, a0, .Lbad_rv_7
    li a0, 264
    bne x8, a0, .Lbad_rv_8
    li a0, 265
    bne x9, a0, .Lbad_rv_9
    ld a0, 80(sp)
    addi a0, a0, -14
    beqz a0, .Lrv_signal_arg
    li a0, 267
    j .Lrv_check_arg
.Lrv_signal_arg:
    li a0, 10
.Lrv_check_arg:
    bne x11, a0, .Lbad_rv_11
    li a0, 268
    bne x12, a0, .Lbad_rv_12
    li a0, 269
    bne x13, a0, .Lbad_rv_13
    li a0, 270
    bne x14, a0, .Lbad_rv_14
    li a0, 271
    bne x15, a0, .Lbad_rv_15
    li a0, 272
    bne x16, a0, .Lbad_rv_16
    ld a0, 80(sp)
    bne x17, a0, .Lbad_rv_17
    li a0, 274
    bne x18, a0, .Lbad_rv_18
    li a0, 275
    bne x19, a0, .Lbad_rv_19
    li a0, 276
    bne x20, a0, .Lbad_rv_20
    li a0, 277
    bne x21, a0, .Lbad_rv_21
    li a0, 278
    bne x22, a0, .Lbad_rv_22
    li a0, 279
    bne x23, a0, .Lbad_rv_23
    li a0, 280
    bne x24, a0, .Lbad_rv_24
    li a0, 281
    bne x25, a0, .Lbad_rv_25
    li a0, 282
    bne x26, a0, .Lbad_rv_26
    li a0, 283
    bne x27, a0, .Lbad_rv_27
    li a0, 284
    bne x28, a0, .Lbad_rv_28
    li a0, 285
    bne x29, a0, .Lbad_rv_29
    li a0, 286
    bne x30, a0, .Lbad_rv_30
    li a0, 287
    bne x31, a0, .Lbad_rv_31
    j .Ldone_rv
.Lbad_rv_1:
    li a0, -1001
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_3:
    li a0, -1003
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_4:
    li a0, -1004
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_5:
    li a0, -1005
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_6:
    li a0, -1006
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_7:
    li a0, -1007
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_8:
    li a0, -1008
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_9:
    li a0, -1009
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_11:
    li a0, -1011
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_12:
    li a0, -1012
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_13:
    li a0, -1013
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_14:
    li a0, -1014
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_15:
    li a0, -1015
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_16:
    li a0, -1016
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_17:
    li a0, -1017
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_18:
    li a0, -1018
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_19:
    li a0, -1019
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_20:
    li a0, -1020
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_21:
    li a0, -1021
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_22:
    li a0, -1022
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_23:
    li a0, -1023
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_24:
    li a0, -1024
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_25:
    li a0, -1025
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_26:
    li a0, -1026
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_27:
    li a0, -1027
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_28:
    li a0, -1028
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_29:
    li a0, -1029
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_30:
    li a0, -1030
    sd a0, 256(sp)
    j .Ldone_rv
.Lbad_rv_31:
    li a0, -1031
    sd a0, 256(sp)
    j .Ldone_rv
.Ldone_rv:
    ld x1, 8(sp)
    ld x3, 24(sp)
    ld x4, 32(sp)
    ld x5, 40(sp)
    ld x6, 48(sp)
    ld x7, 56(sp)
    ld x8, 64(sp)
    ld x9, 72(sp)
    ld x11, 88(sp)
    ld x12, 96(sp)
    ld x13, 104(sp)
    ld x14, 112(sp)
    ld x15, 120(sp)
    ld x16, 128(sp)
    ld x17, 136(sp)
    ld x18, 144(sp)
    ld x19, 152(sp)
    ld x20, 160(sp)
    ld x21, 168(sp)
    ld x22, 176(sp)
    ld x23, 184(sp)
    ld x24, 192(sp)
    ld x25, 200(sp)
    ld x26, 208(sp)
    ld x27, 216(sp)
    ld x28, 224(sp)
    ld x29, 232(sp)
    ld x30, 240(sp)
    ld x31, 248(sp)
    ld a0, 256(sp)
    addi sp, sp, 272
    ret
#elif defined(__x86_64__)
    subq $136, %rsp
    movq %rax, 0(%rsp)
    movq %rbx, 8(%rsp)
    movq %rcx, 16(%rsp)
    movq %rdx, 24(%rsp)
    movq %rsi, 32(%rsp)
    movq %rdi, 40(%rsp)
    movq %rbp, 48(%rsp)
    movq %r8, 56(%rsp)
    movq %r9, 64(%rsp)
    movq %r10, 72(%rsp)
    movq %r11, 80(%rsp)
    movq %r12, 88(%rsp)
    movq %r13, 96(%rsp)
    movq %r14, 104(%rsp)
    movq %r15, 112(%rsp)
    movq $256, %rax
    movq $257, %rbx
    movq $258, %rcx
    movq $259, %rdx
    movq $260, %rsi
    movq $261, %rdi
    movq $262, %rbp
    movq $263, %r8
    movq $264, %r9
    movq $265, %r10
    movq $266, %r11
    movq $267, %r12
    movq $268, %r13
    movq $269, %r14
    movq $270, %r15
    movq 40(%rsp), %rax
    cmpq $14, %rax
    jne .Lx86_call
    movq $1, %rdi
    movq $10, %rsi
.Lx86_call:
    stc
    syscall
    movq %rax, 120(%rsp)
    pushfq
    popq %rax
    testq $1, %rax
    jz .Lbad_x86_flags
    cmpq $257, %rbx
    jne .Lbad_x86_1
    cmpq $259, %rdx
    jne .Lbad_x86_3
    cmpq $14, 40(%rsp)
    je .Lx86_signal_args
    cmpq $260, %rsi
    jne .Lbad_x86_4
    cmpq $261, %rdi
    jne .Lbad_x86_5
    jmp .Lx86_after_args
.Lx86_signal_args:
    cmpq $10, %rsi
    jne .Lbad_x86_4
    cmpq $1, %rdi
    jne .Lbad_x86_5
.Lx86_after_args:
    cmpq $262, %rbp
    jne .Lbad_x86_6
    cmpq $263, %r8
    jne .Lbad_x86_7
    cmpq $264, %r9
    jne .Lbad_x86_8
    cmpq $265, %r10
    jne .Lbad_x86_9
    cmpq $267, %r12
    jne .Lbad_x86_11
    cmpq $268, %r13
    jne .Lbad_x86_12
    cmpq $269, %r14
    jne .Lbad_x86_13
    cmpq $270, %r15
    jne .Lbad_x86_14
    jmp .Ldone_x86
.Lbad_x86_1:
    movq $-1001, 120(%rsp)
    jmp .Ldone_x86
.Lbad_x86_3:
    movq $-1003, 120(%rsp)
    jmp .Ldone_x86
.Lbad_x86_4:
    movq $-1004, 120(%rsp)
    jmp .Ldone_x86
.Lbad_x86_5:
    movq $-1005, 120(%rsp)
    jmp .Ldone_x86
.Lbad_x86_6:
    movq $-1006, 120(%rsp)
    jmp .Ldone_x86
.Lbad_x86_7:
    movq $-1007, 120(%rsp)
    jmp .Ldone_x86
.Lbad_x86_8:
    movq $-1008, 120(%rsp)
    jmp .Ldone_x86
.Lbad_x86_9:
    movq $-1009, 120(%rsp)
    jmp .Ldone_x86
.Lbad_x86_11:
    movq $-1011, 120(%rsp)
    jmp .Ldone_x86
.Lbad_x86_12:
    movq $-1012, 120(%rsp)
    jmp .Ldone_x86
.Lbad_x86_13:
    movq $-1013, 120(%rsp)
    jmp .Ldone_x86
.Lbad_x86_14:
    movq $-1014, 120(%rsp)
    jmp .Ldone_x86
.Lbad_x86_flags:
    movq $-1099, 120(%rsp)
.Ldone_x86:
    movq 8(%rsp), %rbx
    movq 16(%rsp), %rcx
    movq 24(%rsp), %rdx
    movq 32(%rsp), %rsi
    movq 40(%rsp), %rdi
    movq 48(%rsp), %rbp
    movq 56(%rsp), %r8
    movq 64(%rsp), %r9
    movq 72(%rsp), %r10
    movq 80(%rsp), %r11
    movq 88(%rsp), %r12
    movq 96(%rsp), %r13
    movq 104(%rsp), %r14
    movq 112(%rsp), %r15
    movq 120(%rsp), %rax
    addq $136, %rsp
    ret
#endif
.size frame_register_probe, . - frame_register_probe
.section .note.GNU-stack,"",@progbits
