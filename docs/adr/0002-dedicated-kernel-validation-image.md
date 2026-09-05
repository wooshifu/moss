---
status: accepted
date: 2026-09-05
---

# Use a Dedicated Kernel Validation Image

Moss will execute kernel functional tests and benchmarks in a dedicated validation image that reuses the production kernel's architecture bootstrap, kernel modules, and subsystem initialization paths. Validation workloads start only when their required real runtime facilities are ready. This gives automation explicit execution and completion points while preserving the kernel behavior required by [ADR-0001](0001-validate-real-kernel-functions.md).

Sharing source modules alone is insufficient. A VFS test must run against the real mounted filesystem environment, and a process or syscall test must enter the real scheduling or privilege-transition path it claims to cover. The validation image must not substitute a separate allocator, scheduler, or other subsystem implementation to simplify test linking or execution.

The confirmed 2026-09-06 resource revision requires four QEMU vCPUs and 2 GiB guest memory by default. Acceptance must establish actual secondary-CPU execution and usable kernel memory under that configuration, not only the emulator's requested topology and RAM size. Completing missing SMP support and enlarged-memory adaptation is explicitly included in the implementation scope.

This is an accepted design, not a completed implementation. The [design document](../kernel-validation.md) lists the confirmed initial workloads and the benchmark execution policy. [ADR-0003](0003-suite-level-kernel-test-isolation.md) defines the isolation boundary for functional tests.
