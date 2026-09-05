# Moss

Moss is a modern multi-architecture hybrid kernel operating system supporting ARM64, x86_64, and RISC-V architectures. Built with C++26 using latest techniques, it emphasizes performance, modularity, and a clean codebase.

## Quick Start

```bash
# Build all architectures
uv run build.py

# List available build presets
uv run build.py list
```

## Testing

Built kernels can be tested using QEMU with the generated run scripts in `build/*/run_qemu.sh`.

## Lint and formatting

Install system LLVM with `clang-tidy` and `clang-apply-replacements` matching the
build compiler version. The lint script never downloads LLVM tools. Explicit
`--clang-tidy` / `--clang-apply-replacements` paths take priority, followed by tools
beside the build compiler and then PATH. Python dependencies are managed by uv.

```bash
# Configure once; lint incrementally builds to refresh module mappings and BMIs.
uv run cmake --preset arm64-qemu-debug
uv run lint.py --check
uv run lint.py --check --preset x86_64-qemu-debug
uv run lint.py --check --preset riscv-qemu-debug

# Restrict files or check changes since merge-base(HEAD, origin/master).
uv run lint.py --check src/core -j 4
uv run lint.py --check src/userspace --preset arm64-qemu-debug
uv run lint.py --check --changed

# Collect fixes in parallel, apply once, rebuild BMIs, and check again.
uv run lint.py --fix

# Python lint and source formatting are separate commands.
uv run ruff check .
uv run scripts/format.py --check
uv run python -m pytest
```

The default preset is `arm64-qemu-debug`. `--preset NAME` selects `build/NAME`,
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
