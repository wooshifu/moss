---
status: accepted
date: 2026-09-19
---

# Do Not Rebind Capabilities Across Service Restarts

Every start or restart of an Isolated System Service creates a new Service Incarnation. Capabilities targeting that incarnation or its incarnation-bound objects remain bound to those targets. When that incarnation dies, its outstanding calls terminate with an explicit peer-death result and those capabilities become permanently invalid; restarting a service under the same discovery name does not retarget them. This does not invalidate every independent kernel object merely because the service created or transferred authority to it; Code Approval Instances follow [ADR-0031](0031-preserve-code-approvals-across-authority-service-failure.md).

Clients recover through Explicit Service Reconnection: they discover the replacement incarnation, obtain new capabilities and reconstruct their protocol state. The kernel does not replay requests. A client library may retry only when the service protocol explicitly defines the operation as safe to repeat.

## Considered Options

Transparent rebinding of old capabilities was rejected because a replacement service has lost volatile session state and may expose different objects or authority. Automatic kernel replay was rejected because the kernel cannot determine whether an interrupted filesystem, device or other state-changing operation completed or is idempotent. Keeping a stable service name remains useful for discovery, but possession of that name does not grant authority and does not identify a particular incarnation.

## Consequences

Service discovery and supervision must distinguish a stable logical name from an incarnation identity. The Mechanism Kernel Domain must close incarnation-bound endpoints, terminate pending calls and invalidate affected authority on death. Service protocols must define durable state, recovery and idempotency where needed, while clients must handle reconnection explicitly. Tests must cover stale capabilities, restart races, partially completed operations and prevention of late replies from a dead incarnation.
