#pragma once

namespace moss::test::validation {
// Next control opcode after the CPU-bound/STOP probes (60-63). Shared with
// users.frame: actions 0/1/2 park the child, arm the parent with the probe's
// return PC, and check/clean up using the child PID.
inline constexpr long RETURN_RESCHEDULE_PROBE = 64;
long signal_control(long op, long arg1, long arg2);
} // namespace moss::test::validation
