# RISC-V上下文切换实现
# 提供高性能的进程/线程切换

.section .text

# 外部符号声明
.extern current_thread_ptr
.extern next_thread_ptr

# 上下文切换函数
# void context_switch(CpuContext* prev, CpuContext* next)
# a0 = prev context, a1 = next context
.global context_switch
.type context_switch, @function
context_switch:
    # 保存当前上下文到prev
    # 基于CpuContext结构体：struct CpuContext { u64 x[32]; u64 pc; u64 sstatus; }

    # 保存通用寄存器到x[]数组 (offset 0-255)
    sd      x0, 0(a0)        # 保存x0 (zero, 总是0)
    sd      x1, 8(a0)        # 保存ra (x1)
    sd      x2, 16(a0)       # 保存sp (x2)
    sd      x3, 24(a0)       # 保存gp (x3)
    sd      x4, 32(a0)       # 保存tp (x4)
    sd      x5, 40(a0)       # 保存t0 (x5)
    sd      x6, 48(a0)       # 保存t1 (x6)
    sd      x7, 56(a0)       # 保存t2 (x7)
    sd      x8, 64(a0)       # 保存s0/fp (x8)
    sd      x9, 72(a0)       # 保存s1 (x9)
    sd      x10, 80(a0)      # 保存a0 (x10)
    sd      x11, 88(a0)      # 保存a1 (x11)
    sd      x12, 96(a0)      # 保存a2 (x12)
    sd      x13, 104(a0)     # 保存a3 (x13)
    sd      x14, 112(a0)     # 保存a4 (x14)
    sd      x15, 120(a0)     # 保存a5 (x15)
    sd      x16, 128(a0)     # 保存a6 (x16)
    sd      x17, 136(a0)     # 保存a7 (x17)
    sd      x18, 144(a0)     # 保存s2 (x18)
    sd      x19, 152(a0)     # 保存s3 (x19)
    sd      x20, 160(a0)     # 保存s4 (x20)
    sd      x21, 168(a0)     # 保存s5 (x21)
    sd      x22, 176(a0)     # 保存s6 (x22)
    sd      x23, 184(a0)     # 保存s7 (x23)
    sd      x24, 192(a0)     # 保存s8 (x24)
    sd      x25, 200(a0)     # 保存s9 (x25)
    sd      x26, 208(a0)     # 保存s10 (x26)
    sd      x27, 216(a0)     # 保存s11 (x27)
    sd      x28, 224(a0)     # 保存t3 (x28)
    sd      x29, 232(a0)     # 保存t4 (x29)
    sd      x30, 240(a0)     # 保存t5 (x30)
    sd      x31, 248(a0)     # 保存t6 (x31)

    # 保存程序计数器（返回地址） (offset 256)
    sd      x1, 256(a0)      # 保存PC (ra)

    # 保存CSR寄存器 (offset 264)
    csrr    t0, sstatus
    sd      t0, 264(a0)      # 保存pstate (sstatus)

    # 保存栈指针到单独的sp字段 (offset 272)
    sd      x2, 272(a0)      # 保存sp到独立字段

    # 恢复下一个上下文
    # 恢复CSR寄存器
    ld      t0, 264(a1)
    csrw    sstatus, t0      # 恢复pstate (sstatus)

    # 恢复栈指针到x[2]和sp字段
    ld      t1, 272(a1)      # 从独立sp字段恢复
    sd      t1, 16(a1)       # 同步到x[2]

    # 恢复通用寄存器
    ld      x1, 8(a1)        # 恢复ra
    ld      x2, 16(a1)       # 恢复sp
    ld      x3, 24(a1)       # 恢复gp
    ld      x4, 32(a1)       # 恢复tp
    ld      x5, 40(a1)       # 恢复t0
    ld      x6, 48(a1)       # 恢复t1
    ld      x7, 56(a1)       # 恢复t2
    ld      x8, 64(a1)       # 恢复s0/fp
    ld      x9, 72(a1)       # 恢复s1
    ld      x12, 96(a1)      # 恢复a2
    ld      x13, 104(a1)     # 恢复a3
    ld      x14, 112(a1)     # 恢复a4
    ld      x15, 120(a1)     # 恢复a5
    ld      x16, 128(a1)     # 恢复a6
    ld      x17, 136(a1)     # 恢复a7
    ld      x18, 144(a1)     # 恢复s2
    ld      x19, 152(a1)     # 恢复s3
    ld      x20, 160(a1)     # 恢复s4
    ld      x21, 168(a1)     # 恢复s5
    ld      x22, 176(a1)     # 恢复s6
    ld      x23, 184(a1)     # 恢复s7
    ld      x24, 192(a1)     # 恢复s8
    ld      x25, 200(a1)     # 恢复s9
    ld      x26, 208(a1)     # 恢复s10
    ld      x27, 216(a1)     # 恢复s11
    ld      x28, 224(a1)     # 恢复t3
    ld      x29, 232(a1)     # 恢复t4
    ld      x30, 240(a1)     # 恢复t5
    ld      x31, 248(a1)     # 恢复t6

    # 最后恢复a0和a1
    ld      x10, 80(a1)      # 恢复a0
    ld      x11, 88(a1)      # 恢复a1

    ret    # 返回到新的上下文

.size context_switch, .-context_switch

# 进入用户模式的上下文切换
# void switch_to_user(CpuContext* context, uint64_t user_stack)
# a0 = context, a1 = user stack pointer
.global switch_to_user
.type switch_to_user, @function
switch_to_user:
    # 设置sstatus为用户模式
    li      t0, 0x00000000   # 清除SPP位（用户模式）
    csrw    sstatus, t0

    # 设置异常返回地址
    ld      t0, 256(a0)      # 获取用户PC
    csrw    sepc, t0         # 设置异常返回地址

    # 设置用户栈指针（从a1参数）
    mv      sp, a1

    # 同时更新context中的sp字段
    sd      a1, 272(a0)      # 保存用户栈到sp字段
    sd      a1, 16(a0)       # 同步到x[2]

    # 恢复通用寄存器
    ld      x1, 8(a0)        # 恢复ra
    ld      x2, 16(a0)       # 恢复sp (将被用户栈覆盖)
    ld      x3, 24(a0)       # 恢复gp
    ld      x4, 32(a0)       # 恢复tp
    ld      x5, 40(a0)       # 恢复t0
    ld      x6, 48(a0)       # 恢复t1
    ld      x7, 56(a0)       # 恢复t2
    ld      x8, 64(a0)       # 恢复s0/fp
    ld      x9, 72(a0)       # 恢复s1
    ld      x11, 88(a0)      # 恢复a1
    ld      x12, 96(a0)      # 恢复a2
    ld      x13, 104(a0)     # 恢复a3
    ld      x14, 112(a0)     # 恢复a4
    ld      x15, 120(a0)     # 恢复a5
    ld      x16, 128(a0)     # 恢复a6
    ld      x17, 136(a0)     # 恢复a7
    ld      x18, 144(a0)     # 恢复s2
    ld      x19, 152(a0)     # 恢复s3
    ld      x20, 160(a0)     # 恢复s4
    ld      x21, 168(a0)     # 恢复s5
    ld      x22, 176(a0)     # 恢复s6
    ld      x23, 184(a0)     # 恢复s7
    ld      x24, 192(a0)     # 恢复s8
    ld      x25, 200(a0)     # 恢复s9
    ld      x26, 208(a0)     # 恢复s10
    ld      x27, 216(a0)     # 恢复s11
    ld      x28, 224(a0)     # 恢复t3
    ld      x29, 232(a0)     # 恢复t4
    ld      x30, 240(a0)     # 恢复t5
    ld      x31, 248(a0)     # 恢复t6

    # 重新设置用户栈
    mv      sp, a1

    # 最后恢复a0
    ld      x10, 80(a0)      # 恢复a0

    # 切换到用户模式
    sret

.size switch_to_user, .-switch_to_user

# 内核线程启动入口
# void kernel_thread_entry(void (*func)(void*), void* arg)
.global kernel_thread_entry
.type kernel_thread_entry, @function
kernel_thread_entry:
    # a0 = func, a1 = arg
    mv      t0, a0           # 保存函数指针
    mv      a0, a1           # 将arg作为第一个参数
    jalr    ra, t0, 0        # 调用函数

    # 线程结束后应该调用exit_thread
    # TODO: 实现线程退出逻辑
    li      a7, 93           # exit syscall number
    li      a0, 0            # exit code
    ecall

.size kernel_thread_entry, .-kernel_thread_entry