import copy
import hashlib
import json
import subprocess
import sys

import pytest

from scripts import performance_gate as pg
from scripts.kernel_validation import BENCHMARK_KINDS
from scripts.tests.test_kernel_validation import benchmark_report


def report(scale, source="baseline"):
    value = benchmark_report()
    source = hashlib.sha256(source.encode()).hexdigest()
    value["provenance"] = dict(
        source_status="recorded", revision=source, source_sha256=source, image_sha256=source, fixture_sha256="0" * 64
    )
    value["comparison_environment"].update(
        build={"type": "Release", "compiler": "test compiler", "flags": {"compile": "-Os", "link": ""}},
        qemu="test QEMU",
        host={"system": "test"},
        accelerator="tcg",
        clock_policy=1,
        fixture_sha256="0" * 64,
        dtb_sha256=None,
        machine="virt",
        cpu_model="test CPU",
    )
    value["requested"] = list(pg.BENCHMARKS)
    template = value["guests"].pop()
    for name in pg.BENCHMARKS:
        guest = copy.deepcopy(template)
        guest["workload"] = name
        guest["parameters"] = {"order": 0}
        for record in [guest["ready"], guest["worker"], guest["clock"], guest["completion"], *guest["batches"]]:
            record["workload"] = name
        guest["cases"][0]["name"] = name
        guest["cases"][0]["assertions"].update(workload=name, case=name)
        for batch in guest["batches"]:
            batch["ticks"] = round(batch["ticks"] * scale)
            if name in BENCHMARK_KINDS:
                batch["measurement_kind"] = BENCHMARK_KINDS[name]
        value["guests"].append(guest)
    return value


def inputs(scales=(100.1, 100.2, 100.3)):
    return [report(i) for i in (100, 100.5, 101, 101.5, 102)], [report(i, "candidate") for i in scales]


@pytest.mark.parametrize(
    "scales,status",
    [((100.1, 100.2, 100.3), "passed"), ((120, 121, 122), "regression"), ((101.1, 104, 105), "incomplete")],
)
def test_empirical_gate_requires_consistent_repeats(scales, status):
    result = pg.gate(*inputs(scales), 0.1)
    assert result["status"] == status
    assert len(result["scenarios"]) == len(pg.BENCHMARKS)


@pytest.mark.parametrize(
    "damage",
    [
        "missing_noise",
        "excessive_noise",
        "few",
        "duplicate",
        "partial_duplicate",
        "source",
        "missing",
        "environment",
        "invalid",
        "unrecorded",
        "debug",
        "invalid_noise",
        "null_environment",
        "null_guest",
        "null_provenance",
        "missing_environment",
        "missing_parameters",
        "fixture_mismatch",
        "invalid_parameters",
        "missing_flags",
    ],
)
def test_missing_or_untrustworthy_evidence_cannot_pass(damage):
    baseline, current = inputs()
    limit = 0.1
    if damage == "missing_noise":
        limit = None
    elif damage == "excessive_noise":
        limit = 0.001
    elif damage == "few":
        baseline.pop()
    elif damage == "duplicate":
        current[-1] = copy.deepcopy(current[0])
    elif damage == "partial_duplicate":
        current[-1]["guests"][0] = copy.deepcopy(current[0]["guests"][0])
    elif damage == "source":
        current[0]["provenance"]["image_sha256"] = "other"
    elif damage == "missing":
        current[0]["guests"].pop()
    elif damage == "environment":
        current[0]["comparison_environment"]["settings"]["cpus"] = 8
    elif damage == "invalid":
        current[0]["guests"][0]["batches"].pop()
    elif damage == "debug":
        current[0]["comparison_environment"]["build"]["type"] = "Debug"
    elif damage == "invalid_noise":
        limit = float("nan")
    elif damage == "null_environment":
        current[0]["comparison_environment"] = None
    elif damage == "null_guest":
        current[0]["guests"][0] = None
    elif damage == "null_provenance":
        current[0]["provenance"] = None
    elif damage == "missing_environment":
        current[0]["comparison_environment"].pop("host")
    elif damage == "missing_parameters":
        current[0]["guests"][0].pop("parameters")
    elif damage == "fixture_mismatch":
        current[0]["comparison_environment"]["fixture_sha256"] = "1" * 64
    elif damage == "invalid_parameters":
        current[0]["guests"][0]["parameters"]["order"] = True
    elif damage == "missing_flags":
        current[0]["comparison_environment"]["build"]["flags"] = {}
    else:
        current[0]["provenance"]["source_status"] = "unrecorded"
    assert pg.gate(baseline, current, limit)["status"] == "incomplete"


@pytest.mark.parametrize(
    "scale,status,exit_code",
    [(100.1, "passed", 0), (120, "regression", 1), (104, "incomplete", 2), (None, "incomplete", 2)],
)
def test_cli_exit_status_and_retained_inputs(tmp_path, scale, status, exit_code):
    damaged = scale is None
    scale = 100.1 if damaged else scale
    baseline, current = inputs((scale, scale + 0.1, scale + 0.2))
    if damaged:
        current[0]["comparison_environment"] = None
    args = [sys.executable, "-m", "scripts.performance_gate", "--output", str(tmp_path / "gate.json")]
    if status != "incomplete":
        args += ["--max-noise", "0.1"]
    for label, group in (("baseline", baseline), ("current", current)):
        for index, value in enumerate(group):
            path = tmp_path / f"{label}-{index}.json"
            path.write_text(json.dumps(value))
            args += [f"--{label}", str(path)]
    run = subprocess.run(args, capture_output=True, text=True, timeout=10)
    assert run.returncode == exit_code, run.stderr
    output = (tmp_path / "gate.json").read_bytes()
    result = json.loads(output)
    assert result["status"] == status
    assert len(result["reports"]) == 8
    for item in result["reports"]:
        assert len(item["sha256"]) == 64
    # A rerun may not overwrite the previous acceptance evidence.
    assert subprocess.run(args, capture_output=True, timeout=10).returncode == 2
    assert (tmp_path / "gate.json").read_bytes() == output
