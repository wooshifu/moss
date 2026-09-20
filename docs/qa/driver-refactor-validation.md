# Driver refactor validation

Validated on 2026-09-18 against the uncommitted driver refactor based on
`caaab1bce5259ddf2c23437b8f13bb12c233a9c7`. See [driver boundaries and ownership](../drivers.md).

## Build and QEMU matrix

All nine architecture/configuration builds passed, including RelWithDebInfo.
The six Debug/Release CTest configurations passed all 28 checks:

| Architecture | Debug CTest | Release CTest | RelWithDebInfo build |
| --- | --- | --- | --- |
| ARM64 | 5/5, 757.6 s | 5/5, 527.8 s | Passed |
| x64 | 4/4, 809.0 s | 5/5, 574.6 s | Passed |
| RV64 | 4/4, 957.7 s | 5/5, 547.7 s | Passed |

Every CTest configuration completed 1,000 application recovery cycles, alongside
functional, framework and production boot checks. ARM64 Debug also passed the
GDB first-input check; all Release configurations passed their benchmarks.
These are benchmark execution checks, not a claim of native performance improvement.

The driver workload passed on all three architectures. It checks registration
order, matching, IDs and content names, reentrant callbacks, probe rollback,
owned teardown, pinned boot adoption, real heap exhaustion during device/driver
and IRQ registration, and console ring wrap/overflow. Functional checks include
real UART partial reads and empty reads interrupted by signals from another CPU.

## ARM64 hardware variants

GICv3 `virt`, `raspi3ap`, `raspi3b` and `raspi4b` each passed the driver workload
and production boot using the unchanged ARM64 Debug image. Its `moss.bin` SHA-256 is
`6e1857fc7bbf033dd3c8657e49a0caca3b99e0dc257f5720e3d40a9484f4ff83`.
An additional GICv3 run with 16 CPUs and 1 GiB passed all 24 functional workloads.

## Host checks and evidence

`uv run pytest -q scripts/tests` passed 366 tests, including console IRQ/sleep
interleavings with local and remote producers. Ruff passed for the modified
Python files; `git diff --check` passed. All 36 recorded kernel/initramfs hashes
remained unchanged throughout final validation.

Local aggregate results, hashes and CTest logs are in
[`build/driver-refactor-validation/results.json`](../../build/driver-refactor-validation/results.json).
Individual QEMU reports and serial logs remain under each preset's
`build/<preset>/validation/` directory. Only completed runs on the final binaries
are counted; superseded development runs are excluded.

Reproduce the matrix with the existing presets:

```sh
uv run cmake --build --preset <architecture>-<configuration>
uv run ctest --preset <architecture>-<debug-or-release>-test --output-on-failure
uv run pytest -q scripts/tests
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --machine virt,gic-version=3 --cpus 16 --memory-mib 1024
```

All hardware checks above use QEMU. No physical board or native hardware was
tested; physical interrupt routing, timer accuracy and UART electrical behavior
remain separate acceptance requirements.
