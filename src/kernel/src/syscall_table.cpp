// MOSS内核系统调用表实现
// 提供完整的系统调用处理和分发机制

module;

// extern "C" declarations in global module fragment
extern "C" void early_debug_print(const char *message) noexcept;

module moss.kernel;

namespace moss::kernel::syscall {

// 全局系统调用统计
SyscallStats g_syscall_stats = {0, 0, 0, 0, 0};

// 系统调用处理函数实现
namespace handlers {

    // 基础系统调用处理函数
    long sys_debug_print(long arg0, long, long, long, long, long) noexcept {
        if (arg0 != 0) {
            early_debug_print(reinterpret_cast<const char*>(arg0));
            return 0;
        }
        return -1; // EINVAL
    }

    long sys_exit(long exit_code, long, long, long, long, long) noexcept {
        using namespace moss::kernel::process;

        early_debug_print("📋 系统调用: exit() - 进程请求退出\n");

        // 获取全局进程管理器
        if (!g_process_manager) {
            early_debug_print("❌ 进程管理器未初始化\n");
            return -1;
        }

        // 当前用户空间进程的PID是1（第一个创建的进程）
        const ProcessId current_pid = 1;

        early_debug_print("🔄 正在清理进程资源...\n");

        // 调用进程管理器的terminate_process进行完整的进程清理
        auto result = g_process_manager->terminate_process(current_pid, static_cast<i32>(exit_code));

        if (!result) {
            early_debug_print("❌ 进程退出失败\n");
            return -1;
        }

        early_debug_print("✅ 进程已终止，资源已清理\n");
        early_debug_print("🔄 调度器将继续执行其他任务...\n");

        // CRITICAL: exit() should NEVER return to userspace
        // The process has been deleted - returning would jump to freed memory

        // Instead, call scheduler directly to pick next task
        if (g_scheduler) {
            early_debug_print("🎯 直接调用调度器选择下一个任务...\n");
            // This will not return - scheduler takes over execution
            g_scheduler->start_scheduling();
        } else {
            early_debug_print("❌ 调度器未初始化，进入无限循环\n");
            // Fallback: infinite loop to prevent returning to deleted userspace
            while (true) {
                ::moss::kernel::arch::cpu_yield();  // 架构无关的CPU让出
            }
        }

        // This point should NEVER be reached due to scheduler taking over
        // Removing unreachable return statement
    }

    long sys_getpid(long, long, long, long, long, long) noexcept {
        // TODO: 从进程管理器获取当前进程ID
        // 临时返回固定值 1
        return 1;
    }

    long sys_getppid(long, long, long, long, long, long) noexcept {
        // TODO: 从进程管理器获取父进程ID
        // 临时返回固定值 0 (init进程)
        return 0;
    }

    long sys_getuid(long, long, long, long, long, long) noexcept {
        // TODO: 从安全子系统获取用户ID
        // 临时返回 root 用户 (0)
        return 0;
    }

    long sys_getgid(long, long, long, long, long, long) noexcept {
        // TODO: 从安全子系统获取组ID
        // 临时返回 root 组 (0)
        return 0;
    }

    // 进程管理系统调用 - 框架实现
    long sys_fork(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: fork() - 尚未实现\n");
        return -38; // ENOSYS - Function not implemented
    }

    long sys_execve(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: execve() - 尚未实现\n");
        return -38; // ENOSYS
    }

    long sys_wait4(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: wait4() - 尚未实现\n");
        return -38; // ENOSYS
    }

    long sys_kill(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: kill() - 尚未实现\n");
        return -38; // ENOSYS
    }

    // 文件系统调用 - 框架实现
    long sys_open(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: open() - 尚未实现\n");
        return -38; // ENOSYS
    }

    long sys_close(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: close() - 尚未实现\n");
        return -38; // ENOSYS
    }

    long sys_read(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: read() - 尚未实现\n");
        return -38; // ENOSYS
    }

    long sys_write(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: write() - 尚未实现\n");
        return -38; // ENOSYS
    }

    // 内存管理系统调用
    long sys_mmap(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: mmap() - 尚未实现\n");
        return -38; // ENOSYS
    }

    long sys_munmap(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: munmap() - 尚未实现\n");
        return -38; // ENOSYS
    }

    long sys_mprotect(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: mprotect() - 尚未实现\n");
        return -38; // ENOSYS
    }

    long sys_brk(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: brk() - 尚未实现\n");
        return -38; // ENOSYS
    }

    // 网络通信系统调用 - 框架实现
    long sys_socket(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: socket() - 尚未实现\n");
        return -38; // ENOSYS
    }

    long sys_bind(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: bind() - 尚未实现\n");
        return -38; // ENOSYS
    }

    long sys_listen(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: listen() - 尚未实现\n");
        return -38; // ENOSYS
    }

    long sys_accept(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: accept() - 尚未实现\n");
        return -38; // ENOSYS
    }

    // 未实现系统调用的默认处理器
    long sys_not_implemented(long, long, long, long, long, long) noexcept {
        early_debug_print("📋 系统调用: 未知系统调用\n");
        return -38; // ENOSYS - Function not implemented
    }
}

// 全局系统调用表定义
const SyscallDescriptor SYSCALL_TABLE[static_cast<int>(SyscallNumber::MAX_SYSCALL)] = {
    // === 基础系统调用 (0-9) ===
    {"debug_print", handlers::sys_debug_print, 1, true, "调试输出"},
    {"exit", handlers::sys_exit, 1, true, "进程退出"},
    {"getpid", handlers::sys_getpid, 0, true, "获取进程ID"},
    {"getppid", handlers::sys_getppid, 0, true, "获取父进程ID"},
    {"getuid", handlers::sys_getuid, 0, true, "获取用户ID"},
    {"getgid", handlers::sys_getgid, 0, true, "获取组ID"},
    {"geteuid", handlers::sys_not_implemented, 0, false, "获取有效用户ID"},
    {"getegid", handlers::sys_not_implemented, 0, false, "获取有效组ID"},
    {"setsid", handlers::sys_not_implemented, 0, false, "设置会话ID"},
    {"getpgid", handlers::sys_not_implemented, 1, false, "获取进程组ID"},

    // === 进程管理 (10-29) ===
    {"fork", handlers::sys_fork, 0, false, "创建子进程"},
    {"execve", handlers::sys_execve, 3, false, "执行程序"},
    {"wait4", handlers::sys_wait4, 4, false, "等待子进程"},
    {"waitpid", handlers::sys_not_implemented, 3, false, "等待指定进程"},
    {"kill", handlers::sys_kill, 2, false, "发送信号"},
    {"sigaction", handlers::sys_not_implemented, 3, false, "信号处理设置"},
    {"sigprocmask", handlers::sys_not_implemented, 3, false, "信号掩码操作"},
    {"sigreturn", handlers::sys_not_implemented, 0, false, "信号返回"},
    {"pause", handlers::sys_not_implemented, 0, false, "等待信号"},
    {"alarm", handlers::sys_not_implemented, 1, false, "设置闹钟"},
    {"setpgid", handlers::sys_not_implemented, 2, false, "设置进程组"},
    {"setuid", handlers::sys_not_implemented, 1, false, "设置用户ID"},
    {"setgid", handlers::sys_not_implemented, 1, false, "设置组ID"},
    {"seteuid", handlers::sys_not_implemented, 1, false, "设置有效用户ID"},
    {"setegid", handlers::sys_not_implemented, 1, false, "设置有效组ID"},
    {"getpgrp", handlers::sys_not_implemented, 0, false, "获取进程组"},
    {"setpgrp", handlers::sys_not_implemented, 0, false, "设置进程组"},
    {"getsid", handlers::sys_not_implemented, 1, false, "获取会话ID"},
    {"nice", handlers::sys_not_implemented, 1, false, "设置进程优先级"},
    {"getpriority", handlers::sys_not_implemented, 2, false, "获取进程优先级"},

    // === 文件系统操作 (30-59) ===
    {"open", handlers::sys_open, 3, false, "打开文件"},
    {"close", handlers::sys_close, 1, false, "关闭文件"},
    {"read", handlers::sys_read, 3, false, "读取文件"},
    {"write", handlers::sys_write, 3, false, "写入文件"},
    {"lseek", handlers::sys_not_implemented, 3, false, "文件定位"},
    {"stat", handlers::sys_not_implemented, 2, false, "获取文件状态"},
    {"fstat", handlers::sys_not_implemented, 2, false, "获取文件描述符状态"},
    {"lstat", handlers::sys_not_implemented, 2, false, "获取链接文件状态"},
    {"access", handlers::sys_not_implemented, 2, false, "检查文件权限"},
    {"chmod", handlers::sys_not_implemented, 2, false, "修改文件权限"},
    {"chown", handlers::sys_not_implemented, 3, false, "修改文件所有者"},
    {"umask", handlers::sys_not_implemented, 1, false, "设置文件创建掩码"},
    {"dup", handlers::sys_not_implemented, 1, false, "复制文件描述符"},
    {"dup2", handlers::sys_not_implemented, 2, false, "复制文件描述符到指定位置"},
    {"pipe", handlers::sys_not_implemented, 1, false, "创建管道"},
    {"mkdir", handlers::sys_not_implemented, 2, false, "创建目录"},
    {"rmdir", handlers::sys_not_implemented, 1, false, "删除目录"},
    {"link", handlers::sys_not_implemented, 2, false, "创建硬链接"},
    {"unlink", handlers::sys_not_implemented, 1, false, "删除文件"},
    {"symlink", handlers::sys_not_implemented, 2, false, "创建符号链接"},
    {"readlink", handlers::sys_not_implemented, 3, false, "读取符号链接"},
    {"chdir", handlers::sys_not_implemented, 1, false, "改变工作目录"},
    {"getcwd", handlers::sys_not_implemented, 2, false, "获取当前目录"},
    {"rename", handlers::sys_not_implemented, 2, false, "重命名文件"},
    {"truncate", handlers::sys_not_implemented, 2, false, "截断文件"},
    {"ftruncate", handlers::sys_not_implemented, 2, false, "截断文件(通过fd)"},
    {"fsync", handlers::sys_not_implemented, 1, false, "同步文件"},
    {"fdatasync", handlers::sys_not_implemented, 1, false, "同步文件数据"},
    {"sync", handlers::sys_not_implemented, 0, false, "同步所有文件"},
    {"mount", handlers::sys_not_implemented, 5, false, "挂载文件系统"},

    // === 内存管理 (60-79) ===
    {"mmap", handlers::sys_mmap, 6, false, "内存映射"},
    {"munmap", handlers::sys_munmap, 2, false, "取消内存映射"},
    {"mprotect", handlers::sys_mprotect, 3, false, "修改内存保护"},
    {"mlock", handlers::sys_not_implemented, 2, false, "锁定内存页"},
    {"munlock", handlers::sys_not_implemented, 2, false, "解锁内存页"},
    {"mlockall", handlers::sys_not_implemented, 1, false, "锁定所有内存页"},
    {"munlockall", handlers::sys_not_implemented, 0, false, "解锁所有内存页"},
    {"madvise", handlers::sys_not_implemented, 3, false, "内存使用建议"},
    {"msync", handlers::sys_not_implemented, 3, false, "同步内存映射"},
    {"brk", handlers::sys_brk, 1, false, "设置数据段大小"},
    {"sbrk", handlers::sys_not_implemented, 1, false, "调整数据段大小"},
    {"mremap", handlers::sys_not_implemented, 5, false, "重新映射内存"},
    {"mincore", handlers::sys_not_implemented, 3, false, "检查页面是否在内存中"},
    {"mmap2", handlers::sys_not_implemented, 6, false, "内存映射(扩展版)"},
    {"remap_file_pages", handlers::sys_not_implemented, 5, false, "重新映射文件页"},
    {"mbind", handlers::sys_not_implemented, 6, false, "NUMA内存绑定"},
    {"get_mempolicy", handlers::sys_not_implemented, 5, false, "获取内存策略"},
    {"set_mempolicy", handlers::sys_not_implemented, 3, false, "设置内存策略"},
    {"migrate_pages", handlers::sys_not_implemented, 4, false, "迁移内存页"},
    {"move_pages", handlers::sys_not_implemented, 6, false, "移动内存页"},

    // === 时间和定时器 (80-89) ===
    {"time", handlers::sys_not_implemented, 1, false, "获取时间"},
    {"gettimeofday", handlers::sys_not_implemented, 2, false, "获取时间(微秒精度)"},
    {"settimeofday", handlers::sys_not_implemented, 2, false, "设置时间"},
    {"clock_gettime", handlers::sys_not_implemented, 2, false, "获取时钟时间"},
    {"clock_settime", handlers::sys_not_implemented, 2, false, "设置时钟时间"},
    {"clock_getres", handlers::sys_not_implemented, 2, false, "获取时钟分辨率"},
    {"nanosleep", handlers::sys_not_implemented, 2, false, "纳秒级睡眠"},
    {"timer_create", handlers::sys_not_implemented, 3, false, "创建定时器"},
    {"timer_settime", handlers::sys_not_implemented, 4, false, "设置定时器"},
    {"timer_gettime", handlers::sys_not_implemented, 2, false, "获取定时器状态"},

    // === 网络通信 (90-109) ===
    {"socket", handlers::sys_socket, 3, false, "创建socket"},
    {"bind", handlers::sys_bind, 3, false, "绑定地址"},
    {"listen", handlers::sys_listen, 2, false, "监听连接"},
    {"accept", handlers::sys_accept, 3, false, "接受连接"},
    {"connect", handlers::sys_not_implemented, 3, false, "建立连接"},
    {"send", handlers::sys_not_implemented, 4, false, "发送数据"},
    {"recv", handlers::sys_not_implemented, 4, false, "接收数据"},
    {"sendto", handlers::sys_not_implemented, 6, false, "发送数据到指定地址"},
    {"recvfrom", handlers::sys_not_implemented, 6, false, "从指定地址接收数据"},
    {"shutdown", handlers::sys_not_implemented, 2, false, "关闭socket"},
    {"setsockopt", handlers::sys_not_implemented, 5, false, "设置socket选项"},
    {"getsockopt", handlers::sys_not_implemented, 5, false, "获取socket选项"},
    {"getsockname", handlers::sys_not_implemented, 3, false, "获取socket名称"},
    {"getpeername", handlers::sys_not_implemented, 3, false, "获取对端名称"},
    {"socketpair", handlers::sys_not_implemented, 4, false, "创建socket对"},
    {"sendmsg", handlers::sys_not_implemented, 3, false, "发送消息"},
    {"recvmsg", handlers::sys_not_implemented, 3, false, "接收消息"},
    {"select", handlers::sys_not_implemented, 5, false, "I/O多路复用"},
    {"poll", handlers::sys_not_implemented, 3, false, "轮询I/O事件"},
    {"epoll_create", handlers::sys_not_implemented, 1, false, "创建epoll实例"},

    // === 系统信息和控制 (110-129) ===
    {"uname", handlers::sys_not_implemented, 1, false, "获取系统信息"},
    {"sysinfo", handlers::sys_not_implemented, 1, false, "获取系统统计信息"},
    {"getrlimit", handlers::sys_not_implemented, 2, false, "获取资源限制"},
    {"setrlimit", handlers::sys_not_implemented, 2, false, "设置资源限制"},
    {"getrusage", handlers::sys_not_implemented, 2, false, "获取资源使用情况"},
    {"times", handlers::sys_not_implemented, 1, false, "获取进程时间"},
    {"ptrace", handlers::sys_not_implemented, 4, false, "进程跟踪"},
    {"syslog", handlers::sys_not_implemented, 3, false, "系统日志"},
    {"reboot", handlers::sys_not_implemented, 4, false, "系统重启"},
    {"sethostname", handlers::sys_not_implemented, 2, false, "设置主机名"},
    {"gethostname", handlers::sys_not_implemented, 2, false, "获取主机名"},
    {"setdomainname", handlers::sys_not_implemented, 2, false, "设置域名"},
    {"getdomainname", handlers::sys_not_implemented, 2, false, "获取域名"},
    {"iopl", handlers::sys_not_implemented, 1, false, "I/O权限级别"},
    {"ioperm", handlers::sys_not_implemented, 3, false, "I/O端口权限"},
    {"sysctl", handlers::sys_not_implemented, 1, false, "系统控制"},
    {"arch_prctl", handlers::sys_not_implemented, 2, false, "架构特定控制"},
    {"prctl", handlers::sys_not_implemented, 5, false, "进程控制"},
    {"capget", handlers::sys_not_implemented, 2, false, "获取能力"},
    {"capset", handlers::sys_not_implemented, 2, false, "设置能力"}
};

// 系统调用分发器实现
long SyscallDispatcher::dispatch(long syscall_number, long arg0, long arg1,
                                long arg2, long arg3, long arg4, long arg5) noexcept {
    // 更新统计信息
    ++g_syscall_stats.total_syscalls;

    // 检查系统调用号有效性
    if (!is_valid_syscall(syscall_number)) {
        ++g_syscall_stats.invalid_syscalls;
        return -22; // EINVAL
    }

    const SyscallDescriptor* desc = &SYSCALL_TABLE[syscall_number];

    // 检查是否已实现
    if (!desc->implemented) {
        ++g_syscall_stats.unimplemented_syscalls;
        return desc->handler(arg0, arg1, arg2, arg3, arg4, arg5);
    }

    // 调用系统调用处理函数
    long result = desc->handler(arg0, arg1, arg2, arg3, arg4, arg5);

    // 更新统计信息
    if (result >= 0) {
        ++g_syscall_stats.successful_syscalls;
    } else {
        ++g_syscall_stats.failed_syscalls;
    }

    return result;
}

const SyscallDescriptor* SyscallDispatcher::get_syscall_info(long syscall_number) noexcept {
    if (!is_valid_syscall(syscall_number)) {
        return nullptr;
    }
    return &SYSCALL_TABLE[syscall_number];
}

bool SyscallDispatcher::is_implemented(long syscall_number) noexcept {
    if (!is_valid_syscall(syscall_number)) {
        return false;
    }
    return SYSCALL_TABLE[syscall_number].implemented;
}

bool SyscallDispatcher::is_valid_syscall(long syscall_number) noexcept {
    return syscall_number >= 0 &&
           syscall_number < static_cast<long>(SyscallNumber::MAX_SYSCALL);
}

void SyscallDispatcher::get_syscall_stats(u64* total_calls, u64* implemented_calls) noexcept {
    if (total_calls) {
        *total_calls = g_syscall_stats.total_syscalls;
    }

    if (implemented_calls) {
        u64 count = 0;
        for (int i = 0; i < static_cast<int>(SyscallNumber::MAX_SYSCALL); ++i) {
            if (SYSCALL_TABLE[i].implemented) {
                ++count;
            }
        }
        *implemented_calls = count;
    }
}

void SyscallDispatcher::print_implemented_syscalls() noexcept {
    early_debug_print("=== 已实现的系统调用 ===\n");

    for (int i = 0; i < static_cast<int>(SyscallNumber::MAX_SYSCALL); ++i) {
        const auto& desc = SYSCALL_TABLE[i];
        if (desc.implemented) {
            early_debug_print("  ");
            early_debug_print(desc.name);
            early_debug_print(" (");
            early_debug_print(desc.description);
            early_debug_print(")\n");
        }
    }

    early_debug_print("========================\n");
}

} // namespace moss::kernel::syscall
