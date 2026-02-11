// 用户空间Hello World程序
// 使用MOSS内核系统调用接口

// 系统调用号定义（与内核系统调用表匹配）
#define SYS_DEBUG_PRINT 0
#define SYS_EXIT        1

// 系统调用接口函数
static long syscall(long number, long arg0, long arg1, long arg2,
                   long arg3, long arg4, long arg5) {
    register long x8 asm("x8") = number;
    register long x0 asm("x0") = arg0;
    register long x1 asm("x1") = arg1;
    register long x2 asm("x2") = arg2;
    register long x3 asm("x3") = arg3;
    register long x4 asm("x4") = arg4;
    register long x5 asm("x5") = arg5;
    register long ret asm("x0");

    asm volatile(
        "svc #0"
        : "=r"(ret)
        : "r"(x8), "r"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
        : "memory"
    );

    return ret;
}

// 打印字符串到内核调试输出
static void print(const char* message) {
    syscall(SYS_DEBUG_PRINT, (long)message, 0, 0, 0, 0, 0);
}

// 退出程序
static void exit(int status) {
    syscall(SYS_EXIT, status, 0, 0, 0, 0, 0);
    while(1); // 永远不应该到达这里
}

// 程序入口点
int main(void) {
    print("🎉 Hello from user space!\n");
    print("✅ 用户空间程序运行成功！\n");
    print("🚀 MOSS内核用户空间支持验证通过\n");

    exit(0);
    return 0; // 永远不会执行
}

// 程序入口点（符合ELF标准）
void _start(void) {
    int result = main();
    exit(result);
}