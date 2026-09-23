#pragma once

namespace moss::test::validation {
inline constexpr const char *uaccess_cases[] = {
    "allocation_fault", "write_fault",     "read_fault", "partial_read",   "partial_write",    "partial_pipe_read",
    "sigframe_fault",   "sigreturn_fault", "devices",    "cow_copy_fault", "cow_partial_read", "cow_user_fault"};

long uaccess_control(long op, long arg1, long arg2);
} // namespace moss::test::validation
