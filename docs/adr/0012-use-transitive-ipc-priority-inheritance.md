---
status: accepted
date: 2026-09-19
---

# Use Transitive IPC Priority Inheritance

Moss will apply IPC Priority Inheritance while a caller is blocked in a Synchronous Control Call. The kernel temporarily raises the effective priority of the thread servicing the call to the highest effective priority of its blocked callers. If that thread makes a nested synchronous call, the inherited priority propagates along the resulting dependency chain.

Reply, cancellation, deadline expiry, peer closure or death removes the corresponding dependency and causes the kernel to recompute effective priorities. Configured base priorities do not change. CPU-budget accounting, deadline propagation, maximum call-chain depth, call handoff and deadlock policy remain separate decisions.

## Considered Options

Providing no cross-domain inheritance was rejected because moving filesystems, drivers and other system work into Isolated System Services would allow lower-priority service execution to delay higher-priority callers. Direct-caller-only inheritance was rejected because nested service calls would merely move the inversion to the next boundary. Userspace-managed boosts were rejected because they cannot atomically follow kernel wait dependencies or reliably unwind every cancellation and failure race.

## Consequences

The Mechanism Kernel Domain must track synchronous wait dependencies, distinguish base from effective priority, propagate changes transitively and remove individual contributions when calls terminate. It also needs defined behavior for cycles and changing sets of waiters. Tests must cover nested calls, multiple priorities, cancellation and reply races, peer death and complete priority restoration; inheritance does not itself resolve deadlock or guarantee a CPU-time budget.
