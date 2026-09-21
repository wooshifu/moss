---
status: accepted
date: 2026-09-21
---

# Do Not Reactivate Revoked Code Approvals

Reapproving an Immutable Code Version creates a distinct Code Approval Instance; it never restores an instance withdrawn by Code Approval Revocation. An Executable Memory Capability remains bound to the particular approval instance under which it was issued. All copies, attenuated derivatives and transferred capabilities retain that binding, including capabilities transferred before revocation but received afterward. Possession of revoked authority cannot become sufficient for new executable admission merely because the same contents are approved again.

Clients that need new executable mappings must explicitly obtain or receive capabilities under the new approval, within their delegated rights. A content identifier, pathname, service discovery name or stale handle cannot select a replacement approval implicitly. Approval identity is separate from content identity: independent approvals may concern the same immutable contents without requiring duplicate physical pages, but they do not share validity merely because the bytes match.

The Mechanism Kernel Domain must preserve this distinction when ordering mapping admission, revocation and reapproval. A pending operation authorized under the old instance cannot silently retry under the new one; if the old approval has been revoked before admission commits, that operation cannot authorize the mapping. Reusing internal storage must not retarget surviving stale references to a new approval. The representation of identities, handle slots and validity state remains an implementation decision, not a prescribed counter format.

Existing Executable Mappings retain the continuation and repaging semantics of [ADR-0028](0028-separate-code-approval-revocation-from-termination.md) and [ADR-0029](0029-preserve-existing-executable-mappings-across-repaging.md). Reapproval does not rebind those mappings, and their continued execution does not require obtaining replacement capabilities. Creating or cloning mappings, or restoring removed execute permission, still requires currently valid authority.

## Considered Options

Automatically restoring old capabilities when identical code is reapproved was rejected because previously leaked or deliberately withdrawn authority would regain power without a new delegation decision. Looking up the latest approval by content identity was rejected for the same reason. Requiring different code bytes before a fresh approval was rejected because the policy decision can change without changing the program.

[Fuchsia's handle model](https://fuchsia.dev/fuchsia-src/concepts/kernel/handles) distinguishes the referenced kernel object from each handle's rights and describes duplication as retaining the same target object. This is a reference for separating object identity from possession of handles, not a claim that Fuchsia implements Moss's code-approval or revocation policy.

## Consequences

Acceptance must cover reapproval of identical contents, continued rejection of old capabilities and their copies or in-transit transfers, successful admission through explicitly delegated new authority, revocation of the new instance, admission racing with revoke/reapprove, and identity-storage reuse while stale references remain. Previously admitted mappings must retain the behavior established by ADR-0029 throughout these transitions.

This is an accepted target architecture, not an implementation claim. Approval lifetime across Code Authority Service failure is defined in [ADR-0031](0031-preserve-code-approvals-across-authority-service-failure.md). Approval-scope selection, the retention and handoff protocol for approval-management authority, and the authorization and revocation ABI remain separate decisions.
