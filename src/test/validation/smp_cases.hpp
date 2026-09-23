#pragma once

#include "validation_internal.hpp"

namespace moss::test::validation {
const char *selected_suite();
const char *running_case();
bool boot_option(const char *key, char *out, usize capacity);
bool affinity_valid();
void start_case(const char *name);
void end_case();
[[noreturn]] void finish(const char *reason = "complete");
[[noreturn]] void invalid_control();
void wait_for_phase(const u32 &phase, u32 value);
void register_smp_cases();
void register_vfs_smp_cases();
long start_vfs_smp_suite();
long vfs_smp_control(long op, long arg1, long arg2);
long start_smp_suite();
long smp_control(long op, long arg1, long arg2);
void user_copy_version_binding();
void raw_user_copy_fixup();
} // namespace moss::test::validation
