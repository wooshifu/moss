---
status: accepted
date: 2026-09-21
---

# Reboot on Initial System Supervisor Failure

Unexpected loss of the Initial System Supervisor is a system-level failure, not an ordinary service restart. Moss defaults to a controlled reboot rather than transparently constructing a replacement supervisor in the running system. Starting the same executable again cannot establish that the service topology, delegated authority and interrupted operations have been recovered consistently.

The Mechanism Kernel Domain detects supervisor death and enforces the designated system-level failure transition. The transition must not depend on cooperation from the failed supervisor or on reconstructing its service graph inside the kernel. Ordinary service restart and recovery policy remain in userspace under [ADR-0014](0014-keep-service-supervision-in-userspace.md); this fatal boundary does not move them into the kernel.

Controlled reboot means entering a defined platform reset path, with bounded best-effort diagnostics and shutdown work, rather than continuing normal operation under a newly minted supervisor. It does not promise successful filesystem writeback, completion of outstanding requests or preservation of volatile application state. The next boot must establish initial authority through its boot trust chain as defined in [ADR-0027](0027-establish-initial-authority-through-verified-boot.md), not by treating persisted service names or old capabilities as authority.

## Considered Options

Transparent supervisor restart was rejected because process recreation alone cannot recover the authority and coordination state of the system's initial supervisor. Continuing indefinitely without that authority owner was rejected because ordinary supervision and recovery would no longer have a reliable owner. Controlled reboot sacrifices the current system session to re-establish a defined bootstrap boundary.

[Fuchsia's critical-process mechanism](https://fuchsia.dev/reference/syscalls/job_set_critical) lets an authorized caller make a process's termination cause termination of its containing job. It is a reference for explicit failure escalation, not evidence that this mechanism reboots a machine or implements Moss's supervisor policy.

## Consequences

Acceptance must cover supervisor failure during bootstrap and normal operation, including while authority is being delegated or service recovery is in progress. The fatal transition must remain reachable when supervisor-dependent services cannot respond; no replacement process may silently inherit the lost supervisor's identity or authority. Ordinary service death must retain its separate recovery boundary. Reboot necessarily interrupts unrelated applications as well as the failed component.

This is an accepted target architecture, not an implementation claim. The exact reset and shutdown protocol, diagnostic budgets, platform reset-failure behavior, detection of a live but unresponsive supervisor, planned supervisor replacement, repeated-boot-failure handling and entry into a recovery environment remain separate decisions. This decision does not extend the approval-lifetime guarantees of [ADR-0031](0031-preserve-code-approvals-across-authority-service-failure.md) or [ADR-0032](0032-retain-independent-code-approval-revocation-authority.md) across a system reboot.
