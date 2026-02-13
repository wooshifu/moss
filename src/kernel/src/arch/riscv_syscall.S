.section .text

// RISC-V系统调用入口点实现
// 处理ECALL指令引发的trap，提取参数并调用系统调用分发器

.global syscall_entry_point
.global syscall_return

// RISC-V系统调用入口点
// 当用户空间执行ECALL指令时，CPU会跳转到此处
// RISC-V系统调用约定：
//   a7 = 系统调用号
//   a0-a5 = 参数
//   返回值在a0中
syscall_entry_point:
    // 保存用户态寄存器到栈上
    // RISC-V使用sp作为栈指针，ra作为返回地址寄存器

    addi    sp, sp, -256                // 为寄存器分配栈空间

    // 保存通用寄存器 (caller-saved)
    sd      ra, 0(sp)                   // 保存返回地址
    sd      t0, 8(sp)                   // 临时寄存器
    sd      t1, 16(sp)
    sd      t2, 24(sp)
    sd      t3, 32(sp)
    sd      t4, 40(sp)
    sd      t5, 48(sp)
    sd      t6, 56(sp)

    // 保存参数寄存器
    sd      a0, 64(sp)                  // arg0 (也用作返回值)
    sd      a1, 72(sp)                  // arg1
    sd      a2, 80(sp)                  // arg2
    sd      a3, 88(sp)                  // arg3
    sd      a4, 96(sp)                  // arg4
    sd      a5, 104(sp)                 // arg5
    sd      a6, 112(sp)                 // 通常不用于系统调用
    sd      a7, 120(sp)                 // 系统调用号

    // 保存callee-saved寄存器
    sd      s0, 128(sp)                 // frame pointer
    sd      s1, 136(sp)
    sd      s2, 144(sp)
    sd      s3, 152(sp)
    sd      s4, 160(sp)
    sd      s5, 168(sp)
    sd      s6, 176(sp)
    sd      s7, 184(sp)
    sd      s8, 192(sp)
    sd      s9, 200(sp)
    sd      s10, 208(sp)
    sd      s11, 216(sp)

    // 准备调用C函数 system_call_handler
    // RISC-V调用约定：
    //   a0 = 系统调用号 (从a7移动)
    //   a1 = arg0 (从原a0移动)
    //   a2 = arg1 (从原a1移动)
    //   a3 = arg2 (从原a2移动)
    //   a4 = arg3 (从原a3移动)
    //   a5 = arg4 (从原a4移动)
    //   a6 = arg5 (从原a5移动)

    // 从栈上重新加载参数
    ld      a0, 120(sp)                 // 系统调用号 (原a7)
    ld      a1, 64(sp)                  // arg0 (原a0)
    ld      a2, 72(sp)                  // arg1 (原a1)
    ld      a3, 80(sp)                  // arg2 (原a2)
    ld      a4, 88(sp)                  // arg3 (原a3)
    ld      a5, 96(sp)                  // arg4 (原a4)
    ld      a6, 104(sp)                 // arg5 (原a5)

    // 调用系统调用分发器
    call    system_call_handler

    // 返回值现在在a0中，保存到栈上原a0的位置
    sd      a0, 64(sp)

    // 恢复callee-saved寄存器
    ld      s11, 216(sp)
    ld      s10, 208(sp)
    ld      s9, 200(sp)
    ld      s8, 192(sp)
    ld      s7, 184(sp)
    ld      s6, 176(sp)
    ld      s5, 168(sp)
    ld      s4, 160(sp)
    ld      s3, 152(sp)
    ld      s2, 144(sp)
    ld      s1, 136(sp)
    ld      s0, 128(sp)                 // frame pointer

    // 恢复参数寄存器 (除了a0包含返回值)
    ld      a7, 120(sp)                 // 系统调用号
    ld      a6, 112(sp)
    ld      a5, 104(sp)                 // arg5
    ld      a4, 96(sp)                  // arg4
    ld      a3, 88(sp)                  // arg3
    ld      a2, 80(sp)                  // arg2
    ld      a1, 72(sp)                  // arg1
    ld      a0, 64(sp)                  // arg0 (现在包含返回值)

    // 恢复临时寄存器
    ld      t6, 56(sp)
    ld      t5, 48(sp)
    ld      t4, 40(sp)
    ld      t3, 32(sp)
    ld      t2, 24(sp)
    ld      t1, 16(sp)
    ld      t0, 8(sp)
    ld      ra, 0(sp)                   // 返回地址

    // 恢复栈指针
    addi    sp, sp, 256

    // 返回用户空间
    // 注意：这里应该使用SRET指令从监管者模式返回用户模式
    // 但是由于我们目前在内核模式运行，暂时使用ret
    ret

// RISC-V系统调用返回处理
// 从内核空间返回用户空间，并设置返回值
syscall_return:
    // a0包含SyscallContext*指针
    beqz    a0, .return_error           // 如果context为空，返回错误

    // 从context中加载返回值
    ld      a1, 56(a0)                  // 加载ret_value字段 (偏移56字节)

    // 设置返回值到a0寄存器 (用户空间会看到这个值)
    mv      a0, a1

    // 返回
    ret

.return_error:
    // 错误情况：返回-1
    li      a0, -1
    ret