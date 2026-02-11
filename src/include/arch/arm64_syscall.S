.section .text

// ARM64系统调用入口点实现
// 处理SVC指令引发的异常，提取参数并调用系统调用分发器

.global syscall_entry_point
.global syscall_return

// ARM64系统调用入口点
// 当用户空间执行SVC指令时，CPU会跳转到此处
// ARM64系统调用约定：
//   x8 = 系统调用号
//   x0-x5 = 参数
//   返回值在x0中
syscall_entry_point:
    // 早期诊断：系统调用入口点被调用了！
    stp     x29, x30, [sp, #-16]!       // 保存frame pointer和link register

    // 输出诊断信息到UART (最简方式)
    mov     x10, #0x09000000            // UART基地址
    mov     x11, #'S'                   // 'S' for syscall
    str     w11, [x10]
    mov     x11, #'Y'
    str     w11, [x10]
    mov     x11, #'S'
    str     w11, [x10]
    mov     x11, #'!'
    str     w11, [x10]
    mov     x11, #'\n'
    str     w11, [x10]
    stp     x27, x28, [sp, #-16]!       // 保存callee-saved寄存器
    stp     x25, x26, [sp, #-16]!
    stp     x23, x24, [sp, #-16]!
    stp     x21, x22, [sp, #-16]!
    stp     x19, x20, [sp, #-16]!
    stp     x17, x18, [sp, #-16]!       // 保存caller-saved寄存器
    stp     x15, x16, [sp, #-16]!
    stp     x13, x14, [sp, #-16]!
    stp     x11, x12, [sp, #-16]!
    stp     x9, x10, [sp, #-16]!
    stp     x7, x8, [sp, #-16]!         // x8是系统调用号
    stp     x5, x6, [sp, #-16]!
    stp     x3, x4, [sp, #-16]!
    stp     x1, x2, [sp, #-16]!
    str     x0, [sp, #-8]!              // x0是第一个参数，也用作返回值

    // 内存同步屏障，确保寄存器保存完成
    dsb     sy

    // 准备调用C函数 system_call_handler
    // 参数传递：
    //   x0 = 系统调用号 (从x8移动到x0)
    //   x1 = arg0 (原x0)
    //   x2 = arg1 (原x1)
    //   x3 = arg2 (原x2)
    //   x4 = arg3 (原x3)
    //   x5 = arg4 (原x4)
    //   x6 = arg5 (原x5)

    // 从栈上恢复参数寄存器的值
    ldr     x1, [sp]                    // arg0 (原x0)
    ldr     x2, [sp, #8]                // arg1 (原x1)
    ldr     x3, [sp, #16]               // arg2 (原x2)
    ldr     x7, [sp, #24]               // 临时取x3
    ldr     x6, [sp, #32]               // arg4 (原x4)
    ldr     x5, [sp, #40]               // arg5 (原x5)
    mov     x4, x7                      // arg3 (原x3)
    ldr     x0, [sp, #64]               // 系统调用号 (原x8)

    // 调用系统调用分发器
    bl      system_call_handler

    // 返回值现在在x0中，需要恢复到用户空间的x0寄存器位置
    str     x0, [sp]                    // 将返回值存储到栈上x0的位置

    // 内存同步屏障
    dsb     sy

    // 恢复用户态寄存器
    ldr     x0, [sp], #8                // 恢复x0 (包含返回值)
    ldp     x1, x2, [sp], #16           // 恢复x1, x2
    ldp     x3, x4, [sp], #16           // 恢复x3, x4
    ldp     x5, x6, [sp], #16           // 恢复x5, x6
    ldp     x7, x8, [sp], #16           // 恢复x7, x8 (x8是系统调用号)
    ldp     x9, x10, [sp], #16          // 恢复x9, x10
    ldp     x11, x12, [sp], #16         // 恢复x11, x12
    ldp     x13, x14, [sp], #16         // 恢复x13, x14
    ldp     x15, x16, [sp], #16         // 恢复x15, x16
    ldp     x17, x18, [sp], #16         // 恢复x17, x18
    ldp     x19, x20, [sp], #16         // 恢复x19, x20
    ldp     x21, x22, [sp], #16         // 恢复x21, x22
    ldp     x23, x24, [sp], #16         // 恢复x23, x24
    ldp     x25, x26, [sp], #16         // 恢复x25, x26
    ldp     x27, x28, [sp], #16         // 恢复x27, x28
    ldp     x29, x30, [sp], #16         // 恢复frame pointer和link register

    // 返回用户空间 - 使用正确的异常返回
    // 设置异常返回状态寄存器 (SPSR_EL1)
    mov     x2, #0x0              // EL0t模式，中断启用
    msr     spsr_el1, x2

    // 设置异常链接寄存器 (ELR_EL1) 为用户程序的返回地址
    // 返回地址应该是系统调用指令的下一条指令
    mrs     x2, elr_el1           // 读取当前的返回地址
    msr     elr_el1, x2           // 确保返回地址正确

    // 异常返回到用户空间 (EL0)
    eret

// ARM64系统调用返回处理
// 从内核空间返回用户空间，并设置返回值
syscall_return:
    // x0包含SyscallContext*指针
    cbz     x0, .return_error           // 如果context为空，返回错误

    // 从context中加载返回值
    ldr     x1, [x0, #56]               // 加载ret_value字段 (偏移56字节)

    // 设置返回值到x0寄存器 (用户空间会看到这个值)
    mov     x0, x1

    // 返回
    ret

.return_error:
    // 错误情况：返回-1
    mov     x0, #-1
    ret

// 定义节段属性
.section .note.GNU-stack,"",%progbits