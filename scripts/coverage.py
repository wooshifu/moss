#!/usr/bin/env python3
"""Aggregate Moss kernel/userspace coverage arenas into LLVM reports."""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.artifacts import Artifacts

RECORD_MAGIC = 0x4D435652
RECORD_VERSION = 1
RECORD_HEADER = struct.Struct("<IIIIQ32s")
RECORD_HEADER_SIZE = 56
RAW_HEADER_SIZE = 152
RAW_HEADER_QWORDS = 19
RAW_MAGIC = 0xFF6C70726F667281
RAW_VERSION = 11
RAW_VALUE_KIND_LAST = 2
IGNORE_FILENAME = r"third_party|src/test|fdt/libfdt|drivers/src/drivers\.cppm"


class CoverageError(ValueError):
    pass


@dataclass(frozen=True)
class Record:
    program_id: str
    counter_count: int
    raw: bytes


def parse_arena(path: Path) -> list[Record]:
    data = path.read_bytes()
    records = []
    offset = 0
    while offset < len(data):
        if len(data) - offset < RECORD_HEADER_SIZE:
            raise CoverageError(f"{path}: truncated record header at {offset}")
        magic, version, count, program_id_length, raw_size, program_id = RECORD_HEADER.unpack_from(data, offset)
        if magic != RECORD_MAGIC or version != RECORD_VERSION:
            raise CoverageError(f"{path}: invalid record header at {offset}")
        if program_id_length == 0 or program_id_length >= len(program_id) or b"\0" in program_id[:program_id_length]:
            raise CoverageError(f"{path}: invalid program id at {offset}")
        if raw_size < RAW_HEADER_SIZE or (raw_size - RAW_HEADER_SIZE) % 8 or count != (raw_size - RAW_HEADER_SIZE) // 8:
            raise CoverageError(f"{path}: inconsistent counter count at {offset}")
        end = offset + RECORD_HEADER_SIZE + raw_size
        if end > len(data):
            raise CoverageError(f"{path}: truncated raw profile at {offset}")
        raw = data[offset + RECORD_HEADER_SIZE : end]
        header = struct.unpack_from("<19Q", raw)
        if (
            header[0] != RAW_MAGIC
            or header[1] != RAW_VERSION
            or header[5] != count
            or header[18] != RAW_VALUE_KIND_LAST
        ):
            raise CoverageError(f"{path}: invalid raw profile header at {offset}")
        records.append(Record(program_id[:program_id_length].decode("ascii"), count, raw))
        offset = end
    return records


def collect_arenas(build_dir: Path, since: float | None = None) -> list[Path]:
    arenas = sorted((build_dir / "validation").glob("**/coverage/coverage.arena"))
    if since is not None:
        # Drop arenas produced before the current ELFs were linked; otherwise a
        # rebuilt program can leave records that its new binary cannot correlate.
        arenas = [path for path in arenas if path.stat().st_mtime >= since]
    if not arenas:
        raise CoverageError(f"no coverage arenas found under {build_dir / 'validation'}")
    return arenas


def check_capture_health(arena: Path) -> None:
    errors = arena.with_name("coverage.errors")
    if not errors.is_file():
        raise CoverageError(f"missing coverage errors file for {arena}")
    value = struct.unpack("<Q", errors.read_bytes())[0]
    if value:
        raise CoverageError(f"{arena}: runtime reported {value} coverage errors")
    used = arena.with_name("coverage.used")
    if used.is_file() and struct.unpack("<Q", used.read_bytes())[0] != arena.stat().st_size:
        raise CoverageError(f"{arena}: arena size does not match the captured usage")


def program_elfs(build_dir: Path, artifacts: Artifacts) -> dict[str, Path]:
    elfs = {"kernel": artifacts.require("validation_debug_symbols")}
    for path in (build_dir / "src/userspace/mlibc").glob("*.unstripped.elf"):
        elfs[path.name.removesuffix(".unstripped.elf")] = path
    return elfs


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise CoverageError(f"{name} is required")
    return path


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise CoverageError(f"{' '.join(command)} failed: {detail}")
    return result


def totals(export: str) -> dict[str, Any]:
    data = json.loads(export).get("data")
    if not isinstance(data, list) or not data or not isinstance(data[0], dict):
        raise CoverageError("llvm-cov export returned no coverage data")
    value = data[0].get("totals")
    if not isinstance(value, dict):
        raise CoverageError("llvm-cov export returned no totals")
    return value


def generate(build_dir: Path, output: Path) -> dict[str, Any]:
    artifacts = Artifacts.load(build_dir / "moss-artifacts.json")
    if not artifacts.build.get("coverage"):
        raise CoverageError("the artifact manifest is not a coverage build")
    coverage_elfs = program_elfs(build_dir, artifacts)
    cutoff = max(path.stat().st_mtime for path in coverage_elfs.values())
    arenas = collect_arenas(build_dir, cutoff)
    records: dict[str, list[Record]] = {}
    for arena in arenas:
        check_capture_health(arena)
        for record in parse_arena(arena):
            records.setdefault(record.program_id, []).append(record)
    if not records:
        raise CoverageError("coverage arenas contained no records")

    elfs = coverage_elfs
    missing = sorted(records.keys() - elfs.keys())
    if missing:
        raise CoverageError(f"no unstripped ELF for coverage programs: {', '.join(missing)}")

    profdata_tool = require_tool("llvm-profdata")
    cov_tool = require_tool("llvm-cov")
    output.mkdir(parents=True, exist_ok=True)
    for stale in ("html", "profdata", "raw"):
        shutil.rmtree(output / stale, ignore_errors=True)
    for stale in ("summary.txt", "summary.json"):
        (output / stale).unlink(missing_ok=True)
    summary: dict[str, Any] = {"schema_version": 1, "arenas": len(arenas), "programs": {}}
    report_text = []
    for program_id in sorted(records):
        program_records = records[program_id]
        counts = {record.counter_count for record in program_records}
        if len(counts) != 1:
            raise CoverageError(f"{program_id}: records disagree on counter count")
        elf = elfs[program_id]
        raw_dir = output / "raw" / program_id
        raw_dir.mkdir(parents=True, exist_ok=True)
        raws = []
        for index, record in enumerate(program_records):
            path = raw_dir / f"{index:04d}.profraw"
            path.write_bytes(record.raw)
            raws.append(path)
        profdata = output / "profdata" / f"{program_id}.profdata"
        profdata.parent.mkdir(parents=True, exist_ok=True)
        run([profdata_tool, "merge", f"--binary-file={elf}", f"--output={profdata}", *map(str, raws)])
        report = run(
            [
                cov_tool,
                "report",
                str(elf),
                f"-instr-profile={profdata}",
                f"-ignore-filename-regex={IGNORE_FILENAME}",
            ]
        ).stdout
        report_text.append(f"=== {program_id} ===\n{report}")
        html_dir = output / "html" / program_id
        run(
            [
                cov_tool,
                "show",
                str(elf),
                f"-instr-profile={profdata}",
                "-format=html",
                f"-output-dir={html_dir}",
                f"-ignore-filename-regex={IGNORE_FILENAME}",
                "-show-line-counts-or-regions",
                "-show-branches=count",
            ]
        )
        exported = run(
            [
                cov_tool,
                "export",
                str(elf),
                f"-instr-profile={profdata}",
                "-summary-only",
                f"-ignore-filename-regex={IGNORE_FILENAME}",
            ]
        ).stdout
        summary["programs"][program_id] = {
            "elf": str(elf),
            "profdata": str(profdata),
            "records": len(program_records),
            "counters": program_records[0].counter_count,
            "totals": totals(exported),
        }
    (output / "summary.txt").write_text("\n".join(report_text))
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    build_dir = arguments.build_dir.resolve()
    try:
        summary = generate(build_dir, (arguments.output or build_dir / "coverage").resolve())
    except (
        CoverageError,
        OSError,
        subprocess.SubprocessError,
        struct.error,
        json.JSONDecodeError,
        UnicodeDecodeError,
    ) as error:
        print(f"coverage: {error}", file=sys.stderr)
        return 1
    for program_id, value in summary["programs"].items():
        lines = value["totals"].get("lines", {})
        branches = value["totals"].get("branches", {})
        print(
            f"{program_id}: lines {lines.get('covered', 0)}/{lines.get('count', 0)}, "
            f"branches {branches.get('covered', 0)}/{branches.get('count', 0)}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
