"""Exercise preset concurrency through the real CLI and worker processes."""

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

BUILD_SCRIPT = Path(__file__).resolve().parents[2] / "build.py"
PRESETS = ["arm64-debug", "x64-debug", "riscv64-debug"]


@pytest.fixture
def build_cli(tmp_path):
    (tmp_path / "CMakeLists.txt").touch()
    (tmp_path / "CMakePresets.json").write_text("{}")
    fake_cmake = tmp_path / "cmake"
    fake_cmake.write_text(
        f"#!{sys.executable}\n"
        + """import json
import os
import sys
import time
from pathlib import Path

presets = ["arm64-debug", "x64-debug", "riscv64-debug"]
if "--list-presets" in sys.argv:
    for preset in presets:
        print(f'  "{preset}"')
    sys.exit(0)
preset = sys.argv[-1]
start = time.monotonic()
Path(preset + ".started").touch()
deadline = start + 10
while len(list(Path(".").glob("*.started"))) < int(os.environ["EXPECTED_PARALLEL"]):
    if time.monotonic() > deadline:
        sys.exit(8)
    time.sleep(0.01)
time.sleep(0.15)
Path(preset + ".json").write_text(json.dumps({
    "start": start, "end": time.monotonic(), "pid": os.getpid(), "worker": os.getppid()
}))
print("stdout " + preset)
print("stderr " + preset, file=sys.stderr)
sys.exit(7 if preset == os.environ.get("FAIL_PRESET") else 0)
"""
    )
    fake_cmake.chmod(0o755)

    def run(*args, parallel=1, fail=""):
        return subprocess.run(
            [sys.executable, str(BUILD_SCRIPT), *args],
            cwd=tmp_path,
            env={
                **os.environ,
                "PATH": str(tmp_path) + os.pathsep + os.environ["PATH"],
                "EXPECTED_PARALLEL": str(parallel),
                "FAIL_PRESET": fail,
            },
            capture_output=True,
            text=True,
            timeout=30,
        )

    return tmp_path, run


@pytest.mark.parametrize(("args", "parallel"), [((), 3), (("--jobs", "1"), 1), (("-j", "2"), 2), (("-j", "9"), 3)])
def test_concurrency_limit(build_cli, args, parallel):
    root, run = build_cli
    result = run(*args, parallel=parallel)
    assert result.returncode == 0, result.stdout + result.stderr
    records = [json.loads((root / (preset + ".json")).read_text()) for preset in PRESETS]
    events = sorted([(r["start"], 1) for r in records] + [(r["end"], -1) for r in records])
    active = peak = 0
    for _, delta in events:
        active += delta
        peak = max(peak, active)
    assert peak == parallel
    assert len({r["pid"] for r in records}) == 3
    assert len({r["worker"] for r in records}) == parallel


@pytest.mark.parametrize("verbose", [False, True])
def test_failure_keeps_other_presets_and_diagnostics(build_cli, verbose):
    root, run = build_cli
    result = run(*(["--verbose"] if verbose else []), parallel=3, fail=PRESETS[0])
    assert result.returncode == 1
    assert all((root / (preset + ".json")).exists() for preset in PRESETS)
    assert "stdout " + PRESETS[0] in result.stdout
    assert "stderr " + PRESETS[0] in result.stdout
    if verbose:
        assert all("stdout " + preset in result.stdout for preset in PRESETS)


def test_filter_and_dry_run(build_cli):
    root, run = build_cli
    result = run("--arch", "arm64", "--jobs", "2")
    assert result.returncode == 0, result.stdout + result.stderr
    assert (root / "arm64-debug.json").exists()
    assert not (root / "x64-debug.json").exists()
    (root / "arm64-debug.json").unlink()
    result = run("--dry-run", "--clean", "--jobs", "2")
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.count("cmake --workflow --preset") == 3
    assert not any(root.glob("*-debug.json"))


@pytest.mark.parametrize("jobs", ["0", "-1", "invalid"])
def test_invalid_jobs(build_cli, jobs):
    root, run = build_cli
    result = run("--jobs", jobs)
    assert result.returncode == 2
    assert not any(root.glob("*.started"))
