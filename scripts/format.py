#!/usr/bin/env python3
"""Format and lint all tracked source files.

Uses `git ls-files` so .gitignore is automatically respected.
All tools come from pyproject.toml dev dependencies (via uv).

Usage:
    uv run scripts/format.py                     # format everything (default)
    uv run scripts/format.py --check             # dry-run (exit 1 if anything changes)
    uv run scripts/format.py lint                # lint everything
    uv run scripts/format.py lint --fix          # lint with auto-fix (serial)
"""

import json
import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import typer
from rich.console import Console
from rich.progress import BarColumn, MofNCompleteColumn, Progress, TextColumn

console = Console(force_terminal=True)
app = typer.Typer(help="MOSS code quality tools (respects .gitignore)")

CPP_EXTENSIONS = {".cppm", ".cpp", ".cc", ".c", ".h"}
# clang-tidy only processes compilation units listed in compile_commands.json
CLANG_TIDY_EXTENSIONS = {".cppm", ".cpp", ".cc", ".c"}
CMAKE_NAMES = {"CMakeLists.txt"}
CMAKE_EXTENSIONS = {".cmake"}
PYTHON_EXTENSIONS = {".py"}

# Vendored third-party code — never format or lint
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
    return [Path(line) for line in result.stdout.splitlines() if line]


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


def _worker_clang_tidy(file: str, *, clang_tidy: str, build_dir: str) -> tuple[str, bool, str]:
    cmd = [clang_tidy, "-p", build_dir, file]
    r = subprocess.run(cmd, capture_output=True, text=True)
    output = r.stdout + r.stderr
    issues = [line for line in output.splitlines() if ": warning:" in line or ": error:" in line]
    return (file, len(issues) == 0, "\n".join(issues))


def _worker_ruff_lint(file: str) -> tuple[str, bool, str]:
    cmd = ["ruff", "check", file]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        lines = [line.strip() for line in r.stdout.splitlines() if line.strip()]
        return (file, False, "\n".join(lines))
    return (file, True, "")


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


# ── Linters ─────────────────────────────────────────────────────────────


def find_compile_commands() -> Path | None:
    """Find compile_commands.json — prefer root symlink, then any build dir."""
    root = Path("compile_commands.json")
    if root.exists():
        return root.resolve().parent if root.is_symlink() else root.parent

    for p in sorted(Path("build").glob("*/compile_commands.json")):
        return p.parent
    return None


def find_clang_tidy(build_dir: Path) -> str:
    """Find clang-tidy matching the compiler that built .pcm module files.

    Priority: compiler-directory clang-tidy first (guaranteed version
    match), then fall back to the venv version from pyproject.toml.
    """
    cc_json = build_dir / "compile_commands.json"
    if cc_json.exists():
        entries = json.loads(cc_json.read_text())
        if entries:
            compiler = entries[0].get("command", "").split()[0]
            if compiler:
                candidate = Path(compiler).parent / "clang-tidy"
                if candidate.exists():
                    return str(candidate)

    return "clang-tidy"


def _compile_commands_files(build_dir: Path) -> set[str]:
    """Return the set of source file paths listed in compile_commands.json."""
    cc_json = build_dir / "compile_commands.json"
    if not cc_json.exists():
        return set()
    entries = json.loads(cc_json.read_text())
    return {e.get("file", "") for e in entries}


def run_clang_tidy(files: list[Path], *, fix: bool, build_dir: Path) -> bool:
    """Run clang-tidy on C++ source files. Returns True if no warnings."""
    tidy_files = [f for f in files if f.suffix in CLANG_TIDY_EXTENSIONS]
    if not tidy_files:
        return True

    if not (build_dir / "compile_commands.json").exists():
        console.print(f"  [red]compile_commands.json not found in {build_dir} — build first[/]")
        return False

    # Only lint files present in compile_commands.json — cross-arch sources
    # (e.g. riscv/boot_impl.cpp in an arm64 build) lack correct .pcm modules
    # and would produce spurious errors.
    known_files = _compile_commands_files(build_dir)
    if known_files:
        tidy_files = [f for f in tidy_files if str(f.resolve()) in known_files]
    if not tidy_files:
        return True

    clang_tidy = find_clang_tidy(build_dir)

    if fix:
        # Serial: fix one file, rebuild to refresh .pcm, then next file.
        # Without rebuild, clang-tidy reads stale .pcm ASTs and generates
        # duplicate edits on already-fixed module interfaces.
        return _run_clang_tidy_serial(tidy_files, clang_tidy=clang_tidy, build_dir=build_dir)

    # Read-only analysis → parallel
    ok, _ = _run_parallel(
        "clang-tidy",
        tidy_files,
        lambda pool, f: pool.submit(_worker_clang_tidy, f, clang_tidy=clang_tidy, build_dir=str(build_dir)),
    )
    return ok


def _run_clang_tidy_serial(files: list[Path], *, clang_tidy: str, build_dir: Path) -> bool:
    """Run clang-tidy --fix one file at a time, rebuilding between runs to refresh .pcm."""
    issues: list[str] = []

    with _make_progress("clang-tidy [fix]") as progress:
        task = progress.add_task("", total=len(files), current_file="")
        for f in files:
            progress.update(task, current_file=str(f))
            cmd = [clang_tidy, "-p", str(build_dir), "--fix", "--fix-errors", "--fix-notes", str(f)]

            result = subprocess.run(cmd, capture_output=True, text=True)
            output = result.stdout + result.stderr
            for line in output.splitlines():
                if ": warning:" in line or ": error:" in line:
                    issues.append(line)

            # Rebuild project to refresh all .pcm files so the next
            # clang-tidy invocation sees the updated module ASTs.
            rebuild = subprocess.run(["cmake", "--build", str(build_dir)], capture_output=True, text=True)
            if rebuild.returncode != 0:
                progress.stop()
                console.print(f"  [red]build failed after fixing {f} — aborting[/]")
                console.print(rebuild.stderr or rebuild.stdout)
                return False
            progress.advance(task)

    for line in issues:
        console.print(f"  {line}")
    return len(issues) == 0


def run_ruff_lint(files: list[Path], *, fix: bool) -> bool:
    if not files:
        return True

    if fix:
        # --fix modifies files → serial
        return _run_ruff_lint_serial(files, fix=True)

    # Read-only analysis → parallel
    ok, _ = _run_parallel(
        "ruff check",
        files,
        lambda pool, f: pool.submit(_worker_ruff_lint, f),
    )
    return ok


def _run_ruff_lint_serial(files: list[Path], *, fix: bool) -> bool:
    """Run ruff check one file at a time (for --fix mode)."""
    failed: list[str] = []

    with _make_progress("ruff check [fix]") as progress:
        task = progress.add_task("", total=len(files), current_file="")
        for f in files:
            progress.update(task, current_file=str(f))
            cmd = ["ruff", "check", "--fix", str(f)]

            result = subprocess.run(cmd, capture_output=True, text=True)
            if result.returncode != 0:
                for line in result.stdout.splitlines():
                    if line.strip():
                        failed.append(line.strip())
            progress.advance(task)

    for line in failed:
        console.print(f"  {line}")
    return len(failed) == 0


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
def format(
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
        console.print("\n[red]Some files need formatting.[/] Run: uv run scripts/format.py format\n")
        raise typer.Exit(1)
    if not check:
        console.print("\n[green]Done.[/]\n")


@app.command()
def lint(
    fix: bool = typer.Option(False, "--fix", help="Auto-fix issues where possible"),
    cpp_only: bool = typer.Option(False, "--cpp-only", help="Lint C++ files only"),
    py_only: bool = typer.Option(False, "--py-only", help="Lint Python files only"),
    build_dir: str = typer.Option(
        "build/arm64-qemu-debug", "--build-dir", help="CMake build directory (for rebuilding .pcm after --fix)"
    ),
) -> None:
    """Lint all tracked source files (clang-tidy + ruff check)."""
    files = git_tracked_files()
    cpp_files, _, py_files = classify_files(files)

    only = cpp_only or py_only
    if only:
        if not cpp_only:
            cpp_files = []
        if not py_only:
            py_files = []

    mode = "lint --fix [serial]" if fix else "lint [parallel]"
    _print_summary(mode=mode, cpp=len(cpp_files), cmake=0, python=len(py_files))
    all_ok = True

    if cpp_files:
        ok = run_clang_tidy(cpp_files, fix=fix, build_dir=Path(build_dir))
        _print_result("clang-tidy", ok)
        all_ok &= ok
    if py_files:
        ok = run_ruff_lint(py_files, fix=fix)
        _print_result("ruff check", ok)
        all_ok &= ok

    if not all_ok and not fix:
        console.print("\n[yellow]Issues found.[/] Run: uv run scripts/format.py lint --fix\n")
        raise typer.Exit(1)
    if all_ok:
        console.print("\n[green]All clean.[/]\n")


if __name__ == "__main__":
    # Default to "format" when no subcommand is given
    if len(sys.argv) > 1 and sys.argv[1] in ("help", "--help", "-h"):
        sys.argv[1:] = ["--help"]
    elif len(sys.argv) == 1 or (len(sys.argv) > 1 and sys.argv[1].startswith("-")):
        sys.argv.insert(1, "format")
    app()
