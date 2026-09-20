---
status: accepted
date: 2026-09-19
---

# Use First-Class Synchronous Control Calls

Moss will provide Synchronous Control Calls as a native IPC operation for request/reply interactions with Isolated System Services. A call sends a Bounded Control Message, blocks the caller and supplies the selected service with a kernel-managed Reply Capability. That one-shot capability authorizes exactly one reply to the corresponding pending call.

The kernel commits exactly one terminal outcome when a reply races with caller cancellation, deadline expiry, or peer closure or death. Committing an outcome consumes or invalidates the Reply Capability and releases the associated kernel state. Asynchronous notifications and streaming remain complementary mechanisms rather than alternate modes hidden inside the call operation. Priority propagation, maximum call-chain depth and default deadline policy remain separate decisions.

## Considered Options

An asynchronous-only kernel API was rejected because every system RPC framework would need to reconstruct correlation, blocking, cancellation, peer-death handling and cleanup while hiding the scheduling dependency from the kernel. Matching replies with reusable channel identifiers or caller-chosen correlation values was rejected because those names do not themselves confer authority to complete a particular call.

## Consequences

The Mechanism Kernel Domain must maintain pending-call state, block and wake threads, issue and invalidate Reply Capabilities, arbitrate terminal races and reclaim state when either peer exits. System-service APIs can expose conventional call semantics while remaining capability-authorized. Tests must exercise duplicate and late replies, cancellation and reply races, peer death, resource cleanup and nested calls; deadlock prevention and priority inversion still require explicit scheduling decisions.
