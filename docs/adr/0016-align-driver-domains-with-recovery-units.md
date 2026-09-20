---
status: accepted
date: 2026-09-19
---

# Align Driver Domains with Recovery Units

The default protection boundary for an Isolated Device Driver will be the Driver Recovery Domain: the smallest set of device functions that can be independently authorized, isolated, reset and reconstructed after failure. An independently recoverable device instance receives its own domain even when it uses the same driver implementation as another instance.

Functions that share an inseparable IOMMU group, reset line, firmware state or other hardware recovery dependency belong to one domain. A phone SoC pipeline may therefore require a multi-function domain, while unrelated peripheral instances remain separate. Grouping beyond the smallest hardware recovery unit requires an explicit service-manifest declaration and measurements showing that the larger failure boundary is necessary for an accepted memory, power or IPC budget.

## Considered Options

One process per source-level driver was rejected because software-module boundaries do not necessarily match hardware isolation or reset boundaries. A single driver host per device class or for the whole system was rejected because an unrelated defect would revoke and restart otherwise independent devices. Rigidly requiring one process per hardware function was rejected because functions that share DMA isolation, reset or firmware state cannot actually recover independently.

## Consequences

The userspace driver coordinator needs topology and isolation metadata sufficient to construct domains, while the kernel must assign device, interrupt, MMIO, DMA and reset capabilities consistently with the selected unit. Domain manifests must expose intentional grouping for review. Failure tests must verify that restarting one domain neither preserves stale device authority nor disrupts independent domains; platform descriptions must report hardware coupling rather than presenting logical functions as independently isolated.
