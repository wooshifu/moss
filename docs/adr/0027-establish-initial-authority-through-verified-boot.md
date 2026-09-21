---
status: accepted
date: 2026-09-21
---

# Establish Initial Authority Through Verified Boot

For a verified Moss boot, initial authority will be established through a Moss Boot Trust Chain rooted in the platform firmware and bootloader. The boot environment authenticates the kernel, Initial System Supervisor and Boot Authorization Configuration before the kernel supplies the Bootstrap Capability Set. This resolves the bootstrap trust boundary left open by [ADR-0014](0014-keep-service-supervision-in-userspace.md), [ADR-0025](0025-parse-and-load-program-images-in-userspace.md) and [ADR-0026](0026-require-explicit-authority-for-executable-memory.md); the minimal authenticated bootstrap path does not become the ordinary application-loading ABI.

The Initial System Supervisor delegates code-approval authority only to a Code Authority Service whose image and eligibility have been established through that authenticated bootstrap policy. An initial authority service cannot establish its own trust by approving itself, and a service name alone is not proof of identity. Later application approval remains userspace policy over Immutable Code Versions and explicitly delegated capabilities.

The Mechanism Kernel Domain consumes the authenticated boot handoff and enforces capability delegation, object identity and mapping constraints. It does not own certificate or package parsing, publisher selection or ordinary service policy. The boot protocol must bind the authenticated images and configuration to the actual contents used at handoff; a previously checked pathname or a mutable replacement image is insufficient.

PC and development products may use an explicitly configured user trust root. Boot Verification State must distinguish authentication under that root from disabled verification and from authentication under the production root; an unverified boot must not be presented as production-verified. User-selected roots are compatible with verified boot, as illustrated by [AOSP's user-settable root-of-trust design](https://source.android.com/docs/security/features/verifiedboot/device-state#user-settable-root-of-trust), without requiring Moss to adopt Android's ABI or device policy. A status supplied by an untrusted boot path does not establish a security guarantee, and reporting boot state is not remote attestation.

## Considered Options

Allowing the first userspace service to declare itself trusted was rejected because that makes the root of capability delegation circular. Placing ongoing code-approval and publisher policy in the kernel was rejected because phone, PC and development products need different policies without changing the native enforcement model. Treating every custom-root boot as unverified was rejected because root ownership and enforcement of verification are separate properties.

## Consequences

Acceptance must cover substituted kernel or supervisor images, altered authorization configuration, unauthorized initial authority recipients, image changes between verification and use, and honest reporting of custom-root and unverified boots. This is an accepted target architecture, not a claim that verified boot is implemented in the current kernel.

Exact root enrollment and rotation, rollback protection, update and recovery protocols, verification-failure behavior and remote attestation remain separate decisions. Runtime approval revocation is defined in [ADR-0028](0028-separate-code-approval-revocation-from-termination.md). Nothing here makes verified origin proof that a component is free of vulnerabilities.
