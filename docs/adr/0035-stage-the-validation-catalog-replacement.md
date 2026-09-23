---
status: proposed
date: 2026-09-23
---

# Stage the Validation Catalog Replacement

The host currently requires exact suite and case identities and ordering, while the new catalog targets complete system acceptance of the agreed general-purpose profile: real command-line applications, with networking and cross-reboot persistence outside the profile. Use new suite and case IDs rather than assigning new meanings to old IDs. Run both catalogs during migration and map each old capability and failure scenario to new evidence or an explicitly retired requirement; preserve historical reports and remove the old catalog only after the new acceptance gates pass. The host independently maintains a versioned expected catalog and checks the guest's explicit domain registration, so a guest omission cannot silently change the expected results. Temporary duplication makes coverage loss visible while allowing the final catalog to express the new target clearly. Acceptance must cover ARM64, x64 and RISC-V 64 in both Debug and Release; long-run and performance thresholds are being reconsidered separately.
