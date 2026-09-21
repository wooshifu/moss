---
status: accepted
date: 2026-09-21
---

# Preserve Existing Executable Mappings Across Repaging

Moss will apply the existing-mapping rule in [ADR-0028](0028-separate-code-approval-revocation-from-termination.md) to an admitted logical executable mapping, not to the momentary presence of its physical pages. Code Approval Revocation alone does not prevent an Existing Executable Mapping from satisfying a later demand fault with the same Immutable Code Version. This includes the first access to an admitted but never-resident page and refilling a page reclaimed under memory pressure. Otherwise, ordinary memory pressure would turn admission-policy revocation into unpredictable termination of previously admitted execution.

The surviving mapping remains bound to its admitted content version, object offsets, executable range and Execution Domain. A Pager Service cannot substitute a changed file, a newly approved version or mutable replacement contents. If the original contents cannot be supplied, the fault cannot be satisfied with different bytes; continuation is not a guarantee of pager availability or physical residency. Page supply must still validate that the original mapping is live, so a delayed response cannot recreate an unmapped or replaced mapping.

Creating or cloning executable mappings, including mappings in an address-space clone used for POSIX fork, is new admission and requires currently valid executable authority. Sharing physical pages, copying page-table entries or retaining an old handle does not inherit the source mapping's exemption. Removing a mapping and recreating it, or restoring execution permission after removing that permission, likewise requires admission. Concurrent admission and revocation use the ordering defined in ADR-0028; a clone cannot become executable using an approval already revoked in that order.

## Considered Options

Rechecking current code approval on every page-in was rejected because execution would then depend on which pages happened to remain resident. Allowing clones to inherit the exemption was rejected because existing execution could reproduce new executable mappings indefinitely after revocation. Pinning all admitted executable pages was rejected because admission policy should not prevent memory reclamation on phone and PC systems.

[Fuchsia's page-eviction design](https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0068_eviction_hints) describes reclaiming immutable executable contents and reading them back through a userspace pager. Its [mapping API](https://fuchsia.dev/reference/syscalls/vmar_map) separately checks executable rights when creating a mapping. These are references for distinguishing residency from mapping admission, not claims that Fuchsia implements Moss's revocation or clone policy.

## Consequences

Acceptance must cover revocation before first access, eviction and refill after revocation, substituted backing contents, delayed supply racing with unmap, unauthorized executable clones, permission restoration and clone admission racing with revocation. A failed clone must not publish unauthorized executable mappings or change the source mapping's right to continue. Emergency stopping still requires separate Execution Termination Authority under ADR-0028.

This is an accepted target architecture, not an implementation claim. Reapproval identity is defined in [ADR-0030](0030-do-not-reactivate-revoked-code-approvals.md). Approval-scope selection, the pager's content-verification protocol, compatibility error reporting and mapping-move semantics remain separate decisions.
