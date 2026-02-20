#!/usr/bin/env python3
"""Generate initramfs.cpio from a list of files.

Creates a CPIO archive in GNU newc format (070701 magic), which is the
format expected by the MOSS initramfs CPIO parser.

Pure Python implementation — no dependency on external `cpio` command,
works on Linux, macOS, and Windows.

Usage:
    uv run scripts/gen_initramfs.py OUTPUT_PATH SOURCE_DIR FILE [FILE ...]
"""

import sys
from pathlib import Path


def cpio_newc_header(
    *,
    ino: int = 0,
    mode: int = 0,
    uid: int = 0,
    gid: int = 0,
    nlink: int = 1,
    mtime: int = 0,
    filesize: int = 0,
    devmajor: int = 0,
    devminor: int = 0,
    rdevmajor: int = 0,
    rdevminor: int = 0,
    namesize: int = 0,
    check: int = 0,
) -> bytes:
    """Build a 110-byte CPIO newc header (all fields are 8-char uppercase hex)."""
    return (
        b"070701"
        + f"{ino:08X}".encode()
        + f"{mode:08X}".encode()
        + f"{uid:08X}".encode()
        + f"{gid:08X}".encode()
        + f"{nlink:08X}".encode()
        + f"{mtime:08X}".encode()
        + f"{filesize:08X}".encode()
        + f"{devmajor:08X}".encode()
        + f"{devminor:08X}".encode()
        + f"{rdevmajor:08X}".encode()
        + f"{rdevminor:08X}".encode()
        + f"{namesize:08X}".encode()
        + f"{check:08X}".encode()
    )


def align4(n: int) -> int:
    """Round up to the next 4-byte boundary."""
    return (n + 3) & ~3


def make_cpio_entry(filename: str, data: bytes, *, ino: int, mode: int = 0o100755) -> bytes:
    """Create a single CPIO newc entry (header + name + padding + data + padding)."""
    name_bytes = filename.encode("utf-8") + b"\x00"  # NUL-terminated
    namesize = len(name_bytes)

    hdr = cpio_newc_header(
        ino=ino,
        mode=mode,
        nlink=1,
        filesize=len(data),
        namesize=namesize,
    )

    # Pad after header + name to 4-byte boundary
    header_plus_name = len(hdr) + namesize
    name_padding = align4(header_plus_name) - header_plus_name

    # Pad after data to 4-byte boundary
    data_padding = align4(len(data)) - len(data)

    return hdr + name_bytes + b"\x00" * name_padding + data + b"\x00" * data_padding


def make_cpio_trailer() -> bytes:
    """Create the TRAILER!!! entry that marks end of archive."""
    name = b"TRAILER!!!\x00"
    hdr = cpio_newc_header(namesize=len(name), nlink=1)

    header_plus_name = len(hdr) + len(name)
    padding = align4(header_plus_name) - header_plus_name

    return hdr + name + b"\x00" * padding


def generate_cpio(source_dir: Path, filenames: list[str]) -> bytes:
    """Generate a complete CPIO newc archive from the given files."""
    parts: list[bytes] = []

    for ino, filename in enumerate(filenames, start=1):
        filepath = source_dir / filename
        data = filepath.read_bytes()

        # Regular file with rwxr-xr-x permissions
        parts.append(make_cpio_entry(filename, data, ino=ino, mode=0o100755))

    parts.append(make_cpio_trailer())

    # Pad archive to 512-byte boundary (block alignment, matches GNU cpio behavior)
    archive = b"".join(parts)
    block_padding = align4(len(archive)) - len(archive)

    return archive + b"\x00" * block_padding


def main() -> None:
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} OUTPUT SOURCE_DIR FILE [FILE ...]", file=sys.stderr)
        sys.exit(1)

    output = Path(sys.argv[1])
    source_dir = Path(sys.argv[2])
    files = sys.argv[3:]

    # Verify source directory exists
    if not source_dir.is_dir():
        print(f"Error: source directory does not exist: {source_dir}", file=sys.stderr)
        sys.exit(1)

    # Verify all input files exist
    for f in files:
        if not (source_dir / f).exists():
            print(f"Error: file not found: {source_dir / f}", file=sys.stderr)
            sys.exit(1)

    archive = generate_cpio(source_dir, files)

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(archive)
    print(f"Generated {output} ({output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
