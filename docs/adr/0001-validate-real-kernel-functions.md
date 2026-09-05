---
status: accepted
date: 2026-09-05
---

# Validate Real Kernel Functions

The existing standalone test executable links only `moss_core`, and its explicitly registered cases primarily check language operations. This does not establish that the kernel's memory management, scheduling, or other runtime facilities work. Kernel functional tests and kernel benchmarks must exercise the actual Moss implementations under the runtime conditions required by the tested operation; substitute implementations cannot establish functional coverage.

The first delivery covers a working test framework on ARM64, x86_64, and RISC-V, reliable reporting of assertion failures, kernel panics, timeouts, and startup failures, and an initial set of real kernel functional tests and benchmarks. Repairs required to run this framework are in scope. Other kernel defects discovered by these tests remain explicit failures for separate repair; a successful framework delivery does not imply that every kernel subsystem passes.

This records the accepted scope. [ADR-0002](0002-dedicated-kernel-validation-image.md) defines the image organization, [ADR-0003](0003-suite-level-kernel-test-isolation.md) defines functional-test isolation, and [ADR-0004](0004-compare-kernel-performance-in-qemu.md) defines the benchmark's purpose. Implementation was authorized on 2026-09-06, including the SMP and memory prerequisites. The [design document](../kernel-validation.md) tracks acceptance separately from this decision.
