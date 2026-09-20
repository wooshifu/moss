---
status: accepted
date: 2026-09-19
---

# Separate IPC Control and Data Planes

Moss will separate cross-domain control traffic from bulk-data transport. Bounded Control Messages are kernel-mediated, may copy a bounded payload and may atomically transfer Moss Capability Handles. Bulk payloads use a Shared Memory Data Plane over capability-backed Moss Memory Objects; control messages carry the handles plus the range and synchronization metadata needed to interpret that data.

The Mechanism Kernel Domain owns authority checks, mappings, isolation and object lifetime. Isolated System Services own protocol schemas, shared-memory layouts, queue structures and backpressure policy. Message-size limits, copy thresholds, synchronous versus asynchronous calls, ordering and synchronization primitives remain separate decisions.

## Considered Options

A universal kernel message or ring abstraction for control and bulk data was rejected because it would couple authority transfer and scheduling semantics to subsystem-specific throughput, layout and backpressure needs. Copying bulk payloads through kernel queues was rejected because phone and PC workloads can incur avoidable memory-bandwidth, latency and power costs. Using shared mutable memory for all control traffic was rejected because small authority-bearing operations need a bounded kernel mediation point with deterministic validation and transfer semantics.

## Consequences

Moss needs Memory Object operations for controlled mapping, rights reduction, cleanup and any later pinning or DMA integration. Services must validate shared ranges and define how buffers are owned, synchronized and reclaimed after failure. Tests must cover atomic handle transfer, invalid ranges, peer failure and mapping cleanup; measurements must determine whether any particular data path qualifies as a Kernel Residency Exception.
