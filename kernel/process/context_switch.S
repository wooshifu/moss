// ARM64上下文切换实现
// 提供高性能的进程/线程切换

.section .text

// 外部符号声明
.extern current_thread_ptr
.extern next_thread_ptr

// 上下文切换函数
// void context_switch(CpuContext* prev, CpuContext* next)
// x0 = prev context, x1 = next context
.global context_switch
.type context_switch, @function
context_switch:
    // 保存当前上下文到prev
    // 保存通用寄存器 x0-x30
    stp     x0, x1, [x0, #0]     // 保存x0,x1
    stp     x2, x3, [x0, #16]    // 保存x2,x3
    stp     x4, x5, [x0, #32]    // 保存x4,x5
    stp     x6, x7, [x0, #48]    // 保存x6,x7
    stp     x8, x9, [x0, #64]    // 保存x8,x9
    stp     x10, x11, [x0, #80]  // 保存x10,x11
    stp     x12, x13, [x0, #96]  // 保存x12,x13
    stp     x14, x15, [x0, #112] // 保存x14,x15
    stp     x16, x17, [x0, #128] // 保存x16,x17
    stp     x18, x19, [x0, #144] // 保存x18,x19
    stp     x20, x21, [x0, #160] // 保存x20,x21
    stp     x22, x23, [x0, #176] // 保存x22,x23
    stp     x24, x25, [x0, #192] // 保存x24,x25
    stp     x26, x27, [x0, #208] // 保存x26,x27
    stp     x28, x29, [x0, #224] // 保存x28,x29(FP)
    str     x30, [x0, #240]      // 保存x30(LR)

    // 保存栈指针
    mov     x2, sp
    str     x2, [x0, #248]       // 保存SP

    // 保存程序计数器（返回地址）
    str     x30, [x0, #256]      // 保存PC

    // 保存PSTATE（程序状态寄存器）
    mrs     x2, daif
    str     x2, [x0, #264]       // 保存PSTATE

    // 保存浮点状态寄存器
    mrs     x2, fpsr
    mrs     x3, fpcr
    stp     x2, x3, [x0, #272]   // 保存FPSR,FPCR

    // 保存NEON/FP寄存器 v0-v31
    add     x2, x0, #288         // NEON寄存器起始地址
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

    // 保存线程指针寄存器
    mrs     x2, tpidr_el0
    str     x2, [x0, #800]       // 保存TPIDR_EL0

    // 恢复下一个上下文
    // 恢复线程指针寄存器
    ldr     x2, [x1, #800]       // 恢复TPIDR_EL0
    msr     tpidr_el0, x2

    // 恢复NEON/FP寄存器
    add     x2, x1, #288         // NEON寄存器起始地址
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

    // 恢复浮点状态寄存器
    ldp     x2, x3, [x1, #272]   // 恢复FPSR,FPCR
    msr     fpsr, x2
    msr     fpcr, x3

    // 恢复PSTATE
    ldr     x2, [x1, #264]       // 恢复PSTATE
    msr     daif, x2

    // 恢复栈指针
    ldr     x2, [x1, #248]       // 恢复SP
    mov     sp, x2

    // 恢复通用寄存器（除了x0,x1）
    ldp     x2, x3, [x1, #16]    // 恢复x2,x3
    ldp     x4, x5, [x1, #32]    // 恢复x4,x5
    ldp     x6, x7, [x1, #48]    // 恢复x6,x7
    ldp     x8, x9, [x1, #64]    // 恢复x8,x9
    ldp     x10, x11, [x1, #80]  // 恢复x10,x11
    ldp     x12, x13, [x1, #96]  // 恢复x12,x13
    ldp     x14, x15, [x1, #112] // 恢复x14,x15
    ldp     x16, x17, [x1, #128] // 恢复x16,x17
    ldp     x18, x19, [x1, #144] // 恢复x18,x19
    ldp     x20, x21, [x1, #160] // 恢复x20,x21
    ldp     x22, x23, [x1, #176] // 恢复x22,x23
    ldp     x24, x25, [x1, #192] // 恢复x24,x25
    ldp     x26, x27, [x1, #208] // 恢复x26,x27
    ldp     x28, x29, [x1, #224] // 恢复x28,x29(FP)
    ldr     x30, [x1, #240]      // 恢复x30(LR)

    // 最后恢复x0,x1
    ldp     x0, x1, [x1, #0]     // 恢复x0,x1

    // 返回到新线程
    ret

.size context_switch, .-context_switch

// 进入用户模式的上下文切换
// void switch_to_user(CpuContext* context, uint64_t user_stack)
// x0 = context, x1 = user stack pointer
.global switch_to_user
.type switch_to_user, @function
switch_to_user:
    // 设置异常返回状态
    mov     x2, #0                // 用户模式EL0
    msr     spsr_el1, x2

    // 设置用户模式栈指针
    msr     sp_el0, x1

    // 设置返回地址（用户程序入口）
    ldr     x2, [x0, #256]        // 加载PC
    msr     elr_el1, x2

    // 恢复浮点状态寄存器
    ldp     x2, x3, [x0, #272]
    msr     fpsr, x2
    msr     fpcr, x3

    // 恢复NEON/FP寄存器
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

    // 恢复线程指针寄存器
    ldr     x2, [x0, #800]
    msr     tpidr_el0, x2

    // 恢复通用寄存器
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

    // 最后恢复x0,x1
    ldp     x0, x1, [x0, #0]

    // 异常返回到用户模式
    eret

.size switch_to_user, .-switch_to_user

// 内核线程启动入口
// void kernel_thread_entry(void (*func)(void*), void* arg)
.global kernel_thread_entry
.type kernel_thread_entry, @function
kernel_thread_entry:
    // 从栈中获取函数指针和参数
    mov     x0, x19              // arg (之前保存在x19)
    mov     x1, x20              // func (之前保存在x20)

    // 启用中断
    msr     daifclr, #2          // 启用IRQ

    // 调用内核线程函数
    blr     x1

    // 内核线程退出
    // 这里应该调用线程退出函数
    b       .

.size kernel_thread_entry, .-kernel_thread_entry

// 系统调用入口点快速路径
// 用于优化频繁的系统调用
.global syscall_fast_path
.type syscall_fast_path, @function
syscall_fast_path:
    // 保存最少的寄存器
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    // 检查系统调用号
    cmp     x8, #0x100           // 假设快速系统调用号小于0x100
    b.ge    syscall_slow_path

    // 这里处理快速系统调用
    // 具体实现取决于系统调用类型

    // 恢复寄存器并返回
    ldp     x29, x30, [sp], #16
    ret

syscall_slow_path:
    // 跳转到完整的系统调用处理
    ldp     x29, x30, [sp], #16
    b       syscall_handler       // 需要在其他地方实现

.size syscall_fast_path, .-syscall_fast_path

// TLB刷新辅助函数
.global flush_tlb_single
.type flush_tlb_single, @function
flush_tlb_single:
    // x0 = virtual address
    tlbi    vae1, x0             // 刷新单个页面的TLB
    dsb     sy                   // 数据同步屏障
    isb                          // 指令同步屏障
    ret

.size flush_tlb_single, .-flush_tlb_single

.global flush_tlb_all
.type flush_tlb_all, @function
flush_tlb_all:
    tlbi    vmalle1             // 刷新所有TLB条目
    dsb     sy                  // 数据同步屏障
    isb                         // 指令同步屏障
    ret

.size flush_tlb_all, .-flush_tlb_all