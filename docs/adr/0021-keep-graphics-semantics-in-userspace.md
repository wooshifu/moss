---
status: accepted
date: 2026-09-19
---

# Keep Graphics Semantics in Userspace

Moss will implement its operational graphics pipeline in userspace through a Graphics Execution Service, a Display Driver Service and a Compositor Service. Applications exchange capability-authorized surface Memory Objects and synchronization objects with these services, avoiding payload copies without granting ambient access to application memory.

GPU and display roles follow the Driver Recovery Domain rule: they remain separate when hardware authority and reset permit, but may occupy one driver domain when their IOMMU group, firmware or reset state is inseparable. The Compositor Service remains isolated from device-driver authority. A GPU that cannot confine contexts or DMA makes its execution service an explicitly trusted domain rather than weakening the isolation claim.

The Mechanism Kernel Domain provides generic Memory Objects, mapping protection, DMA/IOMMU enforcement, interrupt delivery and wait or event mechanisms. It does not define windows, surfaces, pixel formats, modesetting or composition policy. Minimal boot diagnostics may draw before the Initial System Supervisor starts, but that path is relinquished or isolated from the operational display pipeline.

## Considered Options

A kernel graphics or windowing stack was rejected because graphics policy, complex parsers and rapidly changing hardware protocols do not protect kernel invariants. Combining composition with the device driver was rejected because UI policy faults should not carry MMIO, DMA or reset authority. Requiring GPU and display functions to occupy separate domains on every platform was rejected because some integrated devices cannot be independently isolated or recovered.

## Consequences

Moss needs protocols for buffer allocation and ownership, synchronization, presentation, protected content and explicit failure of submitted work. Early-display handoff must revoke bootstrap authority before normal operation. Tests must verify that services cannot access ungranted surfaces, that stale buffers and synchronization objects are rejected after restart, and that GPU, display and compositor failures have the declared recovery radius. Graphics-specific fast paths require measured Kernel Residency Exceptions rather than a second kernel API.
