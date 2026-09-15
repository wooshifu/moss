---
name: moss-build
description: This skill should be used when building the moss kernel, running it in QEMU, running unit tests, debugging with GDB, or using Docker for CI builds. Also use when the user mentions "build", "compile", "cmake", "preset", "QEMU", "run", "test", "debug", "GDB", "docker compose", "build.py", or asks about build errors, test failures, or how to run the kernel.
---

# Moss Build & Test

```
Build → Run → Test — always in this order.
Build with cmake presets, run/test with the generated QEMU wrapper script.
```

Moss targets 3 architectures (ARM64, x64, RISC-V 64) × 2 build types (debug, release) = 6 presets. Each preset builds, generates QEMU scripts, and optionally runs tests in a single workflow.

## Step 1 — Build

Pick the right command based on scope:

| Goal | Command |
|------|---------|
| Single preset (fastest) | `cmake --workflow --preset arm64-qemu-debug` |
| All 6 presets | `uv run build.py` |
| One architecture, both types | `uv run build.py --arch arm64` |
| Main architectures, debug only | `uv run build.py -m --build-type debug` |
| Preview what would build | `uv run build.py --dry-run` |
| List available presets | `uv run build.py list` |

Available presets: `arm64-qemu-debug`, `arm64-qemu-release`, `x64-qemu-debug`, `x64-qemu-release`, `riscv64-qemu-debug`, `riscv64-qemu-release`.

The project uses `-Weverything -Werror` — every warning is a build failure. Fix all warnings before proceeding.

## Step 2 — Run & Test

After a successful build, CMake generates a QEMU wrapper script in the build directory. Use it for all run/test/debug operations:

```bash
./build/<preset>/run_qemu.sh              # run kernel (ELF mode)
./build/<preset>/run_qemu.sh --test       # run unit tests
./build/<preset>/run_qemu.sh --debug      # start GDB server (port 1234)
./build/<preset>/run_qemu.sh --bin        # boot raw binary image
```

On Windows, use `run_qemu.bat` or `run_qemu.ps1` instead.

CMake targets provide an alternative interface:

```bash
cmake --build --preset arm64-qemu-debug --target run-qemu      # run
cmake --build --preset arm64-qemu-debug --target debug          # GDB
cmake --build --preset arm64-qemu-debug --target test-kernel    # test
```

## Step 3 — Debug

### GDB attach

Start the kernel in debug mode, then attach GDB from another terminal:

```bash
# Terminal 1: start QEMU with GDB server
./build/arm64-qemu-debug/run_qemu.sh --debug

# Terminal 2: attach GDB
gdb-multiarch build/arm64-qemu-debug/bin/moss.elf -ex "target remote :1234"
```

### QEMU trace options

Pass extra QEMU flags to trace interrupts, instruction execution, or device activity:

```bash
# Trace interrupts and executed instructions
./build/<preset>/run_qemu.sh --debug --qemu-args="-d int,in_asm"

# Alternative syntax with double-dash separator
./build/<preset>/run_qemu.sh --debug -- -d int,in_asm

# Log to file + trace virtio devices
./build/<preset>/run_qemu.sh --qemu-args="-d int,in_asm -D qemu.log -trace enable=virtio*"
```

### Disassembly analysis

After each build, the full kernel disassembly is generated at `./build/<preset>/moss.dis`. Cross-reference it with QEMU trace output to follow execution flow:

```bash
# Find a function in the disassembly
grep -A 20 '<kernel_main>:' build/arm64-qemu-debug/moss.dis
```

Other generated files: `moss.sym` (symbol table), `moss.bin` (raw binary), `moss_boot.bin` (boot section).

## Docker (CI / no local toolchain)

Build and test inside Docker without installing Clang or QEMU locally:

```bash
docker compose -f docker/docker-compose.yaml build                           # build image
docker compose -f docker/docker-compose.yaml run moss-qemu                   # build + run
docker compose -f docker/docker-compose.yaml run moss-qemu test              # unit tests
docker compose -f docker/docker-compose.yaml run moss-qemu run --arch x64 # specific arch
docker compose -f docker/docker-compose.yaml run moss-qemu shell             # interactive
```

## Troubleshooting

| Problem | Likely Cause | Fix |
|---------|-------------|-----|
| Build fails with warning-as-error | `-Weverything -Werror` flags a new warning | Fix the warning; do not suppress it without justification |
| QEMU script not found | Build did not complete successfully | Rerun `cmake --workflow --preset <preset>` |
| Test timeout (>30s) | Kernel hangs or infinite loop | Use `--debug` mode with GDB to find the hang point |
| `qemu-system-*` not found | QEMU not installed or not in PATH | Install QEMU or set path in CMakeUserPresets.json |
| Wrong architecture binary | Used wrong preset | Check preset name matches target arch |
