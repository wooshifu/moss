# Running Kernel Validation

The validation executable links the same production object modules and follows the same boot, memory, VFS, scheduler, userspace entry and exec paths as `moss.elf`. Its initramfs contains deterministic fixtures and real userspace validation programs. The framework is `ut_kernel`, from `unit_kenel`, not Unity.

## Build and Test

Run from the repository root. Replace `arm64` with `x86_64` or `riscv` for the other architectures.

```sh
uv run cmake --preset arm64-debug
uv run cmake --build --preset arm64-debug
uv run ctest --preset arm64-debug-test
```

CTest runs `moss-functional` and `moss-framework`. Release also provides `moss-benchmark`. The `test-kernel` build target runs functional and framework tests; `benchmark-kernel` runs benchmarks in Release. Use `kernel_validation.py run` for validation; the normal runner does not dispatch tests. Configure with `-DMOSS_BUILD_TESTS=OFF` to exclude validation images and validation userspace programs.

The default is QEMU TCG with four real online vCPUs and 2048 MiB RAM. CPU count is never silently clamped. The resource suite verifies work executed on every requested CPU and writes to owned memory beyond the old 256 MiB window. The present early mappings require RAM/device addresses below 4 GiB; the largest usable RAM size therefore depends on the firmware's physical layout. The default resource profile remains the baseline regression; additional machine/layout profiles verify image portability.

Each functional suite boots once, executes its cases sequentially, and stops after failure. Subsequent suites get fresh guests. Panic and timeout self-checks each use their own guest. Five warmups and thirty recorded benchmark batches share one guest per scenario, not one boot per sample.

```sh
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --workload mm
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --selftest
uv run scripts/kernel_validation.py run --manifest build/arm64-release/moss-artifacts.json --benchmark --order 2 --output build/results/order2
```

Use the actual `moss-artifacts.json` path printed by your preset if using an overridden build directory. With no `--output`, each run creates a unique directory under `<build>/validation/`. Explicit output directories must not already exist. The terminal prints the canonical report path.

`--cpus`, `--memory-mib`, `--warmup`, `--samples`, `--iterations`, and `--order` are explicit overrides. `--machine`, `--cpu`, `--qemu` and `--dtb` select the runtime environment. `--expected-ram-mib` explicitly checks firmware-visible RAM when firmware reserves part of the installed RAM; it defaults to `--memory-mib` and is recorded separately. Initial deadlines are `--startup-timeout 30`, `--case-timeout 5`, and `--guest-timeout 60`, in host seconds. Increase the case limit for intentionally longer workloads. Ctrl-C or SIGTERM finalizes partial reports and terminates/reaps QEMU; workloads not started are recorded as such.

## Memory Ownership and Boot Inputs

The `heap` and `pfa` suites check real allocation/release and exhaustion. They
snapshot PFA metadata, the early table pool and current page-table trees; heap
writes must not change them, and PFA must never return their pages as free memory.
These are single-worker ownership checks, not concurrent allocator/COW acceptance.

Additional boot-input checks run independently of CMake configure/build:

```sh
uv run scripts/check_pfa_firmware.py --manifest build/riscv-debug/moss-artifacts.json
uv run scripts/check_heap_layout.py --manifest build/arm64-debug/moss-artifacts.json
```

The firmware check requires QEMU and dtc's `fdtget`/`fdtput`. It reuses an unchanged
image with altered DTBs; ARM64 tests reserved regions, while RV64 also tests RAM
bank shape/order/capacity. The heap-layout check requires an existing Ninja build
and LLVM tools. It reuses that build's actual link command to create a disposable
image with an overlapping heap limit; original artifacts and sources are untouched.
Generated images, manifests, DTBs and reports remain under the supplied build directory.

Negative checks require the expected boot diagnostic and no `ready` event. Their
raw guest reports remain errors; the check script succeeds only when rejection is
verified. They do not count an unexecuted functional test as passed.

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

Every run retains `results.json`, per-case `junit.xml`, and original `<workload>/serial.log` and `qemu.log`. The JSON records build/compiler flags, revision, dirty state, image/fixture hashes, QEMU arguments, resources, clock calibration, raw batches, completion, host termination reason and raw child exit status. Expected fatal self-checks retain both the expected and observed outcome.

```sh
uv run scripts/kernel_validation.py run --manifest build/arm64-release/moss-artifacts.json --benchmark --output build/results/baseline
uv run scripts/kernel_validation.py run --manifest build/arm64-release/moss-artifacts.json --benchmark --baseline build/results/baseline/results.json --output build/results/current
uv run scripts/kernel_validation.py compare build/results/baseline/results.json build/results/current/results.json
```

The baseline is read-only and explicit. Matching scenarios reuse its valid operation count. Offline comparison launches no QEMU, revalidates raw evidence, and refuses incomplete/invalid or incompatible measurements. Revision and image hashes can differ; workload version, parameters, fixtures, build policy and execution environment must match. A slowdown is informational, not a CI failure; functional errors and invalid measurements still fail the run.

Host-tool regression tests use `uv run pytest scripts/tests`. Those tests validate orchestration and parsing only; real kernel acceptance is the QEMU matrix above.
