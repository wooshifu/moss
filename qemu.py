#!/usr/bin/env python3
"""Run an existing kernel image. All emulator policy lives here, not in CMake."""

import os
import shlex
import shutil
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path
from typing import Annotated

import typer

from scripts.artifacts import Artifacts

ARCH_CONFIG = {
    "ARM64": {"qemu_system": "qemu-system-aarch64", "machine": "virt", "cpu": "cortex-a72"},
    "X64": {"qemu_system": "qemu-system-x86_64", "machine": "q35", "cpu": "qemu64"},
    "RISCV64": {"qemu_system": "qemu-system-riscv64", "machine": "virt", "cpu": "rv64"},
}


# Validation fork/exec can grow each guest's default cache to 1 GiB of host RAM.
TCG_CACHE_MIB = 64

# QEMU's raspi.c loader exposes at most the lower 1 GiB minus 64 MiB VideoCore RAM.
RASPI_CONFIG = {
    "raspi3ap": {"cpu": "cortex-a53", "memory_mib": 512, "firmware_ram_mib": 448},
    "raspi3b": {"cpu": "cortex-a53", "memory_mib": 1024, "firmware_ram_mib": 960},
    "raspi4b": {"cpu": "cortex-a72", "memory_mib": 2048, "firmware_ram_mib": 960},
}


@contextmanager
def windows_utf8_console():
    """Let QEMU's UTF-8 serial bytes render correctly in a Windows console."""
    if sys.platform != "win32":
        yield
        return

    import ctypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    original_input = kernel32.GetConsoleCP()
    original_output = kernel32.GetConsoleOutputCP()
    changed_input = changed_output = False
    try:
        if original_input and original_input != 65001:
            if not kernel32.SetConsoleCP(65001):
                raise OSError(ctypes.get_last_error(), "failed to select UTF-8 console input")
            changed_input = True
        if original_output and original_output != 65001:
            if not kernel32.SetConsoleOutputCP(65001):
                raise OSError(ctypes.get_last_error(), "failed to select UTF-8 console output")
            changed_output = True
        yield
    finally:
        if changed_output:
            kernel32.SetConsoleOutputCP(original_output)
        if changed_input:
            kernel32.SetConsoleCP(original_input)


def resolve_resources(arch: str, machine: str | None, cpu: str | None, memory_mib: int | None) -> tuple[str, int]:
    profile = RASPI_CONFIG.get((machine or "").split(",")[0], {})
    return cpu or profile.get("cpu", ARCH_CONFIG[arch]["cpu"]), (
        memory_mib if memory_mib is not None else profile.get("memory_mib", 2048)
    )


def resolve_dtb(arch: str, machine: str | None, dtb: Path | None) -> Path | None:
    if machine and machine.split(",")[0] in RASPI_CONFIG:
        model = machine.split(",")[0]
        if arch != "ARM64":
            raise ValueError(f"{model} requires an ARM64 image")
        # Unlike virt, these machines need an external DTB; bundling avoids a host dtc dependency.
        return dtb if dtb is not None else Path(__file__).resolve().parent / "scripts/qemu" / f"{model}.dtb"
    return dtb


def resolve_machine(arch: str, *, smp: int, machine: str | None = None) -> str:
    selected = machine or ARCH_CONFIG[arch]["machine"]
    if arch == "ARM64" and selected.split(",")[0] == "virt" and "gic-version=" not in selected:
        selected += f",gic-version={3 if smp > 8 else 2}"
    return selected


def resolve_qemu(arch: str, executable: str | None = None) -> str:
    name = executable or ARCH_CONFIG[arch]["qemu_system"]
    found = shutil.which(name)
    if not found:
        raise ValueError(f"QEMU executable not found: {name}; install it or pass --qemu")
    return found


def get_qemu_version(qemu_path: str) -> str:
    result = subprocess.run([qemu_path, "--version"], capture_output=True, text=True, check=True, timeout=10)
    return result.stdout.splitlines()[0]


def build_qemu_args(
    artifacts: Artifacts,
    *,
    smp: int = 4,
    memory_mib: int | None = None,
    machine: str | None = None,
    cpu: str | None = None,
    qemu: str | None = None,
    dtb: Path | None = None,
    debug_mode: bool = False,
    validation: bool = False,
    extra_args: list[str] | None = None,
) -> list[str]:
    """Construct an invocation without inspecting CMake files or modifying images."""
    cpu, memory_mib = resolve_resources(artifacts.arch, machine, cpu, memory_mib)
    if not 1 <= smp <= 16:
        raise ValueError("MOSS currently supports 1..16 CPUs")
    if memory_mib < 256:
        raise ValueError("at least 256 MiB RAM is required")
    selected = resolve_machine(artifacts.arch, smp=smp, machine=machine)
    dtb = resolve_dtb(artifacts.arch, selected, dtb)
    model = selected.split(",")[0]
    if model in RASPI_CONFIG and (smp != 4 or memory_mib != RASPI_CONFIG[model]["memory_mib"]):
        raise ValueError(f"{model} requires 4 CPUs and {RASPI_CONFIG[model]['memory_mib']} MiB")
    image = artifacts.require("validation_kernel" if validation else "kernel")
    args = [
        qemu or ARCH_CONFIG[artifacts.arch]["qemu_system"],
        "-nodefaults",
        "-nographic",
        "-chardev",
        "stdio,id=char0,signal=off" if validation else "stdio,id=char0,mux=on,signal=off",
        "-serial",
        "chardev:char0",
        "-machine",
        selected,
        "-cpu",
        cpu,
        "-smp",
        str(smp),
        "-m",
        f"{memory_mib}M",
        "-accel",
        f"tcg,tb-size={TCG_CACHE_MIB}" if validation else "tcg",
        "-kernel",
        str(image),
        "-no-reboot",
    ]
    if not validation:
        # monitor-hmp supersedes QEMU's deprecated -mon syntax while preserving
        # the interactive HMP console on the multiplexed serial terminal.
        args += ["-object", "monitor-hmp,id=mon0,chardev=char0,readline=on"]
    if artifacts.arch == "RISCV64":
        args += ["-bios", "default"]
    initrd = "validation_initramfs" if validation else "initramfs"
    if artifacts.files[initrd] is not None:
        args += ["-initrd", str(artifacts.require(initrd))]
    if dtb is not None:
        if not dtb.is_file():
            raise ValueError(f"DTB does not exist: {dtb}")
        args += ["-dtb", str(dtb.resolve())]
    # Sparse buddy-list writes can stall on host THP allocation/compaction.
    # Require room for all guest RAM: exhausting a tmpfs backing can SIGBUS QEMU.
    if sys.platform == "linux" and not any(
        arg.startswith("-mem-path") or "memory-backend" in arg or "memdev=" in arg
        for arg in [selected, *(extra_args or [])]
    ):
        shm_path = Path("/dev/shm")
        try:
            if (
                shm_path.is_dir()
                and os.access(shm_path, os.W_OK | os.X_OK)
                and shutil.disk_usage(shm_path).free >= memory_mib * 1024**2
            ):
                args += ["-mem-path", str(shm_path)]
        except OSError:
            pass
    if debug_mode:
        args += ["-s", "-S"]
    return args + (extra_args or [])


app = typer.Typer(context_settings={"allow_extra_args": True})


@app.command()
def main(
    ctx: typer.Context,
    manifest: Annotated[Path, typer.Option(help="moss-artifacts.json")],
    machine: str | None = None,
    cpu: str | None = None,
    qemu: str | None = None,
    dtb: Path | None = None,
    smp: int = 4,
    memory_mib: int | None = None,
    debug: bool = False,
    timeout: float | None = None,
    dry_run: bool = False,
) -> None:
    """Boot the manifest's image, including during GDB debugging. Extra arguments follow --."""
    try:
        artifacts = Artifacts.load(manifest)
        args = build_qemu_args(
            artifacts,
            smp=smp,
            memory_mib=memory_mib,
            machine=machine,
            cpu=cpu,
            qemu=resolve_qemu(artifacts.arch, qemu),
            dtb=dtb,
            debug_mode=debug,
            extra_args=ctx.args,
        )
        typer.echo(shlex.join(args))
        if debug:
            typer.echo(f"Symbols: {artifacts.require('debug_symbols')}; target remote localhost:1234")
            if artifacts.arch in ("ARM64", "RISCV64"):
                typer.echo("PIE symbols need the firmware-selected Image load offset; see docs/generic-boot.md.")
        if dry_run:
            return
        with windows_utf8_console():
            result = subprocess.run(args, check=False, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise typer.Exit(124) from None
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise typer.BadParameter(str(error)) from error
    raise typer.Exit(result.returncode)


if __name__ == "__main__":
    app()
