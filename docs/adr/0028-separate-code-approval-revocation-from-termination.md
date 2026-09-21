---
status: accepted
date: 2026-09-21
---

# Separate Code Approval Revocation from Termination

Moss will distinguish Code Approval Revocation from kernel-enforced termination of existing execution. Revoking an approval prevents that approval from authorizing new executable mappings, execution-permission upgrades or program loading. Copying or transferring an old Executable Memory Capability cannot bypass the revocation. Already-established executable mappings remain usable by default; revocation alone does not terminate their Execution Domains.

Userspace policy chooses which approvals to revoke and whether an incident also requires immediate termination. The Initial System Supervisor or another explicitly authorized controller must possess separate Execution Termination Authority to request that the Mechanism Kernel Domain stop the affected domains. Code-approval authority alone does not grant termination authority. Recovery and replacement-service policy remain in userspace.

The kernel must order approval revocation against concurrent executable-mapping commits: a mapping cannot commit using an approval already revoked in that order. A mapping committed before revocation falls under the existing-mapping policy. Revocation cannot relax the immutable-content or global write/execute exclusion requirements for surviving mappings. A successful termination-completion observation must mean the targeted threads have stopped, not merely that an ignorable notification was sent; the exact request and completion ABI remains open.

## Considered Options

Automatically terminating every consumer when an approval is revoked was rejected because it conflates changing future admission policy with stopping existing work, and silently grants termination power to code-approval services. Merely closing one handle was rejected because duplicates or transferred handles could retain the old authority. Treating emergency termination as a cooperative notification was rejected because untrusted code could ignore it.

[Fuchsia's handle rights](https://fuchsia.dev/fuchsia-src/concepts/kernel/rights) separately authorize executable mappings and task termination. That is a reference for separating the rights, not a claim that Fuchsia implements Moss's chosen revocation semantics.

## Consequences

Ordinary revocation does not guarantee that previously admitted code immediately stops. An emergency response requiring that guarantee must also terminate affected domains using independently delegated authority. Acceptance must cover copied and transferred stale capabilities, revocation racing with mapping or permission upgrades, continued existing execution after ordinary revocation, unauthorized termination attempts and observable termination across CPUs.

This is an accepted target architecture, not an implementation claim. Subsequent demand faults and address-space cloning are defined in [ADR-0029](0029-preserve-existing-executable-mappings-across-repaging.md), and reapproval without restoring revoked authority is defined in [ADR-0030](0030-do-not-reactivate-revoked-code-approvals.md). Approval-scope selection, identification of affected domains and the revocation ABI remain separate decisions.
