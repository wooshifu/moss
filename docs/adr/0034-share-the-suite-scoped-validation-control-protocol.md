---
status: proposed
date: 2026-09-23
---

# Share the Suite-Scoped Validation Control Protocol

The validation image and its userspace program share a private control protocol whose operation numbers can have different meanings under different selected suites. While splitting both programs by test domain, keep one C ABI control entry and put cross-boundary operation and mode constants in a C-compatible header, using domain-prefixed names where numbers are reused. Preserve the numeric values and suite-based dispatch so source ownership can change without changing the validation ABI; a global renumbering would add risk without helping this refactor.
