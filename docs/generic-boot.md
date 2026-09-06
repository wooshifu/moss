# Generic kernels and independent runners

[ADR-0005](adr/0005-generic-kernels-and-independent-runners.md) defines the boundary:
one source tree, one native image **per architecture**, reusable on machines that
satisfy that image's supported boot and device contracts. ARM64, RV64 and x86_64
instructions are different; they do not share one executable binary.

## Ownership

| Component | Owns | Does not own |
| --- | --- | --- |
| CMake | Compiler/ISA, build mode, objects, link layout, artifacts | Emulator detection, machine model, RAM/SMP, firmware, QEMU launch |
| Kernel | Architecture entry, runtime hardware discovery, device drivers | QEMU command lines, named-board resource defaults, host process exit |
| Firmware / bootloader | Hardware initialization, image placement, boot data, reserved memory | Kernel scheduler and driver policy |
| QEMU runner | Emulator/model, CPU, RAM/SMP, firmware, debugging | Compiling or patching the kernel, generating kernel configuration |

DTB `compatible` matching and driver register offsets are intentional hardware
contracts, not board selection. Supporting previously unknown devices still
requires drivers. Missing mandatory resources fail boot; they do not select a
`virt` address as a fallback.

## Compile

Install the existing LLVM/LLD and uv prerequisites. QEMU is unnecessary here.

```sh
uv run cmake --preset arm64-debug
uv run cmake --build --preset arm64-debug
```

Replace `arm64` with `riscv` or `x86_64`, and `debug` with `release` or
`relwithdebinfo`. `build.py` and CMake workflows run configure, build and CTest:

```sh
uv run cmake --workflow --preset arm64-debug
```

The workflow's test stage requires QEMU; the separate configure/build commands
above do not. Pass `-DMOSS_BUILD_TESTS=OFF` when configuring to omit validation
artifacts, and use those separate commands for a build-only invocation. The old
`*-qemu-*` presets and generated launch wrappers/config are removed, without
aliases.

`build/<preset>/moss-artifacts.json` is the handoff contract (schema version 1):

- `target`: architecture and boot protocol.
- `build`: build type, compiler and flags, used for validation provenance.
- `artifacts`: relative paths for `kernel`, `debug_symbols`, `initramfs`,
  `validation_kernel`, and `validation_initramfs`; unavailable optional artifacts
  are `null`.

The manifest contains no machine or emulator settings. Its paths must stay within
its directory. It can be copied with its referenced artifacts to a directory
without `CMakeCache.txt` or source code and consumed by the runner. The normal
runner uses `kernel`, not a guessed filename or the validation executable.

| Architecture | Normal boot artifact | Entry / resource contract |
| --- | --- | --- |
| ARM64 | `moss.bin` | Linux Image, DTB in `x0`, firmware-selected placement, EL1/EL2 entry |
| RV64 | `moss.bin` | Linux Image, supervisor mode, boot hart in `a0`, DTB in `a1`, SBI |
| x86_64 | `bin/moss.elf` | Xen PVH ELF, 32-bit protected-mode entry, PVH memory map and ACPI |

ARM64/RV64 are PIE images: the entry stub applies relative relocations before
using absolute pointers. CMake verifies the static Image header and every dynamic
relocation in both normal and validation builds. The x86 PVH loader loads the
ELF's declared physical segments. `moss_boot.bin` / `moss_code.bin` are diagnostic
section dumps, not alternative boot images.

## Run and debug

Install QEMU only on the host that will execute it:

```sh
uv run scripts/run_qemu.py --manifest build/arm64-debug/moss-artifacts.json
uv run scripts/run_qemu.py --manifest build/arm64-debug/moss-artifacts.json \
  --machine virt,gic-version=3 --smp 16 --memory-mib 1024
uv run scripts/run_qemu.py --manifest build/x86_64-debug/moss-artifacts.json --machine pc
uv run scripts/run_qemu.py --manifest build/riscv-debug/moss-artifacts.json --cpu rv64,sstc=false
```

Defaults (`virt` / `q35`, 4 CPUs, 2048 MiB, TCG) are runner policy, not kernel
configuration. `--dtb`, `--qemu`, `--cpu`, `--machine`, `--smp` and `--memory-mib`
select the execution environment. Additional QEMU options follow `--`.
`--dry-run` prints the invocation; `--timeout` bounds an interactive smoke run
(timeout exit status 124 is not a kernel success result).

`--debug` adds QEMU's GDB server and initial pause (`-s -S`) while booting the
**same image through the same loader**, not a separately loaded ELF. Connect GDB
to `localhost:1234`. For x86 use the manifest's ELF directly. For ARM64/RV64,
symbols are linked relative to image address zero: obtain the actual Image load
base from the firmware/loader or QEMU monitor `info roms`, then use GDB
`add-symbol-file <debug_symbols> -o <image_load_base>`. The initial stopped PC
may be in firmware, not the kernel. Do not reuse a `virt` load address for another
machine.

A physical machine does not use this runner: its firmware/bootloader loads the
same architecture artifact and provides the corresponding contract above.
UEFI-native entry and additional boot protocols need separate adapters.

## Validate image reuse

```sh
uv run ctest --preset arm64-debug-test
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json \
  --machine virt,gic-version=3 --cpus 16 --memory-mib 1024 --workload resources
```

The repository includes a **synthetic QEMU test description**, not a production
Raspberry Pi firmware DTB. It exercises a genuinely different ARM64 memory map,
bus `ranges` translation, UART/GIC resources and spin-table CPU startup:

```sh
dtc -I dts -O dtb -o build/arm64-debug/raspi4b-test.dtb scripts/tests/fixtures/raspi4b.dts
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json \
  --machine raspi4b --dtb build/arm64-debug/raspi4b-test.dtb --expected-ram-mib 960
```

No rebuild occurs between these invocations. This QEMU model requires 4 CPUs and
2048 MiB installed RAM; its direct-kernel loader describes 960 MiB to the guest.
`--expected-ram-mib 960` makes that expectation explicit and recorded, instead of
silently reducing an assertion to whatever the guest reports. Use the same
`run_qemu.py` options (without `--expected-ram-mib`) to boot the normal shell image.

Compare `provenance` image hashes in the saved `results.json` files, not just their
filenames. The runner records the actual machine, resources, CPU, DTB hash and
QEMU version. Completion comes from a valid final `@@MOSS` serial record; the host
terminates and reaps QEMU. Raw emulator exit codes are diagnostics, not magic
per-architecture pass codes. See [validation usage](kernel-validation-usage.md)
and [acceptance evidence](generic-boot-acceptance.md).

## Current implementation limits

These limits describe this implementation, not Linux's full hardware coverage:

- At most 16 CPUs and 8 firmware RAM regions. Early physical mappings cover
  addresses below 4 GiB. Page allocation combines eligible banks above
  `kernel_end`, merges adjacent entries and excludes physical holes and reserved
  data. Metadata must fit within one bank; dense PFN metadata spans holes and
  consumes at most 16 MiB under the current address limit. The boot prefix below
  `kernel_end` remains conservatively reserved until all boot protocols describe
  their live low-memory buffers and trampolines explicitly. Final identity/direct
  maps split 1 GiB blocks into 2 MiB/4 KiB leaves at RAM and image-permission
  boundaries. Kernel text (including boot text) is RX, rodata is RO/NX, and
  other RAM/device mappings are RW/NX. All direct-map aliases are NX and aliases
  of text/rodata are read-only. The existing 64-page early table pool remains a
  hard limit; this does not establish arbitrary SoC layout coverage. Firmware reservations,
  kernel storage, DTB and initramfs must not be reused as free pages.
- User address-space creation, cloning and teardown borrow kernel VA ranges by
  ownership, not by leaf size. The four-level mixed low PUD remains private to
  each process. x86 APs use the temporary PVH map only to leave the low-memory
  trampoline, then install the final tables and CR0.WP before going online.
  Runtime permission checks do not constitute complete uaccess, signal-return
  or malicious-user exception-isolation acceptance.
- User mappings occupy the canonical positive range above the 4 GiB identity
  map, bounded by the active MMU's USER_MAX. Ordinary VMAs cannot cover the
  kernel-installed sigreturn page. Anonymous mmap supports only PRIVATE with
  zero offset and fd=-1; unsupported flags (including FIXED) are rejected.
  Valid nonzero hints do not replace mappings and may fall back to the monotonic
  mmap cursor. This is not Linux ABI compatibility or a complete VM transaction model.
- ARM64: ARMv8-A baseline, 4 KiB pages, PL011/16550 console, GICv2 or one standard
  GICv3 redistributor region, architectural virtual timer, PSCI 0.2+ (HVC/SMC) or
  spin-table startup. GICv4 strides, GICv3 range selection for Aff0 >= 16 and
  multi-region redistributors are not implemented.
- RV64: the configured RV64 ISA baseline, Sv39/Sv48, 16550 console and PLIC.
  CPU IDs/context numbers and timer frequency come from DTB; timer/IPI/HSM use
  SBI, so Sstc is not required. AIA/IMSIC/APLIC need separate drivers.
- x86_64: PVH plus minimal ACPI RSDT/XSDT/MADT discovery, xAPIC and one I/O APIC
  covering GSI zero, legacy ISA interrupt overrides, 16550 console via SPCR or
  BIOS data area, and PIT-based TSC/APIC timer calibration. No x2APIC, complete
  ACPI AML subsystem, UEFI loader, or general PCI discovery is provided. The
  current secondary trampoline reserves physical address `0x8000`.
- x86_64 enables baseline x87/FXSR/SSE2 on every CPU and eagerly saves/restores
  the 512-byte legacy state at context switches. Fork snapshots live user state;
  exec installs default state. AVX/XSAVE, complete extended-state signal frames
  and hardware acceptance remain open. CPU setup follows the
  [Intel system programming contract](https://cdrdv2-public.intel.com/812386/253668-sdm-vol-3a.pdf).
- Freestanding ELF programs currently enter `_start(argc, argv)` using the ISA's
  C function ABI, not Linux's initial process-stack ABI. x86 uses a zero return
  slot below argv so entry RSP is 8 modulo 16, as required by the
  [SysV function-call convention](https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/master/x86-64-ABI/low-level-sys-info.tex).
  The shared userspace linker script page-separates RX, R and RW sections,
  including x86 large-model and RISC-V small-data sections. The kernel ELF
  loader's general overlap/permission validation is still incomplete.
- No physical board has been accepted by this change. QEMU `raspi4b` results are
  not proof of real Raspberry Pi firmware/device behavior. Early failures may
  need a debugger if firmware did not describe a usable console.

The runtime resource model, supported boot adapters and reusable device drivers
are the implemented separation; an empty board factory or universal HAL cannot
provide the missing hardware capabilities.
