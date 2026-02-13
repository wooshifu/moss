.section .text

# x86_64系统调用入口点实现
# 处理SYSCALL指令，提取参数并调用系统调用分发器

.global syscall_entry_point
.global syscall_return

# x86_64系统调用入口点
# 当用户空间执行SYSCALL指令时，CPU会跳转到此处
# x86_64系统调用约定：
#   rax = 系统调用号
#   rdi, rsi, rdx, r10, r8, r9 = 参数
#   返回值在rax中
syscall_entry_point:
    # 保存用户态寄存器到栈上
    pushq   %rbp                        # 保存frame pointer
    movq    %rsp, %rbp                  # 设置新的frame pointer

    # 保存callee-saved寄存器
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    # 保存系统调用参数寄存器
    pushq   %rax                        # 系统调用号
    pushq   %rdi                        # arg0
    pushq   %rsi                        # arg1
    pushq   %rdx                        # arg2
    pushq   %r10                        # arg3 (注意：r10而不是rcx)
    pushq   %r8                         # arg4
    pushq   %r9                         # arg5

    # 保存其他寄存器
    pushq   %rcx                        # 用户空间的RIP
    pushq   %r11                        # 用户空间的RFLAGS

    # 准备调用C函数 system_call_handler
    # System V ABI调用约定：
    #   rdi = 系统调用号 (从rax移动)
    #   rsi = arg0 (从rdi移动)
    #   rdx = arg1 (从rsi移动)
    #   rcx = arg2 (保持rdx)
    #   r8  = arg3 (从r10移动)
    #   r9  = arg4 (保持r8)
    #   [rsp+8] = arg5 (从r9推入栈)

    # 从栈上重新加载参数
    movq    72(%rsp), %rdi              # 系统调用号 (原rax)
    movq    64(%rsp), %rsi              # arg0 (原rdi)
    movq    56(%rsp), %rdx              # arg1 (原rsi)
    movq    48(%rsp), %rcx              # arg2 (原rdx)
    movq    40(%rsp), %r8               # arg3 (原r10)
    movq    32(%rsp), %r9               # arg4 (原r8)

    # arg5需要通过栈传递
    pushq   24(%rsp)                    # arg5 (原r9)

    # 调用系统调用分发器
    call    system_call_handler

    # 清理栈上的arg5参数
    addq    $8, %rsp

    # 返回值现在在rax中，保存到栈上
    movq    %rax, 72(%rsp)              # 将返回值存储到原rax的位置

    # 恢复寄存器 (除了rax，它包含返回值)
    popq    %r11                        # 恢复用户RFLAGS
    popq    %rcx                        # 恢复用户RIP
    popq    %r9                         # 恢复r9
    popq    %r8                         # 恢复r8
    popq    %r10                        # 恢复r10
    popq    %rdx                        # 恢复rdx
    popq    %rsi                        # 恢复rsi
    popq    %rdi                        # 恢复rdi
    popq    %rax                        # 恢复rax (包含返回值)

    # 恢复callee-saved寄存器
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx

    # 恢复frame pointer
    popq    %rbp

    # 返回用户空间
    # 注意：这里应该使用SYSRET指令返回用户空间
    # 但是由于我们目前在内核模式运行，暂时使用ret
    ret

# x86_64系统调用返回处理
# 从内核空间返回用户空间，并设置返回值
syscall_return:
    # rdi包含SyscallContext*指针
    testq   %rdi, %rdi                  # 检查context是否为空
    jz      .return_error               # 如果为空，跳转到错误处理

    # 从context中加载返回值
    movq    56(%rdi), %rax              # 加载ret_value字段 (偏移56字节)

    # 返回值已经在rax中了
    ret

.return_error:
    # 错误情况：返回-1
    movq    $-1, %rax
    ret

# 定义节段属性
.section .note.GNU-stack,"",@progbits