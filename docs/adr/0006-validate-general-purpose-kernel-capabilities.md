---
status: accepted
date: 2026-09-15
---

# Validate General-Purpose Kernel Capabilities

The current general-purpose kernel acceptance profile covers reliable command-line applications, with system-level correctness, stability and performance acceptance. The six core paths in ADR-0001 remain prerequisites; the expanded goal includes the missing production capabilities needed by these in-scope applications, together with their end-to-end tests. Networking and persistent storage are explicitly outside this delivery as clarified below; concrete system workloads, execution environments and additional acceptance thresholds remain decisions for the ongoing interview, and existing core or benchmark results do not establish completion of this profile.

## Application Compatibility Confirmed on 2026-09-15

The user selected source compatibility: existing C/POSIX command-line applications may be adapted through a C library or platform layer and rebuilt for Moss, with their required behavior verified by end-to-end tests. Direct execution of existing Linux binaries is outside this acceptance target; this permits a Moss-specific runtime ABI while keeping real applications as the compatibility evidence. The subsequent decisions pin the initial applications, version and C-library/runtime choice; their required interface subset still needs implementation, and this decision does not claim complete POSIX conformance.

## Initial Applications Confirmed on 2026-09-15

Use a selected BusyBox command set as the first real-application acceptance target: `ash` for scripts, pipelines, redirection and exit status; and `ls`, `cat`, `mkdir`, `cp`, `mv`, `rm`, `grep` and `wc` for file, directory and text operations. Verify local file and directory results within the same boot, without requiring persistence across reboot. These applications connect process, pipe and filesystem behavior in observable workflows; their Moss port, reproducible applet configuration and nonpersistent file environment still require implementation. The version is fixed below.

## System Acceptance Matrix Confirmed on 2026-09-15

The complete in-scope BusyBox end-to-end workloads must pass on ARM64, x64 and RISC-V 64 in both Debug and Release. This extends the existing six-configuration requirement to real applications so each supported port demonstrates the same required system behavior. Performance acceptance uses Release builds and compares kernel revisions within the fixed, comparable execution environments defined by ADR-0004; an unexecuted or failing configuration leaves the system matrix incomplete.

## BusyBox Version Confirmed on 2026-09-15

The user selected the official BusyBox **1.37.0** release, not the proposed 1.38.0. Pin the source archive and its checksum for reproducible builds across the six required configurations. The selected applets and observable behavior remain unchanged. C-library and runtime choices remain separate decisions; this version selection does not authorize Linux binary compatibility or additional applets.

Use the official [busybox-1.37.0.tar.bz2](https://busybox.net/downloads/busybox-1.37.0.tar.bz2), with SHA-256 `3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4`. The downloaded archive was checked against the [official checksum](https://busybox.net/downloads/busybox-1.37.0.tar.bz2.sha256) on 2026-09-15. This verifies the source input only; the future build integration must enforce the same checksum before extraction, and no BusyBox build or Moss runtime acceptance is established by this download.

## C Library and Runtime Confirmed on 2026-09-15

The user selected **statically linked mlibc with Moss-specific system-interface adaptation**. Implement the required startup, TLS, RV64 soft-float adaptation and system interfaces for the selected BusyBox application contracts. Dynamic linking and Linux binary compatibility are not part of this runtime choice. Pin mlibc and its build dependencies before integrating them; the choice itself does not establish a working C library, a BusyBox port or full POSIX conformance.

The initial implementation source pin is mlibc **v7.0.0**, commit `7c2a178142625cc9852e59a1a090468c61a62d3b`, resolved from the official repository's release tag. Its [commit-addressed archive](https://codeload.github.com/managarm/mlibc/tar.gz/7c2a178142625cc9852e59a1a090468c61a62d3b) was downloaded with SHA-256 `22535f15a789b463bf19abb45b6d4aded20cf5f0b207eb43922b2f95e4507603`. This is a recorded download digest, not an upstream-signed checksum or a working build. Preserve the pinned revision's dependency wrap revisions instead of following their development branches; build integration and dependency verification remain required.

This follows mlibc's [new-OS sysdeps and static-runtime integration model](https://docs.managarm.org/mlibc-book/porting/implementing_sysdeps_p2.html). The inspected upstream [RV64 setjmp implementation](https://github.com/managarm/mlibc/blob/7c2a178142625cc9852e59a1a090468c61a62d3b/options/internal/riscv64/setjmp.S) uses floating-point instructions, so its existing RISC-V support must not be mistaken for drop-in support of Moss's current `rv64imac/lp64` userspace. Verify the pinned source against each architecture's actual ABI. No-op implementations of required behavior cannot count as application acceptance.

## Network Scope Correction on 2026-09-15

The user excluded networking because it is not implemented. This supersedes the earlier inclusion of networking, `wget` and HTTP download: network implementation, network capability tests and network performance acceptance are outside this delivery. Acceptance of this profile does not establish networking capability.

## Persistent Storage Scope Correction on 2026-09-15

The user also excluded persistent storage from this delivery. This supersedes the earlier local-file/reboot workflow, orderly-reboot persistence, simulated power-loss recovery, file/directory synchronization durability contract and automatic disk-recovery requirement. Persistent-storage implementation, correctness, recovery and performance acceptance are outside this delivery. VFS, file descriptors, pipes and the selected applications' file operations within one boot remain in scope, as do the six-configuration matrix and core stability gates; acceptance of this profile does not establish persistent-storage capability.

## Implementation Authorized on 2026-09-15

The user requested starting implementation with a persistent goal under the corrected scope. Proceed through small regression-test and production-repair slices at the already agreed real-kernel and userspace-syscall boundaries. The application-version and C-library/runtime decisions above resolve those initial porting choices; no further confirmation of those same choices is required. Starting this goal does not establish acceptance or authorize unrelated changes, commits or pushes.
