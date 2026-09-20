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
