# Moss

Moss is a modern multi-architecture hybrid kernel operating system supporting ARM64, x64, and RISC-V 64 architectures. Built with C++26 using latest techniques, it emphasizes performance, modularity, and a clean codebase.

## Quick Start

```bash
# Configure, build and test all workflows concurrently (QEMU required for tests)
uv run build.py

# Limit concurrent presets (use --jobs 1 for sequential workflows)
uv run build.py --jobs 2

# List available build presets
uv run cmake --list-presets workflow
```

`--jobs` / `-j` limits simultaneous preset workflows; it does not change the
compiler parallelism within each preset. Architecture and build-type filters
still apply. Verbose output is grouped by preset and printed when it finishes.

## Testing

CMake configure/build do not require QEMU; workflows also run CTest through the
independent QEMU runner. Each architecture produces its own native kernel (not
one cross-ISA binary).

The static mlibc/BusyBox runtime is built directly from checked-in
sources with native CMake targets. See [third-party sources](docs/third-party-sources.md)
for exact upstream revisions, licenses, the supported profile and build requirements.

The normal initramfs contains only `/busybox.elf`. The embedded init trampoline
starts its interactive ash directly at the `moss$` prompt, including with
`MOSS_BUILD_TESTS=OFF`. The selected applets can be invoked by name using
BusyBox's standalone shell support. The separate validation initramfs starts
`/validation.elf`, which also contains the signal regression cases.

For example, after launching QEMU:

```sh
mkdir /work
printf 'moss\nother\nmoss again\n' > /work/input
cat /work/input | grep moss | wc -l
rm -rf /work
/busybox.elf ash -c 'printf "exec works\n"'
```

Files created during the session live in RAM. The shell retains the current
console's line input and editing; job control and tab completion are not enabled.

```sh
# Configure, build and test in one workflow
uv run cmake --workflow --preset arm64-debug

# Or run each stage separately
uv run cmake --preset arm64-debug
uv run cmake --build --preset arm64-debug
uv run qemu --manifest build/arm64-debug/moss-artifacts.json
uv run ctest --preset arm64-debug-test
```

Replace `arm64` with `riscv64` or `x64`. Machine, CPU, RAM and firmware
are runner options, not build options. See [generic boot](docs/generic-boot.md)
and [ADR-0005](docs/adr/0005-generic-kernels-and-independent-runners.md).

## Lint and formatting

Install system LLVM with `clang-tidy` and `clang-apply-replacements` matching the
build compiler version. The lint script never downloads LLVM tools. Explicit
`--clang-tidy` / `--clang-apply-replacements` paths take priority, followed by tools
beside the build compiler and then PATH. Python dependencies are managed by uv.

```bash
# Configure once; lint incrementally builds to refresh module mappings and BMIs.
uv run cmake --preset arm64-debug
uv run lint.py --check
uv run lint.py --check --preset x64-debug
uv run lint.py --check --preset riscv64-debug

# Restrict files or check changes since merge-base(HEAD, origin/master).
uv run lint.py --check src/core -j 4
uv run lint.py --check src/userspace --preset arm64-debug
uv run lint.py --check --changed

# Collect fixes in parallel, apply once, rebuild BMIs, and check again.
uv run lint.py --fix

# Python lint and source formatting are separate commands.
uv run ruff check .
uv run format --check
uv run python -m pytest
```

The default preset is `arm64-debug`. `--preset NAME` selects `build/NAME`,
following the repository's CMake preset layout; configure that preset first.
`--check` leaves source files untouched, but
updates build artifacts. `--fix-errors` allows fixes despite clang-tidy compiler
diagnostics and requires `--fix`; CMake build failures always stop execution.

Full checks use Git-indexed C/C++ files under `src/`, including `.cppm`, tests and userspace,
intersected with the selected compilation database. `lint.toml` excludes vendored
libfdt sources and header diagnostics. Missing entries are reported explicitly:
other architectures' boot implementations are outside each database's coverage.
The four userspace C programs use CMake compilation targets, so their actual
target and freestanding flags are available to the same lint command. Lint does
not invent compile flags for missing entries.

`--changed` includes committed, staged, unstaged and untracked changes, falling
back to HEAD if `origin/master` is unavailable. Headers, module interfaces and
Lint configuration changes trigger all available translation units, including
when headers/modules are deleted. Changed source files must have compile entries;
regenerate or choose the matching build directory if they are missing. Positional
paths further restrict the selection. Untracked sources are checked only in
`--changed` mode. Exclusion patterns must match indexed C/C++ files and cannot
reinclude files with `!`.

Use `-v` for complete clang-tidy commands and repeat
`--clang-extra-arg-before=ARG` for target-specific additions. Exit codes are `0`
for a clean final check, `1` for diagnostics, `2` for configuration/build/tool
errors, and `130` for interruption.
