#!/usr/bin/env python3
"""Verify and copy ARM64 Linux-compatible kernel Image.

The ARM64 Linux Image header is embedded directly in the boot assembly
(start_arm64.S), so the flat binary produced by `objcopy -O binary` already
contains the correct header at offset 0. This script verifies the header
and copies the binary to the output path.

Usage:
    uv run scripts/create_linux_image.py moss.bin moss.img
"""

import struct
from pathlib import Path
from typing import Annotated

import typer
from rich import print as rprint

app = typer.Typer(help="Verify and package ARM64 Linux-compatible kernel Image")

# ARM64 Linux Image header constants
ARM64_IMAGE_MAGIC = 0x644D5241  # "ARM\x64" little-endian
HEADER_SIZE = 64


def verify_arm64_header(data: bytes) -> bool:
    """Verify the ARM64 Linux Image header at the start of the binary."""
    if len(data) < HEADER_SIZE:
        return False

    # Check ARM64 magic at offset 0x38
    magic = struct.unpack_from("<I", data, 0x38)[0]
    return magic == ARM64_IMAGE_MAGIC


@app.callback(invoke_without_command=True)
def main(
    input_bin: Annotated[Path, typer.Argument(help="Input flat binary (moss.bin)")],
    output_img: Annotated[Path, typer.Argument(help="Output Linux Image (moss.img)")],
) -> None:
    """Verify ARM64 Linux Image header and produce final image.

    The header is embedded in the assembly source (start_arm64.S), so the
    flat binary already contains it. This script verifies correctness and
    copies the binary to the output path.
    """
    if not input_bin.exists():
        rprint(f"[red]Error: input binary not found: {input_bin}[/red]")
        raise typer.Exit(1)

    payload = input_bin.read_bytes()

    if not verify_arm64_header(payload):
        rprint("[red]Error: ARM64 Linux Image header not found in binary[/red]")
        rprint("Expected ARM64 magic (0x644d5241) at offset 0x38")
        rprint("Make sure start_arm64.S contains the Linux Image header")
        raise typer.Exit(1)

    # Read header fields for display
    code0 = struct.unpack_from("<I", payload, 0x00)[0]
    text_offset = struct.unpack_from("<Q", payload, 0x08)[0]
    image_size = struct.unpack_from("<Q", payload, 0x10)[0]

    output_img.write_bytes(payload)

    rprint(f"[green]ARM64 Linux Image verified and written:[/green] {output_img}")
    rprint(f"  code0:       0x{code0:08x} ({'branch' if (code0 >> 26) == 5 else 'other'})")
    rprint(f"  text_offset: 0x{text_offset:x}")
    rprint(f"  image_size:  {image_size:,} bytes ({image_size / 1024:.1f} KB)")
    rprint(f"  file_size:   {len(payload):,} bytes ({len(payload) / 1024:.1f} KB)")
    rprint(f"  magic:       0x{ARM64_IMAGE_MAGIC:08x} (ARM\\x64)")


if __name__ == "__main__":
    app()
