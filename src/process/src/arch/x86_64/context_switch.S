# x86_64上下文切换实现
# 提供高性能的进程/线程切换

.section .text

# 外部符号声明
.extern current_thread_ptr
.extern next_thread_ptr

# 上下文切换函数
# void context_switch(CpuContext* prev, CpuContext* next)
# RDI = prev context, RSI = next context
.global context_switch
.type context_switch, @function
context_switch:
    # 保存当前上下文到prev (基于更新后的CpuContext结构体布局)
    # struct CpuContext { rax,rbx,rcx,rdx, rsi,rdi,rbp,sp, r8-r15, pstate,pc, segments... }

    # 保存通用寄存器
    movq    %rax, 0(%rdi)      # 保存RAX (offset 0)
    movq    %rbx, 8(%rdi)      # 保存RBX (offset 8)
    movq    %rcx, 16(%rdi)     # 保存RCX (offset 16)
    movq    %rdx, 24(%rdi)     # 保存RDX (offset 24)
    movq    %rsi, 32(%rdi)     # 保存RSI (offset 32)
    movq    %rdi, 40(%rdi)     # 保存RDI (offset 40)
    movq    %rbp, 48(%rdi)     # 保存RBP (offset 48)
    movq    %rsp, 56(%rdi)     # 保存SP  (offset 56)
    movq    %r8, 64(%rdi)      # 保存R8  (offset 64)
    movq    %r9, 72(%rdi)      # 保存R9  (offset 72)
    movq    %r10, 80(%rdi)     # 保存R10 (offset 80)
    movq    %r11, 88(%rdi)     # 保存R11 (offset 88)
    movq    %r12, 96(%rdi)     # 保存R12 (offset 96)
    movq    %r13, 104(%rdi)    # 保存R13 (offset 104)
    movq    %r14, 112(%rdi)    # 保存R14 (offset 112)
    movq    %r15, 120(%rdi)    # 保存R15 (offset 120)

    # 保存状态寄存器
    pushfq
    popq    %rax
    movq    %rax, 128(%rdi)    # 保存PSTATE (offset 128)

    # 保存程序计数器（返回地址）
    movq    (%rsp), %rax       # 获取返回地址
    movq    %rax, 136(%rdi)    # 保存PC (offset 136)

    # 恢复下一个上下文
    # 恢复状态寄存器
    movq    128(%rsi), %rax
    pushq   %rax
    popfq

    # 恢复通用寄存器
    movq    0(%rsi), %rax      # 恢复RAX
    movq    8(%rsi), %rbx      # 恢复RBX
    movq    16(%rsi), %rcx     # 恢复RCX
    movq    24(%rsi), %rdx     # 恢复RDX
    movq    48(%rsi), %rbp     # 恢复RBP
    movq    56(%rsi), %rsp     # 恢复SP
    movq    64(%rsi), %r8      # 恢复R8
    movq    72(%rsi), %r9      # 恢复R9
    movq    80(%rsi), %r10     # 恢复R10
    movq    88(%rsi), %r11     # 恢复R11
    movq    96(%rsi), %r12     # 恢复R12
    movq    104(%rsi), %r13    # 恢复R13
    movq    112(%rsi), %r14    # 恢复R14
    movq    120(%rsi), %r15    # 恢复R15

    # 获取PC并设置为返回地址
    movq    136(%rsi), %r10    # 获取PC
    pushq   %r10               # 将PC压栈作为返回地址

    # 恢复RSI和RDI（最后恢复）
    movq    40(%rsi), %rdi     # 恢复RDI
    movq    32(%rsi), %rsi     # 恢复RSI（最后恢复）

    ret    # 返回到新的上下文

.size context_switch, .-context_switch

# 进入用户模式的上下文切换
# void switch_to_user(CpuContext* context, uint64_t user_stack)
# RDI = context, RSI = user stack pointer
.global switch_to_user
.type switch_to_user, @function
switch_to_user:
    # 准备iretq参数
    # 栈布局（从高到低）：SS, RSP, RFLAGS, CS, RIP
    pushq   $0x23          # SS（用户栈段选择子）
    pushq   %rsi           # RSP（用户栈指针）
    pushq   $0x202         # RFLAGS（中断使能）
    pushq   $0x1B          # CS（用户代码段选择子）
    movq    136(%rdi), %rax # 获取用户PC
    pushq   %rax           # RIP（用户程序计数器）

    # 设置段寄存器为用户模式
    movw    $0x23, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs

    # 恢复通用寄存器
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
    movq    40(%rdi), %rdi  # 最后恢复RDI

    # 切换到用户模式
    iretq

.size switch_to_user, .-switch_to_user

# 内核线程启动入口
# void kernel_thread_entry(void (*func)(void*), void* arg)
.global kernel_thread_entry
.type kernel_thread_entry, @function
kernel_thread_entry:
    # RDI = func, RSI = arg
    movq    %rdi, %r11     # 保存函数指针
    movq    %rsi, %rdi     # 将arg作为第一个参数
    callq   *%r11          # 调用函数

    # 线程结束后应该调用exit_thread
    # TODO: 实现线程退出逻辑
    movq    $60, %rax      # exit syscall
    movq    $0, %rdi       # exit code
    syscall

.size kernel_thread_entry, .-kernel_thread_entry