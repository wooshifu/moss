---
status: accepted
date: 2026-09-21
---

# Retain Independent Code Approval Revocation Authority

The Initial System Supervisor retains Code Approval Revocation Authority independently of the Code Authority Service to which it delegates approval work. The retained authority covers only its explicitly authorized approval scope; the supervisor's role or discovery identity does not confer a global revoke privilege. This keeps approvals surviving issuer failure under [ADR-0031](0031-preserve-code-approvals-across-authority-service-failure.md) manageable without requiring cooperation from the failed service.

A replacement Service Incarnation can manage previous approvals only after an authorized holder explicitly delegates the required rights and scope. Such delegation must not enlarge the holder's authority, reactivate revoked approval instances or rebind incarnation-bound IPC capabilities. The supervisor retains an independent recovery path rather than transferring away its sole means of revoking the covered approvals.

Code Approval Revocation Authority does not itself authorize issuing or reapproving code, establishing executable mappings, or terminating Execution Domains. Those operations require their own authority. Revocation keeps the semantics of [ADR-0028](0028-separate-code-approval-revocation-from-termination.md) and [ADR-0030](0030-do-not-reactivate-revoked-code-approvals.md): it blocks new admission under the withdrawn instance without restoring stale capabilities or implicitly terminating existing execution.

## Considered Options

Leaving the only revocation authority inside the issuing service was rejected because its failure could leave surviving approvals unmanageable. Automatically granting a replacement service control over previous approvals was rejected because service identity is not authority. Combining revocation, issuance and termination into one administrative privilege was rejected because recovery does not require all of those powers.

[Fuchsia's handle rights](https://fuchsia.dev/fuchsia-src/concepts/kernel/rights) associate permissions with handles, permit rights reduction and distinguish executable mapping from task termination. This supports separating delegated rights; it is not a claim that Fuchsia implements Moss's approval-recovery policy.

## Consequences

Acceptance must cover revocation by the supervisor after issuer death, rejection outside its delegated scope, denial of prior-approval management by an undelegated replacement, successful explicitly delegated recovery, and rejection of issuance or termination using revocation authority alone. Delegating recovery authority must preserve the supervisor's retained control, and revocation racing with mapping admission must obey the existing commit-order contract.

This is an accepted target architecture, not an implementation claim. Approval-scope selection, the retention and handoff protocol, the authorization ABI and recovery of interrupted requests remain separate decisions. Unexpected supervisor loss defaults to controlled reboot under [ADR-0033](0033-reboot-on-initial-system-supervisor-failure.md); this decision does not promise survival across supervisor failure or system reboot.
