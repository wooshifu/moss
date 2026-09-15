#!/usr/bin/env python3
"""Moss prerequisite checker - detect LLVM toolchain and optional tools.

Two output modes:
  - Human-readable rich table (default)
  - Machine-readable JSON for CMake consumption (--json)

When --build-dir is given, a copy of the JSON result is saved there as
``prerequisites.json`` for later reference.
"""

from __future__ import annotations

import os
import platform
import re
import shutil
import subprocess
from enum import StrEnum
from pathlib import Path

import typer
from packaging.version import InvalidVersion, Version
from pydantic import BaseModel, Field
from rich.console import Console
from rich.table import Table

# ---------------------------------------------------------------------------
# Data models (pydantic — free JSON serialisation)
# ---------------------------------------------------------------------------


class Requirement(StrEnum):
    REQUIRED = "required"
    OPTIONAL = "optional"


class ToolSpec(BaseModel):
    """Definition of a tool to check."""

    name: str
    description: str
    requirement: Requirement = Requirement.REQUIRED
    min_version: str = "21.0"


class ToolInfo(BaseModel):
    """Detected tool information (appears in JSON output)."""

    path: str = ""
    version: str = ""


class CheckResult(BaseModel):
    """Aggregate result of all prerequisite checks."""

    status: str = "ok"
    tools: dict[str, ToolInfo] = Field(default_factory=dict)
    optional_tools: dict[str, ToolInfo] = Field(default_factory=dict)
    missing_required: list[str] = Field(default_factory=list)
    version_errors: list[str] = Field(default_factory=list)
    llvm_bin_dir: str = ""
    install_guide: str = ""


# ---------------------------------------------------------------------------
# Tool definitions
# ---------------------------------------------------------------------------

LLVM_TOOLS: list[ToolSpec] = [
    ToolSpec(name="clang", description="C/C++ compiler"),
    ToolSpec(name="clang++", description="C++ compiler"),
    ToolSpec(name="lld", description="linker"),
    ToolSpec(name="llvm-objdump", description="object dump"),
    ToolSpec(name="llvm-objcopy", description="object copy"),
    ToolSpec(name="llvm-nm", description="symbol table"),
    ToolSpec(name="llvm-readelf", description="ELF reader", requirement=Requirement.OPTIONAL),
    ToolSpec(name="llvm-strip", description="symbol stripper", requirement=Requirement.OPTIONAL),
    ToolSpec(name="llvm-strings", description="string extractor", requirement=Requirement.OPTIONAL),
    ToolSpec(name="llvm-addr2line", description="address resolver", requirement=Requirement.OPTIONAL),
    ToolSpec(name="llvm-cxxfilt", description="symbol demangler", requirement=Requirement.OPTIONAL),
    ToolSpec(name="llvm-ar", description="archiver", requirement=Requirement.OPTIONAL),
    ToolSpec(name="llvm-ranlib", description="archive indexer", requirement=Requirement.OPTIONAL),
    ToolSpec(name="llvm-size", description="size analyzer", requirement=Requirement.OPTIONAL),
]


# ---------------------------------------------------------------------------
# Platform-specific search paths
# ---------------------------------------------------------------------------


def _llvm_search_paths() -> list[Path]:
    """Return platform-specific LLVM search directories."""
    host = platform.system()
    candidates: list[Path] = []
    if host == "Linux":
        candidates = [
            Path("/usr/lib/llvm-21/bin"),
            Path("/usr/lib/llvm-20/bin"),
            Path("/usr/lib/llvm-19/bin"),
            Path("/usr/local/llvm/bin"),
        ]
    elif host == "Darwin":
        candidates = [
            Path("/opt/homebrew/opt/llvm/bin"),
            Path("/usr/local/opt/llvm/bin"),
        ]
    elif host == "Windows":
        candidates = [Path("C:/Program Files/LLVM/bin")]
        local = os.environ.get("LOCALAPPDATA")
        if local:
            candidates.append(Path(local) / "Programs" / "LLVM" / "bin")
    return [p for p in candidates if p.is_dir()]


# ---------------------------------------------------------------------------
# Tool detection helpers
# ---------------------------------------------------------------------------

_VERSION_RE = re.compile(r"(\d+\.\d+(?:\.\d+)*)")


def _which(name: str, search_dir: Path | None = None) -> Path | None:
    """Thin wrapper around ``shutil.which`` that returns a resolved ``Path``.

    *search_dir* restricts the search to a single directory; when ``None``
    the normal system ``PATH`` is used.  ``Path.resolve()`` normalises the
    casing on Windows (e.g. ``.EXE`` → ``.exe``).
    """
    found = shutil.which(name, path=str(search_dir)) if search_dir else shutil.which(name)
    return Path(found).resolve() if found else None


def _get_version(executable: Path) -> Version | None:
    """Run ``<tool> --version`` and parse the first semver-ish version."""
    try:
        proc = subprocess.run(
            [str(executable), "--version"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        m = _VERSION_RE.search(proc.stdout + proc.stderr)
        return Version(m.group(1)) if m else None
    except (subprocess.TimeoutExpired, OSError, InvalidVersion):
        return None


def _version_ok(ver: Version | None, minimum: str) -> bool:
    """Return ``True`` when *ver* meets the *minimum* requirement."""
    return ver is not None and ver >= Version(minimum)


def _resolve_tool(
    name: str,
    min_version: str,
    fallback_dirs: list[Path],
) -> tuple[Path | None, Version | None]:
    """Locate *name* with a two-stage strategy:

    1. Check the system ``PATH`` first.  If found **and** the version is
       acceptable, return immediately — this is the fast path.
    2. Otherwise fall back to the platform-specific *fallback_dirs*, trying
       each one in order until a version-satisfying binary is found.

    Returns ``(executable_path, parsed_version)``.  Either or both may be
    ``None`` when the tool cannot be located / versioned.
    """
    # --- Stage 1: system PATH ---
    exe = _which(name)
    if exe:
        ver = _get_version(exe)
        if _version_ok(ver, min_version) or ver is None:
            # ver=None means the tool exists but doesn't report a version
            # (e.g. `lld --version` fails) — still acceptable.
            return exe, ver

    # --- Stage 2: platform-specific fallback directories ---
    for d in fallback_dirs:
        candidate = _which(name, d)
        if candidate:
            ver = _get_version(candidate)
            if _version_ok(ver, min_version) or ver is None:
                return candidate, ver

    # If we found *something* on PATH (stage 1) but its version was too low,
    # still return it so the caller can report a clear "version too old" error
    # instead of a confusing "not found" error.
    if exe:
        return exe, _get_version(exe)

    return None, None


# ---------------------------------------------------------------------------
# Core check logic
# ---------------------------------------------------------------------------


def check_all(arch: str | None = None) -> CheckResult:
    """Run all prerequisite checks and return a structured result."""
    result = CheckResult()
    llvm_dirs = _llvm_search_paths()
    llvm_bin_dir: Path | None = None

    # --- LLVM tools ---
    for spec in LLVM_TOOLS:
        exe, ver = _resolve_tool(spec.name, spec.min_version, llvm_dirs)
        info = ToolInfo(
            path=exe.as_posix() if exe else "",
            version=str(ver) if ver else "",
        )

        is_required = spec.requirement == Requirement.REQUIRED

        if is_required:
            result.tools[spec.name] = info
            if not exe:
                result.missing_required.append(spec.name)
            elif ver and ver < Version(spec.min_version):
                result.version_errors.append(f"{spec.name}: version {ver} < {spec.min_version}")
        else:
            result.optional_tools[spec.name] = info

        if exe and not llvm_bin_dir and is_required:
            llvm_bin_dir = exe.parent

    # --- Finalise ---
    result.llvm_bin_dir = llvm_bin_dir.as_posix() if llvm_bin_dir else ""

    if result.missing_required or result.version_errors:
        result.status = "error"
        result.install_guide = _build_install_guide(result.missing_required, result.version_errors)

    return result


def _build_install_guide(missing: list[str], ver_errors: list[str]) -> str:
    lines = ["Missing or outdated LLVM tools detected.", ""]
    if missing:
        lines.append(f"  Missing: {', '.join(missing)}")
    if ver_errors:
        lines.append(f"  Version errors: {'; '.join(ver_errors)}")
    lines += [
        "",
        "Install LLVM 21 toolchain:",
        "  Ubuntu/Debian : sudo apt install llvm-21 clang-21 lld-21",
        "  Fedora/RHEL   : sudo dnf install llvm clang lld",
        "  Arch Linux    : sudo pacman -S llvm clang lld",
        "  macOS         : brew install llvm",
        "  Windows       : winget install LLVM.LLVM",
        "",
        "Ensure LLVM binaries are on PATH:",
        '  Linux  : export PATH="/usr/lib/llvm-21/bin:$PATH"',
        '  macOS  : export PATH="$(brew --prefix llvm)/bin:$PATH"',
        "  Windows: add LLVM bin directory to system PATH",
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

app = typer.Typer(help="Moss prerequisite checker", rich_markup_mode="rich")


@app.callback(invoke_without_command=True)
def main(
    output_json: bool = typer.Option(False, "--json", help="Output JSON for CMake"),
    arch: str | None = typer.Option(
        None,
        "--arch",
        "-a",
        help="Target architecture (ARM64, X64, RISCV64)",
    ),
    build_dir: Path | None = typer.Option(
        None,
        "--build-dir",
        "-b",
        help="Save prerequisites.json into this directory",
    ),
) -> None:
    """Check build prerequisites for the Moss kernel project."""
    result = check_all(arch.upper() if arch else None)

    if output_json:
        _output_json(result)
    else:
        _output_table(result)

    if build_dir:
        _save_to_build_dir(result, build_dir)

    if result.status != "ok" and not output_json:
        raise typer.Exit(1)


# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------


def _result_to_json(result: CheckResult) -> str:
    """Serialise *result* to a JSON string (pydantic handles everything)."""
    return result.model_dump_json(indent=2)


def _output_json(result: CheckResult) -> None:
    """Print pure JSON to stdout (no extra decoration)."""
    print(_result_to_json(result))


def _save_to_build_dir(result: CheckResult, build_dir: Path) -> None:
    """Write a ``prerequisites.json`` snapshot into *build_dir*."""
    build_dir.mkdir(parents=True, exist_ok=True)
    (build_dir / "prerequisites.json").write_text(_result_to_json(result), encoding="utf-8")


def _output_table(result: CheckResult) -> None:
    """Print a rich table for human consumption."""
    console = Console()

    table = Table(title="Moss Build Prerequisites")
    table.add_column("Tool", style="cyan")
    table.add_column("Status", justify="center")
    table.add_column("Version")
    table.add_column("Path", style="dim")

    for name, info in result.tools.items():
        if name in result.missing_required:
            status = "[red]MISSING[/red]"
        elif any(name in e for e in result.version_errors):
            status = "[yellow]OUTDATED[/yellow]"
        else:
            status = "[green]OK[/green]"
        table.add_row(name, status, info.version, info.path)

    for name, info in result.optional_tools.items():
        status = "[green]OK[/green]" if info.path else "[dim]not found[/dim]"
        table.add_row(f"{name} (optional)", status, info.version, info.path)

    console.print(table)

    if result.status != "ok":
        console.print(f"\n[red]{result.install_guide}[/red]")
    else:
        console.print("\n[green]All required prerequisites satisfied.[/green]")


if __name__ == "__main__":
    app()
