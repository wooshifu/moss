---
status: accepted
date: 2026-09-06
---

# Build Generic Kernels per Architecture and Discover Hardware at Boot

MOSS follows Linux's generic-kernel model: one source tree produces separate native kernels for ARM64, RV64 and x86_64. Within an architecture, the same kernel image must be reusable across machines whose instruction-set baseline, boot contract and devices it supports. A machine change must not require selecting a board-specific kernel build. This replaces the proposed `virt`/Raspberry Pi build matrix; a portable source tree with a different binary per board does not meet the requirement.

Machine resources come from firmware/bootloader information (DTB, ACPI, memory maps) and bus enumeration. Drivers implement device contracts and match device identities; their register layouts and architectural constants remain legitimate code. Machine-specific UART addresses, RAM assumptions and guessed interrupt-controller resources are not fallback configuration. Missing mandatory boot information is an explicit boot failure; unsupported optional devices remain unavailable. Hardware descriptions cannot replace drivers for previously unsupported hardware.

Architecture entry code converts supported boot protocols into runtime information used by the kernel. Image placement, relocation, early mappings and CPU startup must obey those protocols rather than a particular emulator's memory map. Board initialization needed before kernel entry belongs to firmware or a bootloader. Supporting a new boot protocol or device is independent of supporting a named board. The initial protocols are ARM64 Linux Image + DTB, RV64 supervisor entry + DTB/SBI, and x86_64 Xen PVH; x86_64 initially uses a minimal ACPI MADT/SPCR reader. This does not imply a complete ACPI subsystem, UEFI entry support or compatibility with every real machine.

CMake selects architecture, build mode and compiled capabilities, and produces kernel artifacts plus a versioned, relative-path `moss-artifacts.json`. The manifest describes architecture, boot protocol, build configuration and artifacts, never a machine identity or QEMU options. Configure/build must work without QEMU. An independent runner chooses emulator executable, machine, CPU, RAM, SMP and firmware at execution time. A real-machine bootloader consumes the same architecture image without the QEMU runner. Existing machine-named presets and generated QEMU wrappers/configuration are removed without compatibility aliases. CTest remains an explicit validation entry point; default build workflows only configure and build.

Kernel validation reports completion through the existing `@@MOSS` serial protocol. The host owns emulator process termination; a kernel test does not write emulator-specific exit devices or invoke semihosting to terminate the host process. Benchmark comparisons still require matching recorded execution environments as specified in ADR-0004.

Acceptance requires building with QEMU absent, successful regressions on all three architectures, and reuse of an unchanged image (verified by hash) with different supported hardware descriptions/memory layouts. Separate acceptance evidence is required for physical hardware. Progress on build tooling alone is not completion of generic boot support; unimplemented boot protocols, device capabilities and address-space limits must be documented explicitly.

Implementation and current acceptance limits are recorded in [Generic boot](../generic-boot.md).

## Basis and trade-off

Linux uses [runtime hardware descriptions](https://docs.kernel.org/devicetree/usage-model.html) and [device/driver matching](https://docs.kernel.org/driver-api/driver-model/binding.html). Its [ARM64](https://docs.kernel.org/arch/arm64/booting.html) and [RISC-V](https://docs.kernel.org/arch/riscv/boot.html) boot contracts separate firmware duties from kernel entry. QEMU's [virt documentation](https://www.qemu.org/docs/master/system/arm/virt.html) also directs guests to discover device resources from its DTB.

We accept runtime discovery and a larger compiled driver set in exchange for image reuse. Zephyr's [build-time devicetree model](https://docs.zephyrproject.org/latest/build/dts/intro-input-output.html) and per-board static configuration optimize different constraints and are not the model for MOSS image portability. No board registry, platform factory or board-specific linker-script matrix is introduced.
