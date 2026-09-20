---
status: accepted
date: 2026-09-19
---

# Separate Execution Domains from POSIX Processes

Moss will expose capability-addressed Execution Domains and Threads as its native kernel execution objects. An Execution Domain owns an address space, capability table, resource accounting and termination state. Threads own register, scheduling and exception state. Kernel-internal object identifiers are diagnostic values only and do not grant authority.

A Process Compatibility Service will construct POSIX PID namespaces, parent and child relationships, process groups, sessions, compatibility credentials, wait or zombie state, job control and signal semantics over those native objects. It coordinates `fork` and `exec` behavior using kernel mechanisms for address-space copying or construction, thread creation, termination and explicit handle inheritance. Capability-authorized termination or thread control remains enforced by the kernel.

## Considered Options

Keeping global PIDs, parentage, uid or gid credentials and POSIX signals in the native kernel object was rejected because it would make a compatibility naming and policy model part of the authority foundation. Moving address spaces, thread state or final termination entirely to a process server was rejected because memory isolation, scheduling and resource reclamation are kernel invariants. Treating internal object IDs as usable authority was rejected because lookup by a reusable or observable number creates ambient access and stale-identifier risks.

## Consequences

The existing kernel `Process` and `ProcessManager` responsibilities must be split incrementally: address space, threads, handles and enforcement remain, while PID allocation, family relationships, credentials and compatibility signaling move behind service protocols. Moss needs generic exception delivery, capability-checked thread control, exit observation and explicit handle inheritance. The Process Compatibility Service needs a restart and reconstruction contract so loss of its namespace state is not confused with survival of the underlying domains. Tests must distinguish diagnostic identifiers from authority and cover service failure, stale identities, fork, exec, wait and signal races.
