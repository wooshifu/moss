#pragma once

#include "syscall.h"

static inline long control(long op, long a, long b) { return syscall3(511, op, a, b); }

long frame_register_probe(long number, long signal_pid);
extern const unsigned char vm_rodata[4096];
extern const unsigned char __user_text_start[], __user_text_end[];
int wait_exit(long child, int code);
int wait_signal(long child, int signo);
unsigned long exec_probe(long test);
unsigned long busybox_script(const char *script, const char *expected);
unsigned long wait_status_rollback(void);
unsigned long fork_allocation_rollback(long arm, long release, unsigned cycles);
unsigned long mmap_heap_rollback(void);
unsigned long exec_allocation_rollback(void);
unsigned long exec_mutable_snapshot_rollback(void);
unsigned long exec_boundary_load_plan(void);
unsigned long exec_source_version(void);
unsigned long exec_shared_thread_gate(void);
unsigned long exec_registration_gate(void);
unsigned long uaccess_allocation_fault(void);
unsigned long uaccess_read_fault(void);
unsigned long uaccess_write_fault(void);
unsigned long uaccess_partial_read(void);
unsigned long uaccess_partial_write(void);
unsigned long uaccess_partial_pipe_read(void);
unsigned long uaccess_sigframe_fault(void);
unsigned long uaccess_sigreturn_fault(void);
unsigned long uaccess_devices(void);
unsigned long uaccess_cow_copy_fault(void);
unsigned long uaccess_cow_partial_read(void);
unsigned long uaccess_cow_user_fault(void);
unsigned long vm_private_cow(void);
unsigned long vm_readonly_cow(void);
unsigned long vm_access_permissions(void);
unsigned long vm_kernel_isolation(long target);
unsigned long vm_brk_lifecycle(void);
unsigned long numbers_regression(void);
unsigned long user_ranges(void);
unsigned long long counter(void);
void user_benchmark(long mode);
unsigned long timer_relative_sleep(void);
unsigned long timer_absolute_sleep(void);
unsigned long timer_invalid_arguments(void);
unsigned long timer_relative_interrupted(void);
unsigned long timer_clock_relative_interrupted(void);
unsigned long timer_clock_absolute_interrupted(void);
unsigned long timer_short_reuse(void);
unsigned long timer_early_wakeup(void);
unsigned long timer_arm_failure_recovery(void);
unsigned long timer_cancel_in_flight(void);
int signal_case(const char *name);
