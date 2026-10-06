---
status: accepted
date: 2026-09-19
---

# Use Transitive IPC Priority Inheritance

Moss will apply IPC Priority Inheritance while a caller is blocked in a Synchronous Control Call. The kernel temporarily raises the effective priority of the thread servicing the call to the highest effective priority of its blocked callers. If that thread makes a nested synchronous call, the inherited priority propagates along the resulting dependency chain.

Reply, cancellation, deadline expiry, peer closure or death removes the corresponding dependency and causes the kernel to recompute effective priorities. Configured base priorities do not change. At this decision's acceptance, CPU-budget accounting, deadline propagation, maximum call-chain depth, call handoff and deadlock policy were left for follow-up decisions.

## In-Flight Deadline Tightening

A synchronous call starts with the earlier of its explicit absolute deadline and the deadlines of callers currently donated to its thread. A later Reply binding or handoff with an earlier deadline tightens every active downstream call in that wait chain. Tightening is monotonic for the life of each call: removing the donor does not lengthen a deadline already passed to a downstream service. Zero means no deadline. A strict decrease at every step stops propagation on cycles without assuming that cycles are otherwise safe.

Every pending call reserves one of the timer subsystem's fixed slots before it enters a Channel, including calls that start without a deadline. Exhaustion returns `ENOMEM` before enqueue. Publication and timer arming happen under the same Channel lock, so preemption after enqueue cannot leave a published call without a deadline timer. A first finite deadline activates that reserved timer; a later tighter deadline moves its heap expiry earlier without waiting for the blocked caller to run. The timer queue belongs to CPU 0; changes from another CPU notify CPU 0 to reprogram its local compare instead of replacing the sender CPU's scheduler tick. Reply admission reads the current atomic deadline, while the Channel lock chooses one terminal outcome among reply, expiry, cancellation and peer loss. A concurrent deadline change and reply are decided by the deadline value the reply observes. The timer callback still completes through the Channel after the timer queue lock has been released.

The current native handoff paths bind a Reply before its recipient makes a nested call; Reply authority cannot be copied or inherited, and there is no user operation that assigns a new Reply to a thread already blocked in another call. Kernel scheduler and timer tests exercise that late-binding state directly, while real IPC tests check that existing user paths still honor their initial and inherited deadlines. The full dynamic deadline item remains open until a production path can create a late binding and a real blocked-call interleaving test covers it. Maximum call-chain depth and deadlock policy remain separate decisions.

## Considered Options

Providing no cross-domain inheritance was rejected because moving filesystems, drivers and other system work into Isolated System Services would allow lower-priority service execution to delay higher-priority callers. Direct-caller-only inheritance was rejected because nested service calls would merely move the inversion to the next boundary. Userspace-managed boosts were rejected because they cannot atomically follow kernel wait dependencies or reliably unwind every cancellation and failure race.

## Consequences

The Mechanism Kernel Domain must track synchronous wait dependencies, distinguish base from effective priority, propagate changes transitively and remove individual contributions when calls terminate. It also needs defined behavior for cycles and changing sets of waiters. Tests must cover nested calls, multiple priorities, cancellation and reply races, peer death and complete priority restoration; inheritance does not itself resolve deadlock or guarantee a CPU-time budget.
