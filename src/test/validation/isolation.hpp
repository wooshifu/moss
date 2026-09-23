#pragma once

namespace moss::test::validation {
// Target ordering is shared with userspace/validation.c and the host catalog.
inline constexpr const char *kernel_isolation_cases[] = {"kernel_text", "kernel_rodata", "kernel_data",
                                                         "kernel_page_table", "kernel_mmio"};
// Private validation protocol pair, following exec controls 48/49.
inline constexpr long ISOLATION_PREPARE = 50;
inline constexpr long ISOLATION_VERIFY = 51;
long isolation_control(long op, long arg1, long arg2);
} // namespace moss::test::validation
