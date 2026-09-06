#!/usr/bin/env python3
"""Verify ARM64/RV64 Linux-compatible Image headers and PIE relocations in a flat binary.

The ARM64 Linux Image header is embedded directly in the boot assembly
(start_arm64.S), so the flat binary produced by `objcopy -O binary` already
contains the correct header at offset 0.  This script verifies the header
is intact — it does NOT produce a separate output file.

Usage:
    uv run scripts/verify_linux_image.py moss.bin
"""

import struct
from pathlib import Path
from typing import Annotated

import typer
from rich import print as rprint

app = typer.Typer(help="Verify ARM64/RV64 Linux-compatible Image headers and PIE relocations")

# ARM64 Linux Image header constants
ARM64_IMAGE_MAGIC = 0x644D5241  # "ARM\x64" little-endian
HEADER_SIZE = 64


def verify_arm64_header(data: bytes) -> bool:
    """Verify the ARM64 Linux Image header at the start of the binary."""
    if len(data) < HEADER_SIZE:
        return False

    # Check ARM64 magic at offset 0x38
    magic = struct.unpack_from("<I", data, 0x38)[0]
    code0 = struct.unpack_from("<I", data, 0)[0]
    image_size, flags = struct.unpack_from("<QQ", data, 16)
    return (
        magic == ARM64_IMAGE_MAGIC and code0 >> 26 == 5 and len(data) <= image_size <= 256 * 1024 * 1024 and flags == 10
    )


def verify_riscv_header(data: bytes) -> bool:
    if len(data) < HEADER_SIZE:
        return False
    offset, size, flags, version = struct.unpack_from("<QQQI", data, 8)
    return (
        struct.unpack_from("<I", data, 56)[0] == 0x05435352
        and struct.unpack_from("<I", data, 0)[0] & 0x7F == 0x6F
        and offset == 0x200000
        and len(data) <= size <= 256 * 1024 * 1024
        and flags == 0
        and version == 2
    )


def verify_relocations(elf: bytes, image_size: int, machine: int) -> None:
    """Reject relocations that the tiny in-kernel bootstrap cannot apply."""
    if elf[:6] != b"\x7fELF\x02\x01":
        raise ValueError("expected little-endian ELF64")
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", elf)
    if header[1:3] != (3, machine) or header[11] != 64:
        raise ValueError("expected a PIE for the image architecture")
    count = 0
    for index in range(header[12]):
        section = struct.unpack_from("<IIQQQQIIQQ", elf, header[6] + index * 64)
        if not section[2] & 2 or section[1] != 4:  # Allocated SHT_RELA.
            continue
        if section[9] != 24 or section[5] % 24 or section[4] + section[5] > len(elf):
            raise ValueError("invalid relocation section")
        for offset in range(section[4], section[4] + section[5], 24):
            target, kind, addend = struct.unpack_from("<QQq", elf, offset)
            if kind != (1027 if machine == 183 else 3) or not 64 <= target <= image_size - 8:
                raise ValueError("unsupported relocation or relocation into the static image header")
            if not 0 <= addend <= image_size:
                raise ValueError("relative relocation outside the image")
            count += 1
    if not count:
        raise ValueError("PIE relocation table is missing")


@app.command()
def main(
    input_bin: Annotated[Path, typer.Argument(help="Flat binary to verify (moss.bin)")],
    elf: Annotated[Path | None, typer.Option(help="Also validate the boot relocation contract")] = None,
) -> None:
    """Verify a native ARM64/RV64 Linux Image and its bootstrap relocations.

    Checks that the ARM64 magic ("ARM\\x64") is present at offset 0x38,
    and prints header field details.  Exits with code 1 on failure.
    """
    if not input_bin.exists():
        rprint(f"[red]Error: binary not found: {input_bin}[/red]")
        raise typer.Exit(1)

    payload = input_bin.read_bytes()

    arm64 = verify_arm64_header(payload)
    if not arm64 and not verify_riscv_header(payload):
        rprint("[red]Error: invalid ARM64/RV64 Linux Image header or static image size[/red]")
        raise typer.Exit(1)

    # Read header fields for display
    code0 = struct.unpack_from("<I", payload, 0x00)[0]
    text_offset = struct.unpack_from("<Q", payload, 0x08)[0]
    image_size = struct.unpack_from("<Q", payload, 0x10)[0]
    if elf is not None:
        try:
            verify_relocations(elf.read_bytes(), image_size, 183 if arm64 else 243)
        except (OSError, ValueError, struct.error) as error:
            rprint(f"[red]Error: {error}[/red]")
            raise typer.Exit(1) from error

    rprint(f"[green]{'ARM64' if arm64 else 'RV64'} Linux Image verified:[/green] {input_bin}")
    rprint(f"  code0:       0x{code0:08x} (entry branch)")
    rprint(f"  text_offset: 0x{text_offset:x}")
    rprint(f"  image_size:  {image_size:,} bytes ({image_size / 1024:.1f} KB)")
    rprint(f"  file_size:   {len(payload):,} bytes ({len(payload) / 1024:.1f} KB)")


if __name__ == "__main__":
    app()
