---
status: accepted
date: 2026-09-19
---

# Default to Isolated System Services

Moss will preserve fault containment by default: the Mechanism Kernel Domain owns only the privileged mechanisms and global invariants required to isolate and schedule the system, while policy-rich or failure-prone functionality runs as Isolated System Services. A service failure may fail outstanding work or require client reconnection, but it must not corrupt the Mechanism Kernel Domain; restart and state-recovery contracts are defined per service.

Kernel residency is an exception, justified only by a bootstrap dependency, privilege that cannot be safely mediated, or measurements showing that the isolated boundary cannot meet an explicit latency, throughput or power budget. Describing code as performance-sensitive is not evidence. A justified exception should keep policy and complex state machines isolated when a smaller kernel fast path is sufficient.

## Considered Options

A monolithic-by-default design was rejected because a driver or service defect would retain kernel-wide authority and failure impact. A purity rule forbidding every kernel fast path was also rejected because phone and PC workloads may expose measured data-path or power costs that require a narrowly scoped split implementation.

## Consequences

Moss needs explicit IPC authority, revocation, service lifecycle and failure-observation contracts before moving production subsystems across the boundary. Acceptance must inject service failures as well as measure the end-to-end boundary cost; neither a successful restart nor a microbenchmark alone proves the design.
