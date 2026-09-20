---
status: accepted
date: 2026-09-19
---

# Use Capability Handles for Cross-Domain Authority

Moss will use process-local capability handles as the native authority model between protection domains. A handle references a typed kernel object and carries explicit rights; operations require the corresponding handle right, rights cannot be amplified, and handle transfer, revocation and cleanup are mediated by the Mechanism Kernel Domain. Numeric PIDs, paths, device numbers and globally visible object identifiers may support discovery or diagnostics but do not grant authority by themselves.

POSIX file descriptors, paths, uid/gid credentials and related permission checks remain a POSIX Compatibility View implemented over capability-backed services. This preserves the Moss Native System's selected source compatibility without making ambient POSIX naming the trust foundation.

## Considered Options

Keeping the current PID-, descriptor- and global-ID-oriented model as the native authority system was rejected because moving a service into another process would otherwise preserve ambient lookup authority and broaden confused-deputy and stale-identifier risks. Using capabilities only for selected high-risk resources was rejected because two competing authority models would make delegation, revocation and audit dependent on subsystem-specific rules.

## Consequences

Kernel objects need typed handle tables, rights reduction, transfer and revocation semantics, peer-death observation and deterministic cleanup. Existing file, process and IPC identifiers require an incremental compatibility mapping rather than a flag-day replacement. Protocol servers must validate both the operation encoded by a request and the authority transferred with it.
