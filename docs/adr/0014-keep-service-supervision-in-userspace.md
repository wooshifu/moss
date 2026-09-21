---
status: accepted
date: 2026-09-19
---

# Keep Service Supervision in Userspace

The Mechanism Kernel Domain will create one Initial System Supervisor and supply it with a finite Bootstrap Capability Set. That supervisor constructs the userspace system: it starts Isolated System Services, delegates resources and authority, orders dependencies, maintains or launches discovery facilities, observes service death and applies restart, backoff, degradation or recovery policy.

The kernel supplies process creation, resource controls, capability delegation and revocation, peer-death notification and the other enforcement mechanisms needed by the supervisor. It does not contain the service dependency graph or ordinary restart policy. Unexpected loss of the Initial System Supervisor enters the system-level failure path defined in [ADR-0033](0033-reboot-on-initial-system-supervisor-failure.md): a controlled reboot by default, not an implicit attempt to reconstruct userspace state in the kernel.

## Considered Options

Embedding service discovery, dependency and restart policy in the kernel was rejected because phone and PC products require different boot graphs, optional hardware and recovery behavior, and those policies should be independently replaceable without enlarging the Mechanism Kernel Domain. Allowing services to obtain ambient machine authority or start peers without delegated capabilities was rejected because it would bypass the explicit authority model and make system construction unauditable.

## Consequences

Moss needs an authenticated initial userspace image, a defined Bootstrap Capability Set and kernel mechanisms sufficient to construct every later service without additional ambient privilege. The supervisor needs declarative or equivalent service policy, dependency handling and failure accounting. Tests must cover least-authority delegation, dependency failures, restart loops and supervisor death. The boot-image trust boundary is defined in [ADR-0027](0027-establish-initial-authority-through-verified-boot.md), and the default fatal recovery action in ADR-0033; the detailed protocols and repeated-boot-failure policy remain separate decisions.
