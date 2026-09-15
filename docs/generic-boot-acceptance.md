# Generic Boot Acceptance — 2026-09-06

This records the first implementation of [ADR-0005](adr/0005-generic-kernels-and-independent-runners.md),
using Clang 23.1.0 and QEMU 11.1.1 TCG on the local macOS host. The working tree
was intentionally uncommitted. This is emulated functional acceptance, not
physical-board acceptance or a native-performance claim.

## Build and regressions

All six native Debug/Release builds passed. ARM64/RV64 normal and validation
images passed static Linux Image header and ELF relative-relocation checks.

| Preset | Functional | Framework self-checks | Five benchmark scenarios |
| --- | --- | --- | --- |
| `arm64-debug` | Pass | Pass | Not registered |
| `riscv64-debug` | Pass | Pass | Not registered |
| `x64-debug` | Pass | Pass | Not registered |
| `arm64-release` | Pass | Pass | Pass |
| `riscv64-release` | Pass | Pass | Pass |
| `x64-release` | Pass | Pass | Pass |

Run `uv run ctest --preset <preset>-test`. Each functional invocation runs
`resources`, `mm`, `vfs` and `users`; framework checks exercise normal completion,
assertion failure, intentional panic and timeout in separate guests. Release
CTest invocations were sequential, not concurrent benchmark runners. Benchmark
results are validity regressions; no speedup is claimed.

Final Debug reports, relative to `build/<preset>/validation/`:

| Architecture | Functional `results.json` directory | Framework directory |
| --- | --- | --- |
| ARM64 | `1788689927811538000` | `1788689930150756000` |
| RV64 | `1788689928943169000` | `1788689931612920000` |
| x64 | `1788689928550106000` | `1788689931593479000` |

Final Release reports use the same location convention:

| Architecture | Functional | Framework | Benchmark |
| --- | --- | --- | --- |
| ARM64 | `1788689979913480000` | `1788689981694999000` | `1788689993652649000` |
| RV64 | `1788689998022797000` | `1788690001061832000` | `1788690013011157000` |
| x64 | `1788690015935908000` | `1788690018311666000` | `1788690030727661000` |

The host suite passed **102 tests** (`uv run pytest -q scripts/tests`). It covers
manifest validation, same-image normal/debug invocation, optional QEMU lookup,
Image/relocation rejection, serial protocol validation and child-process
termination/reaping, including a valid end followed by a still-running child and
a duplicate end record. Ruff and `git diff --check` passed. RelWithDebInfo presets
are available but were not included in this acceptance matrix.

## No-QEMU build

A clean ARM64 Debug configure/build succeeded with all three `qemu-system-*`
executables absent from PATH, with `MOSS_BUILD_TESTS=OFF`. It was rebuilt against
the final sources; the default build completed without a validation target.
`moss-artifacts.json` correctly contains null validation fields.

The isolated directory is `build/no-qemu-arm64`. This host used the following
PATH (LLVM and LLD are separately installed Homebrew packages):

```text
/Users/shifu/.local/bin:/Users/shifu/code/os/moss/.venv/bin:/usr/local/opt/llvm/bin:/usr/local/opt/lld/bin:/usr/bin:/bin
```

Under that PATH, `shutil.which` returned `None` for `qemu-system-aarch64`,
`qemu-system-riscv64`, and `qemu-system-x86_64`. Reproduce with your own compiler,
LLD and uv locations, excluding emulator directories:

```sh
uv run cmake --preset arm64-debug -B build/no-qemu-arm64 -DMOSS_BUILD_TESTS=OFF
uv run cmake --build build/no-qemu-arm64 -j 8
```

## Same-image portability evidence

The exact same Debug validation image hash was checked across each row for an
architecture. No recompilation or image patching occurs between machine runs.
Commands are in [generic boot](generic-boot.md#validate-image-reuse).

| Architecture / runtime profile | Result | Report directory |
| --- | --- | --- |
| ARM64 `virt`, GICv2, 4 CPUs, 2048 MiB | Four functional suites pass | `1788689927811538000` |
| ARM64 `virt`, GICv3, 16 CPUs, 1024 MiB | Resources pass | `1788689959872590000` |
| ARM64 `raspi4b`, 4 CPUs, 960 MiB described RAM | Four functional suites pass | `1788689958704140000` |
| ARM64 `raspi4b`, same resources | All framework checks pass, including panic | `1788689963389513000` |
| RV64 `virt`, `rv64`, 4 CPUs, 2048 MiB | Four functional suites pass | `1788689928943169000` |
| RV64 `virt`, `rv64,sstc=false`, 2 CPUs, 1024 MiB | Four functional suites pass | `1788689962189453000` |
| x64 `q35`, 4 CPUs, 2048 MiB | Four functional suites pass | `1788689928550106000` |
| x64 `pc`, 2 CPUs, 1024 MiB | Four functional suites pass | `1788689961037932000` |

Validation image SHA-256:

```text
ARM64   cb4df3406d3d073d33b4ce14ceab0ea8c53b75e88babdc1238a445d7e470a6f2
RV64    70a0a2ac92009921a4d8642c36839d00df3a519efdc87eecee827ef9504d10ab
x64  459bb7adf5765e27e342ab85d8efe2827c12a7a97a76da5a1250a802ecead810
```

The synthetic `raspi4b` fixture exercises different load/RAM/UART/GIC addresses,
two bus translation ranges, and spin-table rather than PSCI secondary startup.
Its compiled DTB hash is
`d1ec690cb90a59acc02308c6948fcf0dc095fd66e7cc5209fa7bf47f88b2a583`.
Installed RAM is 2048 MiB; the QEMU direct loader describes 960 MiB. Validation
asserts that explicit firmware-visible expectation, recorded separately from
installed RAM.

The normal, non-validation kernels also reached `MOSS shell v0.1` / `moss$` via
the independent runner on all three architectures and on ARM64 `raspi4b`.
On `raspi4b`, entering `help` returned the built-in command list and another
prompt. ARM64's normal Image hash is
`6307f510c6117949e1e2cb992fa8bbc7451c25b5fbcab341672f5f283cf056e0`.
Bounded shell smoke runs deliberately exit the runner with status 124; their
acceptance is the observed shell output, not the timeout status.

Local reports/logs are build artifacts, not committed fixtures. Rebuilding may
change hashes; each new acceptance run must compare its own recorded hashes.
No physical device, arbitrary SoC layout, unimplemented driver, x2APIC/AIA, or
UEFI-native boot path is accepted here. See the explicit
[implementation limits](generic-boot.md#current-implementation-limits).
