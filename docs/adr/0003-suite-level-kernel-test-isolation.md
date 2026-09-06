---
status: accepted
date: 2026-09-05
---

# Isolate Kernel Functional Tests by Suite

A fresh kernel for every test case provides independence but repeats the startup cost for every case. The validation framework must support a coarser boundary that reduces this overhead while containing crashes and residual kernel state.

Moss will use one fresh QEMU instance per functional test suite, with related cases running sequentially in that instance. A suite is an explicit group of cases with compatible runtime requirements and defined resource cleanup. Sharing a subsystem name alone does not establish that cases can safely share a kernel instance. Each case must release the resources it owns and check relevant observable state before the next case runs; these checks do not constitute proof that arbitrary kernel corruption is absent.

Cases that intentionally panic, trigger fatal faults, modify persistent global state, or cannot establish the cleanup required by their suite run in a separate QEMU instance. A suite stops on an assertion failure, panic, timeout, or cleanup failure. The host records the failing case and reports the remaining cases as not run, then starts subsequent suites in fresh instances. Failure is not converted into success by a retry.

This accepted design applies to functional tests and is not yet implemented. The [design document](../kernel-validation.md) separately defines the confirmed benchmark execution and sampling policy. Functional-test isolation does not add a generic kernel reset mechanism or depend on restoring a virtual-machine snapshot.

## Startup Observation

The commands and hashes below are historical observations. For the current build
and runner contract, see [ADR-0005](0005-generic-kernels-and-independent-runners.md).

On 2026-09-05, five sequential launches of the existing `arm64-qemu-debug` production image reached the real userspace shell prompt in the times below. The experiment copied the existing kernel and initramfs to a temporary directory and reused `scripts/run_qemu.py` to construct normal boot arguments: QEMU 11.1.1, `virt` with GICv2, `cortex-a72`, one CPU, and 256 MiB RAM. Each launch used a new QEMU process, which was terminated and reaped after the prompt was observed.

| Launch | Seconds to First Shell Prompt |
| --- | --- |
| 1 | 0.231632 |
| 2 | 0.262105 |
| 3 | 0.287675 |
| 4 | 0.255968 |
| 5 | 0.209827 |

The median was 0.255968 seconds. Timing started immediately before creating the QEMU process and stopped when serial output contained the shell prompt. It excludes building, Python startup and imports, argument preparation, and QEMU shutdown. Host filesystem caches were not cleared. This is evidence about startup overhead for these existing artifacts, not a kernel benchmark result or a measurement of the proposed validation image or other architectures.

At this median, 100 independent starts would account for approximately 25.6 seconds; grouping those cases into 10 suites would account for approximately 2.56 seconds. These are startup-only estimates, excluding test execution and the other overheads above.

Measured artifact SHA-256 values:

- Kernel: `701877d2b72518cc2010bd3cacbe042ca1fd720347285a00b7c2b001ecf54d7c`.
- Initramfs: `075b4579b976d07e27f98efbbaf29cf0a19734fb20761034cd7f4fb08b35522b`.
