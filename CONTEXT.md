# Moss Kernel Validation

The vocabulary used to describe evidence about Moss kernel behavior and performance.

## Language

**General-Purpose Kernel Capability Completeness**:
Completeness of the explicitly agreed Moss capability profile, assessed through command-line applications under declared workloads and supported environments. Acceptance covers that profile's required system behaviors, resource lifecycles and performance obligations.
_Avoid_: Passing the existing core test catalog described as complete general-purpose kernel acceptance.

**Moss Application Source Compatibility**:
The ability to rebuild agreed existing C/POSIX command-line applications for Moss, with C-library or platform adaptation, while preserving their required observable behavior.
_Avoid_: Linux binary compatibility; complete POSIX conformance inferred from selected applications.

**ut_kernel**:
Moss's kernel unit-testing framework, abbreviated from the project's term `unit_kenel`.
_Avoid_: Unity.

**Kernel Functional Test**:
A check of an actual Moss kernel facility under the conditions required by that facility. Its result provides evidence only for the behavior and conditions it exercises.
_Avoid_: Language demonstrations described as kernel functional coverage.

**Kernel Stability Test**:
A check that Moss preserves its required behavior and resource ownership across repeated lifecycle operations or concurrent execution under declared workloads and resource limits.
_Avoid_: A successful boot described as kernel stability acceptance.

**Kernel Lifecycle Cycle**:
A complete execution of a declared Moss resource lifecycle, from creation through use and release to verification of the expected post-release state.
_Avoid_: Individual operations or polling iterations counted as completed lifecycles.

**Kernel Benchmark**:
A repeatable measurement of an operation performed by an actual Moss kernel facility under a specified workload and execution environment.
_Avoid_: General arithmetic measurements described as kernel performance.

**Kernel Benchmark Scenario**:
A named Moss kernel operation together with fixed workload parameters that identify a single performance comparison target.

**Kernel Benchmark Baseline**:
An explicitly selected set of prior kernel benchmark measurements used as the reference for matching workloads under comparable conditions.

**Kernel Performance Regression**:
A repeatable worsening of a Moss kernel operation's measured performance relative to its benchmark baseline under matching workloads and comparable execution conditions.
_Avoid_: A single slower sample described as a confirmed regression.

**Kernel Function Microbenchmark**:
A kernel benchmark that repeatedly invokes one selected production kernel function using declared inputs and preconditions.
_Avoid_: Runtime function profiling.

**Kernel Validation Image**:
A bootable Moss kernel dedicated to executing kernel functional tests and kernel benchmarks.

**Kernel Validation Report**:
A record of kernel test outcomes, benchmark measurements, and their execution conditions, including failures and selected workloads that did not run.

**Kernel Test Suite**:
A declared group of kernel functional tests with compatible runtime requirements and resource-cleanup expectations. Its cases can share one kernel lifetime.

**Destructive Kernel Test**:
A kernel functional test whose expected failure or persistent state changes prevent trustworthy subsequent tests in the same kernel lifetime.
