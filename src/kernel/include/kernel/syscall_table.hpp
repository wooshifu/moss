#pragma once

// MOSS内核系统调用表和管理系统
// 实现完整的POSIX兼容系统调用接口

#include "core/types.hpp"

namespace moss::kernel::syscall {

// 系统调用号枚举 - 按照功能分组管理
enum class SyscallNumber : long {
    // === 基础系统调用 (0-9) ===
    SYS_DEBUG_PRINT = 0,        // 调试输出
    SYS_EXIT = 1,               // 进程退出
    SYS_GETPID = 2,             // 获取进程ID
    SYS_GETPPID = 3,            // 获取父进程ID
    SYS_GETUID = 4,             // 获取用户ID
    SYS_GETGID = 5,             // 获取组ID
    SYS_GETEUID = 6,            // 获取有效用户ID
    SYS_GETEGID = 7,            // 获取有效组ID
    SYS_SETSID = 8,             // 设置会话ID
    SYS_GETPGID = 9,            // 获取进程组ID

    // === 进程管理 (10-29) ===
    SYS_FORK = 10,              // 创建子进程
    SYS_EXECVE = 11,            // 执行程序
    SYS_WAIT4 = 12,             // 等待子进程
    SYS_WAITPID = 13,           // 等待指定进程
    SYS_KILL = 14,              // 发送信号
    SYS_SIGACTION = 15,         // 信号处理设置
    SYS_SIGPROCMASK = 16,       // 信号掩码操作
    SYS_SIGRETURN = 17,         // 信号返回
    SYS_PAUSE = 18,             // 等待信号
    SYS_ALARM = 19,             // 设置闹钟
    SYS_SETPGID = 20,           // 设置进程组
    SYS_SETUID = 21,            // 设置用户ID
    SYS_SETGID = 22,            // 设置组ID
    SYS_SETEUID = 23,           // 设置有效用户ID
    SYS_SETEGID = 24,           // 设置有效组ID
    SYS_GETPGRP = 25,           // 获取进程组
    SYS_SETPGRP = 26,           // 设置进程组
    SYS_GETSID = 27,            // 获取会话ID
    SYS_NICE = 28,              // 设置进程优先级
    SYS_GETPRIORITY = 29,       // 获取进程优先级

    // === 文件系统操作 (30-59) ===
    SYS_OPEN = 30,              // 打开文件
    SYS_CLOSE = 31,             // 关闭文件
    SYS_READ = 32,              // 读取文件
    SYS_WRITE = 33,             // 写入文件
    SYS_LSEEK = 34,             // 文件定位
    SYS_STAT = 35,              // 获取文件状态
    SYS_FSTAT = 36,             // 获取文件描述符状态
    SYS_LSTAT = 37,             // 获取链接文件状态
    SYS_ACCESS = 38,            // 检查文件权限
    SYS_CHMOD = 39,             // 修改文件权限
    SYS_CHOWN = 40,             // 修改文件所有者
    SYS_UMASK = 41,             // 设置文件创建掩码
    SYS_DUP = 42,               // 复制文件描述符
    SYS_DUP2 = 43,              // 复制文件描述符到指定位置
    SYS_PIPE = 44,              // 创建管道
    SYS_MKDIR = 45,             // 创建目录
    SYS_RMDIR = 46,             // 删除目录
    SYS_LINK = 47,              // 创建硬链接
    SYS_UNLINK = 48,            // 删除文件
    SYS_SYMLINK = 49,           // 创建符号链接
    SYS_READLINK = 50,          // 读取符号链接
    SYS_CHDIR = 51,             // 改变工作目录
    SYS_GETCWD = 52,            // 获取当前目录
    SYS_RENAME = 53,            // 重命名文件
    SYS_TRUNCATE = 54,          // 截断文件
    SYS_FTRUNCATE = 55,         // 截断文件(通过fd)
    SYS_FSYNC = 56,             // 同步文件
    SYS_FDATASYNC = 57,         // 同步文件数据
    SYS_SYNC = 58,              // 同步所有文件
    SYS_MOUNT = 59,             // 挂载文件系统

    // === 内存管理 (60-79) ===
    SYS_MMAP = 60,              // 内存映射
    SYS_MUNMAP = 61,            // 取消内存映射
    SYS_MPROTECT = 62,          // 修改内存保护
    SYS_MLOCK = 63,             // 锁定内存页
    SYS_MUNLOCK = 64,           // 解锁内存页
    SYS_MLOCKALL = 65,          // 锁定所有内存页
    SYS_MUNLOCKALL = 66,        // 解锁所有内存页
    SYS_MADVISE = 67,           // 内存使用建议
    SYS_MSYNC = 68,             // 同步内存映射
    SYS_BRK = 69,               // 设置数据段大小
    SYS_SBRK = 70,              // 调整数据段大小
    SYS_MREMAP = 71,            // 重新映射内存
    SYS_MINCORE = 72,           // 检查页面是否在内存中
    SYS_MMAP2 = 73,             // 内存映射(扩展版)
    SYS_REMAP_FILE_PAGES = 74,  // 重新映射文件页
    SYS_MBIND = 75,             // NUMA内存绑定
    SYS_GET_MEMPOLICY = 76,     // 获取内存策略
    SYS_SET_MEMPOLICY = 77,     // 设置内存策略
    SYS_MIGRATE_PAGES = 78,     // 迁移内存页
    SYS_MOVE_PAGES = 79,        // 移动内存页

    // === 时间和定时器 (80-89) ===
    SYS_TIME = 80,              // 获取时间
    SYS_GETTIMEOFDAY = 81,      // 获取时间(微秒精度)
    SYS_SETTIMEOFDAY = 82,      // 设置时间
    SYS_CLOCK_GETTIME = 83,     // 获取时钟时间
    SYS_CLOCK_SETTIME = 84,     // 设置时钟时间
    SYS_CLOCK_GETRES = 85,      // 获取时钟分辨率
    SYS_NANOSLEEP = 86,         // 纳秒级睡眠
    SYS_TIMER_CREATE = 87,      // 创建定时器
    SYS_TIMER_SETTIME = 88,     // 设置定时器
    SYS_TIMER_GETTIME = 89,     // 获取定时器状态

    // === 网络通信 (90-109) ===
    SYS_SOCKET = 90,            // 创建socket
    SYS_BIND = 91,              // 绑定地址
    SYS_LISTEN = 92,            // 监听连接
    SYS_ACCEPT = 93,            // 接受连接
    SYS_CONNECT = 94,           // 建立连接
    SYS_SEND = 95,              // 发送数据
    SYS_RECV = 96,              // 接收数据
    SYS_SENDTO = 97,            // 发送数据到指定地址
    SYS_RECVFROM = 98,          // 从指定地址接收数据
    SYS_SHUTDOWN = 99,          // 关闭socket
    SYS_SETSOCKOPT = 100,       // 设置socket选项
    SYS_GETSOCKOPT = 101,       // 获取socket选项
    SYS_GETSOCKNAME = 102,      // 获取socket名称
    SYS_GETPEERNAME = 103,      // 获取对端名称
    SYS_SOCKETPAIR = 104,       // 创建socket对
    SYS_SENDMSG = 105,          // 发送消息
    SYS_RECVMSG = 106,          // 接收消息
    SYS_SELECT = 107,           // I/O多路复用
    SYS_POLL = 108,             // 轮询I/O事件
    SYS_EPOLL_CREATE = 109,     // 创建epoll实例

    // === 系统信息和控制 (110-129) ===
    SYS_UNAME = 110,            // 获取系统信息
    SYS_SYSINFO = 111,          // 获取系统统计信息
    SYS_GETRLIMIT = 112,        // 获取资源限制
    SYS_SETRLIMIT = 113,        // 设置资源限制
    SYS_GETRUSAGE = 114,        // 获取资源使用情况
    SYS_TIMES = 115,            // 获取进程时间
    SYS_PTRACE = 116,           // 进程跟踪
    SYS_SYSLOG = 117,           // 系统日志
    SYS_REBOOT = 118,           // 系统重启
    SYS_SETHOSTNAME = 119,      // 设置主机名
    SYS_GETHOSTNAME = 120,      // 获取主机名
    SYS_SETDOMAINNAME = 121,    // 设置域名
    SYS_GETDOMAINNAME = 122,    // 获取域名
    SYS_IOPL = 123,             // I/O权限级别
    SYS_IOPERM = 124,           // I/O端口权限
    SYS_SYSCTL = 125,           // 系统控制
    SYS_ARCH_PRCTL = 126,       // 架构特定控制
    SYS_PRCTL = 127,            // 进程控制
    SYS_CAPGET = 128,           // 获取能力
    SYS_CAPSET = 129,           // 设置能力

    // 系统调用总数标记
    MAX_SYSCALL = 130
};

// 系统调用处理函数类型
using SyscallHandler = long(*)(long arg0, long arg1, long arg2,
                              long arg3, long arg4, long arg5) noexcept;

// 系统调用描述符
struct SyscallDescriptor {
    const char* name;           // 系统调用名称
    SyscallHandler handler;     // 处理函数指针
    u8 arg_count;              // 参数个数
    bool implemented;           // 是否已实现
    const char* description;    // 功能描述

    constexpr SyscallDescriptor() noexcept
        : name(nullptr), handler(nullptr), arg_count(0),
          implemented(false), description(nullptr) {}

    constexpr SyscallDescriptor(const char* n, SyscallHandler h, u8 argc,
                               bool impl, const char* desc) noexcept
        : name(n), handler(h), arg_count(argc),
          implemented(impl), description(desc) {}
};

// 系统调用处理函数声明
namespace handlers {
    // 基础系统调用
    long sys_debug_print(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_exit(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_getpid(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_getppid(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_getuid(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_getgid(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

    // 进程管理 - 框架实现
    long sys_fork(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_execve(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_wait4(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_kill(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

    // 文件系统 - 框架实现
    long sys_open(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_close(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_read(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_write(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

    // 内存管理
    long sys_mmap(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_munmap(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_mprotect(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_brk(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

    // 网络通信 - 框架实现
    long sys_socket(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_bind(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_listen(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
    long sys_accept(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;

    // 未实现系统调用的默认处理器
    long sys_not_implemented(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) noexcept;
}

// 全局系统调用表
extern const SyscallDescriptor SYSCALL_TABLE[static_cast<int>(SyscallNumber::MAX_SYSCALL)];

// 系统调用分发器
class SyscallDispatcher {
public:
    // 分发系统调用
    static long dispatch(long syscall_number, long arg0, long arg1, long arg2,
                        long arg3, long arg4, long arg5) noexcept;

    // 获取系统调用信息
    static const SyscallDescriptor* get_syscall_info(long syscall_number) noexcept;

    // 检查系统调用是否已实现
    static bool is_implemented(long syscall_number) noexcept;

    // 验证系统调用号有效性
    static bool is_valid_syscall(long syscall_number) noexcept;

    // 获取系统调用统计信息
    static void get_syscall_stats(u64* total_calls, u64* implemented_calls) noexcept;

    // 调试：打印所有已实现的系统调用
    static void print_implemented_syscalls() noexcept;
};

// 系统调用统计信息
struct SyscallStats {
    u64 total_syscalls;         // 总调用次数
    u64 successful_syscalls;    // 成功调用次数
    u64 failed_syscalls;        // 失败调用次数
    u64 unimplemented_syscalls; // 未实现调用次数
    u64 invalid_syscalls;       // 无效调用次数
};

// 全局系统调用统计
extern SyscallStats g_syscall_stats;

} // namespace moss::kernel::syscall
