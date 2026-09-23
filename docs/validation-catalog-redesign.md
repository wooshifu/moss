# Validation Source and Catalog Redesign

Status: proposed. Discussion snapshot: 2026-09-23. This is a design for the next validation catalog, not evidence that the new catalog or system acceptance has passed.

## Target and boundaries

- Replace the current suite/case IDs with a new catalog for complete system acceptance of the agreed general-purpose profile in ADR-0006: real command-line applications and the kernel capabilities they require. Networking and cross-reboot persistence remain outside this profile.
- Build and run ARM64, x64, and RISC-V 64 in both Debug and Release under QEMU. This evidence does not establish behavior or absolute performance on physical hardware.
- Keep one fresh guest per functional suite; destructive cases receive their own guest. Application workflows and long-running stability workloads use separate, sustained instances.
- Preserve the existing C-linkage entry points, numeric kernel/userspace control values, and the suite-scoped meaning of reused operation numbers. Suite and case IDs are intentionally replaced. The host's expected catalog remains independent of guest registration and rejects missing or reordered cases.
- Kernel validation and userspace validation are both split by test domain and shared fixture ownership. Their entry files retain only startup and thin dispatch. The internal state and case logic may be redesigned, but every changed assertion or failure condition needs an explicit new requirement and old-to-new evidence mapping.
- The delivery includes production repairs needed for the agreed profile. A failing or inconclusive run is retained as evidence, not converted into a pass by catalog changes or retries.

## Catalog transition

Before changing IDs, freeze the current catalog, source revision, artifact hashes, and representative passing and failing reports. Concurrent work is changing the catalog; re-inventory at the implementation base commit.

Maintain an explicit mapping for every old case and every accepted capability or failure scenario: old ID, required behavior, new ID, runtime conditions, architecture applicability, and evidence or an explicit retirement rationale. Introduce the new catalog beside the old one on an isolated branch. The final image and host use only new IDs after the new catalog meets the acceptance gates. Historical reports remain immutable and carry their original catalog meaning.

Proposed new suite families, to be refined into exact suite/case IDs after the mapping is complete:

| New family | Existing sources of coverage |
| --- | --- |
| `platform.*` | `resources`, `drivers`, `interrupts.smp`, `users.console_irq` |
| `memory.*` | `mm`, `pfa`, `heap`, `mm.permissions`, `mm.transactions`, `mm.lifetime`, `mm.concurrent`, `mm.uaccess`, `mm.tlb_*`, `users.vm`, `users.uaccess` |
| `concurrency.*` | `containers`, `containers.smp`, scheduler ownership and controlled interleavings |
| `execution.*` | `scheduler`, `process`, `users`, `users.frame`, `users.exec`, `users.signals`, `users.simd_fault` |
| `time.*` | `timers`, `users.timers` |
| `io.*` | `vfs`, `vfs.smp`, pipe and console cases currently under `users` and `users.signals` |
| `runtime.*` | `users.libc` and runtime-facing process/exec checks |
| `applications.*` | `users.busybox`, `users.applications`, `users.lifecycle` |
| `framework.*` | `self`, `self.fail`, `self.panic`, `self.timeout` |
| `performance.*` | Existing `bench.*` scenarios and newly required profile coverage |

A family is a naming and source-ownership guide, not permission to combine cases with incompatible guest-lifetime or cleanup requirements into one suite. The final catalog must include end-to-end BusyBox behavior, resource recovery, error/rollback paths, controlled concurrent interleavings, and representative performance for the agreed profile. Gaps remain explicit until production and test code close them.

## Source ownership

| Area | Proposed files and responsibility |
| --- | --- |
| Shared contract | `src/validation_protocol.h`: C/C++-compatible names for cross-boundary operations and modes. Domain prefixes distinguish reused numeric values without renumbering them. |
| Kernel entry | `src/test/validation.cpp`: boot entry, explicit suite registration order, and thin C ABI forwarding. `src/test/validation/runtime.*` owns common reporting and case state. |
| Kernel cases | `src/test/validation/{memory_pages,memory_mapping,memory_concurrency,memory_tlb,heap,containers,vfs,vfs_concurrency,timers,scheduler,process,signals,benchmarks,framework,drivers}.cpp`, with fixtures and domain-specific protocol handling kept beside their cases. Adjust file boundaries to keep cohesive files roughly 500-1000 lines. |
| Userspace entry | `src/userspace/validation.c`: `_start`, mode dispatch, and the validation control call wrapper. A small internal header declares only cross-file entry functions. |
| Userspace cases | `src/userspace/validation/{exec,uaccess,vm,process,pipes,timers,signals,signals_frames,benchmarks,busybox,libc}.c`, split further only where a fixture or runtime requirement warrants it. |
| Host | Keep a versioned, independently authored expected catalog in `scripts/kernel_validation.py`; require exact guest catalog order and complete case accounting. Record the selected catalog version in host reports without rewriting old reports. |

Continue using explicit registration, not static-constructor discovery. New kernel C++ test files must use the same global module-unit form as the existing split `drivers.cpp`; changing them to `module moss.kernel;` changes linkage. Keep cross-domain state behind the smallest necessary internal APIs; do not expose fixture internals or a shared mutable global bag merely to make compilation easy.

## Delivery sequence and gates

1. Freeze a clean base and old-catalog evidence in an isolated worktree. Do not incorporate concurrent uncommitted changes from the main worktree by overwrite. Establish the old functional, framework, application, and benchmark baseline for all six configurations.
2. Add the shared control header and narrow internal entry interfaces. Build both C and C++ sides without changing numeric protocol behavior. Extract one domain at a time; each slice must preserve or explicitly revise its assertions and pass focused guest tests before the next slice.
3. Keep the old catalog running while defining the new requirement-to-case map and implementing new IDs and host expectations. Record each intentional semantic change separately, including its effect on historical comparison. Do not remove an old case until its required behavior has new evidence or an approved retirement rationale.
4. Repair production defects exposed by the new catalog. Rerun the affected suite on all applicable configurations, then the full six-configuration matrix. Preserve the first failing report and the passing report from the repaired revision.
5. Calibrate replacement long-run thresholds from six-configuration measurements of complete lifecycle rates, resource drift, and failure windows. Until replacement numbers are accepted, the existing 30-minute and 10,000-cycle per-configuration gate remains provisional. Release performance uses comparable QEMU baselines, measured noise, and scenario-specific relative regression thresholds; missing or inconclusive calibration cannot pass.
6. Switch host and guest to the new catalog only after the agreed functional, application, stability, and performance gates pass. Update usage/design documentation and mark superseded validation ADRs explicitly. The final report states the exact source revision, catalog version, environment, results, and physical-hardware limit.

The source split is not complete while either entry file remains a large collection of test bodies. The system acceptance claim is not complete merely because the catalog was replaced, the builds succeeded, or short functional suites passed.
