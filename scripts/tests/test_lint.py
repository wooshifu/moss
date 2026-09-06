"""Repository selection and process-level contracts for the lint entrypoint."""

from __future__ import annotations

import json
import os
import shlex
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import pygit2
import pytest

import lint


def write(root: Path, path: str, content: str = "\n") -> Path:
    target = root / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content, encoding="utf-8")
    return target


def commit(repo: pygit2.Repository) -> pygit2.Oid:
    repo.index.add_all()
    repo.index.write()
    author = pygit2.Signature("Lint Test", "lint@example.com")
    parents = [] if repo.head_is_unborn else [repo.head.target]
    return repo.create_commit("HEAD", author, author, "fixture", repo.index.write_tree(), parents)


def database(root: Path, files: list[str], preset: str = "arm64-debug") -> Path:
    build = root / "build" / preset
    entries = [{"directory": str(root), "file": path, "arguments": ["clang++", "-c", path]} for path in files]
    write(build, "compile_commands.json", json.dumps(entries))
    write(build, "CMakeCache.txt")
    return build


@pytest.fixture
def project(tmp_path: Path) -> tuple[Path, pygit2.Repository]:
    root = (tmp_path / "repository with spaces").resolve()
    root.mkdir()
    repo = pygit2.init_repository(root, initial_head="master")
    write(root, ".gitignore", "build/\n")
    write(root, "lint.toml", 'exclude = ["src/vendor/**"]\n')
    write(
        root, ".clang-tidy", "Checks: '-*,modernize-use-nullptr'\nWarningsAsErrors: '*'\nHeaderFilterRegex: 'src/.*'\n"
    )
    for name in ["main.cpp", "other.cpp", "api.cppm", "api.hpp", "vendor/api.h"]:
        write(root, "src/" + name)
    commit(repo)
    database(root, ["src/main.cpp", "src/other.cpp", "src/api.cppm"])
    return root, repo


@pytest.fixture
def fake_tools(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    """Executable lookup is real; replace only the subprocess boundary."""
    tool_dir = tmp_path / "system llvm"
    for name in ["clang++", "clang-tidy", "clang-apply-replacements", "cmake"]:
        write(tool_dir, name).chmod(0o755)
    monkeypatch.setenv("PATH", str(tool_dir) + os.pathsep + os.environ.get("PATH", ""))
    calls = []
    outcomes = {}

    def run(_self, path, argv, _root, *, live=False):
        calls.append((path, tuple(argv), live))
        if "--version" in argv:
            label = "clang" if Path(argv[0]).name == "clang++" else Path(argv[0]).name
            return lint.LintResult(path, 0, f"{label} version 23.1.0\n")
        result = outcomes.get(path, lint.LintResult(path, 0))
        if isinstance(result, BaseException):
            raise result
        for arg in argv:
            if arg.startswith("--export-fixes="):
                Path(arg.split("=", 1)[1]).write_text("---\nDiagnostics: []\n", encoding="utf-8")
        return result

    monkeypatch.setattr(lint.ProcessRegistry, "run", run)
    return tool_dir, calls, outcomes


def test_full_selection_uses_index_and_database_and_keeps_cppm(project, fake_tools, capsys):
    root, _repo = project
    write(root, "src/untracked.cpp")
    build = database(root, ["src/main.cpp", "src/api.cppm", "src/untracked.cpp"], preset="x86_64-debug")
    _tools, calls, _outcomes = fake_tools
    assert lint.run(["--check", "--preset", "x86_64-debug", "-j", "2"], repository_root=root, cwd=root) == 0
    checked = {path for path, _argv, _live in calls if path.startswith("src/")}
    assert checked == {"src/main.cpp", "src/api.cppm"}
    assert "skipped: src/other.cpp" in capsys.readouterr().err
    command = next(argv for path, argv, _live in calls if path == "src/main.cpp")
    assert f"-p={build}" in command
    build_command = next(argv for path, argv, _live in calls if path == "cmake")
    assert build_command[1:4] == ("--build", str(build), "--parallel")
    assert any(arg.startswith("--exclude-header-filter=") and "vendor" in arg for arg in command)


def test_changed_includes_committed_staged_unstaged_and_untracked(project, fake_tools):
    root, repo = project
    repo.create_reference("refs/remotes/origin/master", repo.head.target)
    write(root, "src/main.cpp", "// committed\n")
    commit(repo)
    write(root, "src/other.cpp", "// staged\n")
    repo.index.add("src/other.cpp")
    repo.index.write()
    write(root, "src/unstaged.cpp")
    repo.index.add("src/unstaged.cpp")
    repo.index.write()
    write(root, "src/unstaged.cpp", "// unstaged\n")
    write(root, "src/new.cpp")
    files = ["src/main.cpp", "src/other.cpp", "src/unstaged.cpp", "src/new.cpp"]
    database(root, [*files, "src/api.cppm"])
    assert lint.run(["--check", "--changed"], repository_root=root, cwd=root) == 0
    assert {path for path, _, _live in fake_tools[1] if path.startswith("src/")} == set(files)


@pytest.mark.parametrize("path", ["src/api.hpp", "src/api.cppm", ".clang-tidy", "lint.toml"])
def test_changed_wide_inputs_check_all_tus(project, fake_tools, path):
    root, _repo = project
    target = root / path
    target.write_text(target.read_text() + "\n", encoding="utf-8")
    assert lint.run(["--check", "--changed"], repository_root=root, cwd=root) == 0
    assert {name for name, _, _live in fake_tools[1] if name.startswith("src/")} == {
        "src/main.cpp",
        "src/other.cpp",
        "src/api.cppm",
    }


@pytest.mark.parametrize("path", ["src/api.hpp", "src/api.cppm"])
def test_deleted_wide_inputs_still_check_importers(project, fake_tools, path):
    root, _repo = project
    (root / path).unlink()
    database(root, ["src/main.cpp", "src/other.cpp"])
    assert lint.run(["--check", "--changed"], repository_root=root, cwd=root) == 0
    assert {name for name, _, _live in fake_tools[1] if name.startswith("src/")} == {"src/main.cpp", "src/other.cpp"}


def test_no_changes_and_deleted_source_need_no_build(project, fake_tools, capsys):
    root, _repo = project
    for delete in [False, True]:
        if delete:
            (root / "src/main.cpp").unlink()
        assert lint.run(["--check", "--changed"], repository_root=root, cwd=root) == 0
    assert fake_tools[1] == []
    assert "origin/master unavailable" in capsys.readouterr().out


def test_changed_source_without_database_entry_fails(project, fake_tools, capsys):
    root, _repo = project
    write(root, "src/new.cpp")
    assert lint.run(["--check", "--changed"], repository_root=root, cwd=root) == 2
    assert "missing from compilation database: src/new.cpp" in capsys.readouterr().err


@pytest.mark.parametrize("pattern", ["!src/main.cpp", "/src/main.cpp", "src\\main.cpp", "", "src/typo/**"])
def test_invalid_or_unmatched_exclusion_fails_before_tools(project, fake_tools, pattern):
    root, _repo = project
    write(root, "lint.toml", "exclude = " + json.dumps([pattern]))
    assert lint.run(["--check"], repository_root=root, cwd=root) == 2
    assert fake_tools[1] == []


def test_subdirectory_filters_and_outside_paths(project, fake_tools):
    root, _repo = project
    assert lint.run(["--check", "main.cpp"], repository_root=root, cwd=root / "src") == 0
    assert {path for path, _, _live in fake_tools[1] if path.startswith("src/")} == {"src/main.cpp"}
    assert lint.run(["--check", ".."], repository_root=root, cwd=root) == 2


def test_database_preserves_duplicate_commands_and_response_files(project):
    root, _repo = project
    build = root / "build/arm64-debug"
    command = ["/system llvm/clang++", "--target=aarch64-unknown-elf", "@module.modmap", "-c", "../../src/api.cppm"]
    entry = {"directory": str(build), "file": "../../src/api.cppm", "command": shlex.join(command)}
    write(build, "compile_commands.json", json.dumps([entry, {**entry, "arguments": command, "command": "ignored"}]))
    commands = lint.load_compilation_database(root, build)
    assert len(commands) == 2
    assert all(item.path == "src/api.cppm" and item.arguments == tuple(command) for item in commands)


@pytest.mark.parametrize("entries", [None, [], [{}], [{"file": "src/a.cpp", "directory": ".", "command": []}], [42]])
def test_invalid_database_is_configuration_error(project, entries):
    root, _repo = project
    build = root / "build/arm64-debug"
    write(build, "compile_commands.json", json.dumps(entries))
    with pytest.raises(lint.LintError):
        lint.load_compilation_database(root, build)


def test_database_rejects_repository_escape(project):
    root, _repo = project
    build = database(root, ["../outside.cpp"])
    with pytest.raises(lint.LintError, match="escapes"):
        lint.load_compilation_database(root, build)


def test_tool_priority_and_check_does_not_require_apply(project, fake_tools, monkeypatch):
    root, _repo = project
    tool_dir, _calls, _outcomes = fake_tools
    alternate = write(root.parent, "alternate/clang-tidy")
    alternate.chmod(0o755)
    monkeypatch.setenv("PATH", str(alternate.parent) + os.pathsep + os.environ["PATH"])
    # Resolve the compiler explicitly so its sibling takes precedence over PATH.
    commands = [lint.CompilationCommand("src/api.cppm", root, (str(tool_dir / "clang++"), "-c"))]
    args = lint.build_parser().parse_args(["--check"])
    registry = lint.ProcessRegistry()
    (tool_dir / "clang-apply-replacements").unlink()
    tidy, apply = lint.resolve_tools(commands, root, registry, args)
    assert tidy.path == tool_dir / "clang-tidy"
    assert apply is None
    args.clang_tidy = str(alternate)
    assert lint.resolve_tools(commands, root, registry, args)[0].path == alternate
    args.clang_tidy = None
    (tool_dir / "clang-tidy").unlink()
    assert lint.resolve_tools(commands, root, registry, args)[0].path == alternate


def test_mismatched_tool_version_fails_before_build(project, fake_tools, monkeypatch):
    root, _repo = project
    original = lint.probe_tool

    def probe(path, root, registry, *, compiler=False):
        tool = original(path, root, registry, compiler=compiler)
        return tool if compiler else lint.Tool(tool.path, "22.1.0")

    monkeypatch.setattr(lint, "probe_tool", probe)
    assert lint.run(["--check"], repository_root=root, cwd=root) == 2
    assert not any(path == "cmake" for path, _, _live in fake_tools[1])


@pytest.mark.parametrize("mode", ["--check", "--fix"])
def test_build_failure_prevents_analysis(project, fake_tools, mode):
    root, _repo = project
    fake_tools[2]["cmake"] = lint.LintResult("cmake", 1)
    assert lint.run([mode], repository_root=root, cwd=root) == 2
    assert not any(path.startswith("src/") for path, _, _live in fake_tools[1])


def test_reloads_database_after_incremental_build(project, fake_tools, monkeypatch):
    root, _repo = project
    write(root, "src/new.cpp")
    original = lint.rebuild

    def rebuild(root, build_dir, jobs, registry):
        original(root, build_dir, jobs, registry)
        database(root, ["src/new.cpp"])

    monkeypatch.setattr(lint, "rebuild", rebuild)
    assert lint.run(["--check", "--changed"], repository_root=root, cwd=root) == 0
    assert any(path == "src/new.cpp" for path, _, _live in fake_tools[1])


def test_fix_collects_then_applies_once_then_rebuilds_and_rechecks(project, fake_tools):
    root, _repo = project
    assert lint.run(["--fix"], repository_root=root, cwd=root) == 0
    calls = [(path, argv) for path, argv, _live in fake_tools[1] if "--version" not in argv]
    assert len(calls) == 9
    assert [calls[index][0] for index in (0, 4, 5)] == ["cmake", "clang-apply-replacements", "cmake"]
    expected = {"src/api.cppm", "src/main.cpp", "src/other.cpp"}
    assert {path for path, _ in calls[1:4]} == expected
    assert {path for path, _ in calls[6:]} == expected
    exports = [arg for _, argv in calls for arg in argv if arg.startswith("--export-fixes=")]
    assert len(set(exports)) == 3
    assert all(not Path(arg.split("=", 1)[1]).exists() for arg in exports)
    assert not any("--fix" in argv for _, argv in calls)


@pytest.mark.parametrize("returncode,output", [(-11, "crashed"), (2, "bad arguments"), (1, "tool failed")])
def test_tool_failures_never_apply_fixes(project, fake_tools, returncode, output):
    root, _repo = project
    fake_tools[2]["src/main.cpp"] = lint.LintResult("src/main.cpp", returncode, output)
    assert lint.run(["--fix", "--fix-errors"], repository_root=root, cwd=root) == 2
    assert not any(path == "clang-apply-replacements" for path, _, _live in fake_tools[1])


def test_compiler_errors_require_opt_in_but_final_check_still_fails(project, fake_tools):
    root, _repo = project
    fake_tools[2]["src/main.cpp"] = lint.LintResult(
        "src/main.cpp", 1, "main.cpp:1:1: error: broken [clang-diagnostic-error]\n"
    )
    assert lint.run(["--fix"], repository_root=root, cwd=root) == 2
    assert not any(path == "clang-apply-replacements" for path, _, _live in fake_tools[1])
    assert lint.run(["--fix", "--fix-errors"], repository_root=root, cwd=root) == 1
    assert any(path == "clang-apply-replacements" for path, _, _live in fake_tools[1])


def test_remaining_lint_diagnostics_fail_final_check(project, fake_tools):
    root, _repo = project
    fake_tools[2]["src/main.cpp"] = lint.LintResult(
        "src/main.cpp", 1, "main.cpp:1:1: error: braces [readability-braces-around-statements,-warnings-as-errors]\n"
    )
    assert lint.run(["--fix"], repository_root=root, cwd=root) == 1
    assert any(path == "clang-apply-replacements" for path, _, _live in fake_tools[1])


def test_apply_failure_prevents_rebuild_and_recheck(project, fake_tools):
    root, _repo = project
    fake_tools[2]["clang-apply-replacements"] = lint.LintResult("apply", 1, "conflicting replacements")
    assert lint.run(["--fix"], repository_root=root, cwd=root) == 2
    assert sum(path == "cmake" for path, _, _live in fake_tools[1]) == 1


def test_missing_export_prevents_partial_fix_application(project, fake_tools, monkeypatch):
    root, _repo = project
    original = lint.ProcessRegistry.run

    def run(self, path, argv, root, *, live=False):
        result = original(self, path, argv, root, live=live)
        if path == "src/main.cpp":
            for arg in argv:
                if arg.startswith("--export-fixes="):
                    Path(arg.split("=", 1)[1]).unlink()
            return lint.LintResult(path, 1, "main.cpp:1:1: error: braces [readability-braces-around-statements]\n")
        return result

    monkeypatch.setattr(lint.ProcessRegistry, "run", run)
    assert lint.run(["--fix", "--fix-errors"], repository_root=root, cwd=root) == 2
    assert not any(path == "clang-apply-replacements" for path, _, _live in fake_tools[1])


def test_post_fix_build_failure_prevents_final_check(project, fake_tools, monkeypatch):
    root, _repo = project
    original = lint.rebuild
    builds = []

    def rebuild(root, build_dir, jobs, registry):
        builds.append(build_dir)
        if len(builds) == 2:
            raise lint.LintError("post-fix build failed")
        original(root, build_dir, jobs, registry)

    monkeypatch.setattr(lint, "rebuild", rebuild)
    assert lint.run(["--fix"], repository_root=root, cwd=root) == 2
    assert sum(path.startswith("src/") for path, _, _live in fake_tools[1]) == 3


def test_keyboard_interrupt_returns_130(project, fake_tools):
    root, _repo = project
    fake_tools[2]["cmake"] = KeyboardInterrupt()
    assert lint.run(["--check"], repository_root=root, cwd=root) == 130


@pytest.mark.skipif(os.name != "posix", reason="POSIX process-group cancellation")
def test_interrupt_terminates_running_tool_and_children(tmp_path):
    # Run the registry in its own interpreter so SIGINT exercises real cleanup.
    pidfile = tmp_path / "pids"
    parent_code = "import subprocess, sys, time; from pathlib import Path; " + (
        "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)']); "
        f"Path({str(pidfile)!r}).write_text(str(child.pid)); time.sleep(60)"
    )
    code = (
        "import lint, sys\nfrom pathlib import Path\nr = lint.ProcessRegistry()\n"
        f"try:\n r.run('test', [sys.executable, '-c', {parent_code!r}], Path.cwd())\n"
        "except KeyboardInterrupt:\n r.terminate_all(); raise SystemExit(130)\n"
    )
    process = subprocess.Popen([sys.executable, "-c", code])
    try:
        deadline = time.monotonic() + 10
        while not pidfile.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        assert pidfile.exists()
        child = int(pidfile.read_text())
        process.send_signal(signal.SIGINT)
        assert process.wait(timeout=10) == 130
        # A terminated, not-yet-reaped child may briefly appear as a zombie.
        state = subprocess.run(["ps", "-o", "stat=", "-p", str(child)], capture_output=True, text=True, check=False)
        assert not state.stdout.strip() or state.stdout.strip().startswith("Z")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def test_format_cli_keeps_both_default_and_explicit_forms(monkeypatch):
    from typer.testing import CliRunner

    from scripts import format as formatter

    monkeypatch.setattr(formatter, "git_tracked_files", lambda: [])
    result = CliRunner().invoke(formatter.app, ["format", "--check"])
    assert result.exit_code == 0, result.output
    help_result = CliRunner().invoke(formatter.app, ["--help"])
    assert help_result.exit_code == 0
    assert "lint " not in help_result.output


def test_real_cppm_fix_rebuild_and_header_exclusion(tmp_path):
    """Build a tiny freestanding module project with real system CMake/LLVM."""
    compiler = shutil.which("clang++")
    if compiler is None or shutil.which("cmake") is None or shutil.which("ninja") is None:
        pytest.skip("system Clang, CMake and Ninja required")
    if any(shutil.which(name) is None for name in ["clang-tidy", "clang-apply-replacements"]):
        pytest.skip("system clang-tidy and clang-apply-replacements required")
    root = (tmp_path / "real modules").resolve()
    root.mkdir()
    repo = pygit2.init_repository(root, initial_head="master")
    write(root, ".gitignore", "build/\n")
    write(root, "lint.toml", 'exclude = ["src/vendor/**"]\n')
    write(root, ".clang-tidy", "Checks: '-*,modernize-use-nullptr'\nWarningsAsErrors: '*'\nHeaderFilterRegex: '.*'\n")
    write(root, ".clang-format", "BasedOnStyle: LLVM\n")
    vendor = write(root, "src/vendor/api.h", "inline int* vendor_pointer() { return 0; }\n")
    module = write(
        root,
        "src/api.cppm",
        'module;\n#include "vendor/api.h"\nexport module api;\nexport inline int* pointer() { return 0; }\n',
    )
    write(root, "src/main.cpp", "import api;\nint* use_pointer() { return pointer(); }\n")
    write(
        root,
        "CMakeLists.txt",
        """cmake_minimum_required(VERSION 3.31)
project(lint_fixture LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_library(example OBJECT)
target_compile_options(example PRIVATE -ffreestanding)
target_sources(example PRIVATE src/main.cpp PUBLIC FILE_SET CXX_MODULES FILES src/api.cppm)
""",
    )
    commit(repo)
    build = root / "build/arm64-debug"
    result = subprocess.run(
        ["cmake", "-S", str(root), "-B", str(build), "-G", "Ninja", f"-DCMAKE_CXX_COMPILER={compiler}"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert lint.run(["--check", "-j", "2"], repository_root=root, cwd=root) == 1
    assert "return 0" in module.read_text()
    vendor_before = vendor.read_bytes()
    assert lint.run(["--fix", "-j", "2"], repository_root=root, cwd=root) == 0
    assert "return nullptr" in module.read_text()
    assert vendor.read_bytes() == vendor_before
    # Add an invalid importer assertion after a module change. The entrypoint must
    # rebuild and reject it rather than checking against the previously built BMI.
    write(root, "src/api.cppm", module.read_text() + "\nexport constexpr int value = 2;\n")
    write(root, "src/main.cpp", "import api;\nstatic_assert(value == 1);\n")
    assert lint.run(["--check", "-j", "2"], repository_root=root, cwd=root) == 2
