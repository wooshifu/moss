#!/usr/bin/env python3
"""Format and lint all tracked source files.

Uses `git ls-files` so .gitignore is automatically respected.
All tools come from pyproject.toml dev dependencies (via uv).

Usage:
    uv run scripts/format.py format              # format everything
    uv run scripts/format.py format --check      # dry-run (exit 1 if anything changes)
    uv run scripts/format.py lint                 # lint everything
    uv run scripts/format.py lint --fix           # lint with auto-fix
"""

import json
import subprocess
from pathlib import Path

import typer
from rich.console import Console
from rich.progress import BarColumn, MofNCompleteColumn, Progress, TextColumn

console = Console(force_terminal=True)
app = typer.Typer(help="MOSS code quality tools (respects .gitignore)")

CPP_EXTENSIONS = {".cppm", ".cpp", ".c", ".h"}
# clang-tidy only processes compilation units listed in compile_commands.json
CLANG_TIDY_EXTENSIONS = {".cppm", ".cpp", ".c"}
CMAKE_NAMES = {"CMakeLists.txt"}
CMAKE_EXTENSIONS = {".cmake"}
PYTHON_EXTENSIONS = {".py"}

# Vendored third-party code — never format or lint
EXCLUDE_PREFIXES = ("src/fdt/libfdt/",)


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


# ── Formatters ──────────────────────────────────────────────────────────


def run_clang_format(files: list[Path], *, check: bool) -> bool:
    if not files:
        return True

    failed: list[Path] = []
    with _make_progress("clang-format") as progress:
        task = progress.add_task("", total=len(files), current_file="")
        for f in files:
            progress.update(task, current_file=str(f))
            cmd = ["clang-format"]
            cmd += ["--dry-run", "--Werror"] if check else ["-i"]
            cmd.append(str(f))
            result = subprocess.run(cmd, capture_output=True, text=True)
            if result.returncode != 0:
                failed.append(f)
            progress.advance(task)

    for f in failed:
        console.print(f"  [yellow]needs formatting:[/] {f}")
    return len(failed) == 0


def run_cmake_format(files: list[Path], *, check: bool) -> bool:
    if not files:
        return True

    failed: list[Path] = []
    with _make_progress("cmake-format") as progress:
        task = progress.add_task("", total=len(files), current_file="")
        for f in files:
            progress.update(task, current_file=str(f))
            if check:
                formatted = subprocess.run(["cmake-format", str(f)], capture_output=True, text=True)
                if formatted.stdout != f.read_text():
                    failed.append(f)
            else:
                subprocess.run(["cmake-format", "-i", str(f)], capture_output=True, text=True)
            progress.advance(task)

    for f in failed:
        console.print(f"  [yellow]needs formatting:[/] {f}")
    return len(failed) == 0


def run_ruff_format(files: list[Path], *, check: bool) -> bool:
    if not files:
        return True

    failed: list[Path] = []
    with _make_progress("ruff format") as progress:
        task = progress.add_task("", total=len(files), current_file="")
        for f in files:
            progress.update(task, current_file=str(f))
            cmd = ["ruff", "format"]
            if check:
                cmd += ["--check"]
            cmd.append(str(f))
            result = subprocess.run(cmd, capture_output=True, text=True)
            if result.returncode != 0:
                failed.append(f)
            progress.advance(task)

    for f in failed:
        console.print(f"  [yellow]needs formatting:[/] {f}")
    return len(failed) == 0


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


def run_clang_tidy(files: list[Path], *, fix: bool) -> bool:
    """Run clang-tidy on C++ source files. Returns True if no warnings."""
    tidy_files = [f for f in files if f.suffix in CLANG_TIDY_EXTENSIONS]
    if not tidy_files:
        return True

    build_dir = find_compile_commands()
    if build_dir is None:
        console.print("  [red]compile_commands.json not found — build first[/]")
        return False

    clang_tidy = find_clang_tidy(build_dir)
    issues: list[str] = []

    with _make_progress("clang-tidy") as progress:
        task = progress.add_task("", total=len(tidy_files), current_file="")
        for f in tidy_files:
            progress.update(task, current_file=str(f))
            cmd = [clang_tidy, "-p", str(build_dir)]
            if fix:
                cmd += ["--fix", "--fix-errors"]
            cmd.append(str(f))

            result = subprocess.run(cmd, capture_output=True, text=True)
            output = result.stdout + result.stderr
            for line in output.splitlines():
                if ": warning:" in line or ": error:" in line:
                    issues.append(line)
            progress.advance(task)

    for line in issues:
        console.print(f"  {line}")
    return len(issues) == 0


def run_ruff_lint(files: list[Path], *, fix: bool) -> bool:
    if not files:
        return True

    failed: list[str] = []
    with _make_progress("ruff check") as progress:
        task = progress.add_task("", total=len(files), current_file="")
        for f in files:
            progress.update(task, current_file=str(f))
            cmd = ["ruff", "check"]
            if fix:
                cmd += ["--fix"]
            cmd.append(str(f))

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
    console.print(f"\n  mode:   [bold]{mode}[/]")
    console.print(f"  C++:    {cpp} files")
    console.print(f"  CMake:  {cmake} files")
    console.print(f"  Python: {python} files\n")


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

    _print_summary(mode="lint --fix" if fix else "lint", cpp=len(cpp_files), cmake=0, python=len(py_files))
    all_ok = True

    if cpp_files:
        ok = run_clang_tidy(cpp_files, fix=fix)
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
    app()
