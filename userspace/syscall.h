// MOSS userspace syscall wrappers — shared header for all user programs
//
// Provides inline ARM64 syscall stubs and POSIX-like wrapper functions.
// All user programs should #include "syscall.h" instead of defining their own.

#pragma once

// ============================================================================
// Syscall numbers (must match kernel-syscall_table.cppm SyscallNumber enum)
// ============================================================================

#define SYS_EXIT     1
#define SYS_GETPID   2
#define SYS_GETPPID  3
#define SYS_FORK     10
#define SYS_EXECVE   11
#define SYS_WAIT4    12
#define SYS_WAITPID  13
#define SYS_OPEN     30
#define SYS_CLOSE    31
#define SYS_READ     32
#define SYS_WRITE    33
#define SYS_LSEEK    34
#define SYS_FSTAT    36
#define SYS_DUP      42
#define SYS_DUP2     43
#define SYS_PIPE     44

// ============================================================================
// Low-level syscall wrappers (ARM64: x8=nr, x0-x5=args, svc #0)
// ============================================================================

static inline long syscall0(long number) {
    register long x8 asm("x8") = number;
    register long ret asm("x0");
    asm volatile("svc #0"
        : "=r"(ret)
        : "r"(x8)
        : "memory");
    return ret;
}

static inline long syscall1(long number, long a0) {
    register long x8 asm("x8") = number;
    register long x0 asm("x0") = a0;
    register long ret asm("x0");
    asm volatile("svc #0"
        : "=r"(ret)
        : "r"(x8), "r"(x0)
        : "memory");
    return ret;
}

static inline long syscall2(long number, long a0, long a1) {
    register long x8 asm("x8") = number;
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long ret asm("x0");
    asm volatile("svc #0"
        : "=r"(ret)
        : "r"(x8), "r"(x0), "r"(x1)
        : "memory");
    return ret;
}

static inline long syscall3(long number, long a0, long a1, long a2) {
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

// ============================================================================
// POSIX-like wrapper functions
// ============================================================================

static inline void _exit(int status) {
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

static inline long getpid(void) {
    return syscall0(SYS_GETPID);
}

static inline long getppid(void) {
    return syscall0(SYS_GETPPID);
}

static inline long fork(void) {
    return syscall0(SYS_FORK);
}

static inline long execve(const char *pathname, char *const argv[],
                           char *const envp[]) {
    return syscall3(SYS_EXECVE, (long)pathname, (long)argv, (long)envp);
}

static inline long waitpid(long pid, int *wstatus, int options) {
    return syscall3(SYS_WAITPID, pid, (long)wstatus, options);
}

static inline long open(const char *pathname, int flags) {
    return syscall2(SYS_OPEN, (long)pathname, flags);
}

static inline long close(int fd) {
    return syscall1(SYS_CLOSE, fd);
}

static inline long read(int fd, void *buf, long count) {
    return syscall3(SYS_READ, fd, (long)buf, count);
}

static inline long write(int fd, const void *buf, long count) {
    return syscall3(SYS_WRITE, fd, (long)buf, count);
}

static inline long dup(int oldfd) {
    return syscall1(SYS_DUP, oldfd);
}

static inline long dup2(int oldfd, int newfd) {
    return syscall2(SYS_DUP2, oldfd, newfd);
}

static inline long pipe(long pipefd[2]) {
    return syscall1(SYS_PIPE, (long)pipefd);
}

// ============================================================================
// String utilities (no libc available)
// ============================================================================

static inline int strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static inline int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static inline int strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static inline void strcpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static inline void print(const char *msg) {
    write(1, msg, strlen(msg));
}

static inline void eprint(const char *msg) {
    write(2, msg, strlen(msg));
}
