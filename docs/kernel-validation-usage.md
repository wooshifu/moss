# Running Kernel Validation

The validation executable links the same production object modules and follows the same boot, memory, VFS, scheduler, userspace entry and exec paths as `moss.elf`. Its initramfs contains deterministic fixtures and real userspace validation programs. The framework is `ut_kernel`, from `unit_kenel`, not Unity.

## Build and Test

Run from the repository root. Replace `arm64` with `x86_64` or `riscv` for the other architectures.

```sh
uv run cmake --preset arm64-qemu-debug
uv run cmake --build --preset arm64-qemu-debug
uv run ctest --preset arm64-qemu-debug-test
```

CTest runs `moss-functional` and `moss-framework`. Release also provides `moss-benchmark`. The `test-kernel` build target runs functional and framework tests; `benchmark-kernel` runs benchmarks in Release. `run_qemu.py --test` delegates to the same functional runner. Configure with `-DMOSS_BUILD_TESTS=OFF` to exclude validation images and validation userspace programs.

The default is QEMU TCG with four real online vCPUs and 2048 MiB RAM. CPU count is never silently clamped. The resource suite verifies work executed on every requested CPU and writes to owned memory beyond the old 256 MiB window. The present low physical map limits ARM64/x86_64 to 3072 MiB and RISC-V to 2048 MiB. Only the default profile is the acceptance profile; smaller diagnostic profiles do not replace it.

Each functional suite boots once, executes its cases sequentially, and stops after failure. Subsequent suites get fresh guests. Panic and timeout self-checks each use their own guest. Five warmups and thirty recorded benchmark batches share one guest per scenario, not one boot per sample.

```sh
uv run scripts/kernel_validation.py run --config build/arm64-qemu-debug/qemu_config.json --workload mm
uv run scripts/kernel_validation.py run --config build/arm64-qemu-debug/qemu_config.json --selftest
uv run scripts/kernel_validation.py run --config build/arm64-qemu-release/qemu_config.json --benchmark --order 2 --output build/results/order2
```

Use the actual `qemu_config.json` path printed by your preset if using an overridden build directory. With no `--output`, each run creates a unique directory under `<build>/validation/`. Explicit output directories must not already exist. The terminal prints the canonical report path.

`--cpus`, `--memory-mib`, `--warmup`, `--samples`, `--iterations`, and `--order` are explicit overrides. Initial deadlines are `--startup-timeout 30`, `--case-timeout 5`, and `--guest-timeout 60`, in host seconds. Increase the case limit for intentionally longer workloads. Ctrl-C or SIGTERM finalizes partial reports and terminates/reaps QEMU; workloads not started are recorded as such.

## Single-Function Measurements

The five built-ins are `bench.allocate`, `bench.release`, `bench.combined`, `bench.read`, and `bench.getpid`. Allocation and release support orders 0 through 4. `bench.read` measures 256-byte reads from a real 64 KiB ramfs file. `bench.getpid` brackets real user-to-kernel-to-user calls from userspace, rather than calling a handler directly.

Add a callback in `src/test/validation.cpp` and register it during `moss_validation_boot`:

```cpp
bench::register_benchmark("bench.my_function", [](bench::Context& context) {
  // Create bounded, scenario-owned fixtures here, not during registration.
  context.measure_batches(
      [&](usize count) { return prepare_owned_inputs(count); },
      [&](usize index) { invoke_production_function(index); },
      [&](usize count) { return validate_and_release_owned_inputs(count); });
});
```

The three application-specific functions above are author-provided: prepare and cleanup return `bool`; the operation returns `void` and retains its result in the fixture for untimed validation. A failed prepare still invokes cleanup, so partially initialized fixtures must be safe to release. Cleanup failures invalidate the scenario. Refer to `allocation_benchmark` for a complete production example with bounded ownership and free-page accounting.

Add the stable scenario ID to the host `CATALOG` in `scripts/kernel_validation.py`. Similarly, add functional cases with `ut::register_test` inside an explicit `ut::register_suite`, then update that suite's host catalog. IDs and descriptor strings must have static lifetime and use ASCII letters/digits or `_-.=/`, at most 80 characters. Capacities are 128 cases, 16 suites and 16 benchmarks. Change the workload version when changing its definition or fixture semantics.

Counters are ordered and frequencies are validated: ARM64 CNTFRQ/CNTVCT, RISC-V DTB timebase/time, and x86 CPUID.15 or three bounded PIT-channel-0 calibration samples. A bounded pilot selects an operation count, then all batches keep it fixed. The worker is pinned to CPU 0; other CPUs stay online and normal interrupts remain enabled during measured kernel operations.

Raw ticks and empty-loop/counter overhead are retained, without exact overhead subtraction. Reported medians summarize batch-average elapsed time per operation, including callees and residual loop/result-storage costs. They are not per-call latency percentiles, exclusive function CPU time, hardware CPU cycles, or native-hardware performance claims. Do not run other benchmark runners concurrently when collecting comparison data.

## Reports and Baselines

Every run retains `results.json`, per-case `junit.xml`, and original `<workload>/serial.log` and `qemu.log`. The JSON records build/compiler flags, revision, dirty state, image/fixture hashes, QEMU arguments, resources, clock calibration, raw batches, completion and normalized exit status. Expected fatal self-checks retain both the expected and observed outcome.

```sh
uv run scripts/kernel_validation.py run --config build/arm64-qemu-release/qemu_config.json --benchmark --output build/results/baseline
uv run scripts/kernel_validation.py run --config build/arm64-qemu-release/qemu_config.json --benchmark --baseline build/results/baseline/results.json --output build/results/current
uv run scripts/kernel_validation.py compare build/results/baseline/results.json build/results/current/results.json
```

The baseline is read-only and explicit. Matching scenarios reuse its valid operation count. Offline comparison launches no QEMU, revalidates raw evidence, and refuses incomplete/invalid or incompatible measurements. Revision and image hashes can differ; workload version, parameters, fixtures, build policy and execution environment must match. A slowdown is informational, not a CI failure; functional errors and invalid measurements still fail the run.

Host-tool regression tests use `uv run pytest scripts/tests`. Those tests validate orchestration and parsing only; real kernel acceptance is the QEMU matrix above.
