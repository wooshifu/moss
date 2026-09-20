---
status: accepted
date: 2026-09-19
---

# Require Hardware-Enforced DMA Confinement

Moss will grant bus-master capability to an ordinary Isolated Device Driver only when hardware provides DMA Confinement for its Driver Recovery Domain. An IOMMU, SMMU or enforceable device DMA window must restrict the device to pages explicitly mapped for that domain. Userspace address-space isolation and bounce-buffer allocation alone do not prevent a driver that controls DMA address registers from targeting arbitrary physical memory.

Without enforceable confinement, Moss fails closed by default and does not enable bus mastering. A platform that must support such a device must explicitly select non-DMA operation, a device-specific mediated submission path justified as a Kernel Residency Exception, a DMA-Trusted Driver Domain recorded as part of the trusted computing base, or disabling the device. The system must not describe a DMA-trusted domain as fully isolated.

## Considered Options

Treating every userspace driver as isolated was rejected because an unconstrained device can bypass CPU page tables. Treating bounce buffers as a security boundary was rejected because they constrain cooperative transfers but do not stop arbitrary addresses programmed into the device. Automatically moving every unconstrained driver into the kernel was rejected because kernel residency does not create DMA confinement and would hide rather than remove the expanded trust boundary.

## Consequences

The Mechanism Kernel Domain needs authority and lifetime rules for IOMMU groups, DMA mappings, pinned pages, invalidation, device quiescence and reset. Bus mastering must not begin before confinement is installed, and mappings must not disappear while the device can still access them. Platform manifests must expose DMA-trusted domains for review. Tests must exercise out-of-range DMA rejection and teardown races on supported hardware or faithful emulation; environments unable to generate such transactions cannot claim that isolation was validated.
