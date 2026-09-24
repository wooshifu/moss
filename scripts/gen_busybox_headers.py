"""Generate the BusyBox headers needed by the native CMake build.

The upstream BusyBox generators are POSIX shell scripts.  This small host-side
replacement keeps the CMake build independent of a shell and the usual Unix
text utilities, which are not available on a standard Windows installation.
"""

from __future__ import annotations

import argparse
import bz2
import subprocess
from collections.abc import Iterable
from pathlib import Path


def _source_files(source_dir: Path) -> list[Path]:
    """Match the one- and two-directory-deep globs used by BusyBox."""
    files = [*source_dir.glob("*/*.c"), *source_dir.glob("*/*/*.c")]
    return sorted(files, key=lambda path: path.as_posix())


def _extract_lines(files: Iterable[Path], prefix: str) -> Iterable[str]:
    for source in files:
        # BusyBox still contains a few source comments in legacy single-byte
        # encodings.  Latin-1 gives us a lossless byte-to-text mapping, just
        # like the sed-based upstream generator.
        for line in source.read_text(encoding="latin-1").splitlines():
            if line.startswith(prefix):
                yield line[len(prefix) :]


def _render_template(template: Path, header: str, inserted: Iterable[str]) -> bytes:
    lines = template.read_text(encoding="latin-1").splitlines()
    try:
        marker = lines.index("INSERT")
    except ValueError as error:
        raise ValueError(f"{template} does not contain an INSERT marker") from error

    rendered = [header, *lines[:marker], *inserted, *lines[marker + 1 :]]
    return ("\n".join(rendered) + "\n").encode("latin-1")


def _write_if_changed(target: Path, content: bytes) -> None:
    if target.exists() and target.read_bytes() == content:
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(f"{target.name}.tmp")
    temporary.write_bytes(content)
    temporary.replace(target)
    print(f"  GEN     {target}")


def generate(source_dir: Path, binary_dir: Path) -> None:
    sources = _source_files(source_dir)

    applets = list(_extract_lines(sources, "//applet:"))
    embed_dir = source_dir / "embed"
    if embed_dir.is_dir():
        for script in sorted(embed_dir.iterdir(), key=lambda path: path.name):
            applets.append(
                "IF_FEATURE_SH_EMBEDDED_SCRIPTS("
                f"APPLET_SCRIPTED({script.name}, scripted, BB_DIR_USR_BIN, BB_SUID_DROP, scripted))"
            )

    applets_header = _render_template(
        source_dir / "include" / "applets.src.h",
        "/* DO NOT EDIT. This file is generated from applets.src.h */",
        applets,
    )
    _write_if_changed(binary_dir / "include" / "applets.h", applets_header)

    usage: list[str] = []
    for line in _extract_lines(sources, "//usage:"):
        if not line.startswith((" ", "\t")):
            usage.append("")
        usage.append(f"{line} \\")

    usage_header = _render_template(
        source_dir / "include" / "usage.src.h",
        "/* DO NOT EDIT. This file is generated from usage.src.h */",
        usage,
    )
    _write_if_changed(binary_dir / "include" / "usage.h", usage_header)


def _octal_chunks(data: bytes, *, prefix: str, separator: str) -> Iterable[str]:
    for offset in range(0, len(data), 16):
        yield separator.join(f"{prefix}{byte:03o}" for byte in data[offset : offset + 16])


def compress_usage(usage_executable: Path, output: Path) -> None:
    usage = subprocess.run([usage_executable], check=True, stdout=subprocess.PIPE).stdout
    compressed = bz2.compress(usage, compresslevel=1)
    if not compressed.startswith(b"BZ"):
        raise RuntimeError("Python's bzip2 output does not have the expected BZ header")

    lines = ['#define UNPACKED_USAGE "" \\']
    lines.extend(f'"{chunk}" \\' for chunk in _octal_chunks(usage, prefix="\\", separator=""))
    lines.extend(("", f"#define UNPACKED_USAGE_LENGTH {len(usage)}", "", "#define PACKED_USAGE \\"))
    lines.extend(f"{chunk}, \\" for chunk in _octal_chunks(compressed[2:], prefix="0", separator=","))
    lines.append("")
    _write_if_changed(output, ("\n".join(lines) + "\n").encode())


def main() -> None:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    generate_parser = subparsers.add_parser("generate")
    generate_parser.add_argument("source_dir", type=Path)
    generate_parser.add_argument("binary_dir", type=Path)

    compress_parser = subparsers.add_parser("compress-usage")
    compress_parser.add_argument("usage_executable", type=Path)
    compress_parser.add_argument("output", type=Path)

    args = parser.parse_args()
    if args.command == "generate":
        generate(args.source_dir, args.binary_dir)
    else:
        compress_usage(args.usage_executable, args.output)


if __name__ == "__main__":
    main()
