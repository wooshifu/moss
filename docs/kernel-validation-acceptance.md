# Kernel Validation Acceptance

Recorded on 2026-09-06. This accepts the initial production-backed workloads and framework on QEMU TCG, four vCPUs, and 2048 MiB. It is not a claim that every kernel feature is tested or that QEMU timings predict native hardware performance. See [usage](kernel-validation-usage.md) for normal preset commands and extension examples.

## Build and Execution Matrix

Both `moss.elf` and `moss.test.elf` were built in each configuration. The isolated acceptance build directories use the existing presets:

```sh
uv run cmake --preset arm64-qemu-debug -B build/validation-arm64-debug -DMOSS_CPU_CORES=4
uv run cmake --build build/validation-arm64-debug -j8
uv run ctest --test-dir build/validation-arm64-debug --output-on-failure
```

Repeat with `arm64` replaced by `riscv` or `x86_64`, and `debug` replaced by `release`. Release keeps the repository's `-Os`, section garbage collection, stripped image, and compact NOLOAD sections. Validation does not substitute a different production optimization policy. Userspace retains its existing standalone `-O2` policy.

| Architecture | Mode | Functional | Framework | Benchmark | CTest Total |
| --- | --- | --- | --- | --- | --- |
| ARM64 | Debug | Pass, 3.44 s | Pass, 13.44 s | Not registered | 16.88 s |
| RISC-V | Debug | Pass, 3.91 s | Pass, 13.95 s | Not registered | 17.87 s |
| x86_64 | Debug | Pass, 3.98 s | Pass, 14.19 s | Not registered | 18.18 s |
| ARM64 | Release | Pass, 3.70 s | Pass, 12.67 s | Pass, 3.29 s | 19.67 s |
| RISC-V | Release | Pass, 2.87 s | Pass, 12.77 s | Pass, 3.60 s | 19.24 s |
| x86_64 | Release | Pass, 2.97 s | Pass, 12.96 s | Pass, 4.10 s | 20.04 s |

These are observed invocation durations, not timing guarantees or architecture comparisons. Debug invocations overlapped other correctness tests. Release benchmark runners were serial, but the ARM64 CTest run overlapped host regression build fixtures; it establishes valid execution, not a clean performance baseline. A separate idle-host sequence below exercises baseline reuse.

Each functional invocation launches four guests for seven real cases. Framework self-validation launches four guests; its intentional panic and intentional hang each consume the five-second case deadline. The benchmark invocation launches five guests, each containing five warmups and thirty recorded batches. There is no QEMU restart per ordinary case or measurement batch.

## What Actually Ran

- Physical memory: production allocator orders, alignment, simultaneous non-overlap, sentinel contents, release/reuse, error paths, and free-page accounting after cleanup.
- VFS: production mounted ramfs, known file contents, read position, EOF, close, missing paths, and rejected writes.
- Userspace: real architecture entry instructions for getpid and an invalid syscall; actual fork, exec of a child ELF, exit status 37, wait, and rejection of repeated reaping.
- Framework: deferred registration, duplicate IDs, capacities, assertion accounting, normal-return cleanup, heap bounds, a real assertion failure, a real kernel panic, and a real nonterminating case. The failed suite's later case is recorded `not_run`; subsequent guests still execute.
- Benchmarks: allocation-only, release-only, combined allocation/release, 256-byte real file reads, and userspace getpid round trips. Timing uses validated ARM64 CNTFRQ/CNTVCT, RISC-V DTB timebase/time, and x86 TSC with three PIT-channel-0 calibration samples in this QEMU profile.

The resource reports show detected CPU count 4, online mask 15, work mask 15, and an actual worker CPU of 0 with affinity mask 1 on all architectures. Each CPU performs bounded allocator-backed work. Owned allocations beyond the former 256 MiB window are written, read, and released by the resource case.

| Architecture | Firmware RAM Bytes | Allocator-Managed Pages |
| --- | --- | --- |
| ARM64 | 2147483648 | 518901 |
| RISC-V | 2147483648 | 519069 |
| x86_64 | 2147085312 | 519316 |

These values are from the Release resource reports. x86 boot-map holes and reserved kernel, firmware, page-table, metadata, DTB, and initramfs regions are not falsely counted as allocatable RAM.

## Artifact Locations

All paths below are relative to the repository and identify the initial acceptance runs. Under each build's `validation/` directory, each run produced `results.json`, `junit.xml`, and per-workload `serial.log` and `qemu.log`. These are local, ignored build artifacts, not committed fixtures. The initial isolated build directories were no longer present at commit preparation; fresh pre-commit runs use `build/<arch>-qemu-<mode>/validation/`, with their report paths in each build's `Testing/Temporary/LastTest.log`.

| Build Directory | Functional Run | Framework Run | Benchmark Run |
| --- | --- | --- | --- |
| `build/validation-arm64-debug` | `1788633597763079000` | `1788633601278284000` | n/a |
| `build/validation-riscv-debug` | `1788633598967712000` | `1788633602871610000` | n/a |
| `build/validation-x86_64-debug` | `1788633600212543000` | `1788633604137171000` | n/a |
| `build/validation-arm64-release` | `1788632788572052000` | `1788632791807099000` | `1788632804450431000` |
| `build/validation-riscv-release` | `1788632879202825000` | `1788632882038116000` | `1788632894772353000` |
| `build/validation-x86_64-release` | `1788632955694399000` | `1788632958599602000` | `1788632971594061000` |

Production smoke runs used the following command, repeated for all three architectures:

```sh
uv run scripts/run_qemu.py --config build/validation-arm64-release/qemu_config.json --smp 4 --memory-mib 2048 --timeout 5
```

All reached `MOSS shell v0.1` and the `moss$` prompt. The interactive guests were then intentionally terminated at five seconds, so the runner returned 124, not a validation success exit. Logs are `<build-directory>/production-smoke.log`. No owned QEMU process remained after cleanup.

## Baseline and Optimized Code Checks

```sh
uv run scripts/kernel_validation.py run --config build/validation-arm64-release/qemu_config.json --benchmark --output build/validation-runs/arm64-release-baseline
uv run scripts/kernel_validation.py run --config build/validation-arm64-release/qemu_config.json --benchmark --baseline build/validation-runs/arm64-release-baseline/results.json --output build/validation-runs/arm64-release-current
uv run scripts/kernel_validation.py compare build/validation-runs/arm64-release-baseline/results.json build/validation-runs/arm64-release-current/results.json
```

The two real runs executed serially with no other build or benchmark runner active. All five scenarios passed and compared successfully. Both used 256 operations per batch for allocate/release/combined/read and 512 for getpid, with exactly thirty recorded batches each. The baseline's SHA-256 remained `d2c7d59473b095e76e748c719f4f773728ac75f9f1f1ff8e749764f362a7bf62` before and after the second run and offline comparison.

This is a comparison-interface check using the same build, not evidence of a kernel optimization: observed differences ranged from about -1.8% to +28.3%. Host scheduling, QEMU translation and interrupt effects remain visible. Slowdowns are informational; no regression threshold is asserted. RISC-V and x86 saved benchmark reports also passed raw-evidence revalidation through offline self-comparison, which does not constitute a second measurement run.

Representative ARM64 Release object disassembly was inspected at `build/validation-arm64-release/benchmark-disassembly.txt` and `userspace-benchmark-disassembly.txt`. The allocator's actual call relocations and loop back edge were inside the ordered CNTVCT interval; cleanup calls were after it. The userspace getpid loop contained `svc` inside its counter interval, with the empty-loop measurement and reporting syscalls outside. The work was not folded away or moved outside the measured loop.

## Host Regression Checks

`uv run pytest scripts/tests -q` passed 96 tests in 44.18 seconds, including 52 validation-runner tests. Coverage includes malformed and duplicate JSON, missing completion, invalid clocks/samples, raw-exit consistency, incompatible baselines, JUnit accounting, real host child launch/exit/timeout/cancellation, and kill/reap when SIGTERM is ignored. These host probes do not replace kernel execution evidence.

## Required Kernel Repairs and Limits

Execution exposed prerequisites in production paths: missing x86/RISC-V secondary startup and per-CPU runtime integration, boot memory discovery/reservation handling, user address-space/context-switch defects, and unsafe exit-time stack switching. These were repaired rather than bypassing real syscalls or process lifecycle tests. An 8 MiB bounded NOLOAD runtime heap replaces the mismatched 4 KiB reservation that allowed heap growth to overwrite page tables and allocator metadata. SMP UART writes are serialized so ordinary logs cannot corrupt structured records.

Earlier failed diagnostic runs were retained in `build/validation-runs/` during acceptance and were not relabeled as passes; like the other local artifacts, they are subject to later build cleanup. No outstanding defect blocks the initial required matrix. This does not establish coverage for other syscalls, devices, long-running SMP stress, alternate memory/CPU profiles, hardware acceleration, or native hardware. The current physical mappings limit ARM64/x86_64 to 3072 MiB and RISC-V to 2048 MiB. Timing remains inclusive elapsed batch-average cost, not per-call percentiles or exclusive CPU time. CMake's existing CMP0211 file-set warnings remain non-fatal and outside this change.

## Pre-Commit Recheck

All six Debug/Release configurations were rebuilt and their CTest suites passed again in the normal preset directories. Release invocations ran serially. Production Release images again reached the shell on every architecture; logs are `build/<arch>-qemu-release/precommit-production-smoke.log`, with intentional timeout exit 124 after startup.

The repository gate passed: ARM64 build, clang-tidy on 80 target translation units, `uv run ruff check .`, and `uv run scripts/format.py format --check`. RISC-V/x86 boot and validation translation units were additionally checked with their architecture-specific compilation databases. Required lint repairs use explicit initializers and clearer conditions, normalize the internal x86 boot class name, and replace constant macros/add braces in the shared userspace header, shell, and top. Syscall numbers, expected test outcomes, and benchmark timing boundaries are unchanged.
