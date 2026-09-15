#!/usr/bin/env python3
"""Run an existing kernel image. All emulator policy lives here, not in CMake."""

import shlex
import shutil
import subprocess
from pathlib import Path
from typing import Annotated

import typer

from scripts.artifacts import Artifacts

ARCH_CONFIG = {
    "ARM64": {"qemu_system": "qemu-system-aarch64", "machine": "virt", "cpu": "cortex-a72"},
    "X64": {"qemu_system": "qemu-system-x86_64", "machine": "q35", "cpu": "qemu64"},
    "RISCV64": {"qemu_system": "qemu-system-riscv64", "machine": "virt", "cpu": "rv64"},
}


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
    memory_mib: int = 2048,
    machine: str | None = None,
    cpu: str | None = None,
    qemu: str | None = None,
    dtb: Path | None = None,
    debug_mode: bool = False,
    validation: bool = False,
    extra_args: list[str] | None = None,
) -> list[str]:
    """Construct an invocation without inspecting CMake files or modifying images."""
    if not 1 <= smp <= 16:
        raise ValueError("MOSS currently supports 1..16 CPUs")
    if memory_mib < 256:
        raise ValueError("at least 256 MiB RAM is required")
    selected = resolve_machine(artifacts.arch, smp=smp, machine=machine)
    if selected.split(",")[0] == "raspi4b" and (dtb is None or smp != 4 or memory_mib != 2048):
        raise ValueError("raspi4b requires --dtb, 4 CPUs and 2048 MiB")
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
        cpu or ARCH_CONFIG[artifacts.arch]["cpu"],
        "-smp",
        str(smp),
        "-m",
        f"{memory_mib}M",
        "-accel",
        "tcg",
        "-kernel",
        str(image),
        "-no-reboot",
    ]
    if not validation:
        args += ["-mon", "chardev=char0,mode=readline"]
    if artifacts.arch == "RISCV64":
        args += ["-bios", "default"]
    initrd = "validation_initramfs" if validation else "initramfs"
    if artifacts.files[initrd] is not None:
        args += ["-initrd", str(artifacts.require(initrd))]
    if dtb is not None:
        if not dtb.is_file():
            raise ValueError(f"DTB does not exist: {dtb}")
        args += ["-dtb", str(dtb.resolve())]
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
    memory_mib: int = 2048,
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
        result = subprocess.run(args, check=False, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise typer.Exit(124) from None
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise typer.BadParameter(str(error)) from error
    raise typer.Exit(result.returncode)


if __name__ == "__main__":
    app()
