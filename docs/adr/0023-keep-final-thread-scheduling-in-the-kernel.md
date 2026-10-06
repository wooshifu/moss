---
status: accepted
date: 2026-09-19
---

# Keep Final Thread Scheduling in the Kernel

Moss will keep the Kernel Scheduling Core in the Mechanism Kernel Domain. It owns run queues, preemption, blocking and wakeup, final context switches, CPU affinity enforcement, CPU-time accounting, resource limits and admission control. It also implements IPC Priority Inheritance atomically with synchronous call dependencies and coordinates scheduling with interrupts, timers and Atomic Power Transitions.

Userspace resource and product-policy services may delegate Scheduling Profile Capabilities for selected threads or groups. These capabilities request bounded configurations such as foreground, background or latency-sensitive treatment; the kernel validates them and remains the final enforcement point. Application runtimes may schedule coroutines or green threads over kernel carrier threads but cannot directly manipulate kernel run queues.

## Considered Options

A general userspace final scheduler was rejected because its own blocking, paging or failure could remove system-wide progress and because it could not atomically maintain interrupt, timer and IPC wait invariants. Embedding product roles and foreground policy directly in the kernel was rejected because those classifications change independently across phone and PC systems. Treating the current fair-scheduler algorithm as the native ABI was rejected because the mechanism boundary must survive later algorithm changes.

## Consequences

The kernel needs explicit base and effective scheduling state, accounting domains, capability-checked profile changes and admission failure results. Userspace policy needs observable metrics without access to mutable run-queue internals. Tests must cover profile-rights enforcement, overload, multicore migration, priority inheritance and policy-service failure. Scheduling classes, fairness algorithms, real-time budget models and deadline propagation remain separate decisions.

## Initial CPU Budget Profile Contract

The first public Scheduling Profile Capability controls the periodic CPU allowance of one execution domain incarnation's main kernel thread. The initial supervisor creates a profile for a domain it can inspect, setting an immutable period and maximum runtime in nanoseconds. The capability can be transferred or duplicated with reduced `APPLY` and `OBSERVE` rights. Applying it remotely chooses a positive runtime no greater than its maximum. This is a one-time request for that thread: a second apply is rejected so callers cannot restart the accounting window to escape a limit. Fork creates a new, unbudgeted thread; a profile stays bound to its original thread and does not authorize a replacement thread or reused PID. This contract only controls CPU allowance; the existing POSIX `nice` operation remains separate.

Admission reserves the requested runtime when the kernel accepts the apply. Publicly budgeted threads share one period while any reservation is active, and their runtime sum cannot exceed one CPU's period. This deliberately conservative bound remains valid if every thread is pinned or migrated to the same CPU. It rejects a different period or excess capacity rather than offering a reservation the scheduler cannot safely account for; supporting independent periods or using all online CPUs requires a later affinity-aware admission model. The scheduler applies an accepted request at the target's next safe scheduling point, and status distinguishes accepted from effective configuration. Exit releases the reservation even while a capability retains the terminated domain object.

The charged resource is actual calibrated CPU time of the running thread, including kernel work and time spent serving synchronous IPC. Priority donation does not move the charge to a blocked caller. An unused allowance does not accumulate, and a tick or non-preemptible overrun is carried as debt. Enforcement remains a soft limit at the scheduler's existing tick cadence. The status operation reports the admitted runtime and period, cumulative CPU nanoseconds, throttle transitions and pending/parked/terminated flags; it does not expose run-queue internals or promise a simultaneous snapshot of CPU counters on different CPUs.

When the calibrated timer is unavailable, apply fails without reserving capacity or silently using raw architectural counters. Closing the last profile handle or losing the policy service does not remove a configured limit; the kernel retains enforcement and admission until the target exits. A restarted service may observe a surviving profile only if it receives an appropriate capability from the supervisor. Dynamic deadline tightening, call-chain depth and deadlock policy remain separate work.
