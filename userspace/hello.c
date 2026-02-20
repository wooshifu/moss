// MOSS userspace hello program — loaded via execve from initramfs
//
// This is a minimal freestanding program that uses MOSS syscalls to write
// a message to stdout and then exit cleanly.

#include "syscall.h"

void _start(void) {
    print("Hello from initramfs!\n");
    print("MOSS execve() works!\n");
    _exit(0);
}
