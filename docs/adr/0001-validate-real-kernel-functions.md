---
status: accepted
date: 2026-09-05
---

# Validate Real Kernel Functions

The existing standalone test executable links only `moss_core`, and its explicitly registered cases primarily check language operations. This does not establish that the kernel's memory management, scheduling, or other runtime facilities work. Kernel functional tests and kernel benchmarks must exercise the actual Moss implementations under the runtime conditions required by the tested operation; substitute implementations cannot establish functional coverage.

The initial delivery covered a working test framework on ARM64, x64, and RISC-V 64, reliable reporting of assertion failures, kernel panics, timeouts, and startup failures, and an initial set of real kernel functional tests and benchmarks. Repairs required to run this framework were in scope; other kernel defects remained explicit failures for separate repair. A successful framework delivery did not imply that every kernel subsystem passed. The expanded delivery scope below supersedes that repair boundary for the current work.

This records the accepted scope. [ADR-0002](0002-dedicated-kernel-validation-image.md) defines the image organization, [ADR-0003](0003-suite-level-kernel-test-isolation.md) defines functional-test isolation, and [ADR-0004](0004-compare-kernel-performance-in-qemu.md) defines the benchmark's purpose. Implementation was authorized on 2026-09-06, including the SMP and memory prerequisites. The [design document](../kernel-validation.md) tracks acceptance separately from this decision.

## Delivery Scope Extended on 2026-09-14

The current delivery includes the real kernel repairs needed for the six required core paths to pass their agreed correctness, stability and performance acceptance: memory/user isolation, scheduling/wakeup, timers, process lifecycle, signals, and VFS/pipes. Delivering tests that expose a defect in this required scope does not complete the work; repair the production path and rerun the relevant acceptance checks.

Preserve the original failure evidence and the assertions that express the required behavior. A subsequent passing run supplies new evidence without relabeling the original failure. This couples framework delivery with the required kernel repairs so that the agreed core acceptance can be completed; it does not claim correctness beyond the exercised contracts and configurations.
