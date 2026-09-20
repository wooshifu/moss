---
status: accepted
date: 2026-09-19
---

# Separate Power Policy from Atomic Transitions

Moss will place performance, energy, ordinary thermal and system-suspend policy in a Power Policy Service. That service evaluates workload and platform constraints, coordinates service and driver dependencies, authorizes wake sources and requests transitions through bounded capabilities. Clock, regulator and device power-domain protocols remain in their corresponding Isolated Device Drivers.

The Mechanism Kernel Domain retains Atomic Power Transitions that must be synchronized with run queues, timers, interrupts, CPU state and final platform entry or early resume. It also enforces resource and safety limits, applies emergency thermal protection when the platform exposes a suitable mechanism and enters a conservative safe state if the policy service dies. These mechanisms validate and commit requests but do not select product policy.

## Considered Options

Keeping the complete power governor and device dependency graph in the kernel was rejected because phone and PC products require different workload, battery, thermal and recovery policy. Allowing userspace to execute final idle, wake or suspend sequences directly was rejected because runnable work, timers or interrupts can race with those transitions. Delegating all thermal protection to a restartable service was rejected because loss of that service must not remove a hard safety boundary.

## Consequences

Moss needs capability-scoped performance and wake constraints, observable accounting, a transactional suspend protocol across services and drivers, and a final kernel commit or rollback point. The kernel and platform descriptions must expose supported transitions and safety limits without embedding product heuristics. Tests must inject work and interrupts at transition boundaries, fail services during suspend and verify safe fallback. Idle-state selection rules, thermal thresholds, DVFS algorithms and product recovery behavior remain platform or policy decisions backed by measurements.
