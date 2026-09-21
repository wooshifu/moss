---
status: accepted
date: 2026-09-21
---

# Preserve Code Approvals Across Authority Service Failure

A Code Approval Instance has kernel-maintained validity independent of the continued execution of the Code Authority Service that issued it. Death of that Service Incarnation does not revoke its committed approvals. Holders of valid Executable Memory Capabilities may continue to establish new executable mappings within their delegated rights, subject to the immutable-content and write/execute invariants of [ADR-0026](0026-require-explicit-authority-for-executable-memory.md). Existing Executable Mappings retain the continuation semantics of [ADR-0029](0029-preserve-existing-executable-mappings-across-repaging.md).

The failed service can no longer process new approval requests. Its incarnation-bound endpoints and outstanding calls still obey [ADR-0013](0013-do-not-rebind-capabilities-across-service-restarts.md); survival of an approval neither keeps its issuing service alive nor rebinds old IPC capabilities to a replacement. The kernel enforces already-committed authority without consulting the failed service, but does not assume its signature, package-origin or local-approval policy. A failed call does not by itself prove that an approval was never committed, and the kernel does not replay the request.

A replacement Code Authority Service receives approval or approval-management authority only through explicit delegation. A matching service name, executable image or content identifier grants no access to prior approvals. Restart does not reactivate revoked approval instances, as required by [ADR-0030](0030-do-not-reactivate-revoked-code-approvals.md). An independently authorized controller may still revoke surviving approvals; service failure is not itself that operation and grants no termination authority.

## Considered Options

Automatically revoking every approval on issuer death was rejected because an ordinary service failure would unexpectedly invalidate already-delegated admission authority. Requiring a live issuer for each mapping was rejected because the kernel can enforce committed rights without a policy-service round trip. Automatically granting a restarted service control of previous approvals was rejected because discovery identity does not establish authority.

[Fuchsia's handle model](https://fuchsia.dev/fuchsia-src/concepts/kernel/handles#garbage-collection) distinguishes closing a handle from destroying the referenced kernel object when other references remain. This is a reference for separating a holder's lifetime from object lifetime, not a claim that Fuchsia implements Moss's code-approval or recovery policy.

## Consequences

Acceptance must cover new executable admission after issuer death, continued existing execution, rejection of revoked approvals before and after restart, dead IPC endpoints, failure racing with approval commit, and rejection of approval-management operations by an undelegated replacement service. Losing the issuer must not relax content identity, rights or cross-domain write/execute exclusion.

This is an accepted target architecture, not an implementation claim. It specifies service failure within one running kernel, not persistence across system reboot. Independent retention and explicit redelegation of revocation authority are defined in [ADR-0032](0032-retain-independent-code-approval-revocation-authority.md). Approval-scope selection, the detailed recovery protocol, recovery of interrupted requests and the authorization ABI remain separate decisions.
