#!/usr/bin/env python3
"""Aggregate Moss kernel/userspace coverage arenas into LLVM reports."""

from __future__ import annotations

import argparse
import json
import re
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
BASE_IGNORE_FILENAME = (r"third_party|src/test|src/fdt/libfdt")
CRITICAL_BRANCH_MODULES = {
    "kernel/src/syscall_table.cpp": 83.5,
    "process/src/process-scheduler.cppm": 83.5,
    "kernel/src/control_ipc.cpp": 71.0,
}


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



def load_ignore_regex(path: Path) -> str:
    """Build the llvm-cov filename filter from the explicit ignore manifest."""
    patterns = [BASE_IGNORE_FILENAME]
    if not path.is_file():
        return "|".join(patterns)
    for raw in path.read_text().splitlines():
        value = raw.split("#", 1)[0].strip()
        if not value:
            continue
        # Paths are repository-relative. llvm-cov matches the absolute
        # filename, so an unanchored escaped path is sufficient and keeps the
        # manifest portable across build directories.
        pattern = re.escape(value).replace(r"\*", ".*")
        patterns.append(pattern)
    # llvm-cov uses POSIX-style regex and does not understand non-capturing
    # groups; alternation alone is sufficient here.
    return "|".join(patterns)


def parse_report_files(report: str) -> dict[str, dict[str, Any]]:
    rows: dict[str, dict[str, Any]] = {}
    for line in report.splitlines():
        parts = line.split()
        if len(parts) != 13:
            continue
        if not all(parts[index].endswith("%") for index in (3, 6, 9, 12)):
            continue
        name = parts[0]
        rows[name] = {
            "regions": {"count": int(parts[1]), "missed": int(parts[2])},
            "functions": {"count": int(parts[4]), "missed": int(parts[5])},
            "lines": {"count": int(parts[7]), "missed": int(parts[8])},
            "branches": {"count": int(parts[10]), "missed": int(parts[11])},
        }
    return rows


def audit_coverage(
    summary: dict[str, Any],
    min_lines: float,
    min_branches: float,
    enforce: bool,
) -> None:
    errors: list[str] = []
    for program_id, program in summary["programs"].items():
        value = program["totals"]
        lines = value.get("lines", {})
        branches = value.get("branches", {})
        line_percent = float(lines.get("percent", 0.0))
        branch_percent = float(branches.get("percent", 0.0))
        program["files"] = {
            name: {
                **row,
                "lines": {
                    **row["lines"],
                    "percent": (row["lines"]["count"] - row["lines"]["missed"])
                    * 100.0
                    / row["lines"]["count"]
                    if row["lines"]["count"]
                    else 0.0,
                },
                "branches": {
                    **row["branches"],
                    "percent": (row["branches"]["count"] - row["branches"]["missed"])
                    * 100.0
                    / row["branches"]["count"]
                    if row["branches"]["count"]
                    else 0.0,
                },
            }
            for name, row in program.get("file_rows", {}).items()
        }
        program.pop("file_rows", None)
        if program_id == "kernel" and line_percent < min_lines:
            errors.append(f"{program_id} lines {line_percent:.2f}% < {min_lines:.2f}%")
        if program_id == "kernel" and branch_percent < min_branches:
            errors.append(f"{program_id} branches {branch_percent:.2f}% < {min_branches:.2f}%")
        if program_id == "kernel":
            for module, minimum in CRITICAL_BRANCH_MODULES.items():
                row = program["files"].get(module)
                if row is None:
                    errors.append(f"critical module missing from report: {module}")
                    continue
                actual = row["branches"]["percent"]
                if actual < minimum:
                    errors.append(f"critical module {module} branches {actual:.2f}% < {minimum:.2f}%")
    summary["gates"] = {
        "min_lines": min_lines,
        "min_branches": min_branches,
        "enforced": enforce,
        "errors": errors,
    }
    if enforce and errors:
        raise CoverageError("coverage gate failed: " + "; ".join(errors))


def generate(
    build_dir: Path,
    output: Path,
    ignore_file: Path | None = None,
    min_lines: float = 92.0,
    min_branches: float = 83.0,
    enforce: bool = True,
) -> dict[str, Any]:
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
    ignore_regex = load_ignore_regex(ignore_file or Path(__file__).resolve().parents[1] / ".coverage.ignore")
    summary: dict[str, Any] = {
        "schema_version": 1,
        "arenas": len(arenas),
        "ignore_regex": ignore_regex,
        "programs": {},
    }
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
                f"-ignore-filename-regex={ignore_regex}",
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
                f"-ignore-filename-regex={ignore_regex}",
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
                f"-ignore-filename-regex={ignore_regex}",
            ]
        ).stdout
        summary["programs"][program_id] = {
            "elf": str(elf),
            "profdata": str(profdata),
            "records": len(program_records),
            "counters": program_records[0].counter_count,
            "totals": totals(exported),
            "file_rows": parse_report_files(report),
        }
    (output / "summary.txt").write_text("\n".join(report_text))
    audit_coverage(summary, min_lines, min_branches, enforce)
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--ignore-file", type=Path)
    parser.add_argument("--min-lines", type=float, default=92.0)
    parser.add_argument("--min-branches", type=float, default=83.0)
    parser.add_argument("--no-gate", action="store_true", help="Generate reports without enforcing thresholds")
    arguments = parser.parse_args()
    build_dir = arguments.build_dir.resolve()
    try:
        summary = generate(
            build_dir,
            (arguments.output or build_dir / "coverage").resolve(),
            arguments.ignore_file,
            arguments.min_lines,
            arguments.min_branches,
            not arguments.no_gate,
        )
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
            f"{program_id}: lines {lines.get('percent', 0.0):.2f}% "
            f"({lines.get('covered', 0)}/{lines.get('count', 0)}), "
            f"branches {branches.get('percent', 0.0):.2f}% "
            f"({branches.get('covered', 0)}/{branches.get('count', 0)})"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
