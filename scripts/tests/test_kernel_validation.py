"""Host-tool tests only; real kernel acceptance is run separately in QEMU."""

import copy
import json
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

from scripts import kernel_validation as kv
from scripts.artifacts import Artifacts

LIFECYCLE_RESOURCES = dict(
    heap_bytes=4096,
    free_pages=100000,
    processes=1,
    threads=1,
    descriptors=3,
    file_refs=3,
    user_pages=8,
    stack_pages=2,
    vfs_inodes=30,
    vfs_dentries=30,
    vfs_files=3,
)


def event(workload, event_type, **fields):
    return dict(v=1, event=event_type, workload=workload, **fields)


def emit(state, event_type, **fields):
    state.accept(b"@@MOSS " + json.dumps(event(state.workload, event_type, **fields)).encode())


def ready(workload="mm", warmup=1, samples=2, stability=False):
    state = kv.Protocol(workload, 4, 2048, warmup, samples, stability)
    emit(state, "ready", detected_cpus=4, online_mask=15, work_mask=15, ram_bytes=2**31, managed_pages=500000)
    for name in kv.CATALOG[workload]:
        emit(state, "catalog", case=name)
    emit(state, "worker", cpu=0, affinity=1)
    return state


def finish(state, failed=False):
    for name in state.expected:
        emit(state, "case_start", case=name)
        emit(state, "case_end", case=name, passed=1, failed=int(failed))
        if failed:
            break
    emit(state, "end", completed=len(state.cases), selected=len(state.expected), failed=int(failed))


def benchmark_report():
    state = ready("bench.allocate")
    emit(state, "case_start", case=state.workload)
    emit(state, "clock", source="cntfrq_el0", frequency=1000000, uncertainty_ppm=0)
    for i in range(3):
        emit(state, "batch", index=i, ticks=100 + i, operations=4, warmup=int(i == 0), cpu=0, overhead_ticks=1)
    emit(state, "case_end", case=state.workload, passed=0, failed=0)
    emit(state, "end", completed=1, selected=1, failed=0)
    return {
        "schema_version": 2,
        "finalized": True,
        "requested": [state.workload],
        "not_run": [],
        "comparison_environment": {"arch": "ARM64", "settings": dict(cpus=4, memory_mib=2048, warmup=1, samples=2)},
        "provenance": {"revision": "old"},
        "guests": [
            dict(
                workload=state.workload,
                version=1,
                status="passed",
                observed="pass",
                expected="pass",
                ready=state.ready,
                worker=state.worker,
                clock=state.clock,
                calibration=[],
                batches=state.batches,
                cases=state.cases,
                completion=state.end,
                raw_exit=0,
                termination="protocol_end",
                parameters={},
            )
        ],
    }


def test_required_performance_catalog_covers_all_core_paths():
    assert set(kv.BENCHMARKS) == {
        f"bench.{name}"
        for name in (
            "allocate",
            "release",
            "combined",
            "read",
            "getpid",
            "fault",
            "cow",
            "switch",
            "wakeup",
            "timer",
            "lifecycle",
            "signal",
            "pipe",
        )
    }


def test_latency_batches_require_explicit_measurement_semantics():
    state = ready("bench.signal")
    emit(state, "case_start", case=state.workload)
    emit(state, "clock", source="cntfrq_el0", frequency=1000000, uncertainty_ppm=0)
    with pytest.raises(ValueError, match="measurement kind"):
        emit(state, "batch", index=0, ticks=100, operations=4, warmup=1, cpu=0, overhead_ticks=1)
    emit(
        state,
        "batch",
        index=0,
        ticks=100,
        operations=4,
        warmup=1,
        cpu=0,
        overhead_ticks=1,
        measurement_kind="event_sum",
    )


def test_full_functional_completion_and_exit_are_both_required():
    state = ready()
    assert state.outcome("protocol_end", None, b"PASS")[0] == "error"
    finish(state)
    assert state.outcome("protocol_end", None, b"") == ("passed", "pass")
    assert state.outcome("process_exit", None, b"")[0] == "error"
    assert state.outcome("protocol_end", None, b"[P] allocator corruption") == ("error", "unexpected_kernel_panic")
    with pytest.raises(ValueError):
        emit(state, "end", completed=2, selected=2, failed=0)


@pytest.mark.parametrize(
    "field", ["application_cycles", "user_pages", "stack_pages", "vfs_inodes", "vfs_dentries", "vfs_files"]
)
def test_lifecycle_requires_application_and_pool_evidence(field):
    state = ready("users.lifecycle")
    case = state.expected[0]
    emit(state, "case_start", case=case)
    fields = LIFECYCLE_RESOURCES | {"application_cycles": 0}
    del fields[field]
    with pytest.raises(ValueError, match=field):
        emit(state, "checkpoint", case=case, cycles=0, elapsed_ns=0, **fields)


@pytest.mark.parametrize("application_cycles", [None, -1, True, 9, 11])
def test_lifecycle_rejects_unmatched_application_cycles(application_cycles):
    state = ready("users.applications")
    case = state.expected[0]
    emit(state, "case_start", case=case)
    emit(state, "checkpoint", case=case, cycles=0, application_cycles=0, elapsed_ns=0, **LIFECYCLE_RESOURCES)
    with pytest.raises(ValueError, match="application_cycles"):
        emit(
            state,
            "checkpoint",
            case=case,
            cycles=10,
            application_cycles=application_cycles,
            elapsed_ns=10**9,
            **LIFECYCLE_RESOURCES,
        )


@pytest.mark.parametrize("mode", ["complete", "missing", "duplicate", "wrong_case", *LIFECYCLE_RESOURCES])
@pytest.mark.parametrize("workload", ["users.lifecycle", "users.applications"])
def test_lifecycle_requires_ordered_resource_recovery_evidence(mode, workload):
    state = ready(workload)
    interval = kv.LIFECYCLE_INTERVAL[workload]
    case = state.expected[0]
    emit(state, "case_start", case=case)
    fields = LIFECYCLE_RESOURCES
    if mode in ("duplicate", "wrong_case"):
        emit(state, "checkpoint", case=case, cycles=0, application_cycles=0, elapsed_ns=0, **fields)
        with pytest.raises(ValueError):
            emit(
                state,
                "checkpoint",
                case="wrong" if mode == "wrong_case" else case,
                cycles=0,
                application_cycles=0,
                elapsed_ns=0,
                **fields,
            )
        return
    for cycle in range(0, 1001, interval):
        if mode == "missing" and cycle == 1000:
            break
        emit(
            state,
            "checkpoint",
            case=case,
            cycles=cycle,
            application_cycles=cycle if workload == "users.applications" else 0,
            elapsed_ns=cycle * 10**6,
            **(fields | {mode: fields[mode] + 1} if mode in fields and cycle else fields),
        )
    if mode != "complete":
        with pytest.raises(ValueError, match="resource recovery"):
            emit(state, "case_end", case=case, passed=1, failed=0)
        return
    emit(state, "case_end", case=case, passed=1, failed=0)
    emit(state, "end", completed=1, selected=1, failed=0)
    assert state.outcome("protocol_end", None, b"") == ("passed", "pass")
    assert len(state.checkpoints) == 1000 // interval + 1


@pytest.mark.parametrize("workload", [None, ["containers.smp"], ["vfs.smp"], ["scheduler"]])
def test_smp_workload_rejects_one_cpu_before_loading_artifacts(workload):
    with pytest.raises(kv.typer.BadParameter, match="requires at least 2 CPUs"):
        kv.run(manifest=Path("not-loaded.json"), workload=workload, cpus=1)


@pytest.mark.parametrize("cycles,seconds", [(9900, 1800), (10000, 1799), (10000, 1800), (12000, 1900)])
@pytest.mark.parametrize("workload", ["users.lifecycle", "users.applications"])
def test_stability_requires_both_duration_and_complete_cycles(cycles, seconds, workload):
    state = ready(workload, stability=True)
    case = state.expected[0]
    emit(state, "case_start", case=case)
    for cycle in range(0, cycles + 1, kv.LIFECYCLE_INTERVAL[workload]):
        emit(
            state,
            "checkpoint",
            case=case,
            cycles=cycle,
            application_cycles=cycle if workload == "users.applications" else 0,
            elapsed_ns=seconds * 10**9 * cycle // cycles,
            **LIFECYCLE_RESOURCES,
        )
    if cycles < 10000 or seconds < 1800:
        with pytest.raises(ValueError, match="resource recovery"):
            emit(state, "case_end", case=case, passed=1, failed=0)
    else:
        emit(state, "case_end", case=case, passed=1, failed=0)
        emit(state, "end", completed=1, selected=1, failed=0)
        assert state.outcome("protocol_end", None, b"") == ("passed", "pass")


@pytest.mark.parametrize("mode", ["stalled", "false_time", "false_time_exit"])
def test_stability_host_rejects_stalls_and_instant_guest_duration(tmp_path, monkeypatch, mode):
    workload = "users.lifecycle"
    case = kv.CATALOG[workload][0]
    records = [
        event(workload, "ready", detected_cpus=4, online_mask=15, work_mask=15, ram_bytes=2**31, managed_pages=500000),
        event(workload, "catalog", case=case),
        event(workload, "worker", cpu=0, affinity=1),
        event(workload, "case_start", case=case),
    ]
    for cycle in range(0, 1 if mode == "stalled" else 10001, 100):
        records.append(
            event(
                workload,
                "checkpoint",
                case=case,
                cycles=cycle,
                application_cycles=0,
                elapsed_ns=cycle * 180000000,
                **LIFECYCLE_RESOURCES,
            )
        )
    if mode != "stalled":
        records += [
            event(workload, "case_end", case=case, passed=1, failed=0),
            event(workload, "end", completed=1, selected=1, failed=0),
        ]
    script = "\n".join(f"print({('@@MOSS ' + json.dumps(record))!r}, flush=True)" for record in records)
    if mode != "false_time_exit":
        script += "\nimport time; time.sleep(30)"
    monkeypatch.setattr(kv, "build_qemu_args", lambda *_a, **_kw: [sys.executable, "-c", script])
    cfg = Artifacts(tmp_path / "manifest.json", "ARM64", "linux-image", {}, {})
    settings = dict(
        cpus=4,
        memory_mib=2048,
        warmup=1,
        samples=2,
        order=0,
        startup_timeout=1,
        case_timeout=0.1,
        guest_timeout=2,
        stability=True,
    )
    result = kv.run_guest(cfg, workload, tmp_path / "guest", settings, 0)
    assert result["status"] == "error"
    assert result["observed"] == (
        "no_progress_timeout"
        if mode == "stalled"
        else "infrastructure: stability guest completed before 30 host minutes"
    )


@pytest.mark.parametrize("workload", ["users.lifecycle", "users.applications"])
def test_stability_host_releases_live_guest_only_after_duration_and_cycles(tmp_path, monkeypatch, workload):
    case = kv.CATALOG[workload][0]
    records = [
        event(workload, "ready", detected_cpus=4, online_mask=15, work_mask=15, ram_bytes=2**31, managed_pages=500000),
        event(workload, "catalog", case=case),
        event(workload, "worker", cpu=0, affinity=1),
        event(workload, "case_start", case=case),
    ]
    script = "import sys, json\n" + "\n".join(
        f"print({('@@MOSS ' + json.dumps(record))!r}, flush=True)" for record in records
    )
    # Generate in the child: 1001 literal print statements exceed Linux's
    # single-argument size limit for python -c, independently of the runner.
    checkpoint = event(workload, "checkpoint", case=case, **LIFECYCLE_RESOURCES)
    script += (
        f"\nfor cycle in range(0, 10001, {kv.LIFECYCLE_INTERVAL[workload]}):\n"
        f" record = {checkpoint!r} | dict(cycles=cycle, application_cycles="
        f"cycle if {workload == 'users.applications'} else 0, elapsed_ns=cycle * 180000000)\n"
        " print('@@MOSS ' + json.dumps(record), flush=True)\n"
    )
    script += "\nassert sys.stdin.buffer.read(1) == b'S'\n"
    for record in [
        event(workload, "case_end", case=case, passed=1, failed=0),
        event(workload, "end", completed=1, selected=1, failed=0),
    ]:
        script += f"print({('@@MOSS ' + json.dumps(record))!r}, flush=True)\n"
    script += "import time; time.sleep(30)\n"
    real_clock, offset = kv.time.monotonic, [0]
    monkeypatch.setattr(kv.time, "monotonic", lambda: real_clock() + offset[0])
    original_accept = kv.Protocol.accept

    def accept(state, line):
        if b'"cycles": 10000' in line:
            offset[0] = 1800
        original_accept(state, line)

    monkeypatch.setattr(kv.Protocol, "accept", accept)
    monkeypatch.setattr(kv, "build_qemu_args", lambda *_a, **_kw: [sys.executable, "-c", script])
    cfg = Artifacts(tmp_path / "manifest.json", "ARM64", "linux-image", {}, {})
    settings = dict(
        cpus=4,
        memory_mib=2048,
        warmup=1,
        samples=2,
        order=0,
        startup_timeout=1,
        case_timeout=1,
        guest_timeout=1805,
        stability=True,
    )
    result = kv.run_guest(cfg, workload, tmp_path / "guest", settings, 0)
    assert result["status"] == "passed", result["observed"]
    assert result["stability_release_seconds"] >= 1800
    assert result["cases"][0]["elapsed_seconds"] >= 1800


@pytest.mark.parametrize("mode", ["progress", "stalled", "total_limit"])
def test_application_watchdog_requires_progress_and_a_total_bound(tmp_path, monkeypatch, mode):
    workload, case = "users.applications", "core_application_recovery"
    records = [
        event(workload, "ready", detected_cpus=4, online_mask=15, work_mask=15, ram_bytes=2**31, managed_pages=500000),
        event(workload, "catalog", case=case),
        event(workload, "worker", cpu=0, affinity=1),
        event(workload, "case_start", case=case),
    ]
    script = "import time, json\n" + "\n".join(
        f"print({('@@MOSS ' + json.dumps(record))!r}, flush=True)" for record in records
    )
    checkpoint = event(workload, "checkpoint", case=case, **LIFECYCLE_RESOURCES)
    script += (
        f"\nfor cycle in range(0, {1 if mode == 'stalled' else 1001}, 10):\n"
        f" record = {checkpoint!r} | dict(cycles=cycle, application_cycles=cycle, elapsed_ns=cycle * 10**8)\n"
        " print('@@MOSS ' + json.dumps(record), flush=True)\n time.sleep(0.01)\n"
    )
    if mode != "stalled":
        for record in [
            event(workload, "case_end", case=case, passed=1, failed=0),
            event(workload, "end", completed=1, selected=1, failed=0),
        ]:
            script += f"print({('@@MOSS ' + json.dumps(record))!r}, flush=True)\n"
    script += "time.sleep(30)\n"
    real_clock, offset = kv.time.monotonic, [0]
    monkeypatch.setattr(kv.time, "monotonic", lambda: real_clock() + offset[0])
    original_accept = kv.Protocol.accept

    def accept(state, line):
        if b'"event": "checkpoint"' in line:
            offset[0] = json.loads(line[7:])["cycles"] / 10
        original_accept(state, line)

    monkeypatch.setattr(kv.Protocol, "accept", accept)
    monkeypatch.setattr(kv, "build_qemu_args", lambda *_a, **_kw: [sys.executable, "-c", script])
    cfg = Artifacts(tmp_path / "manifest.json", "ARM64", "linux-image", {}, {})
    settings = dict(
        cpus=4,
        memory_mib=2048,
        warmup=1,
        samples=2,
        order=0,
        startup_timeout=30,
        case_timeout=2,
        guest_timeout=5 if mode == "total_limit" else None,
    )
    result = kv.run_guest(cfg, workload, tmp_path / "guest", settings, 0)
    assert (
        result["observed"]
        == {"progress": "pass", "stalled": "no_progress_timeout", "total_limit": "guest_timeout"}[mode]
    )
    assert result["case_timeout_kind"] == "no_progress"
    if mode == "progress":
        assert result["cases"][0]["elapsed_seconds"] >= 100
        assert result["guest_timeout_seconds"] == 232


def test_simd_fault_is_explicit_and_failure_is_not_an_expected_pass():
    assert "users.simd_fault" not in kv.FUNCTIONAL
    state = ready("users.simd_fault")
    finish(state, failed=True)
    assert state.outcome("protocol_end", 0, b"") == ("failed", "assertion")


def test_simd_fault_rejects_other_architectures_before_launch(tmp_path, monkeypatch):
    cfg = Artifacts(tmp_path / "manifest.json", "ARM64", "linux-image", {}, {})
    monkeypatch.setattr(Artifacts, "load", lambda _: cfg)
    with pytest.raises(kv.typer.BadParameter, match="requires x64"):
        kv.run(manifest=cfg.manifest, workload=["users.simd_fault"])


def test_case_duration_uses_host_observation_time(monkeypatch):
    observed = iter([10.0, 10.75])
    monkeypatch.setattr(kv.time, "monotonic", lambda: next(observed))
    state = ready()
    emit(state, "case_start", case=state.expected[0])
    emit(state, "case_end", case=state.expected[0], passed=1, failed=0)
    assert state.cases[0]["elapsed_seconds"] == 0.75


@pytest.mark.parametrize(
    "workload,override,expected",
    [
        ("pfa", None, 30),
        ("mm", None, 5),
        ("pfa", 0.1, 0.1),
        ("pfa", 40, 40),
        ("users.busybox", None, 30),
        ("users.busybox", 1, 1),
    ],
)
def test_workload_deadline_is_recorded_and_explicit_override_wins(tmp_path, monkeypatch, workload, override, expected):
    monkeypatch.setattr(kv, "build_qemu_args", lambda *_args, **_kwargs: [str(tmp_path / "missing-qemu")])
    cfg = Artifacts(tmp_path / "manifest.json", "ARM64", "linux-image", {}, {})
    settings = dict(cpus=4, memory_mib=2048, warmup=1, samples=2, order=0, case_timeout=override)
    result = kv.run_guest(cfg, workload, tmp_path / "guest", settings, 0)
    assert result["case_timeout_seconds"] == expected
    assert result["status"] == "error"  # Selecting a budget must never turn launch failure into success.


@pytest.mark.parametrize(
    "line",
    [
        b"@@MOSS {}",
        b"x@@MOSS {}",
        b"@@MOSS not json",
        b"@@MOSS []",
        b'@@MOSS {"v":1,"v":1,"event":"ready","workload":"mm"}',
        b"@@MOSS " + b"x" * 1024,
        b'@@MOSS {"v":2,"event":"ready","workload":"mm"}',
    ],
)
def test_malformed_protocol_fails_closed(line):
    with pytest.raises(ValueError):
        kv.Protocol("mm", 4, 2048, 1, 2).accept(line)


@pytest.mark.parametrize(
    "field,value",
    [
        ("detected_cpus", 1),
        ("online_mask", 7),
        ("work_mask", 7),
        ("ram_bytes", 2**28),
        ("managed_pages", 0),
        ("managed_pages", 524289),
        ("detected_cpus", True),
    ],
)
def test_reported_resources_must_match_real_profile(field, value):
    fields = dict(detected_cpus=4, online_mask=15, work_mask=15, ram_bytes=2**31, managed_pages=500000)
    fields[field] = value
    with pytest.raises(ValueError):
        emit(kv.Protocol("mm", 4, 2048, 1, 2), "ready", **fields)


def test_small_memory_profile_has_valid_allocator_accounting():
    state = kv.Protocol("mm", 4, 256, 1, 2)
    emit(state, "ready", detected_cpus=4, online_mask=15, work_mask=15, ram_bytes=256 * 2**20, managed_pages=60000)
    for name in kv.CATALOG["mm"]:
        emit(state, "catalog", case=name)
    emit(state, "worker", cpu=0, affinity=1)
    finish(state)
    assert state.outcome("protocol_end", None, b"") == ("passed", "pass")


@pytest.mark.parametrize(
    "kind,fields",
    [
        ("case_start", {"case": "unknown"}),
        ("case_end", {"case": "orders_alignment", "passed": 1, "failed": 0}),
        ("worker", {"cpu": 0, "affinity": 1}),
        ("catalog", {"case": "reuse"}),
        ("end", {"completed": 0, "selected": 2, "failed": 0}),
    ],
)
def test_bad_lifecycle_events_are_rejected(kind, fields):
    with pytest.raises(ValueError):
        emit(ready(), kind, **fields)


def test_assertion_failure_stops_suite_without_losing_not_run_cases():
    state = ready("self.fail")
    finish(state, failed=True)
    assert len(state.cases) == 1
    assert state.outcome("protocol_end", None, b"") == ("passed", "assertion")
    assert state.outcome("process_exit", None, b"")[0] == "error"


def test_expected_panic_needs_matching_case_and_actual_panic_evidence():
    state = ready("self.panic")
    emit(state, "case_start", case=state.expected[0])
    emit(state, "fatal", case=state.expected[0], kind="panic")
    assert state.outcome(None, "case_timeout", b"")[0] == "error"
    panic = b"[P] validation intentional panic\nKERNEL PANIC: validation intentional panic"
    assert state.outcome(None, "case_timeout", panic) == ("passed", "panic")
    assert state.outcome(None, "cancelled", b"[P] validation intentional panic")[0] == "error"


def test_offline_comparison_recomputes_samples_and_allows_revision_changes():
    before = benchmark_report()
    after = copy.deepcopy(before)
    after["provenance"]["revision"] = "new"
    for batch in after["guests"][0]["batches"]:
        batch["ticks"] *= 2
    after["guests"][0]["measurement"] = {"median_ns_per_operation": 1}
    result = kv.comparison(before, after)[0]
    assert result["status"] == "comparable"
    assert result["relative_change"] == 1
    assert result["informational_only"]


@pytest.mark.parametrize("termination", ["process_exit", "timeout", None, True])
def test_saved_measurement_requires_protocol_termination(termination):
    report = benchmark_report()
    report["guests"][0]["termination"] = termination
    assert kv.comparison(report, report)[0]["status"] == "not_comparable"


def test_saved_measurement_requires_architecture_specific_clock():
    report = benchmark_report()
    report["comparison_environment"]["arch"] = "X64"
    guest = report["guests"][0]
    guest["clock"]["source"] = "cpuid.15"
    guest["raw_exit"] = -15  # Host-owned termination is not a test result.
    assert kv.comparison(report, report)[0]["status"] == "comparable"
    guest["clock"]["source"] = "cntfrq_el0"
    assert kv.comparison(report, report)[0]["status"] == "not_comparable"


def test_uncalibrated_clock_frequency_changes_are_not_comparable():
    before = benchmark_report()
    after = copy.deepcopy(before)
    after["guests"][0]["clock"]["frequency"] *= 2
    assert kv.comparison(before, after)[0]["status"] == "not_comparable"


@pytest.mark.parametrize("damage", ["failed", "missing_batch", "clock", "count", "cpu", "version", "completion"])
def test_invalid_saved_measurements_are_not_comparable(damage):
    before = benchmark_report()
    after = copy.deepcopy(before)
    guest = after["guests"][0]
    if damage == "failed":
        guest["status"] = "failed"
    elif damage == "missing_batch":
        guest["batches"].pop()
    elif damage == "clock":
        guest["clock"]["frequency"] = 0
    elif damage == "count":
        guest["batches"][1]["operations"] = 8
    elif damage == "cpu":
        guest["batches"][0]["cpu"] = 1
    elif damage == "version":
        guest["version"] = 2
    else:
        guest["completion"] = None
    assert kv.comparison(before, after)[0]["status"] == "not_comparable"


def test_unfinished_or_duplicate_reports_are_rejected():
    report = benchmark_report()
    report["finalized"] = False
    with pytest.raises(ValueError):
        kv.comparison(report, report)
    report["finalized"] = True
    report["guests"] *= 2
    with pytest.raises(ValueError):
        kv.comparison(report, report)


@pytest.mark.parametrize("change", ["cpus", "governor"])
def test_environment_changes_are_not_comparable(change):
    before = benchmark_report()
    before["comparison_environment"]["host"] = {"cpu_governors": {"8": "ondemand"}}
    after = copy.deepcopy(before)
    if change == "cpus":
        after["comparison_environment"]["settings"]["cpus"] = 8
    else:
        after["comparison_environment"]["host"]["cpu_governors"]["8"] = "performance"
    assert kv.comparison(before, after)[0]["status"] == "not_comparable"


def test_junit_preserves_failed_and_unstarted_cases(tmp_path):
    report = {
        "guests": [
            dict(
                workload="mm",
                status="failed",
                expected="pass",
                observed="assertion",
                cases=[{"name": "orders_alignment", "status": "failed"}, {"name": "reuse", "status": "not_run"}],
            )
        ],
        "not_run": ["vfs"],
    }
    kv.write_reports(report, tmp_path)
    xml = ET.parse(tmp_path / "junit.xml")
    assert len(xml.findall(".//failure")) == 1
    assert len(xml.findall(".//skipped")) == 1 + len(kv.CATALOG["vfs"])
    assert json.loads((tmp_path / "results.json").read_text()) == report


@pytest.mark.parametrize(
    "mode",
    [
        "early_exit",
        "malformed",
        "timeout",
        "ignore_term",
        "cancel",
        "launch_failure",
        "complete",
        "duplicate_end",
        "capture_error",
    ],
)
def test_host_child_lifecycle_reaps_every_spawn(monkeypatch, tmp_path, mode):
    prefix = ""
    if mode in ("timeout", "ignore_term", "cancel", "complete", "duplicate_end", "capture_error"):
        records = [
            event("mm", "ready", detected_cpus=4, online_mask=15, work_mask=15, ram_bytes=2**31, managed_pages=500000)
        ]
        records += [event("mm", "catalog", case=name) for name in kv.CATALOG["mm"]]
        records += [event("mm", "worker", cpu=0, affinity=1)]
        if mode in ("complete", "duplicate_end"):
            for name in kv.CATALOG["mm"]:
                records += [
                    event("mm", "case_start", case=name),
                    event("mm", "case_end", case=name, passed=1, failed=0),
                ]
            records += [event("mm", "end", completed=len(kv.CATALOG["mm"]), selected=len(kv.CATALOG["mm"]), failed=0)]
            if mode == "duplicate_end":
                records += [records[-1]]
        else:
            records += [event("mm", "case_start", case=kv.CATALOG["mm"][0])]
        prefix = "".join(f"print({('@@MOSS ' + json.dumps(record))!r}, flush=True);" for record in records)
    if mode == "ignore_term":
        prefix = "import signal;signal.signal(signal.SIGTERM, signal.SIG_IGN);" + prefix
    script = prefix + (
        "import time;time.sleep(30)" if prefix else "print('@@MOSS invalid')" if mode == "malformed" else "pass"
    )
    args = [sys.executable, "-c", script, "stdio,id=char0,mux=on,signal=off", "-mon", "placeholder"]
    if mode == "launch_failure":
        args[0] = str(tmp_path / "missing-qemu")
    if mode == "cancel":
        original_sleep = kv.time.sleep
        interrupted_once = False

        def interrupted(_seconds):
            nonlocal interrupted_once
            if not interrupted_once:
                interrupted_once = True
                raise KeyboardInterrupt
            original_sleep(_seconds)

        monkeypatch.setattr(kv.time, "sleep", interrupted)
    monkeypatch.setattr(kv, "build_qemu_args", lambda *_args, **_kwargs: args)
    children = []
    original = subprocess.Popen

    def spawn(*args, **kwargs):
        child = original(*args, **kwargs)
        children.append(child)
        return child

    monkeypatch.setattr(kv.subprocess, "Popen", spawn)
    captures = []

    def capture(_cfg, process, _sockets, _directory, _cpus, _gdb):
        assert process.poll() is None  # The original guest must still exist when collecting evidence.
        captures.append(process.pid)
        if mode == "capture_error":
            raise OSError("capture failed")
        return {"status": "captured"}

    monkeypatch.setattr(kv, "capture_failure", capture)
    cfg = Artifacts(tmp_path / "manifest.json", "ARM64", "linux-image", {}, {})
    # This checks post-startup timeout/reaping, not Python launch latency under
    # concurrent cross-compilation. Keep the case and termination deadlines tight.
    settings = dict(
        cpus=4, memory_mib=2048, warmup=1, samples=2, order=0, startup_timeout=30, case_timeout=0.1, guest_timeout=35
    )
    result = kv.run_guest(cfg, "mm", tmp_path / "guest", settings, 0)
    assert result["status"] == ("passed" if mode == "complete" else "error")
    if mode == "complete":
        assert result["termination"] == "protocol_end"
    assert all(child.poll() is not None for child in children)
    assert Path(result["serial_log"]).exists()
    if mode in ("timeout", "ignore_term", "capture_error"):
        assert result["observed"] == "case_timeout"
        assert result["cases"][0]["elapsed_seconds"] >= 0.1
        if mode == "ignore_term":
            assert result["elapsed_seconds"] - result["cases"][0]["elapsed_seconds"] >= 2
        assert result["cases"][1]["status"] == "not_run"
        assert "elapsed_seconds" not in result["cases"][1]
        assert captures
        assert result["diagnostics"]["status"] == ("error" if mode == "capture_error" else "captured")
    if mode == "cancel":
        assert result["observed"] == "cancelled"
        assert not captures
    if mode == "complete":
        assert not captures and result["diagnostics"]["status"] == "not_needed"


def test_timeout_selftest_does_not_hide_an_unrelated_panic():
    state = ready("self.timeout")
    emit(state, "case_start", case=state.expected[0])
    emit(state, "fatal", case=state.expected[0], kind="timeout")
    assert state.outcome(None, "case_timeout", b"[P] allocator corrupted") == ("error", "unexpected_kernel_panic")


def test_counter_calibration_must_match_raw_reference_samples():
    state = ready("bench.allocate")
    emit(state, "case_start", case=state.workload)
    emit(state, "clock", source="pit.channel0", frequency=1193182, uncertainty_ppm=1000)
    for i in range(2):
        emit(state, "calibration", index=i, ticks=20000, reference_ticks=20000, reference_frequency=1193182)
    with pytest.raises(ValueError, match="inconsistent PIT"):
        emit(state, "calibration", index=2, ticks=40000, reference_ticks=20000, reference_frequency=1193182)
