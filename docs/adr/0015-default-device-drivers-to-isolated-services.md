---
status: accepted
date: 2026-09-19
---

# Default Device Drivers to Isolated Services

Moss will run device drivers as Isolated Device Drivers by default. Device enumeration and matching policy, hardware protocol state machines, descriptor-ring management, firmware-loading policy, device-class behavior and operational suspend, resume and recovery logic belong in userspace driver services.

The Mechanism Kernel Domain retains Device Authority Primitives: interrupt-controller operation and safe interrupt delivery, capability-scoped MMIO or I/O-port mapping, DMA/IOMMU mapping and page-lifetime enforcement, exclusive device ownership and reset authority. It may also retain the minimal platform-specific code required to reach the Initial System Supervisor, such as CPU, interrupt, timer and early diagnostic mechanisms. Any larger in-kernel driver remains a Kernel Residency Exception.

## Considered Options

Keeping the existing in-kernel driver object and callback model as the default was rejected because driver protocol defects would retain kernel-wide failure impact and authority. Requiring every device interaction to be interpreted by a generic kernel driver framework was rejected because it would move device-specific policy and complexity back into the Mechanism Kernel Domain. A rule forbidding all boot-time device code in the kernel was rejected because some platform mechanisms must operate before userspace can be constructed.

## Consequences

Moss needs capability types and APIs for device resources, interrupt delivery, DMA mappings, ownership transfer and reset, plus a userspace coordinator for enumeration and binding. The current direct `Driver` callbacks and kernel-owned matching tables require an incremental replacement rather than being exposed as the native cross-domain API. Devices that can DMA without an effective IOMMU are not fully isolated and require a separately chosen bounce-buffer, trusted-domain or residency policy. Tests must inject driver crashes and malformed device activity and verify interrupt, mapping, DMA and restart cleanup.
