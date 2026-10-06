---
name: moss-build
description: This skill should be used when building the moss kernel, running it in QEMU, running unit/validation tests, debugging with GDB, or using Docker for CI builds. Also use when the user mentions "build", "compile", "cmake", "preset", "QEMU", "run", "test", "ctest", "debug", "GDB", "docker compose", "build.py", "qemu.py", "kernel_validation.py", or asks about build errors, test failures, or how to run the kernel.
---

# Moss Build & Test

```
Build → Run → Test — always in this order.
Build with CMake presets; run and test from the build's artifact manifest (build/<preset>/moss-artifacts.json).
```

Moss targets 3 architectures (ARM64, x64, RISC-V 64) × 3 build types (debug, release, relwithdebinfo) = 9 presets. Each workflow preset configures, builds, and runs CTest, which boots the validation kernel in QEMU. CMake, Ninja and CTest come from the uv dev group, so run them through `uv run`.

## Step 1 — Build

Pick the right command based on scope:

| Goal | Command |
|------|---------|
| Build one preset (fastest, no QEMU) | `uv run cmake --preset arm64-debug && uv run cmake --build --preset arm64-debug` |
| One preset: configure + build + CTest | `uv run cmake --workflow --preset arm64-debug` |
| All 9 workflows, run concurrently | `uv run build.py` |
| Limit concurrent workflows | `uv run build.py -j 2` |
| One architecture, all build types | `uv run build.py --arch arm64` |
| One build type, all architectures | `uv run build.py --build-type debug` |
| One specific workflow | `uv run build.py --preset x64-release` |
| Clean before building | `uv run build.py --clean` |
| Preview what would build | `uv run build.py --dry-run` |
| List available presets | `uv run cmake --list-presets workflow` |

Presets are `{arm64,x64,riscv64}-{debug,release,relwithdebinfo}`, e.g. `arm64-debug`, `x64-release`, `riscv64-relwithdebinfo`. Each builds into `build/<preset>`, and its test preset is `<preset>-test`. Configuring without a preset is a fatal error. After the first configure, `uv run cmake --build --preset <preset>` alone is enough.

The project uses `-Weverything -Werror` — every warning is a build failure. Fix all warnings before proceeding.

## Step 2 — Run & Test

Every build writes the artifact manifest `build/<preset>/moss-artifacts.json`. The Python runners read it; CMake holds no QEMU, machine or board settings. `qemu.py` boots the production image:

```bash
uv run qemu.py --manifest build/arm64-debug/moss-artifacts.json                  # boot (4 vCPUs by default)
uv run qemu.py --manifest build/arm64-debug/moss-artifacts.json --smp 2 --memory-mib 1024
uv run qemu.py --manifest build/arm64-debug/moss-artifacts.json --dry-run        # print the QEMU command only
```

Other options: `--machine`, `--cpu`, `--dtb`, `--timeout <seconds>`, `--qemu <executable>`. Extra QEMU arguments go after `--`. The console multiplexes the serial port and the QEMU monitor: `Ctrl-a c` toggles the monitor, `Ctrl-a x` quits.

Tests boot the separate validation kernel (`moss.test.elf`):

```bash
uv run ctest --preset arm64-debug-test                             # all CTest entries
uv run ctest --preset arm64-debug-test -R moss-production-boot     # one CTest entry
uv run cmake --build --preset arm64-debug --target test-kernel     # functional + application tests (label "kernel")
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --workload users.signals   # one suite
```

CTest entries:
- All presets: `moss-functional`, `moss-applications`, `moss-framework`, `moss-production-boot`, `moss-supervisor-reset`.
- x64: `moss-pvh-initrd`.
- ARM64 Debug: `moss-console-input`, `moss-console-registration`.
- Release: `moss-benchmark` (also the `benchmark-kernel` target).

The 30-minute `stability-kernel` target is not part of CTest. The smallest selectable unit is a suite (`--workload`, repeatable); suite IDs are the keys of `CATALOG` in `scripts/kernel_validation.py`. Reports go to `build/<preset>/validation/<id>/results.json`. See `docs/kernel-validation-usage.md` for all runner options.

## Step 3 — Debug

### GDB attach

`--debug` boots the same image through the same loader, paused (`-s -S`), with a GDB server on `localhost:1234`. Attach GDB from another terminal:

```bash
# Terminal 1: start QEMU paused with a GDB server
uv run qemu.py --manifest build/x64-debug/moss-artifacts.json --debug

# Terminal 2 (x64): the ELF runs at its linked addresses
gdb-multiarch build/x64-debug/bin/moss.elf -ex "target remote :1234"
```

ARM64 and RV64 images are position-independent and linked at address zero. Read the actual Image load base from the QEMU monitor (`Ctrl-a c`, then `info roms`), then load symbols at that offset:

```bash
gdb-multiarch -ex "target remote :1234"
(gdb) add-symbol-file build/arm64-debug/bin/moss.elf -o <image_load_base>
```

The first stop may be in firmware, not the kernel. Don't reuse one machine's load address for another machine.

### QEMU trace options

Pass extra QEMU flags after `--` to trace interrupts, instruction execution, or device activity:

```bash
# Trace interrupts and executed instructions
uv run qemu.py --manifest build/arm64-debug/moss-artifacts.json --debug -- -d int,in_asm

# Log to file + trace virtio devices
uv run qemu.py --manifest build/arm64-debug/moss-artifacts.json -- -d int,in_asm -D qemu.log -trace 'enable=virtio*'
```

### Disassembly analysis

After each build, the full kernel disassembly is generated at `./build/<preset>/moss.dis`. Cross-reference it with QEMU trace output to follow execution flow. On ARM64/RV64 the image is linked at address zero, so runtime addresses are offset by the image load base (see GDB attach):

```bash
# Find a function in the disassembly
grep -A 20 '<kernel_main>:' build/arm64-debug/moss.dis
```

Other generated files:
- `moss.sym`: sorted symbol table.
- `moss.bin`: raw binary; this is the boot Image on ARM64/RV64.
- `moss_boot.bin`: `.text.boot` only.
- `moss_code.bin`: code and data sections.
- `bin/moss.elf`: kernel ELF with symbols.
- `moss-artifacts.json`: artifact manifest.

## Docker (CI / no local toolchain)

Build and test inside Docker without installing Clang or QEMU locally:

```bash
docker compose -f docker/docker-compose.yaml build                           # build image
docker compose -f docker/docker-compose.yaml run moss-qemu                   # build + run
docker compose -f docker/docker-compose.yaml run moss-qemu test              # unit tests
docker compose -f docker/docker-compose.yaml run moss-qemu run --arch x64 # specific arch
docker compose -f docker/docker-compose.yaml run moss-qemu shell             # interactive
```

`MOSS_ARCH` and `MOSS_BUILD_TYPE` select the preset (default `arm64-debug`). `test` runs the default functional validation suites via `scripts/kernel_validation.py run`.

## Troubleshooting

| Problem | Likely Cause | Fix |
|---------|-------------|-----|
| Build fails with warning-as-error | `-Weverything -Werror` flags a new warning | Fix the warning; do not suppress it without justification |
| `Moss must be configured with a CMake preset` | Configured without `--preset` | `uv run cmake --preset <preset>` |
| `moss-artifacts.json` not found | Configure or build did not complete | Rerun `uv run cmake --workflow --preset <preset>` |
| Validation suite fails or times out | Kernel hang, panic or regression | Rerun only that suite with `--workload <id>`. Inspect `results.json` and the QMP/GDB captures in `<workload>/diagnostics/`, or boot with `qemu.py --debug` |
| Runner rejects a `--workload` selection | Suite needs more vCPUs (`users.timers` needs 3) or another architecture | Raise `--cpus` or use a supported preset |
| `QEMU executable not found` | QEMU not installed or not on PATH | Install QEMU or pass `--qemu <executable>` |
| ARM64 Debug configure fails to find GDB | `moss-console-*` tests require `gdb-multiarch` or `gdb` | Install either debugger |
| Host runs out of memory in `build.py` | Concurrent allocator tests touch all guest RAM | Limit concurrency: `uv run build.py -j 2` |
| Wrong architecture binary | Used wrong preset | Check preset name matches target arch |
