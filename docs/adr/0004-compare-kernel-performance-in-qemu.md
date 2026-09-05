---
status: accepted
date: 2026-09-05
---

# Compare Kernel Performance Within Fixed QEMU Environments

The first version of the kernel benchmark will compare real Moss kernel operations between revisions under a fixed QEMU execution environment. It will measure operation duration and throughput in the dedicated kernel validation image, recording the target architecture, build parameters, and runtime environment alongside results. Comparisons require matching workloads and comparable configurations; results from different architectures or environments do not establish a change caused by a kernel revision.

These measurements describe performance in the recorded virtual environment. They do not establish physical-machine performance. [QEMU's instruction-counting documentation](https://www.qemu.org/docs/master/devel/tcg-icount.html) explains that QEMU does not model the time each instruction would take on real hardware and that instruction counting is not cycle-accurate emulation.

Each benchmark scenario and parameter combination runs serially in a fresh QEMU instance using a Release build. Warmup and repeated measurement batches share that instance. Startup, warmup, framework logging, and result validation stay outside the timed interval. Results retain raw batch measurements and report per-operation duration or throughput and the median across batch-derived values.

The framework also supports explicitly registered function microbenchmarks with declared inputs and untimed preparation, validation, and cleanup. These scenarios invoke the actual production function, including its callees, and reuse the same execution and reporting mechanisms. Automatic profiling of function calls during normal kernel workloads is outside this first delivery.

Correctness and measurement validity are mandatory CI checks. Functional errors, unexpected panics, timeouts, and invalid timing fail the relevant run. Performance decreases are reported against a comparable baseline with raw samples retained, but do not automatically fail CI in this first version. Automatic performance thresholds are deferred until workload noise ranges are established.

Benchmark time conversion requires a validated frequency source: the ARM64 counter-frequency register, the RISC-V boot device tree, or complete x86_64 CPUID frequency information with bounded PIT calibration when that information is unavailable. Discovery and calibration occur during preparation in the scenario's existing guest, not in additional per-batch QEMU instances. An unvalidated clock makes the benchmark run fail; diagnostic ticks are not valid time or throughput measurements.

Reports retain complete structured results, raw samples, and original diagnostics, with JUnit and terminal summaries as derived views. A baseline is an explicitly selected prior report, never an automatically overwritten reference. Two saved reports can be compared without another kernel boot; incompatible workloads or environments remain explicitly not comparable.

This records accepted design, not a completed implementation. The [design document](../kernel-validation.md) lists the confirmed initial workloads, sampling, clock-validation mechanism, CI policy, and result-artifact interface.
