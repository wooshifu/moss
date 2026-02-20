// MOSS userspace hello program — loaded via execve from initramfs
//
// This is a minimal freestanding program that uses MOSS syscalls to write
// a message to stdout and then exit cleanly.

#define SYS_EXIT  1
#define SYS_WRITE 33

static long syscall3(long number, long a0, long a1, long a2) {
    register long x8 asm("x8") = number;
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long ret asm("x0");
    asm volatile("svc #0"
        : "=r"(ret)
        : "r"(x8), "r"(x0), "r"(x1), "r"(x2)
        : "memory");
    return ret;
}

static long write(int fd, const char *buf, long count) {
    return syscall3(SYS_WRITE, fd, (long)buf, count);
}

static int strlen_simple(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *msg) {
    write(1, msg, strlen_simple(msg));
}

void _start(void) {
    print("Hello from initramfs!\n");
    print("MOSS execve() works!\n");
    syscall3(SYS_EXIT, 0, 0, 0);
    while (1) {}
}
