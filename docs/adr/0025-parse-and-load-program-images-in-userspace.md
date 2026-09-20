---
status: accepted
date: 2026-09-19
---

# Parse and Load Program Images in Userspace

Moss will use a Loader Service to parse ordinary ELF and any future executable formats. The service obtains an authorized file or Memory Object, interprets segments, interpreters, dynamic-linking and TLS metadata, creates an Execution Domain, requests mappings, builds initial stack and thread state and transfers only the capabilities selected for inheritance.

The Mechanism Kernel Domain validates the caller's authority, address ranges, mapping rights, resource limits and generic executable-memory invariants, then installs mappings and creates the initial Thread. It does not parse ELF headers or implement dynamic linking. Starting the Initial System Supervisor uses a minimal fixed boot protocol or an image description already authenticated by the boot environment; that bootstrap exception does not become the normal execution ABI.

The Process Compatibility Service can implement POSIX `exec` by constructing and validating the replacement domain before atomically updating compatibility identity and retiring the old domain. A malformed image therefore fails in the Loader Service without partially replacing the existing execution state.

## Considered Options

Keeping ELF parsing and `exec` image construction in the kernel was rejected because executable formats, interpreters and dynamic-linking behavior are policy-rich parsers that do not protect kernel isolation. Allowing a loader to install arbitrary page-table entries was rejected because generic mapping and execution constraints remain kernel invariants. Requiring the kernel to support every future native image format was rejected because format evolution should not expand the system-call ABI.

## Consequences

The current kernel ELF load planner and `execve` implementation require an incremental replacement with capability-checked domain-construction primitives. Loader, Process Compatibility, Filesystem and Pager Services need explicit protocols for image identity, mappings, inherited handles and failure cleanup. Tests must submit malformed and adversarial images and verify that only the target domain can be affected. Code-signing policy, executable-memory authority, JIT permission and the boot trust chain remain separate decisions.
