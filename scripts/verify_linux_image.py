#!/usr/bin/env python3
"""Verify ARM64 Linux-compatible Image header in a flat binary.

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

app = typer.Typer(help="Verify ARM64 Linux-compatible Image header")

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
    input_bin: Annotated[Path, typer.Argument(help="Flat binary to verify (moss.bin)")],
) -> None:
    """Verify ARM64 Linux Image header in a flat binary.

    Checks that the ARM64 magic ("ARM\\x64") is present at offset 0x38,
    and prints header field details.  Exits with code 1 on failure.
    """
    if not input_bin.exists():
        rprint(f"[red]Error: binary not found: {input_bin}[/red]")
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

    rprint(f"[green]ARM64 Linux Image header verified:[/green] {input_bin}")
    rprint(f"  code0:       0x{code0:08x} ({'branch' if (code0 >> 26) == 5 else 'other'})")
    rprint(f"  text_offset: 0x{text_offset:x}")
    rprint(f"  image_size:  {image_size:,} bytes ({image_size / 1024:.1f} KB)")
    rprint(f"  file_size:   {len(payload):,} bytes ({len(payload) / 1024:.1f} KB)")
    rprint(f"  magic:       0x{ARM64_IMAGE_MAGIC:08x} (ARM\\x64)")


if __name__ == "__main__":
    app()
