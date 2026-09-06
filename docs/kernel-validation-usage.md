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

`--cpus`, `--memory-mib`, `--warmup`, `--samples`, `--iterations`, and `--order` are explicit overrides. `--machine`, `--cpu`, `--qemu` and `--dtb` select the runtime environment. `--expected-ram-mib` explicitly checks firmware-visible RAM when firmware reserves part of the installed RAM; it defaults to `--memory-mib` and is recorded separately. Default host deadlines are 30 s for startup, 60 s per guest, and 5 s per case except `pfa` (30 s: exhaustive 2 GiB memory access can exceed 5 s under host pressure). An explicit `--case-timeout` overrides either case default, including a shorter value. Reports retain the effective `case_timeout_seconds` per guest and host-observed `elapsed_seconds` per executed case; these include observation/polling effects and are not kernel microbenchmarks. CTest allows 600 s for the functional/framework runner to finish its guests, cleanup and reports. Ctrl-C or SIGTERM finalizes partial reports and terminates/reaps QEMU; workloads not started are recorded as such.

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

## Page Permission Boundaries

`mm.permissions` is part of the default functional run. It checks supervisor-only
table defaults, rejects USER attributes in the kernel mapping API, and walks the
production kernel identity/direct-map trees and the current process's hardware
page-table root. Kernel leaves must not carry USER; x86 user mappings must retain
USER throughout their permission chain. ARM64 also verifies the active TTBR1 root.
`kernel_wx` checks that executable kernel leaves are read-only and confined to
the linked text range (including boot text), all direct-map aliases are NX,
and no alias of text/rodata is writable. x86 also checks CR0.WP. The
`address_space_ownership` case creates, COW-clones and destroys real address
spaces, checking inherited kernel permissions, physical-page references,
page-count recovery and unchanged shared kernel tables.
`vma_boundaries` exercises production AddressSpace admission: complete user
address bounds, alignment, empty/reversed ranges, permissions and the reserved
sigreturn page. It also checks access across adjacent VMAs and rejection at a
gap or incompatible permission.

This is structural U/S and W^X evidence for the present 0-4 GiB mapping contract,
not complete malicious-user fault containment or physical-hardware acceptance.
Ownership snapshots ignore only hardware Accessed/Dirty state; address, permission
and software ownership bits remain protected by the hash. Framework self-checks
verify this distinction without modifying an installed page table.

The `users.user_ranges` case uses actual architecture syscalls to reject invalid
debug pointers, kernel/overflow/reserved mmap ranges, unsupported flags and
read-only/PROT_NONE output. It creates two adjacent anonymous VMAs and exercises
copy-in, copy-out and pathname copying across their boundary, then removes one
VMA and requires EFAULT. Bounded strings without a NUL return ENAMETOOLONG instead
of silently using a truncated name. This follows the current Moss ABI:
clock_gettime writes one u64 nanosecond value; zero-count read/write return EINVAL.
These are policy checks, not recoverable CPU-fault tests: copy helpers and VFS
buffer accesses still need fault fixups and VM lifetime protection (MOSS-002).
The subsequent users case covers fork/exec/reaping; containers.smp covers CPU1.

## User Floating-Point State

On x86_64, `users.fork_exec_exit_reap` also checks x87 data/control, MXCSR and
XMM15 across yield and fork, child state changes without parent contamination,
default state after exec, and real x87 invalid-operation termination followed by
parent continuation. Exec validates argc/argv. `containers.smp` additionally
checks inherited FP state on its actual CPU1 child and the surviving CPU0 parent.
These checks do not cover every extended register, signal frame or CPU feature.

The additional `users.simd_fault` workload requires a real unmasked SSE invalid
operation to terminate only the child. It is explicit, not part of the default
functional set, and a missing exception remains a failure:

```sh
uv run scripts/kernel_validation.py run --manifest build/x86_64-debug/moss-artifacts.json \
  --workload users.simd_fault
```

The current QEMU 11.1.1 TCG run fails this assertion: its CPU trace records #MF
but no #XM. QEMU's [upstream explanation](https://github.com/qemu/qemu/commit/418b0f93d12a1589d5031405de857844f32e9ccc)
distinguishes SSE status-flag emulation from trapping support. Do not replace the
arithmetic with a software interrupt or classify its early return as a pass.
SIMD-fault isolation remains unaccepted until exercised in a suitable environment.

## Container Ownership

The `containers` suite runs production `LockedList` and `LockedHashMap`: reachable
values survive insertion, unlink/replacement destroys each owned value once, and
a 1,024-node clear/reuse restores heap accounting. Lookup returns a value copy
(a retained `shared_ptr` for owned objects), not node storage. The `held_reader`
case keeps that owner across removal; `reentry` checks snapshot callbacks and
destructors accessing the same container after its lock is released. The fake
RCU reader/callback queue has been removed, not given a periodic drain.

`containers.smp` forks a real userspace child with inherited CPU1 affinity. CPU0
removes an object while CPU1 retains it, then barriers force two creators past
the same-key lookup and race insertion/removal. Assertions verify the actual CPU
IDs, a single published value, exactly one successful removal and final destruction.
Only CPU0 records assertions; the peer publishes results with acquire/release
handshakes. The parent reaps the child before destroying the fixture. A stuck
handshake fails the host case deadline; there are no timed sleeps or mock workers.

Both suites are in the default functional selection (nine suites, twenty-seven cases).
Run either alone with `--workload containers` or `--workload containers.smp`.
The SMP suite requires at least two CPUs; for `--cpus 1`, explicitly select
single-worker workloads. This is bounded container/SMP boot coverage, not full
scheduler, IRQ-context, driver or IPC lifecycle acceptance; MOSS-006 remains open.

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
