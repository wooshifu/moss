#!/usr/bin/env python3
"""Format all tracked source files.

Uses `git ls-files` so .gitignore is automatically respected.
All tools come from pyproject.toml dev dependencies (via uv).

Usage:
    uv run format                       # format everything
    uv run format --check               # dry-run (exit 1 if anything changes)
"""

import os
import subprocess
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import typer
from rich.console import Console
from rich.progress import BarColumn, MofNCompleteColumn, Progress, TextColumn

console = Console(force_terminal=True)
app = typer.Typer(help="MOSS source formatting (respects .gitignore)")

CPP_EXTENSIONS = {".cppm", ".cpp", ".cc", ".c", ".h"}
CMAKE_NAMES = {"CMakeLists.txt"}
CMAKE_EXTENSIONS = {".cmake"}
PYTHON_EXTENSIONS = {".py"}

# Vendored third-party code — never format
EXCLUDE_PREFIXES = ("src/fdt/libfdt/",)

# Worker count: leave 2 cores free for the OS and progress rendering
MAX_WORKERS = max(1, (os.cpu_count() or 4) - 2)


# ── File discovery ──────────────────────────────────────────────────────


def git_tracked_files() -> list[Path]:
    """Return all git-tracked files as Path objects."""
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        capture_output=True,
        text=True,
        check=True,
    )
    return [Path(line) for line in result.stdout.splitlines() if line and Path(line).is_file()]


def classify_files(files: list[Path]) -> tuple[list[Path], list[Path], list[Path]]:
    """Split files into (cpp_files, cmake_files, python_files)."""
    cpp, cmake, python = [], [], []
    for f in files:
        f_str = f.as_posix()
        if any(f_str.startswith(p) for p in EXCLUDE_PREFIXES):
            continue
        if f.suffix in CPP_EXTENSIONS:
            cpp.append(f)
        elif f.name in CMAKE_NAMES or f.suffix in CMAKE_EXTENSIONS:
            cmake.append(f)
        elif f.suffix in PYTHON_EXTENSIONS:
            python.append(f)
    return cpp, cmake, python


def _make_progress(label: str) -> Progress:
    return Progress(
        TextColumn(f"  [bold]{label}[/]"),
        BarColumn(bar_width=30),
        MofNCompleteColumn(),
        TextColumn("[dim]{task.fields[current_file]}[/]"),
        console=console,
        transient=True,
    )


# ── Per-file worker functions (run in subprocess pool) ─────────────────
#
# Each returns (file_path_str, ok, detail) so the main process can
# update the progress bar and collect failures.


def _worker_clang_format(file: str, *, check: bool) -> tuple[str, bool, str]:
    cmd = ["clang-format"]
    cmd += ["--dry-run", "--Werror"] if check else ["-i"]
    cmd.append(file)
    r = subprocess.run(cmd, capture_output=True, text=True)
    return (file, r.returncode == 0, "")


def _worker_cmake_format_check(file: str) -> tuple[str, bool, str]:
    formatted = subprocess.run(["cmake-format", file], capture_output=True, text=True)
    original = Path(file).read_text()
    return (file, formatted.stdout == original, "")


def _worker_cmake_format_fix(file: str) -> tuple[str, bool, str]:
    subprocess.run(["cmake-format", "-i", file], capture_output=True, text=True)
    return (file, True, "")


def _worker_ruff_format(file: str, *, check: bool) -> tuple[str, bool, str]:
    cmd = ["ruff", "format"]
    if check:
        cmd += ["--check"]
    cmd.append(file)
    r = subprocess.run(cmd, capture_output=True, text=True)
    return (file, r.returncode == 0, "")


# ── Parallel runner ────────────────────────────────────────────────────


def _run_parallel(label: str, files: list[Path], submit_fn) -> tuple[bool, list[str]]:
    """Run submit_fn for each file using a process pool.

    submit_fn(executor, file) -> Future that resolves to (file, ok, detail).
    Returns (all_ok, detail_lines).
    """
    if not files:
        return True, []

    details: list[str] = []
    failed_files: list[str] = []

    with (
        _make_progress(label) as progress,
        ProcessPoolExecutor(max_workers=MAX_WORKERS) as pool,
    ):
        task = progress.add_task("", total=len(files), current_file="")

        # Submit all jobs
        future_to_file = {}
        for f in files:
            fut = submit_fn(pool, str(f))
            future_to_file[fut] = f

        # Collect results as they complete
        for fut in as_completed(future_to_file):
            f = future_to_file[fut]
            progress.update(task, current_file=str(f))
            file_str, ok, detail = fut.result()
            if not ok:
                failed_files.append(file_str)
            if detail:
                details.extend(detail.splitlines())
            progress.advance(task)

    # Print failures
    if failed_files and not details:
        for f in failed_files:
            console.print(f"  [yellow]needs formatting:[/] {f}")
    for line in details:
        console.print(f"  {line}")

    return len(failed_files) == 0, details


# ── Formatters ──────────────────────────────────────────────────────────


def run_clang_format(files: list[Path], *, check: bool) -> bool:
    ok, _ = _run_parallel(
        "clang-format",
        files,
        lambda pool, f: pool.submit(_worker_clang_format, f, check=check),
    )
    return ok


def run_cmake_format(files: list[Path], *, check: bool) -> bool:
    if check:
        ok, _ = _run_parallel(
            "cmake-format",
            files,
            lambda pool, f: pool.submit(_worker_cmake_format_check, f),
        )
    else:
        ok, _ = _run_parallel(
            "cmake-format",
            files,
            lambda pool, f: pool.submit(_worker_cmake_format_fix, f),
        )
    return ok


def run_ruff_format(files: list[Path], *, check: bool) -> bool:
    ok, _ = _run_parallel(
        "ruff format",
        files,
        lambda pool, f: pool.submit(_worker_ruff_format, f, check=check),
    )
    return ok


# ── Commands ────────────────────────────────────────────────────────────


def _print_summary(*, mode: str, cpp: int, cmake: int, python: int) -> None:
    console.print(f"\n  mode:    [bold]{mode}[/]")
    console.print(f"  C++:     {cpp} files")
    console.print(f"  CMake:   {cmake} files")
    console.print(f"  Python:  {python} files")
    console.print(f"  workers: {MAX_WORKERS}\n")


def _print_result(label: str, ok: bool) -> None:
    status = "[green]ok[/]" if ok else "[red]issues found[/]"
    console.print(f"  [bold]{label}[/] {status}")


@app.command()
def main(
    check: bool = typer.Option(False, "--check", help="Check only, don't modify (exit 1 if diff)"),
    cpp_only: bool = typer.Option(False, "--cpp-only", help="Format C++ files only"),
    cmake_only: bool = typer.Option(False, "--cmake-only", help="Format CMake files only"),
    py_only: bool = typer.Option(False, "--py-only", help="Format Python files only"),
) -> None:
    """Format all tracked source files."""
    files = git_tracked_files()
    cpp_files, cmake_files, py_files = classify_files(files)

    only = cpp_only or cmake_only or py_only
    if only:
        if not cpp_only:
            cpp_files = []
        if not cmake_only:
            cmake_files = []
        if not py_only:
            py_files = []

    _print_summary(
        mode="check" if check else "format", cpp=len(cpp_files), cmake=len(cmake_files), python=len(py_files)
    )
    all_ok = True

    if cpp_files:
        ok = run_clang_format(cpp_files, check=check)
        _print_result("clang-format", ok)
        all_ok &= ok
    if cmake_files:
        ok = run_cmake_format(cmake_files, check=check)
        _print_result("cmake-format", ok)
        all_ok &= ok
    if py_files:
        ok = run_ruff_format(py_files, check=check)
        _print_result("ruff format", ok)
        all_ok &= ok

    if check and not all_ok:
        console.print("\n[red]Some files need formatting.[/] Run: uv run format\n")
        raise typer.Exit(1)
    if not check:
        console.print("\n[green]Done.[/]\n")


if __name__ == "__main__":
    app()
