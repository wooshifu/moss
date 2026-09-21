---
status: accepted
date: 2026-09-21
---

# Require Explicit Authority for Executable Memory

Moss will require explicit capability authority for every ordinary executable mapping. A Code Authority Service uses delegated authorization rights to approve an Immutable Code Version according to product policy and obtain an Executable Memory Capability. Ordinary Memory Object ownership, write permission, or a Loader Service's ability to construct domains does not allow it to add execution rights. The minimal authenticated bootstrap exception remains as defined in ADR-0025.

The Mechanism Kernel Domain enforces capability rights, content-version identity and global write/execute exclusion; it does not parse certificates or packages or choose trusted publishers. The same backing storage must not have simultaneous writable and executable aliases across Execution Domains, Memory Objects or authorized device mappings. Renaming an object, duplicating a handle or constructing another mapping cannot evade this invariant. Pager-backed executable memory must remain bound to the approved immutable contents; later faults cannot substitute changed file bytes under an earlier approval.

JIT Authority is a separately delegated right, not an exemption from write/execute exclusion. Publishing generated code requires ending writable access, completing the necessary cross-CPU mapping and instruction-cache synchronization, and committing an immutable version before executable access begins. Editing the same storage requires ending executable access before writable access resumes; the changed contents require a new publication and cannot retain the previous version's approval. The kernel must enforce these transitions against concurrent mappings and access, rather than trusting a JIT runtime to sequence them correctly.

## Considered Options

Allowing ordinary memory owners to add execution rights was rejected because it would bypass code-approval policy. Enforcing W^X only on individual mappings was rejected because a writable alias in another domain could modify executable contents. Checking a filename or mutable file once was rejected because a pager or subsequent writer could replace the approved bytes. Embedding signing and publisher policy in the kernel was rejected because phone, PC and development products need different approval rules without different native memory ABIs.

## Consequences

Memory ownership, pager protocols, mapping revocation and JIT publication need a shared immutable-version and alias-lifetime contract. Acceptance must cover unauthorized permission upgrades, cross-domain and shared-backing aliases, pager-content substitution, concurrent publication and permission changes, stale approval, and failure cleanup without temporarily granting conflicting access. This is an accepted target architecture, not a claim that these mechanisms are implemented today.

Phone release policy may require signed code and restrict JIT delegation; PC or development policy may authorize locally approved unsigned programs through the same mechanism. The boot trust boundary is defined in [ADR-0027](0027-establish-initial-authority-through-verified-boot.md), and approval revocation is separated from termination of existing execution in [ADR-0028](0028-separate-code-approval-revocation-from-termination.md). Exact policy roots and the authorization ABI remain separate decisions.
