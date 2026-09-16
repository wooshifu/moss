#!/usr/bin/env python3
"""Run clang-tidy on Moss C/C++ translation units, including C++ module interfaces.

The Git index and lint.toml select repository-owned files; compile_commands.json
provides all target flags and module mappings. CMake incrementally builds before
analysis and after fixes so importers always see current BMIs. No LLVM tools are
downloaded: use tools beside the build compiler, on PATH, or explicit overrides.

--changed includes committed, staged, unstaged and untracked changes relative to
merge-base(HEAD, origin/master), falling back to HEAD when origin/master is absent.
Header/module/config changes check all available TUs; positional paths further
restrict that selection. Full mode checks indexed files only.

--fix collects replacements in parallel, applies them once, rebuilds, then checks
again. Exit codes: 0 clean, 1 diagnostics, 2 configuration/build/tool failure,
130 interrupted. --check never edits source files, but does update build artifacts.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import tomllib
from collections.abc import Sequence
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

import pathspec
import pygit2
from rich.console import Console
from rich.progress import BarColumn, MofNCompleteColumn, Progress, TextColumn

MODULE_SUFFIXES = {".cppm"}
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"} | MODULE_SUFFIXES
CPP_SUFFIXES = SOURCE_SUFFIXES | {".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"}
GLOBAL_INPUTS = {".clang-tidy", "lint.toml"}
REGULAR_FILE_MODES = {pygit2.GIT_FILEMODE_BLOB, pygit2.GIT_FILEMODE_BLOB_EXECUTABLE}


class LintError(Exception):
    """Invalid configuration, build failure, or tool execution failure."""


@dataclass(frozen=True)
class ExcludePattern:
    source: str
    spec: pathspec.PathSpec


@dataclass(frozen=True)
class CompilationCommand:
    path: str
    directory: Path
    arguments: tuple[str, ...]


@dataclass(frozen=True)
class Tool:
    path: Path
    version: str


@dataclass(frozen=True)
class LintResult:
    path: str
    returncode: int
    stdout: str = ""
    stderr: str = ""


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--check", action="store_true", help="Check without editing source files")
    action.add_argument("--fix", action="store_true", help="Collect fixes, apply once, rebuild and recheck")
    parser.add_argument(
        "--fix-errors", action="store_true", help="Allow fixes despite lint compiler errors; requires --fix"
    )
    parser.add_argument(
        "--preset",
        default="arm64-debug",
        metavar="NAME",
        help="Configured CMake preset in build/<name> (default: %(default)s)",
    )
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1, help="Parallel build and lint jobs")
    parser.add_argument("-v", "--verbose", action="store_true", help="Print selected files and full commands")
    parser.add_argument("--changed", action="store_true", help="Check changes since merge-base(HEAD, origin/master)")
    parser.add_argument("--clang-tidy", metavar="PATH", help="Use this installed clang-tidy")
    parser.add_argument("--clang-apply-replacements", metavar="PATH", help="Use this installed replacement tool")
    parser.add_argument(
        "--clang-extra-arg-before", action="append", default=[], metavar="ARG", help="Repeat for extra flags"
    )
    parser.add_argument("paths", nargs="*", metavar="PATH", help="Repository files or directories to filter TUs")
    return parser


def open_repository(root: Path) -> pygit2.Repository:
    git_dir = pygit2.discover_repository(str(root))
    if git_dir is None:
        raise LintError(f"not a Git repository: {root}")
    repo = pygit2.Repository(git_dir)
    if repo.is_bare or repo.workdir is None or Path(repo.workdir).resolve() != root:
        raise LintError("lint.py must belong to the Git worktree root")
    repo.index.read()
    if repo.index.conflicts is not None:
        raise LintError("resolve Git index conflicts before linting")
    return repo


def load_excludes(root: Path) -> tuple[ExcludePattern, ...]:
    try:
        config = tomllib.loads((root / "lint.toml").read_text(encoding="utf-8"))
    except (OSError, UnicodeError, tomllib.TOMLDecodeError) as exc:
        raise LintError(f"cannot load lint.toml: {exc}") from exc
    if unknown := set(config) - {"exclude"}:
        raise LintError(f"unknown lint.toml key(s): {', '.join(sorted(unknown))}")
    values = config.get("exclude", [])
    if not isinstance(values, list) or any(not isinstance(value, str) for value in values):
        raise LintError("lint.toml exclude must be an array of strings")
    patterns = []
    for value in values:
        if not value or value.startswith(("!", "/", "#")) or "\\" in value:
            raise LintError(f"invalid exclude pattern {value!r}: use relative POSIX paths without reinclusion")
        patterns.append(ExcludePattern(value, pathspec.PathSpec.from_lines("gitignore", [value])))
    return tuple(patterns)


def apply_excludes(paths: set[str], patterns: Sequence[ExcludePattern]) -> set[str]:
    return {path for path in paths if not any(pattern.spec.match_file(path) for pattern in patterns)}


def tracked_cpp_files(repo: pygit2.Repository) -> set[str]:
    return {
        entry.path
        for entry in repo.index
        if entry.mode in REGULAR_FILE_MODES and PurePosixPath(entry.path).suffix.lower() in CPP_SUFFIXES
    }


def changed_repository_files(repo: pygit2.Repository) -> set[str]:
    if repo.head_is_unborn:
        raise LintError("--changed requires an existing HEAD commit")
    head = repo.head.peel(pygit2.Commit)
    try:
        upstream = repo.revparse_single("origin/master").peel(pygit2.Commit)
    except KeyError:
        base = head
        description = f"HEAD@{str(head.id)[:8]} (origin/master unavailable)"
    else:
        merge_base = repo.merge_base(head.id, upstream.id)
        if merge_base is None:
            raise LintError("HEAD and origin/master have no merge-base")
        base = repo[merge_base].peel(pygit2.Commit)
        description = f"merge-base(HEAD, origin/master)@{str(base.id)[:8]}"
    paths = set()
    for diff in (repo.index.diff_to_tree(base.tree), repo.diff()):
        for patch in diff:
            # Retain both names: deleting or renaming a module/header affects importers.
            for file in (patch.delta.old_file, patch.delta.new_file):
                if file.path:
                    paths.add(file.path)
    paths.update(
        path for path, status in repo.status(untracked_files="all").items() if status & pygit2.GIT_STATUS_WT_NEW
    )
    print(f"Changed-file mode: {len(paths)} path(s) since {description}.", flush=True)
    return paths


def changed_inputs(root: Path, paths: set[str], excludes: Sequence[ExcludePattern]) -> tuple[set[str], set[str]]:
    cpp = apply_excludes(
        {path for path in paths if PurePosixPath(path).suffix.lower() in CPP_SUFFIXES},
        excludes,
    )
    sources = {
        path for path in cpp if PurePosixPath(path).suffix.lower() in SOURCE_SUFFIXES and (root / path).is_file()
    }
    wide = {path for path in cpp if PurePosixPath(path).suffix.lower() not in SOURCE_SUFFIXES - MODULE_SUFFIXES}
    return sources, wide | (paths & GLOBAL_INPUTS)


def repository_path(root: Path, path: Path) -> str:
    # Preserve lexical in-repository symlinks recorded by the build system.
    try:
        return Path(os.path.abspath(path)).relative_to(root).as_posix()
    except ValueError as exc:
        raise LintError(f"compilation database path escapes the repository: {path}") from exc


def load_compilation_database(root: Path, build_dir: Path) -> tuple[CompilationCommand, ...]:
    database = build_dir / "compile_commands.json"
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise LintError(f"cannot load {database}; configure the selected build first: {exc}") from exc
    if not isinstance(entries, list) or not entries:
        raise LintError(f"{database} must contain a nonempty array of compile commands")
    commands = []
    for index, entry in enumerate(entries):
        try:
            if not isinstance(entry, dict) or any(
                not isinstance(entry.get(key), str) or not entry[key] for key in ("directory", "file")
            ):
                raise ValueError("directory and file must be nonempty strings")
            directory = Path(entry["directory"])
            if not directory.is_absolute():
                directory = build_dir / directory
            if "arguments" not in entry and not isinstance(entry.get("command"), str):
                raise ValueError("command must be a string when arguments is absent")
            arguments = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
            if not isinstance(arguments, list) or not arguments or any(not isinstance(arg, str) for arg in arguments):
                raise ValueError("arguments must be a nonempty array of strings")
            if not arguments[0]:
                raise ValueError("missing compiler executable")
            path = repository_path(root, directory / entry["file"])
            commands.append(CompilationCommand(path, directory, tuple(arguments)))
        except (KeyError, TypeError, ValueError) as exc:
            raise LintError(f"invalid compile command #{index + 1} in {database}: {exc}") from exc
    # clang-tidy itself processes every command for a file, including synthetic BMIs.
    return tuple(commands)


def select_sources(
    root: Path,
    commands: Sequence[CompilationCommand],
    tracked: set[str],
    excludes: Sequence[ExcludePattern],
    changed: tuple[set[str], set[str]] | None,
    filters: Sequence[str],
    cwd: Path,
) -> list[str]:
    available = apply_excludes(
        {command.path for command in commands if PurePosixPath(command.path).suffix.lower() in SOURCE_SUFFIXES},
        excludes,
    )
    expected = apply_excludes(
        {path for path in tracked if PurePosixPath(path).suffix.lower() in SOURCE_SUFFIXES and (root / path).is_file()},
        excludes,
    )
    selected = expected & available
    if changed is not None:
        sources, wide = changed
        if missing := sources - available:
            raise LintError(
                "changed translation unit(s) missing from compilation database: " + ", ".join(sorted(missing))
            )
        selected = selected | sources if wide else sources
        if wide:
            print("Changed header/module/config: checking all available translation units.", flush=True)
    if (changed is None or changed[1]) and (missing := expected - available):
        print(
            f"WARNING: compilation database does not cover {len(missing)} tracked translation unit(s):",
            file=sys.stderr,
        )
        for path in sorted(missing):
            print(f"  skipped: {path}", file=sys.stderr)
    if filters:
        prefixes = []
        for value in filters:
            try:
                prefix = (cwd / value).resolve().relative_to(root).as_posix()
            except ValueError as exc:
                raise LintError(f"path filter escapes the repository: {value}") from exc
            prefixes.append("" if prefix == "." else prefix)
        selected = {
            path
            for path in selected
            if any(not prefix or path == prefix or path.startswith(prefix + "/") for prefix in prefixes)
        }
    if not selected:
        raise LintError("selection contains no lintable translation units; check path filters and build directory")
    return sorted(selected)


class ProcessRegistry:
    """Track running processes and prevent new work after cancellation."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._processes: set[subprocess.Popen[str]] = set()
        self._stopped = False

    def run(self, path: str, argv: Sequence[str], root: Path, *, live: bool = False) -> LintResult:
        with self._lock:
            if self._stopped:
                raise LintError("execution cancelled")
            process = subprocess.Popen(
                argv,
                cwd=root,
                stdout=None if live else subprocess.PIPE,
                stderr=None if live else subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                start_new_session=os.name == "posix",
            )
            self._processes.add(process)
        try:
            stdout, stderr = process.communicate()
            return LintResult(path, process.returncode, stdout or "", stderr or "")
        finally:
            # Keep an interrupted live child registered for the caller's cleanup.
            if process.poll() is not None:
                with self._lock:
                    self._processes.discard(process)

    def terminate_all(self) -> None:
        with self._lock:
            self._stopped = True
            processes = list(self._processes)
        for process in processes:
            with contextlib.suppress(ProcessLookupError):
                if os.name == "posix":
                    os.killpg(process.pid, signal.SIGTERM)
                else:
                    process.terminate()
        deadline = time.monotonic() + 2
        for process in processes:
            try:
                process.wait(timeout=max(0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                with contextlib.suppress(ProcessLookupError):
                    if os.name == "posix":
                        os.killpg(process.pid, signal.SIGKILL)
                    else:
                        process.kill()
                process.wait()
        with self._lock:
            self._processes.difference_update(processes)


def find_executable(value: str, cwd: Path) -> Path:
    candidate = str(cwd / value) if Path(value).parent != Path(".") or value.startswith(".") else value
    found = shutil.which(candidate)
    if found is None:
        raise LintError(f"executable not found: {value}; install it on the system or specify its path")
    return Path(found).resolve()


def probe_tool(path: Path, root: Path, registry: ProcessRegistry, *, compiler: bool = False) -> Tool:
    result = registry.run(str(path), [str(path), "--version"], root)
    output = result.stdout + result.stderr
    match = re.search(r"\bversion\s+(\d+\.\d+\.\d+[\w.+-]*)", output, re.IGNORECASE)
    if result.returncode or match is None or (compiler and "clang version" not in output.lower()):
        raise LintError(f"cannot identify {'Clang compiler' if compiler else 'LLVM tool'} {path}: {output.strip()}")
    return Tool(path, match[1])


def resolve_tools(
    commands: Sequence[CompilationCommand],
    root: Path,
    registry: ProcessRegistry,
    args: argparse.Namespace,
) -> tuple[Tool, Tool | None]:
    compilers = sorted(
        {
            find_executable(command.arguments[0], command.directory)
            for command in commands
            if PurePosixPath(command.path).suffix.lower() in SOURCE_SUFFIXES
        }
    )
    if not compilers:
        raise LintError("compilation database has no C/C++ compiler")
    compiler_tools = [probe_tool(path, root, registry, compiler=True) for path in compilers]
    versions = {tool.version for tool in compiler_tools}
    if len(versions) != 1:
        raise LintError("compilation database uses different Clang versions: " + ", ".join(sorted(versions)))
    resolved = []
    requested = [("clang-tidy", args.clang_tidy)]
    if args.fix:
        requested.append(("clang-apply-replacements", args.clang_apply_replacements))
    for name, override in requested:
        if override:
            path = find_executable(override, Path.cwd())
        else:
            siblings = [compiler.parent / name for compiler in compilers]
            path = next(
                (candidate for candidate in siblings if candidate.is_file() and os.access(candidate, os.X_OK)), None
            )
            if path is None:
                path = find_executable(name, root)
        tool = probe_tool(path.resolve(), root, registry)
        if tool.version not in versions:
            raise LintError(
                f"{name} {tool.path} is version {tool.version}, but the build compiler is "
                f"{compiler_tools[0].path} ({compiler_tools[0].version}); "
                "use matching system LLVM tools and rebuild BMIs"
            )
        resolved.append(tool)
    for label, tool in [("compiler", tool) for tool in compiler_tools] + [("clang-tidy", resolved[0])]:
        print(f"{label}: {tool.path} ({tool.version})", flush=True)
    if args.fix:
        print(f"clang-apply-replacements: {resolved[1].path} ({resolved[1].version})", flush=True)
    return resolved[0], resolved[1] if args.fix else None


def rebuild(root: Path, build_dir: Path, jobs: int, registry: ProcessRegistry) -> None:
    if not (build_dir / "CMakeCache.txt").is_file():
        raise LintError(f"not a configured CMake build directory: {build_dir}; run cmake --preset <preset> first")
    argv = [str(find_executable("cmake", root)), "--build", str(build_dir), "--parallel", str(jobs)]
    print(f"Updating build artifacts: {shlex.join(argv)}", flush=True)
    result = registry.run("cmake", argv, root, live=True)
    if result.returncode:
        raise LintError(f"CMake build failed with exit {result.returncode}; lint requires current module BMIs")


def build_command(
    path: str,
    root: Path,
    build_dir: Path,
    tidy: Tool,
    excluded: Sequence[str],
    extra_args: Sequence[str] = (),
    export_fixes: Path | None = None,
) -> list[str]:
    argv = [str(tidy.path), path, "--quiet", f"-p={build_dir}", f"--config-file={root / '.clang-tidy'}"]
    argv.extend(f"--extra-arg-before={arg}" for arg in extra_args)
    if excluded:
        # Expand Git patterns once, then escape actual paths for LLVM's regex engine.
        expression = "|".join(re.escape(path) for path in sorted(excluded))
        argv.append(f"--exclude-header-filter=(^|.*/)({expression})$")
    if export_fixes is not None:
        argv.append(f"--export-fixes={export_fixes}")
    return argv


def print_result(result: LintResult) -> None:
    for output, stream in [(result.stdout, sys.stdout), (result.stderr, sys.stderr)]:
        if output:
            print(output, end="" if output.endswith("\n") else "\n", file=stream, flush=True)


def run_tidy(
    paths: Sequence[str],
    root: Path,
    build_dir: Path,
    tidy: Tool,
    excluded: Sequence[str],
    args: argparse.Namespace,
    registry: ProcessRegistry,
    replacements_dir: Path | None = None,
) -> list[LintResult]:
    label = "Collecting fixes" if replacements_dir else "Checking"
    print(f"{label}: {len(paths)} translation unit(s), {args.jobs} job(s).", flush=True)
    results = []
    futures = []
    console = Console(stderr=True)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        try:
            for index, path in enumerate(paths):
                fixes = replacements_dir / f"{index:06}.yaml" if replacements_dir else None
                argv = build_command(path, root, build_dir, tidy, excluded, args.clang_extra_arg_before, fixes)
                if args.verbose:
                    print(f"[{path}] $ {shlex.join(argv)}", flush=True)
                futures.append(pool.submit(registry.run, path, argv, root))
            with Progress(
                TextColumn(label), BarColumn(), MofNCompleteColumn(), console=console, disable=not console.is_terminal
            ) as progress:
                task = progress.add_task(label, total=len(futures))
                for future in as_completed(futures):
                    results.append(future.result())
                    progress.advance(task)
        except BaseException:
            for future in futures:
                future.cancel()
            registry.terminate_all()
            raise
    for result in sorted(results, key=lambda result: result.path):
        print_result(result)
    for result in results:
        output = result.stdout + result.stderr
        if result.returncode not in (0, 1) or (
            result.returncode == 1 and not re.search(r": (?:fatal )?(?:error|warning):", output)
        ):
            raise LintError(f"clang-tidy execution failed for {result.path} (exit {result.returncode})")
    if replacements_dir is not None:
        diagnosed = {result.path for result in results if result.returncode}
        for index, path in enumerate(paths):
            if path in diagnosed and not (replacements_dir / f"{index:06}.yaml").is_file():
                raise LintError(f"clang-tidy did not export diagnostics for {path}; no replacements applied")
    return results


def has_compiler_error(result: LintResult) -> bool:
    return any(
        re.search(r": (?:fatal )?error:", line) and ",-warnings-as-errors]" not in line
        for line in (result.stdout + result.stderr).splitlines()
    )


def run(argv: Sequence[str] | None = None, *, repository_root: Path | None = None, cwd: Path | None = None) -> int:
    args = build_parser().parse_args(argv)
    registry = ProcessRegistry()
    try:
        if args.jobs < 1:
            raise LintError("--jobs must be at least 1")
        if args.fix_errors and not args.fix:
            raise LintError("--fix-errors requires --fix")
        if not args.preset or args.preset in {".", ".."} or "/" in args.preset or "\\" in args.preset:
            raise LintError("--preset requires a preset name, not a directory path")
        root = (repository_root or Path(__file__).parent).resolve()
        repo = open_repository(root)
        excludes = load_excludes(root)
        if not (root / ".clang-tidy").is_file():
            raise LintError("missing .clang-tidy configuration")
        tracked = tracked_cpp_files(repo)
        for pattern in excludes:
            if not any(pattern.spec.match_file(path) for path in tracked):
                raise LintError(f"exclude pattern matched no tracked C/C++ files: {pattern.source}")
        excluded = sorted(tracked - apply_excludes(tracked, excludes))
        changed = changed_inputs(root, changed_repository_files(repo), excludes) if args.changed else None
        if changed is not None and not any(changed):
            print("No changed lintable source, header, module or lint configuration; nothing to check.")
            return 0
        build_dir = (root / "build" / args.preset).resolve()
        commands = load_compilation_database(root, build_dir)
        print(f"Preset: {args.preset}\nBuild directory: {build_dir}", flush=True)
        tidy, apply = resolve_tools(commands, root, registry, args)
        print(f"Extra compiler arguments: {shlex.join(args.clang_extra_arg_before) or '(none)'}", flush=True)
        rebuild(root, build_dir, args.jobs, registry)
        refreshed = load_compilation_database(root, build_dir)
        if refreshed != commands:
            tidy, apply = resolve_tools(refreshed, root, registry, args)
        commands = refreshed
        paths = select_sources(root, commands, tracked, excludes, changed, args.paths, (cwd or Path.cwd()).resolve())
        if args.fix:
            if args.fix_errors:
                print(
                    "WARNING: --fix-errors allows edits despite lint compiler errors; "
                    "build failures still stop execution.",
                    file=sys.stderr,
                )
            with tempfile.TemporaryDirectory(prefix="moss-clang-tidy-") as temporary:
                replacements = Path(temporary)
                results = run_tidy(paths, root, build_dir, tidy, excluded, args, registry, replacements)
                if not args.fix_errors and any(has_compiler_error(result) for result in results):
                    raise LintError(
                        "compiler errors prevented applying fixes; fix the build/extra arguments or use --fix-errors"
                    )
                assert apply is not None
                apply_argv = [str(apply.path), "--format", "--style=file", str(replacements)]
                if args.verbose:
                    print(f"$ {shlex.join(apply_argv)}", flush=True)
                result = registry.run("clang-apply-replacements", apply_argv, root)
                print_result(result)
                if result.returncode:
                    raise LintError(f"clang-apply-replacements failed with exit {result.returncode}")
            rebuild(root, build_dir, args.jobs, registry)
            # Re-read after CMake regeneration, but keep the requested file selection.
            commands = load_compilation_database(root, build_dir)
            if set(paths) - {command.path for command in commands}:
                raise LintError("selected translation units disappeared from the compilation database after fixes")
        results = run_tidy(paths, root, build_dir, tidy, excluded, args, registry)
        failures = [result for result in results if result.returncode]
        if failures:
            print(f"clang-tidy reported diagnostics in {len(failures)} translation unit(s):", file=sys.stderr)
            for result in sorted(failures, key=lambda result: result.path):
                print(f"  {result.path} (exit {result.returncode})", file=sys.stderr)
            return 1
        print(f"Successfully checked {len(paths)} translation unit(s).")
        return 0
    except KeyboardInterrupt:
        registry.terminate_all()
        print("clang-tidy interrupted", file=sys.stderr)
        return 130
    except (LintError, OSError, pygit2.GitError) as exc:
        registry.terminate_all()
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(run())
