---
status: accepted
date: 2026-09-05
---

# Compare Kernel Performance Within Fixed QEMU Environments

The first version of the kernel benchmark will compare real Moss kernel operations between revisions under a fixed QEMU execution environment. It will measure operation duration and throughput in the dedicated kernel validation image, recording the target architecture, build parameters, and runtime environment alongside results. Comparisons require matching workloads and comparable configurations; results from different architectures or environments do not establish a change caused by a kernel revision.

These measurements describe performance in the recorded virtual environment. They do not establish physical-machine performance. [QEMU's instruction-counting documentation](https://www.qemu.org/docs/master/devel/tcg-icount.html) explains that QEMU does not model the time each instruction would take on real hardware and that instruction counting is not cycle-accurate emulation.

Each benchmark scenario and parameter combination runs serially in a fresh QEMU instance using a Release build. Warmup and repeated measurement batches share that instance. Startup, warmup, framework logging, and result validation stay outside the timed interval. Results retain raw batch measurements and report per-operation duration or throughput and the median across batch-derived values.

The framework also supports explicitly registered function microbenchmarks with declared inputs and untimed preparation, validation, and cleanup. These scenarios invoke the actual production function, including its callees, and reuse the same execution and reporting mechanisms. Automatic profiling of function calls during normal kernel workloads is outside this first delivery.

Correctness and measurement validity are mandatory CI checks. Functional errors, unexpected panics, timeouts, and invalid timing fail the relevant run. The initial delivery reports performance decreases against a comparable baseline with raw samples retained, without automatically failing CI. The revised acceptance policy below supersedes that informational-only requirement; its implementation and validation remain required work.

Benchmark time conversion requires a validated frequency source: the ARM64 counter-frequency register, the RISC-V 64 boot device tree, or complete x64 CPUID frequency information with bounded PIT calibration when that information is unavailable. Discovery and calibration occur during preparation in the scenario's existing guest, not in additional per-batch QEMU instances. An unvalidated clock makes the benchmark run fail; diagnostic ticks are not valid time or throughput measurements.

Reports retain complete structured results, raw samples, and original diagnostics, with JUnit and terminal summaries as derived views. A baseline is an explicitly selected prior report, never an automatically overwritten reference. Two saved reports can be compared without another kernel boot; incompatible workloads or environments remain explicitly not comparable.

This records accepted design, not a completed implementation. The [design document](../kernel-validation.md) lists the confirmed initial workloads, sampling, clock-validation mechanism, CI policy, and result-artifact interface.

## Acceptance Boundary Reconfirmed on 2026-09-14

The current validation review targets reliable detection of kernel performance regressions between revisions in fixed QEMU environments. Absolute latency and throughput on physical hardware require separate acceptance. This keeps the current work grounded in the existing repeatable execution environment and bounds what its results establish.

This confirmation resolves the performance acceptance boundary. The [required benchmark coverage](../kernel-validation.md#required-benchmark-coverage), also confirmed on 2026-09-14, retains the existing five scenarios and adds representative measurements for all six core paths. Measured noise ranges and numerical regression thresholds remain to be established; these decisions do not establish that regression detection has already been implemented or validated.

## Performance Gate Confirmed on 2026-09-14

Calibrate each scenario's noise range and regression threshold using repeated measurements of the same revision under comparable conditions. A degradation beyond that threshold, confirmed by repeated measurement against a comparable baseline, must fail performance acceptance. Missing baselines, incompatible environments or excessive noise leave acceptance incomplete and cannot produce a performance pass.

This replaces the initial informational-only policy so that confirmed regressions block acceptance while inconclusive measurements remain distinct from proven kernel regressions. Numerical thresholds and the measurement decision rule require calibration evidence; no fixed percentage has been accepted. Retain all original measurements, including failed and inconclusive results.

## Trial Noise Ceiling Confirmed on 2026-09-15

The user accepted a 5% maximum run-to-run noise ratio for trial operation.
Apply it explicitly with `--max-noise 0.05` to both baseline and candidate repeat
groups. A scenario exceeding that ceiling remains incomplete; it is neither a
performance pass nor a confirmed regression. This is not a 5% slowdown allowance:
the existing scenario-specific empirical regression bounds remain unchanged.

Retain the original reports, outliers and unconfigured calibration outputs.
Record the configured gate result separately. This trial policy does not close
the missing benchmark coverage or the overall performance acceptance.
