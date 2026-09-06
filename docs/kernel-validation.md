# Kernel Validation Design

Status: implemented and validated on 2026-09-06 for ARM64, x86_64, and RISC-V under QEMU TCG with four vCPUs and 2 GiB. This includes the required SMP and enlarged-memory repairs. See the [usage guide](kernel-validation-usage.md) and [acceptance record](kernel-validation-acceptance.md) for executable commands, evidence, and remaining limits.

## Confirmed Decisions

- [ADR-0001](adr/0001-validate-real-kernel-functions.md): validate real kernel functions on ARM64, x86_64, and RISC-V, with reliable failure reporting and an initial set of real workloads.
- [ADR-0002](adr/0002-dedicated-kernel-validation-image.md): run validation workloads in a dedicated image that reuses production kernel startup and subsystem implementations.
- [ADR-0003](adr/0003-suite-level-kernel-test-isolation.md): use one fresh QEMU instance per functional suite, with separate instances for destructive cases and explicit reporting of cases not run after a failure.
- [ADR-0004](adr/0004-compare-kernel-performance-in-qemu.md): compare kernel revisions under fixed QEMU conditions and report results with their environment.

## Framework Reuse

Extend the existing `ut_kernel` framework for real kernel functional tests, suite-level isolation, and reliable result reporting. `ut_kernel` is the project's kernel unit-testing framework, not Unity; Unity will not be introduced for this work.

The implementation adds a distinct benchmark interface, including explicitly registered function microbenchmarks, while sharing the validation image, workload selection, and result-reporting infrastructure described here. See the [usage guide](kernel-validation-usage.md) for commands and authoring examples.

### Test Declaration and Registration

This authoring interface is implemented by extending the existing `ut_kernel` APIs; it does not introduce another testing library.

Keep the familiar `register_test(name, function)` and `expect(condition)` entry points. Add explicit `register_suite` grouping with a stable suite ID and declared runtime prerequisites. A suite's declaration callback registers its cases but does not execute their bodies. A single explicit registration entry point invokes the suite registration functions before selection and execution; correctness must not depend on global-constructor execution or static-initialization order.

The following sketch omits the existing framework namespace. `register_suite` is implemented; the named case functions represent tests of the production page allocator.

```cpp
void register_mm_tests() {
  register_suite("mm", [] {
    register_test("page_alignment", test_page_alignment);
    register_test("page_reuse", test_page_reuse);
  });
}
```

An author adds a case to its suite's registration function. Adding a new suite also requires one entry in the central suite catalog. This explicit catalog favors predictable kernel startup and auditable selection over automatic discovery of arbitrary global test objects. Tests create their runtime fixtures when their bodies execute, not while registering themselves.

Registration uses static-lifetime descriptors and bounded framework storage, without allocating registry nodes from the kernel heap. Preserve declaration order within each suite. Validate duplicate IDs, invalid metadata, and capacity overflow before executing cases; these are explicit registration failures, never silent omissions. The registered catalog and selected case list support enumeration and the host's completion accounting.

Ordinary `expect` records an assertion without panicking. When a failed precondition makes further operations unsafe, the case uses an explicit guard and normal return, allowing constructed local resource guards to run. An assertion evaluates its condition once and contributes exactly once to the assertion counts, including when its result is inspected by a guard. After the case returns and cleanup is checked, any failure prevents subsequent cases in that suite from running. Do not introduce exceptions or `setjmp`/`longjmp` to escape arbitrary test-helper call stacks; helpers must propagate failure through normal control flow.

Keep suite and case setup and cleanup explicit where needed, and validate cleanup against the real owned resources. Destructive behavior and expected panic or timeout belong to case metadata and separate guest execution, not an in-guest recovery mechanism. The current flag-based panic simulation must not count as evidence of a real kernel panic.

The separate benchmark API continues to use the confirmed named scenario and untimed preparation / timed operation / untimed validation-and-cleanup model. Registering a benchmark must not perform its measured work or create its live fixture prematurely. A registered callback can create local fixtures and pass capturing lambdas to the templated batch loop without requiring heap-backed callable storage in the registry.

Framework self-validation covers deferred case execution, catalog completeness, duplicate IDs, capacity overflow, assertion evaluation and accounting, normal-return cleanup, suite stop behavior, and the real fatal paths already required below.

## Initial Functional Coverage

| Area | Required Behavior |
| --- | --- |
| Physical memory | Allocate pages of different orders; verify alignment and non-overlap of live allocations; release pages and exercise repeated reuse. |
| Page permission boundary | Inspect production kernel and active user roots for supervisor-only kernel leaves, x86 user permission chains, kernel W^X and read-only/NX direct-map aliases. Create/clone/free address spaces without changing shared kernel tables. This is not complete user-fault containment acceptance. |
| VFS | Open and read real files; verify contents, file position, EOF, close, and error handling, including rejected writes to the current read-only ramfs. |
| Userspace and processes | Verify syscall return values, user-domain/VMA policy, bounded strings, cross-VMA copies and rejection, then process creation, execution, exit and reaping. Policy rejection is not CPU-fault recovery or complete uaccess acceptance. |

Physical-memory cases must exercise the production page allocator. VFS cases must use the real mounted filesystem environment. Tests that claim userspace syscall coverage must invoke the architecture's real user-to-kernel entry path; directly calling a syscall handler does not establish that coverage.

x86 users tests also exercise legacy FP state through yield/fork/exec and x87
exception termination; the SMP case checks inheritance on CPU1. The explicit
`users.simd_fault` workload is separate from the default set because the current
TCG execution does not deliver #XM. Its assertion remains a failure, not an
expected-pass classification; it does not establish SIMD-fault acceptance.

Framework self-validation must include intentional assertion failures, kernel panics, and timeouts, and must verify that failures reach the host and CTest. Startup and image-loading failures must remain distinguishable from completed test results, as required by ADR-0001.

## Initial Benchmark Workloads

| Workload | Measurement |
| --- | --- |
| Physical-page allocation and release | Operation duration for different allocation orders. |
| VFS file reads | Throughput while reading actual file contents. |
| Userspace `getpid` | Round-trip duration through the actual user-to-kernel syscall path. |

The current ramfs rejects writes, so the first version measures its read behavior. Process creation, execution, exit, and reaping are included in functional coverage; their performance is not part of this initial benchmark set.

## Benchmark Execution and Sampling

Each kernel benchmark scenario, identified by its operation and workload parameters, runs in a fresh QEMU instance. Scenarios execute serially and separately from functional tests, using a Release build of the validation image. All warmup and recorded batches for one scenario share that instance; individual samples do not each require a new kernel boot.

Startup, warmup, framework logging, and result validation are outside the timed interval. Each recorded batch measures repeated real kernel operations. Preserve the raw batch measurements and operation counts, derive per-operation duration or throughput, and report the median across those batch-derived values. A distribution of batch averages is not a distribution of individually timed operation latencies.

The execution, sampling, and clock-validation policies are confirmed. The requested multi-vCPU and larger-memory revision replaces the earlier single-vCPU, 256 MiB proposal. The CI policy defines how correctness, measurement validity, and performance changes affect the result.

### Initial Configurable Defaults

The resource requirement and concrete defaults of four vCPUs and 2 GiB are confirmed. These defaults are not measured accuracy or execution-time guarantees. Effective values and any overrides must appear in the report.

| Setting | Initial Default |
| --- | --- |
| Functional and benchmark guest resources | Four vCPUs and 2 GiB RAM, configurable and checked against actual kernel CPU and memory readiness. |
| Benchmark execution profile | Release validation image; explicit TCG acceleration; the architecture's existing QEMU machine and CPU model; no instruction-counting mode. |
| Warmup | Five completed, untallied measurement batches after clock validation and any batch-sizing pilot. |
| Recorded samples | Thirty completed batches for each scenario. |
| Batch sizing | For a new baseline, a bounded pilot targets approximately 1 ms per batch, subject to the scenario's declared iteration and resource capacity. The resulting operation count is fixed for the recorded batches. |
| Host deadlines | At most 30 s for validation startup, 5 s per ordinary functional case including its setup and cleanup, and 60 s total per guest. Explicit workload metadata or runner options can override these limits. |

The PFA exhaustion workload now declares a 30 s case default: on 2026-09-06,
four concurrent 2 GiB RV64 Debug guests all hit the old 5 s deadline, but the same
images completed all ownership checks in about 16 s with a diagnostic 20 s limit.
Other workloads retain 5 s; explicit `--case-timeout` wins. This changes a host
execution budget, not allocation correctness or a performance threshold. Effective
budgets and host-observed case durations are recorded, and historical failures remain
failures. Functional/framework CTest has a 600 s outer guard for up to nine guests
and cleanup. Evidence and limitations are in `moss-todo.md`, section 3.10.

A baseline comparison reuses the baseline's fixed operation count when the workload and resource capacity permit it. It must not independently change allocator occupancy or other batch preconditions and silently call the results comparable. A caller may also specify a fixed operation count directly. Pilot work and its cleanup are outside the recorded samples; any operation or cleanup failure during a pilot or warmup still fails the scenario.

The batch-duration target is a sizing heuristic, not permission to exceed a fixture's capacity or a claim of statistical precision. Record the actual counter interval and instrumentation overhead. A zero or otherwise unresolvable interval is not a valid operation-duration sample; overhead-dominated measurements must state their limitation and must not be presented as exact isolated function time. Changing these defaults changes the recorded comparison conditions.

Functional cases and benchmark scenarios retain the required real interrupt and scheduler behavior; the framework does not globally suppress interrupts to improve a result. A production function's own locking and interrupt control remain part of that function's behavior. Multi-vCPU execution is now part of the required profile. Hardware-accelerated benchmark profiles remain outside the initial acceptance matrix.

A multi-vCPU guest does not change suite isolation or make benchmark scenarios execute concurrently. Single-function measurement uses one benchmark worker pinned to CPU 0, with its real interrupt and scheduler behavior retained. Record and verify the affinity and observed CPU identity. Parallel-worker throughput is a separately declared workload, not an implicit replacement for single-function operation timing. A baseline must match the CPU count, memory profile, affinity, and worker policy.

Deadlines are enforced by the host rather than relying on the guest clock under validation. Intentionally timed-out self-validation cases declare their expected timeout separately. An outer CTest deadline must leave enough time for the runner's applicable deadlines, partial-report finalization, and QEMU termination and reaping; it must not kill the runner before that work can finish.

### Multi-vCPU and Memory Prerequisites

The initial source inspection on 2026-09-06 found the following prerequisites. They have since been repaired and exercised in the acceptance matrix; this list records why changing QEMU arguments alone was insufficient:

- `scripts/run_qemu.py::build_qemu_args` forced test mode to one vCPU and used 256 MiB; its architecture configuration also capped x86_64 and RISC-V at one vCPU.
- `src/boot/src/arch/x86_64/boot_impl.cpp` and `src/boot/src/arch/riscv/boot_impl.cpp` left secondary-CPU activation unimplemented, and their `wait_for_all_cpus_active` functions returned one.
- ARM64 already had secondary-CPU initialization, but the four-vCPU, 2 GiB profile needed real execution evidence.
- x86_64 hardware initialization populated RAM information from platform defaults rather than the requested QEMU RAM size. Actual boot memory discovery and usable mappings needed adaptation for the enlarged profile.

The confirmed prerequisite work is to complete the missing secondary-CPU startup and required per-CPU runtime integration, verify CPU identity and affinity behavior, and make memory discovery and mapping reflect the actual boot environment. This explicitly includes implementation beyond changing the test runner's defaults.

Before accepting the profile, verify that all requested CPUs are actually online and can each execute bounded real kernel work. An emulator CPU count, a firmware topology entry, or an incremented shared count alone is not sufficient. Report requested, detected, and online CPUs separately. CPU affinity for a microbenchmark must constrain real execution, not only annotate its result.

Host memory configuration, generated device trees or boot memory maps, kernel mappings, and allocator-managed memory must agree after accounting for legitimate reserved regions. Use bounded owned allocations and accesses to demonstrate usable memory beyond the previous 256 MiB RAM window; do not infer that capability solely from `-m 2G` or require all 2 GiB to be free. Preserve the distinction between physical RAM and allocatable RAM in reports.

Do not silently clamp the new profile to one CPU or the old memory limit. Missing SMP or memory readiness is a failed or unsupported profile with its reason, not successful multi-core validation. An explicitly requested smaller diagnostic profile can aid repair but does not satisfy this acceptance requirement.

### Clock Validation

The architecture-specific frequency sources and calibration policy below are implemented and exercised by the Release benchmark matrix.

The current `hal::timer::frequency()` in `src/hal/timer/src/timer_hal.cppm` reads the ARM64 frequency register, but the x86_64 path contains a 1 GHz placeholder fallback and the RISC-V path uses a platform default. Those defaults are not sufficient evidence for benchmark time conversion. The existing counter reads also need architecture-appropriate ordering for measurement boundaries.

| Architecture | Counter and Frequency Source |
| --- | --- |
| ARM64 | Ordered `CNTVCT_EL0` reads with a nonzero `CNTFRQ_EL0` frequency. [Arm documents these counter and frequency registers](https://learn.arm.com/learning-paths/servers-and-cloud-computing/arm_pmu/assembly/). |
| RISC-V | Ordered `time` CSR reads with a validated `/cpus/timebase-frequency` property from the actual boot device tree. [Linux's RISC-V initialization](https://raw.githubusercontent.com/torvalds/linux/master/arch/riscv/kernel/time.c) uses this property; do not substitute the current fixed 10 MHz value when it is missing. |
| x86_64 | Ordered TSC reads. Use CPUID leaf `0x15` only when its ratio and crystal-frequency information are complete and nonzero, as specified in the [Intel architecture manual](https://cdrdv2-public.intel.com/868137/325462-089-sdm-vol-1-2abcd-3abcd-4.pdf). Otherwise calibrate against the explicitly enabled QEMU microvm i8254 PIT. [QEMU documents the optional PIT device](https://www.qemu.org/docs/master/system/i386/microvm.html). |

Frequency discovery or calibration runs once during each scenario's guest preparation, before warmup, and is reused by its batches. It does not require an additional QEMU instance per batch. Record the frequency source and value; a calibrated frequency additionally retains its reference-counter samples and uncertainty checks. PIT calibration must use the counter facilities actually available in microvm, preserve the runtime's timer requirements, bound retries and waiting, and reject ambiguous or inconsistent samples. The existing kernel clock derived from the same unverified TSC frequency is not an independent calibration reference.

Before accepting measurements, verify counter progress and nondecreasing reads, apply the required compiler and architecture ordering, and validate tick-to-time arithmetic against zero frequency and overflow. Frequency validation does not by itself establish adequate resolution or eliminate timing overhead for a short function; the batch and overhead rules below still apply.

If the counter or frequency cannot be validated, retain any readable raw ticks as diagnostic data, mark the benchmark measurement invalid or unsupported with its reason, and return a nonzero benchmark-run result. Do not publish derived nanoseconds, throughput, or a baseline comparison as valid. Functional suites remain independently runnable. The three initial supported QEMU profiles must demonstrate valid timing to satisfy benchmark delivery; reporting an unsupported clock is not a substitute for that acceptance evidence.

## Individual Function Performance

An additional confirmed requirement is to measure selected production kernel functions, beyond the initial fixed workload list. These measurements retain the real kernel environment and the revision-comparison rules above. Explicitly registered function microbenchmarks and the preparation, measurement, and cleanup model below are implemented in `src/test/framework/benchmark.hpp`.

### Explicit Function Microbenchmarks

Register a named benchmark scenario whose measured operation invokes the selected production function with declared inputs and preconditions. Reuse the same validation image, scenario selection, warmup, batch sampling, and reporting mechanisms. A function's own callees are included in its measured operation; this does not attribute exclusive CPU time to individual functions in a call graph.

Separate untimed batch preparation, the timed operation loop, and untimed result validation and cleanup. In an allocation-only benchmark, each timed call allocates pages into a bounded result buffer, and cleanup releases the successful allocations after timing. In a release-only benchmark, preparation allocates the pages and the timed loop releases them. Allocation followed by release inside the same timed operation is a distinct combined workload.

The following uses the implemented callback interface with an author-supplied `PageAllocationBatch` fixture. The fixture prepares bounded result storage, validates results, and releases owned pages. Preparation and cleanup return `bool`, including safe cleanup after partially failed preparation. The built-in `allocation_benchmark` in `src/test/validation.cpp` is the complete production-backed example.

```cpp
bench::register_benchmark("bench.my_allocate", [](bench::Context& context) {
  PageAllocationBatch fixture;
  context.measure_batches(
      [&](usize count) { return fixture.prepare(count); },
      [&](usize index) {
        fixture.results[index] = mm::PageFrameAllocator::allocate_pages(0);
      },
      [&](usize count) { return fixture.verify_and_release(count); });
});
```

The timing loop should invoke a templated callable so registration dispatch does not add an indirect call on every operation. Retain the production Release optimization policy. Supply appropriate compiler barriers and observable inputs and outputs, and inspect optimized code for representative small-function benchmarks. A result sink alone does not establish that a call executes on every iteration: [Google Benchmark's optimization guidance](https://google.github.io/benchmark/user_guide.html#preventing-optimization) documents constant-folding and result-reuse limitations of `DoNotOptimize`.

Use validated, ordered counter reads around each batch and record timing-source metadata. Keep raw counter ticks; report time units such as nanoseconds only when the frequency and conversion are validated. A timer tick must not be labeled as a CPU cycle without evidence that it is one. Short-function measurements include residual loop, result-storage, and measurement costs; report or bound that overhead and retain unadjusted samples rather than presenting unchecked subtraction as exact function time.

Bounded batch execution changes allocator occupancy, cache state, and other preconditions across calls. Each scenario must describe that workload. Functions requiring different per-call state need an appropriate fixture or a separately labeled measurement strategy. The elapsed interval also includes any scheduling, interruption, or waiting that occurs under the declared runtime conditions; it is not automatically exclusive on-CPU execution time.

The confirmed requirement is explicitly registered function microbenchmarks. Automatic recording of function calls during normal kernel workloads is a separate profiling model outside this first delivery.

## Result Validity and CI

Correctness and measurement validity are mandatory. Functional errors, unexpected kernel panics, timeouts, and invalid timing cause the relevant run to fail and return a nonzero exit status. Framework self-validation treats intentionally injected failures according to its declared expected outcome and verifies that the underlying failure is correctly reported.

Performance changes are reported against a comparable baseline with the original samples retained. In the first version, a performance decrease alone does not fail CI. Noise ranges for these workloads have not yet been established; automatic performance-regression thresholds are deferred until measurements can support them.

Valid measurements without a comparable baseline remain measurements, not evidence that a performance comparison passed. Failed or incomplete operations must not be presented as successful performance samples.

## Result Artifacts and Baseline Interface

The result-delivery contract below is implemented. Actual successful, deliberately failing, and timed-out guests produce the documented artifacts; the acceptance record identifies the saved runs used to verify offline comparison.

### Run Reports

The host runner produces the following artifacts in a run-specific output directory:

| Artifact | Contents and Purpose |
| --- | --- |
| `results.json` | Versioned complete report: requested and executed workloads, observed outcomes, failure reasons, cases not run, raw benchmark batches, derived metrics, clock validation, and environment provenance. |
| `junit.xml` | CI view of individual functional cases and benchmark-scenario validity, with failures and infrastructure errors visible. Cases not run are represented as skipped with their cause, never as passed. Performance changes alone remain informational. |
| Per-guest serial and QEMU diagnostic logs | Original guest output and emulator diagnostics, retained on both success and failure and linked from the report. |

JSON is the canonical structured report; terminal summaries and JUnit are derived views. Raw diagnostic ticks from an invalid run remain distinguishable from accepted benchmark samples. Framework self-validation records both the expected and observed outcome so an intentional panic is not confused with a production test passing normally.

Reuse the existing Python tooling and QEMU argument construction. Parse structured events and report data with standard JSON support and the existing Pydantic dependency; generate XML through a structured XML API. No report database, web service, or dashboard is required for the first version. [CTest supports JUnit output](https://cmake.org/cmake/help/latest/manual/ctest.1.html#cmdoption-ctest-output-junit), but when one CTest invocation represents a kernel suite, the runner's case-level report supplies the individual guest-case detail. CTest still receives the runner's normalized nonzero failure status.

### Completion and Failure Evidence

The guest emits bounded, versioned machine-readable records distinguishable from ordinary serial logs. The host checks workload identity, case start and completion, expected case accounting, benchmark sample validity, and the terminal run record against architecture-specific QEMU termination. A raw QEMU exit code, a human-readable success line, or some completed cases alone cannot establish that the requested run passed.

An empty workload selection, duplicate or unknown case identity, malformed protocol record, unexpected termination, or missing completion record cannot become a successful run. The report distinguishes failure before validation startup from a failure in an identified case. A suite interrupted by a failing case retains that failure and marks its remaining selected cases not run; later suites still run in fresh guests. Intentionally fatal self-validation cases use their declared expected termination evidence instead of requiring a normal completion record.

Preserve and incrementally record available evidence so timeout handling can finish a partial report before returning failure. If an external force-kill prevents report finalization, missing finalization is not evidence of success. The runner owns its child QEMU processes and must terminate and reap them on timeout or cancellation.

### Explicit Baseline Comparison

Allow the caller to select workloads, choose an output directory, and optionally name a previous JSON report as the baseline. A separate comparison operation can compare two saved reports without booting QEMU again. Baselines are explicit inputs and are never silently selected or overwritten by a new run.

Record the kernel revision and dirty state, actual image hashes, compiler and effective build options, workload identity and definition version, parameters and batch policy, QEMU version and effective arguments, accelerator, machine and CPU configuration, memory, guest CPU count, host identity, clock source, and frequency-validation evidence. Revision and kernel-image hashes are provenance expected to differ across revisions, not fields that must match to permit comparison. Different workload definitions, fixture contents, build modes, or execution environments are not automatically comparable. Calibrated frequency estimates retain their uncertainty and are not compared as if they were exact constants.

For matching valid workloads, report both medians, an absolute and relative change where defined, the metric's improvement direction, and links to the raw samples. Missing or incompatible baseline entries are explicitly unavailable or not comparable, not a performance pass or regression. This does not change the confirmed policy that a performance decrease alone does not fail CI in the first version.

## Implementation and Acceptance Plan

The following sequence records the implemented plan. The evidence checklist is satisfied for the initial workloads and default QEMU profile, not for arbitrary kernel subsystems or native hardware.

### Implementation Order

1. Establish genuine multi-vCPU and enlarged-memory readiness on all three architectures, including missing x86_64 and RISC-V secondary-CPU startup and required memory discovery and mapping changes. Preserve existing unrelated worktree changes throughout.
2. Extend `ut_kernel` registration, assertion accounting, case lifecycle, and catalog validation. Add focused tests for the host-side event parser, report schema, timeout handling, and architecture-specific exit normalization. Host test doubles validate host tooling only, not kernel functionality.
3. Integrate the dedicated validation image with production architecture startup and real subsystem initialization. Add explicit validation dispatch at the appropriate readiness points and real guest failure reporting. Keep validation behavior out of normal production execution.
4. Connect guest execution to the host runner and CTest. Prove ordinary pass and failure, actual kernel panic, timeout, startup failure, suite stop behavior, and subsequent-suite isolation before treating kernel-suite results as trustworthy.
5. Add the initial real page-allocation, VFS, and userspace/process functional suites and the necessary userspace probes and fixtures. Exercise actual user-to-kernel entry instructions where syscall coverage is claimed.
6. Implement the architecture-specific benchmark clocks, bounded batch execution, function fixtures, and the initial benchmark scenarios. Complete structured reports, JUnit output, explicit baseline selection, and offline comparison.
7. Run the architecture and build verification matrix, inspect representative optimized microbenchmarks, record actual evidence and remaining kernel defects, and document runnable commands and authoring examples using the implemented interfaces.

### Required Acceptance Evidence

| Area | Required Evidence |
| --- | --- |
| Builds and startup | Production and validation images build for ARM64, x86_64, and RISC-V. Normal production startup remains usable. Validation startup reaches the actual subsystem prerequisites rather than a substitute test-only implementation. |
| Multi-vCPU and enlarged memory | The requested CPUs actually come online and each executes real kernel work. The enlarged memory is discovered, mapped, and usable through owned allocations, with reservations accounted for. The single-function benchmark's declared CPU affinity is enforced. Single-core or old-memory fallback does not satisfy the revised profile. |
| Framework reliability | On all three architectures, real guest pass, assertion failure, panic, and timeout produce the declared host outcomes. Launch failure, empty selection, malformed or missing completion records, duplicate registration, and registry overflow cannot report success. Expected fatal self-checks must identify their intended case and failure, not accept any unrelated crash as success. |
| Isolation and cleanup | A failing case stops its suite, remaining cases are reported not run, and a later suite starts in a fresh guest. Resource-owning cases verify cleanup. Observed process launches follow the suite/scenario isolation policy, with no new QEMU process per ordinary case or measurement batch. Timeout and cancellation leave no owned QEMU process running. |
| Real functional coverage | Each initial kernel behavior has an executed production-path test on each supported architecture. A failed test remains a failure. Cases not reached after an earlier suite failure require a separately recorded targeted run before claiming they were exercised. Arithmetic demos and host mocks do not count as this evidence. |
| Function microbenchmarks | Demonstrate allocation-only, release-only, and combined allocation/release scenarios with their different timing boundaries, real VFS reads, and the userspace `getpid` round trip. Inspect representative Release output to verify that measured work is not folded away or hoisted out of the loop. |
| Timing validity | All three initial QEMU profiles demonstrate valid counter and frequency handling and valid samples for the initial benchmark workloads. Record raw ticks, counts, frequency provenance, calibration evidence where needed, and overhead limitations. Invalid clocks, operations, or cleanup do not produce accepted performance samples. |
| Report and comparison behavior | Actual successful and failing guest runs produce JSON, case-level JUnit, and original diagnostics. Matching saved reports can be compared without launching QEMU. Missing or incompatible baselines remain explicit; a reported slowdown alone does not fail CI. |
| Build-mode checks | Run the functional coverage matrix in Debug, run framework self-validation and representative real functional cases in Release, and run all benchmark acceptance in Release. Do not infer Release measurement behavior from a Debug-only run. |
| Delivery record | Publish the actual commands, architecture/build coverage, artifact locations, failure classifications, and outstanding defects. Passing host tests or a successful compile alone is not end-to-end acceptance. |

Framework delivery and kernel correctness remain separate conclusions, as agreed in ADR-0001. Repairs required to execute the framework and produce the required valid benchmark evidence are in scope. Other kernel defects exposed by real functional tests remain explicitly failing results for separate repair; they are not hidden by weakening assertions, skipping required cases, or replacing the tested implementation. A targeted diagnostic rerun is additional evidence and does not erase the original failed run.

## Implementation Status

- [x] Genuine SMP and enlarged-memory readiness on all three architectures.
- [x] Production-backed validation image and bounded `ut_kernel` registration.
- [x] Host protocol, reports, isolation, and failure self-validation.
- [x] Real allocator, VFS, and userspace/process cases.
- [x] Validated clocks, function benchmarks, and baseline comparison.
- [x] Debug/Release acceptance matrix and delivery evidence.
