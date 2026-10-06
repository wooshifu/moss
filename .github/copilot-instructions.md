# Copilot instructions for Moss

Moss is a hybrid kernel for ARM64, x64 and RISC-V 64, written in freestanding C++26 modules. Its userspace is freestanding C: a supervisor, isolated services, and static mlibc + BusyBox. Each architecture builds its own native image, and everything boots under QEMU.

## Build, run, test, lint

You need Clang/LLVM ≥ 23 with LLD and the LLVM binutils, plus a matching `clang-tidy`/`clang-apply-replacements` for lint. You also need `uv`, and QEMU for anything that boots. CMake, Ninja, clang-format, cmake-format, ruff and pytest come from the uv dev group, so prefix `cmake`/`ctest` with `uv run`.

The presets are `{arm64,x64,riscv64}-{debug,release,relwithdebinfo}`. Each one builds into `build/<preset>`, and its test preset is `<preset>-test`. Configuring without a preset is a fatal error.

```sh
uv run cmake --preset arm64-debug                # configure
uv run cmake --build --preset arm64-debug        # build (no QEMU needed)
uv run cmake --workflow --preset arm64-debug     # configure + build + CTest
uv run build.py [--arch x64] [--build-type release] [-j 2] [--dry-run]   # all 9 workflows, concurrently
uv run qemu.py --manifest build/arm64-debug/moss-artifacts.json [--smp N] [--memory-mib N] [--machine M] [-- <extra qemu args>]
```

Tests boot the validation kernel `moss.test.elf` in QEMU:

```sh
uv run ctest --preset arm64-debug-test                            # all CTest entries
uv run ctest --preset arm64-debug-test -R moss-production-boot    # a single CTest entry
# A single validation suite (repeat --workload for more). Suite IDs are the keys of CATALOG in scripts/kernel_validation.py.
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --workload users.signals
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --selftest
uv run scripts/kernel_validation.py run --manifest build/arm64-release/moss-artifacts.json --benchmark   # Release only
uv run python -m pytest scripts/tests/test_kernel_validation.py -k <name>                                 # host-side Python tests
```

- The smallest unit you can select is a **suite**. Each suite boots a fresh guest, runs its cases in order and stops at the first failure. Some suites need ≥2 vCPUs (`users.timers` needs 3) or a specific architecture, and the runner rejects selections that don't meet those requirements.
- Reports are written to `build/<preset>/validation/<id>/results.json` (plus `junit.xml`). When a live guest fails, QMP/GDB captures go to `<workload>/diagnostics/`.
- CTest entries:
  - All presets: `moss-functional`, `moss-applications`, `moss-framework`, `moss-production-boot`, `moss-supervisor-reset`.
  - x64: `moss-pvh-initrd`.
  - ARM64 Debug: `moss-console-input` and `moss-console-registration`. Configuring this preset fails unless `gdb-multiarch` or `gdb` is installed.
  - Release: `moss-benchmark`.
  - The 30-minute stability run is a separate `stability-kernel` build target.
- Allocator tests touch all of guest RAM, and `RUN_SERIAL` only serializes within one CTest process. Running presets concurrently can therefore exhaust host memory; limit with `build.py -j 2`.
- `-DMOSS_BUILD_TESTS=OFF` omits the validation image and its userspace programs.

The quality gate must pass with zero warnings before any commit:

```sh
uv run cmake --build --preset arm64-debug          # -Weverything -Werror
uv run lint.py --check                             # clang-tidy, WarningsAsErrors '*'; needs a configured build dir and rebuilds BMIs first
uv run lint.py --check src/mm --preset x64-debug   # restrict paths / use another configured preset
uv run lint.py --check --changed                   # only changes since merge-base(HEAD, origin/master)
uv run lint.py --fix                               # apply fixes once, rebuild, recheck
uv run ruff check .
uv run format --check                              # clang-format + cmake-format + ruff format; `uv run format` to apply
```

**Debugging.** `qemu.py --debug` pauses the guest with a GDB server on `localhost:1234`.
- x64: load the manifest's ELF directly.
- ARM64/RV64: the images are PIE. Run `add-symbol-file <debug_symbols> -o <load_base>`, taking the load base from the QEMU monitor's `info roms` (see `docs/generic-boot.md`).

Every build also writes `build/<preset>/moss.dis` (full disassembly) and `moss.sym`.

`.claude/skills/` has longer guides for building and debugging, C++ modules, commits and the quality gate.

## Architecture

- **Build graph.** Each area under `src/` (including sub-areas such as `hal/mmu` and `drivers/serial`) is a CMake **OBJECT** library `moss_<area>` that exports one or more `moss.*` modules. For example, `src/core` provides `moss.std`, `moss.types` and `moss.result`, and `src/aal` provides `moss.arch`.
  - The top-level `CMakeLists.txt` adds the areas in dependency order (`intrinsics` → `core` → `abi` → … → `kernel` → `boot`) and links them into `moss.elf`.
  - These must stay OBJECT libraries. Symbols referenced only from assembly or vector tables would be dropped from STATIC archives.
  - OBJECT libraries cannot form dependency cycles. Break a cycle with an `extern "C"` shim, as `moss_heap_allocate` in `mm.cppm` does.
  - `src/test` links the **same** libraries, plus the test-only `moss_drivers_validation`, into `moss.test.elf`. Validation therefore exercises production code.
- **Toolchain flags live in presets, not in CMakeLists.** `cmake/presets/arch/<arch>.json` holds the target triple, ISA flags, `-ffreestanding -fno-exceptions -fno-rtti` and the `-Weverything -Werror` warning list. Linker scripts are in `src/linker/kernel_<arch>.ld`. `src/userspace/CMakeLists.txt` deliberately clears the kernel flags and builds userspace C with its own freestanding flags, linking directly with `ld.lld`.
- **Artifact manifest.** Each build writes `build/<preset>/moss-artifacts.json`, and every runner reads it (`qemu.py`, `scripts/kernel_validation.py`, `scripts/check_*.py`). Under ADR-0005, CMake owns compiler, ISA and link layout; runners own machine model, CPUs, RAM, firmware and DTB. Never put QEMU or board settings in CMake. Boot protocols per architecture:
  - ARM64: Linux Image (`moss.bin`).
  - RV64: Linux Image over SBI (`moss.bin`).
  - x64: Xen PVH ELF (`bin/moss.elf`).
- **Architecture-specific code** uses two mechanisms:
  - The `MOSS_ARCH_ARM64` / `MOSS_ARCH_X64` / `MOSS_ARCH_RISCV64` compile definitions.
  - Per-architecture `.cpp`/`.S` files that CMake selects by `MOSS_TARGET_ARCH` (`ARM64`/`X64`/`RISCV64`), e.g. `src/boot/src/arch/<arch>/`.

  `moss.arch` (`src/aal`) and `moss.hal.*` are the abstraction layers. Every change must build on all three architectures.
- **Kernel/userspace ABI.**
  - `src/abi/include/moss/` holds the C headers shared by kernel and userspace.
  - `syscall_numbers.def` is an X-macro included by the kernel syscall enum (`kernel-syscall_table.cppm`), by `src/userspace/syscall.h` and by the mlibc build. Syscall numbers are ABI; never renumber them.
  - Syscalls return Linux errno values. Internal kernel code uses `ErrorCode` from `moss.types` instead.
- **Userspace and the native-system migration.**
  - `src/userspace/moss_init.c` is the Initial System Supervisor. It receives the bootstrap capabilities, starts and restarts the isolated services, and launches BusyBox ash.
  - Each service is a `moss_<name>_service.c` paired with a `moss_<name>_protocol.h`: file, namespace, process, pipe, console, loader and code authority.
  - The accepted ADRs (`docs/adr/0008`–`0035`) move policy out of the kernel: VFS and paths, POSIX process semantics, ELF loading and drivers. The kernel keeps mechanisms: capability handles, synchronous control calls with one-shot Reply Capabilities, and Memory Objects.
  - The migration is **partial**. The legacy kernel VFS, the in-kernel `exec` ELF loader and the PID/signal syscalls in `src/kernel/src/syscall_table.cpp` still serve ordinary shell paths.
  - Before changing either side, check `docs/native-system-migration.md` to see which path is live. Don't add new policy to the kernel without the residency justification that ADR-0008 requires.
- **Vendored sources.** `third_party/` holds BusyBox, mlibc (with Moss sysdeps in `third_party/mlibc/sysdeps/moss/`), frigg, compiler-rt builtins and related dependencies. They are built by native CMake in `src/userspace/mlibc/`, with no download or patch step (ADR-0007). `third_party/` and `src/fdt/libfdt` are excluded from lint and format, and `third_party/` is marked `-text` in `.gitattributes`. Record any local adaptation in `docs/third-party-sources.md`.

## Validation framework (adding or changing tests)

The kernel test framework is `ut_kernel`, not Unity. It lives in `src/test/framework/ut_kernel.hpp` and offers a boost::ut-style API, used via `namespace ut = boost::ut`. A case's identity must appear **in the same order** in all of these places:

1. **`CATALOG` in `scripts/kernel_validation.py`.** This is the host's independent expected list, and the runner fails on any missing, extra, duplicated or reordered case.
   - Add a new suite to `FUNCTIONAL`, or to the architecture-gated selection in `functional_workloads()`, so it runs by default.
   - Bump `CATALOG_VERSION` when an existing identity or meaning changes.
   - Update `scripts/tests/test_kernel_validation.py` wherever it asserts catalog contents.
2. **Kernel registration.** Cases are registered with `ut::register_suite` / `ut::register_test`, either in `declare_cases()` in `src/test/validation/registry.cpp` or in a domain `register_*_cases()` function. Registration is explicit, with no static-constructor discovery. Fixtures are created when the case body runs, and declaration order defines catalog order.
3. **Userspace bodies for `users.*` suites.** The kernel registers only `empty_case` placeholders for these suites. The real body is C code in `src/userspace/validation.c` or `src/userspace/validation/*.c`. It reports by **positional index** through the validation control protocol: `control(1, index, …)` starts a case and `control(2, passed, errors)` reports its result.

Kernel test files in `src/test/validation/` are plain global-module translation units: they `import` production modules and `#include` local `.hpp` helpers. Keep that form, because declaring them `module moss.kernel;` changes linkage. List any new file in `src/test/CMakeLists.txt`. Give destructive cases their own suite, since all cases in a suite share one guest lifetime.

## Code conventions

- **C++26 modules, freestanding.** No standard library, exceptions or RTTI. Import `moss.std`, `moss.types`, `moss.result` and the other core modules instead; they provide `u8`…`u64`, `PhysAddr`, `VirtAddr` and the type traits. Production kernel code has no headers of its own. The shared C ABI headers enter through a Global Module Fragment.
- **Module file roles:**
  - Primary interface: `export module moss.x;` in `x.cppm`.
  - Partition: `export module moss.x:part;` in `x-part.cppm`, re-exported from the primary with `export import :part;`.
  - Implementation unit: `module moss.x;` in a `.cpp`.
- **Module layout rules:**
  - Use a single `.cppm` for a module under ~500 lines, and partitions for one over ~1000.
  - List every `.cppm` under `target_sources(... PUBLIC FILE_SET CXX_MODULES FILES ...)` and every `.cpp` under `PRIVATE`.
  - Put all `import`s immediately after the module declaration.
  - The GMF (`module;`) may contain only C headers (vendored, or Moss ABI from `src/abi/include/moss`), `extern "C"` declarations (linker-script symbols, entry points shared with assembly) and required macros.
  - Keep `static` functions out of `export namespace` blocks.
  - Keep attributes such as `[[noreturn]]` identical on declarations and definitions.
- **Errors.** Return `Result<T, E>` or `KernelResult<T>` (that is, `Result<T, ErrorCode>`), built with `Ok(...)` / `Err(...)`.
- **Naming** (enforced by `.clang-tidy`):
  - Types use `CamelCase`.
  - Functions, variables and namespaces use `lower_case`.
  - Private and protected members use `lower_case_` with a trailing underscore.
  - Kernel code mostly lives in `moss::kernel::<area>` (exceptions include `moss::boot` and `moss::fdt`), and tests in `moss::test::validation`.
- **Warnings.** Kernel C++ builds with `-Weverything -Werror`, and clang-tidy treats every warning as an error. Fix the warning; don't add a suppression without a stated reason.
- **Scripts and CMake.**
  - Write tooling in Python 3.14 only, and add no new shell scripts unless asked. Run scripts with `uv run` and declare dependencies in `pyproject.toml`.
  - Prefer the libraries already in use (typer, rich, pydantic) over hand-rolled code.
  - Ruff's line length is 120. Chinese text in strings and comments is intentional.
  - Keep CMake flat and readable, matching the existing style.
- **Domain vocabulary.** `CONTEXT.md` defines the project's terms (Execution Domain, Moss Capability Handle, Synchronous Control Call, Reply Capability, Service Incarnation, Kernel Validation Catalog, …) and lists terms to avoid. Use these terms in code, docs and commit messages.
- **Evidence-scoped claims.** The docs, `todo.md` and `moss-todo.md` record exact presets, report IDs and limits; the two todo files are large Chinese capability and audit logs kept in sync. A passing focused run proves only what it exercised. Keep failing reports as evidence rather than retrying until a run passes.

## Commits

- Format: `[module][subsystem] imperative lowercase summary`, in English. Example: `[ipc][deadline] inherit caller deadlines across nested calls`.
- Common module tags: `kernel`, `userspace`, `test`, `process`, `ipc`, `mm`, `boot`, `infra`, `build`, `cmake`, `docs`, `cleanup`.
- The body is optional and explains *why*.
- Never add `Co-Authored-By` or any other AI-attribution trailer.
- Run the quality gate before every commit.
