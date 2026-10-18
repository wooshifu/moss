---
status: accepted
date: 2026-10-18
---

# Develop a General-Purpose Hybrid Kernel

Moss targets a general-purpose operating system, beginning with command-line
applications on Raspberry Pi 3B+. Keep ARM64, x64 and RV64 builds and QEMU
regressions. Networking and persistent storage are part of the development
roadmap, beyond the original nonpersistent BusyBox acceptance profile.

Keep VM, final scheduling, POSIX process execution, ordinary ELF `exec`, VFS,
the page cache, initial storage drivers/filesystems and the initial network
data path in the kernel. This establishes a concrete hybrid boundary without
requiring a userspace round trip for each file operation, fault or packet.
Kernel residency increases the impact of parser/driver defects; bounds,
ownership, failure rollback and adversarial-input tests remain required.
Residency alone does not establish good performance.

Keep supervision, service discovery, recovery coordination and management
policy in isolated services where their contracts permit it. Preserve
capabilities, bounded IPC, Memory Objects, native domain isolation and
kernel authority enforcement. Isolation of additional drivers or services
requires an explicit recovery and resource-ownership contract and measured
end-to-end cost. A driver able to DMA outside its buffers is not isolated
merely because it runs in userspace; retain ADR-0017's confinement requirement.

Use one authoritative POSIX file-description and VM state for the ordinary
application path. Existing native file, process and Loader services are real
implemented capabilities, but expanding them into a second complete POSIX
system is no longer the prerequisite for storage, networking or application
work. Reconcile or retire overlapping paths incrementally, with consumer and
recovery tests; this decision does not remove existing ABI behavior by itself.
Executable-memory policy, credentials and capability enforcement must also
cover any retained kernel path before claiming a unified security boundary.

## Decisions Superseded

- ADR-0008's default that all larger subsystems must be isolated, and
  ADR-0015's driver-residency default, yield to the explicit boundary above.
- ADR-0018's required VFS removal, ADR-0019's mandatory userspace external
  pagers, ADR-0020's mandatory network-stack service and ADR-0025's mandatory
  ordinary ELF migration are superseded. Their authority, lifetime and
  failure cases remain useful test requirements.
- ADR-0024's mandatory migration of ordinary POSIX process policy is
  superseded; diagnostic IDs still do not grant native domain authority.
- ADR-0006's and proposed ADR-0035's exclusion of network/persistence applies
  to the historical BusyBox delivery only. Preserve those results and source
  pins; they do not establish completion of this expanded system target.

The other authority, revocation, supervision and scheduling decisions remain
in force except where this decision explicitly changes residency.

## Acceptance

Complete each feature with current-source builds, relevant real-kernel and
userspace regressions, and a physical Raspberry Pi 3B+ run. Compilation,
host-tool tests and QEMU each establish a different part of that evidence.
Hardware-unverified work remains pending rather than checked off.

Use Release builds on the same board for performance regression baselines,
recording image/source identity, parameters, affinity, temperature, retries
and raw samples. Also compare representative equivalent operations with
Linux on that board. QEMU timing and batch means cannot stand in for board
performance or individual-operation tail latency. Set justified budgets for
the selected workloads from measured baselines; do not invent a universal
throughput or latency threshold. A correctness pass alone is not a speed claim.

The ordered implementation work and remaining gaps are tracked in
[the kernel roadmap](../kernel-roadmap.md).
