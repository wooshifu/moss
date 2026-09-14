"""Run real Moss suites/scenarios, validate completion, and retain original evidence."""

import hashlib
import json
import math
import platform
import signal
import statistics
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Annotated, Any, Literal

import typer
from pydantic import BaseModel, ConfigDict

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from qemu import ARCH_CONFIG, build_qemu_args, get_qemu_version, resolve_machine, resolve_qemu
from scripts.artifacts import Artifacts

CATALOG = {
    "resources": ["cpu_memory"],
    "mm": ["orders_alignment", "reuse"],
    "pfa": ["release_contract", "exhaustion"],
    "heap": ["alignment", "invalid_requests", "release_contract", "reuse", "exhaustion"],
    "containers": ["ownership", "release_reuse", "map_ownership", "held_reader", "reentry"],
    "containers.smp": ["interleaving"],
    "vfs": ["read_position_eof", "errors_readonly"],
    "users": ["syscall_values", "user_ranges", "fork_exec_exit_reap"],
    "users.vm": ["private_cow", "readonly_cow", "access_permissions"],
    "users.frame": ["native_frame", "fork_registers", "signal_return"],
    "users.uaccess": [
        "allocation_fault",
        "write_fault",
        "read_fault",
        "partial_read",
        "partial_write",
        "partial_pipe_read",
        "sigframe_fault",
        "sigreturn_fault",
        "devices",
    ],
    "users.simd_fault": ["isolation"],  # Explicit x86 acceptance; TCG may not deliver #XM.
    "mm.permissions": [
        "table_defaults",
        "kernel_mappings",
        "active_user_mappings",
        "kernel_wx",
        "address_space_ownership",
        "cow_clone_permissions",
        "vma_boundaries",
    ],
    "mm.transactions": [
        "map_preserves_existing",
        "map_allocation_rollback",
        "map_rejects_blocks",
        "clone_preserves_destination",
        "clone_allocation_rollback",
    ],
    "self": ["accounting_registration", "registry_limits", "cleanup_guards", "heap_bounds"],
    "self.fail": ["intentional_assertion", "not_run"],
    "self.panic": ["intentional_panic"],
    "self.timeout": ["intentional_timeout"],
    **{f"bench.{name}": [f"bench.{name}"] for name in ("allocate", "release", "combined", "read", "getpid")},
}
FUNCTIONAL = [
    "resources",
    "mm",
    "mm.permissions",
    "mm.transactions",
    "pfa",
    "heap",
    "containers",
    "containers.smp",
    "vfs",
    "users",
    "users.vm",
    "users.frame",
    "users.uaccess",
]
BENCHMARKS = [name for name in CATALOG if name.startswith("bench.")]
SELFTESTS = ["self", "self.fail", "self.panic", "self.timeout"]


class Record(BaseModel):
    model_config = ConfigDict(extra="allow", strict=True)
    v: Literal[1]
    event: Literal[
        "ready", "catalog", "worker", "case_start", "case_end", "clock", "calibration", "batch", "fatal", "end"
    ]
    workload: str


def unique_object(pairs: list[tuple[str, Any]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(value: str | bytes) -> dict:
    result = json.loads(value, object_pairs_hook=unique_object)
    if not isinstance(result, dict):
        raise ValueError("expected JSON object")
    return result


def integer(data: dict, key: str, minimum: int = 0, maximum: int = 2**64 - 1) -> int:
    value = data.get(key)
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"invalid {key}: {value!r}")
    return value


class Protocol:
    """Fail-closed event state machine; no text PASS heuristic or exit-code-only success."""

    def __init__(self, workload: str, cpus: int, memory_mib: int, warmup: int, samples: int):
        self.workload = workload
        self.expected = CATALOG[workload]
        self.cpus, self.memory_mib = cpus, memory_mib
        self.warmup, self.samples = warmup, samples
        self.ready: dict | None = None
        self.catalog: list[str] = []
        self.worker: dict | None = None
        self.active: str | None = None
        self.case_started: float | None = None
        self.cases: list[dict] = []
        self.clock: dict | None = None
        self.calibration: list[dict] = []
        self.batches: list[dict] = []
        self.fatal: str | None = None
        self.end: dict | None = None
        self.failed = False

    def accept(self, line: bytes) -> None:
        if b"@@MOSS " not in line:
            return
        if not line.startswith(b"@@MOSS ") or len(line) > 1024:
            raise ValueError("malformed or oversized protocol line")
        record = Record.model_validate(load_json(line[7:])).model_dump()
        if record["workload"] != self.workload or self.end:
            raise ValueError("wrong workload or record after completion")
        event = record["event"]
        if event == "ready":
            if self.ready:
                raise ValueError("duplicate ready")
            self.ready = record
            mask = (1 << self.cpus) - 1
            if any(
                integer(record, key) != value
                for key, value in (("detected_cpus", self.cpus), ("online_mask", mask), ("work_mask", mask))
            ):
                raise ValueError("requested CPUs did not all execute kernel work")
            ram = integer(record, "ram_bytes")
            if not self.memory_mib * 2**20 - 2**21 <= ram <= self.memory_mib * 2**20:
                raise ValueError("firmware RAM differs from requested RAM")
            if integer(record, "managed_pages") * 4096 <= 256 * 2**20:
                raise ValueError("allocator did not discover enlarged RAM")
            return
        if not self.ready:
            raise ValueError("validation not ready")
        if event == "catalog":
            case = record.get("case")
            if self.worker or len(self.catalog) >= len(self.expected) or case != self.expected[len(self.catalog)]:
                raise ValueError("invalid catalog")
            if case in self.catalog:
                raise ValueError("duplicate catalog identity")
            self.catalog.append(case)
            return
        if self.catalog != self.expected:
            raise ValueError("incomplete or reordered catalog")
        if event == "worker":
            if self.worker or integer(record, "cpu") != 0 or integer(record, "affinity") != 1:
                raise ValueError("invalid worker affinity")
            self.worker = record
        elif event == "case_start":
            if not self.worker or self.active or self.failed or len(self.cases) >= len(self.expected):
                raise ValueError("invalid case start")
            if record.get("case") != self.expected[len(self.cases)]:
                raise ValueError("unexpected case identity/order")
            self.active = record["case"]
            self.case_started = time.monotonic()
        elif event == "case_end":
            if not self.active or record.get("case") != self.active:
                raise ValueError("case completion without matching start")
            count = integer(record, "failed")
            passed = integer(record, "passed")
            if not passed and not count and not self.workload.startswith("bench."):
                raise ValueError("case has no assertions")
            self.failed |= count > 0
            self.cases.append(
                {
                    "name": self.active,
                    "status": "failed" if count else "passed",
                    "assertions": record,
                    "elapsed_seconds": time.monotonic() - self.case_started,
                }
            )
            self.active = None
            self.case_started = None
        elif event == "clock":
            if not self.active or self.clock or not self.workload.startswith("bench."):
                raise ValueError("unexpected clock")
            integer(record, "frequency", 1000, 10**11)
            integer(record, "uncertainty_ppm", 0, 100000)
            if record.get("source") not in ("cntfrq_el0", "dtb.timebase-frequency", "cpuid.15", "pit.channel0"):
                raise ValueError("invalid clock source")
            self.clock = record
        elif event == "calibration":
            if not self.clock or self.batches or self.clock["source"] != "pit.channel0":
                raise ValueError("unexpected calibration")
            if integer(record, "index", 0, 2) != len(self.calibration):
                raise ValueError("calibration sequence")
            integer(record, "ticks", 1)
            integer(record, "reference_ticks", 16384, 60000)
            if integer(record, "reference_frequency") != 1193182:
                raise ValueError("invalid PIT reference")
            self.calibration.append(record)
            if len(self.calibration) == 3:
                frequencies = [s["ticks"] * 1193182 // s["reference_ticks"] for s in self.calibration]
                low, high = min(frequencies), max(frequencies)
                if not low or (high - low) * 100 > low * 5 or self.clock["frequency"] != statistics.median(frequencies):
                    raise ValueError("inconsistent PIT calibration")
                if self.clock["uncertainty_ppm"] < (high - low) * 1000000 // low + 1000:
                    raise ValueError("understated calibration uncertainty")
        elif event == "batch":
            if not self.active or not self.clock:
                raise ValueError("sample before validated clock/case")
            index = integer(record, "index")
            if index != len(self.batches) or index >= self.warmup + self.samples:
                raise ValueError("sample count/order mismatch")
            if integer(record, "warmup", 0, 1) != int(index < self.warmup) or integer(record, "cpu") != 0:
                raise ValueError("sample phase/CPU mismatch")
            integer(record, "ticks", 1)
            integer(record, "overhead_ticks")
            count = integer(record, "operations", 1, 65536)
            if self.batches and self.batches[0]["operations"] != count:
                raise ValueError("iteration count changed during sampling")
            self.batches.append(record)
        elif event == "fatal":
            if (
                not self.active
                or self.fatal
                or record.get("case") != self.active
                or record.get("kind") not in ("panic", "timeout")
            ):
                raise ValueError("invalid fatal evidence")
            self.fatal = record["kind"]
        elif event == "end":
            if self.active or integer(record, "completed") != len(self.cases):
                raise ValueError("incomplete active case")
            if integer(record, "selected") != len(self.expected) or integer(record, "failed", 0, 1) != int(self.failed):
                raise ValueError("completion accounting mismatch")
            if not self.failed and len(self.cases) != len(self.expected):
                raise ValueError("missing cases")
            if not self.failed and self.workload.startswith("bench."):
                if len(self.batches) != self.warmup + self.samples:
                    raise ValueError("missing benchmark batches")
                if self.clock and self.clock["source"] == "pit.channel0" and len(self.calibration) != 3:
                    raise ValueError("missing calibration evidence")
            self.end = record

    def outcome(self, termination: str | None, reason: str | None, serial: bytes) -> tuple[str, str]:
        if reason == "cancelled":
            return "error", "cancelled"
        expected = {"self.fail": "assertion", "self.panic": "panic", "self.timeout": "timeout"}.get(
            self.workload, "pass"
        )
        panic_lines = [line for line in serial.splitlines() if b"[P]" in line or b"KERNEL PANIC:" in line]
        if any(b"validation intentional panic" not in line for line in panic_lines):
            return "error", "unexpected_kernel_panic"
        if expected != "panic" and panic_lines:
            return "error", "unexpected_kernel_panic"
        if any(
            marker in serial
            for marker in (b"KERNEL PAGE FAULT", b"HALTED", b"RISC-V EXCEPTION", b"UNHANDLED USER EXCEPTION")
        ):
            return "error", "unexpected_kernel_panic"
        if (
            expected == "panic"
            and self.fatal == "panic"
            and self.active == self.expected[0]
            and b"[P]" in serial
            and b"KERNEL PANIC: validation intentional panic" in serial
            and reason == "case_timeout"
        ):
            return "passed", "panic"
        if expected == "timeout" and self.fatal == "timeout" and reason == "case_timeout":
            return "passed", "timeout"
        if any(marker in serial for marker in (b"[P]", b"KERNEL PAGE FAULT", b"HALTED", b"RISC-V EXCEPTION")):
            return "error", "unexpected_kernel_panic"
        if reason:
            return "error", reason
        if not self.end:
            return "error", "missing_completion"
        if termination != "protocol_end":
            return "error", "unexpected_process_exit"
        if self.failed:
            return ("passed", "assertion") if expected == "assertion" else ("failed", "assertion")
        return ("passed", "pass") if expected == "pass" else ("failed", "expected_failure_not_observed")


def command_output(args: list[str]) -> str:
    return subprocess.run(args, capture_output=True, text=True, check=True, timeout=30).stdout.strip()


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run_guest(cfg: Artifacts, workload: str, directory: Path, settings: dict, iterations: int) -> dict:
    directory.mkdir()
    case_timeout = settings.get("case_timeout")
    if case_timeout is None:
        # Exhaustion touches all 2 GiB of guest RAM; host memory pressure can
        # take it past the ordinary 5 s budget without a kernel failure.
        case_timeout = 30.0 if workload == "pfa" else 5.0
    state = Protocol(
        workload,
        settings["cpus"],
        settings.get("expected_ram_mib", settings["memory_mib"]),
        settings["warmup"],
        settings["samples"],
    )
    bootargs = " ".join(
        f"moss.{k}={v}"
        for k, v in {
            "validation": workload,
            "cpus": settings["cpus"],
            "memory": settings.get("expected_ram_mib", settings["memory_mib"]),
            "warmup": settings["warmup"],
            "samples": settings["samples"],
            "iterations": iterations,
            "order": settings["order"],
        }.items()
    )
    args = build_qemu_args(
        cfg,
        smp=settings["cpus"],
        memory_mib=settings["memory_mib"],
        validation=True,
        qemu=settings.get("qemu"),
        machine=settings.get("machine"),
        cpu=settings.get("cpu"),
        dtb=settings.get("dtb"),
        debug_mode=False,
        extra_args=["-append", bootargs],
    )
    serial_path, error_path = directory / "serial.log", directory / "qemu.log"
    reason, raw_exit, process = None, None, None
    termination = "process_exit"
    pending = b""
    start = time.monotonic()
    try:
        with (
            serial_path.open("wb") as serial_out,
            error_path.open("wb") as diagnostics,
            serial_path.open("rb") as serial_in,
        ):
            process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=serial_out, stderr=diagnostics)
            while True:
                if serial_path.stat().st_size > 32 * 2**20:
                    raise ValueError("serial log exceeded 32 MiB")
                pending += serial_in.read(2**20)
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    state.accept(line.rstrip(b"\r"))
                if len(pending) > 65536:
                    raise ValueError("unbounded partial serial line")
                if state.end:
                    termination = "protocol_end"
                    break
                raw_exit = process.poll()
                now = time.monotonic()
                if raw_exit is not None:
                    pending += serial_in.read()
                    for line in pending.splitlines():
                        state.accept(line)
                    if state.end:
                        termination = "protocol_end"
                    break
                if not state.ready and now - start >= settings["startup_timeout"]:
                    reason = "startup_timeout"
                elif state.case_started and now - state.case_started >= case_timeout:
                    reason = "case_timeout"
                elif now - start >= settings["guest_timeout"]:
                    reason = "guest_timeout"
                if reason:
                    termination = "timeout"
                    break
                time.sleep(0.02)
    except (OSError, ValueError) as error:
        reason = f"infrastructure: {error}"
    except KeyboardInterrupt:
        reason = "cancelled"
    finally:
        observed_end = time.monotonic()  # Exclude termination/reaping from an unfinished case's duration.
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if process:
            raw_exit = process.returncode
    serial = serial_path.read_bytes() if serial_path.exists() else b""
    # Recheck the complete log after reaping, including records written during termination.
    if termination == "protocol_end":
        try:
            replay = Protocol(
                workload,
                settings["cpus"],
                settings.get("expected_ram_mib", settings["memory_mib"]),
                settings["warmup"],
                settings["samples"],
            )
            for line in serial.splitlines():
                replay.accept(line)
        except ValueError as error:
            reason = f"infrastructure: {error}"
    status, observed = state.outcome(termination, reason, serial)
    cases = list(state.cases)
    for name in state.expected[len(cases) :]:
        cases.append({"name": name, "status": "error" if name == state.active else "not_run", "reason": observed})
        if name == state.active and state.case_started is not None:
            cases[-1]["elapsed_seconds"] = observed_end - state.case_started
    result = {
        "workload": workload,
        "version": 1,
        "status": status,
        "observed": observed,
        "expected": {"self.fail": "assertion", "self.panic": "panic", "self.timeout": "timeout"}.get(workload, "pass"),
        "cases": cases,
        "ready": state.ready,
        "worker": state.worker,
        "clock": state.clock,
        "calibration": state.calibration,
        "batches": state.batches,
        "raw_exit": raw_exit,
        "termination": termination,
        "completion": state.end,
        "elapsed_seconds": time.monotonic() - start,
        "case_timeout_seconds": case_timeout,
        "qemu_args": args,
        "serial_log": str(serial_path),
        "qemu_log": str(error_path),
        "parameters": {"order": settings["order"]},
    }
    if status == "passed" and workload.startswith("bench.") and state.clock:
        values = [
            batch["ticks"] * 1e9 / state.clock["frequency"] / batch["operations"]
            for batch in state.batches
            if not batch["warmup"]
        ]
        result["measurement"] = {
            "median_ns_per_operation": statistics.median(values),
            "batch_ns_per_operation": values,
            "interpretation": "elapsed batch averages, not individual-call latency or exclusive CPU time",
        }
        if workload == "bench.read":
            result["measurement"]["median_bytes_per_second"] = 256 * 1e9 / statistics.median(values)
    return result


def validate_report(report: dict) -> None:
    if integer(report, "schema_version") != 2 or report.get("finalized") is not True:
        raise ValueError("unsupported or unfinished report")
    requested, guests = report.get("requested"), report.get("guests")
    if not isinstance(requested, list) or not requested or any(name not in CATALOG for name in requested):
        raise ValueError("invalid requested workloads")
    if len(set(requested)) != len(requested) or not isinstance(guests, list):
        raise ValueError("duplicate requests or invalid guests")
    if [item.get("workload") for item in guests] != requested[: len(guests)]:
        raise ValueError("unexpected, duplicate, or reordered guests")
    if report.get("not_run") != requested[len(guests) :]:
        raise ValueError("missing workload accounting")


def saved_measurement(report: dict, item: dict) -> float | None:
    """Revalidate raw evidence; never trust an edited derived metric alone."""
    try:
        if item["status"] != "passed" or item["observed"] != "pass" or integer(item, "version") != 1:
            return None
        environment = report["comparison_environment"]
        arch = environment["arch"]
        sources = {
            "ARM64": ("cntfrq_el0",),
            "RISCV": ("dtb.timebase-frequency",),
            "X86_64": ("cpuid.15", "pit.channel0"),
        }
        if item["clock"]["source"] not in sources[arch]:
            return None
        if item.get("termination") != "protocol_end":
            return None
        settings = environment["settings"]
        state = Protocol(
            item["workload"],
            settings["cpus"],
            settings.get("expected_ram_mib", settings["memory_mib"]),
            settings["warmup"],
            settings["samples"],
        )

        def accept(record: dict) -> None:
            state.accept(b"@@MOSS " + json.dumps(record, allow_nan=False).encode())

        def event(kind: str, **fields: Any) -> None:
            accept(dict(v=1, event=kind, workload=item["workload"], **fields))

        accept(item["ready"])
        for name in state.expected:
            event("catalog", case=name)
        accept(item["worker"])
        event("case_start", case=state.expected[0])
        accept(item["clock"])
        for record in item["calibration"] + item["batches"]:
            accept(record)
        accept(item["cases"][0]["assertions"])
        accept(item["completion"])
        if state.outcome(item["termination"], None, b"") != ("passed", "pass"):
            return None
        values = [
            b["ticks"] * 1e9 / state.clock["frequency"] / b["operations"] for b in state.batches if not b["warmup"]
        ]
        value = statistics.median(values)
        return value if math.isfinite(value) and value > 0 else None
    except (KeyError, TypeError, ValueError, IndexError, ZeroDivisionError):
        return None


def comparison(before: dict, after: dict) -> list[dict]:
    validate_report(before)
    validate_report(after)
    output = []
    prior = {item["workload"]: item for item in before["guests"]}
    for item in after["guests"]:
        if not item["workload"].startswith("bench."):
            continue
        old = prior.get(item["workload"])
        result = {"workload": item["workload"], "status": "unavailable"}
        if old:
            same = before.get("comparison_environment") == after.get("comparison_environment")
            same &= old.get("parameters") == item.get("parameters")
            old_clock, new_clock = old.get("clock") or {}, item.get("clock") or {}
            same &= bool(old_clock and new_clock and old_clock.get("source") == new_clock.get("source"))
            same &= old_clock.get("source") == "pit.channel0" or old_clock.get("frequency") == new_clock.get(
                "frequency"
            )
            same &= bool(
                old.get("batches")
                and item.get("batches")
                and old["batches"][0]["operations"] == item["batches"][0]["operations"]
            )
            a, b = saved_measurement(before, old), saved_measurement(after, item)
            if same and a is not None and b is not None:
                result.update(
                    status="comparable",
                    baseline_median_ns=a,
                    current_median_ns=b,
                    absolute_change_ns=b - a,
                    relative_change=(b - a) / a if a else None,
                    improvement="lower",
                    informational_only=True,
                )
            else:
                result.update(
                    status="not_comparable", reason="environment, workload, iterations, clock or validity differs"
                )
        output.append(result)
    return output


def write_reports(report: dict, output: Path) -> None:
    temporary = output / "results.json.tmp"
    temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    temporary.replace(output / "results.json")
    root = ET.Element("testsuites")
    for guest in report["guests"]:
        suite = ET.SubElement(root, "testsuite", name=guest["workload"])
        for case in guest["cases"]:
            node = ET.SubElement(suite, "testcase", name=case["name"], classname=guest["workload"])
            expected_failure = guest["expected"] != "pass" and guest["status"] == "passed"
            if case["status"] == "not_run":
                ET.SubElement(node, "skipped", message=case.get("reason", "suite stopped"))
            elif not expected_failure and case["status"] in ("failed", "error"):
                ET.SubElement(node, "failure" if case["status"] == "failed" else "error", message=guest["observed"])
        if guest["status"] == "error" and not any(c["status"] == "error" for c in guest["cases"]):
            node = ET.SubElement(suite, "testcase", name="infrastructure", classname=guest["workload"])
            ET.SubElement(node, "error", message=guest["observed"])
    for name in report.get("not_run", []):
        suite = ET.SubElement(root, "testsuite", name=name)
        for case in CATALOG[name]:
            node = ET.SubElement(suite, "testcase", name=case, classname=name)
            ET.SubElement(node, "skipped", message="guest not run")
    ET.ElementTree(root).write(output / "junit.xml", encoding="utf-8", xml_declaration=True)


app = typer.Typer()


@app.command()
def compare(baseline: Path, current: Path) -> None:
    """Compare two explicit saved reports without launching QEMU."""
    typer.echo(json.dumps(comparison(load_json(baseline.read_text()), load_json(current.read_text())), indent=2))


@app.command()
def run(
    manifest: Annotated[Path, typer.Option()],
    machine: str | None = None,
    cpu: str | None = None,
    qemu: str | None = None,
    dtb: Path | None = None,
    output: Annotated[Path | None, typer.Option()] = None,
    workload: Annotated[list[str] | None, typer.Option()] = None,
    benchmark: bool = False,
    selftest: bool = False,
    baseline: Annotated[Path | None, typer.Option()] = None,
    cpus: int = 4,
    memory_mib: int = 2048,
    expected_ram_mib: int | None = None,
    warmup: int = 5,
    samples: int = 30,
    iterations: int = 0,
    order: int = 0,
    startup_timeout: float = 30,
    case_timeout: float | None = None,
    guest_timeout: float = 60,
) -> None:
    """One fresh guest per suite/scenario; samples reuse that guest."""
    selected = (
        workload if workload is not None else (BENCHMARKS if benchmark else SELFTESTS if selftest else FUNCTIONAL)
    )
    if not selected or len(set(selected)) != len(selected) or any(name not in CATALOG for name in selected):
        raise typer.BadParameter("select unique, nonempty known workload IDs")
    if not (
        1 <= cpus <= 16 and 0 <= warmup <= 100 and 1 <= samples <= 1000 and 0 <= iterations <= 65536 and 0 <= order <= 4
    ):
        raise typer.BadParameter("invalid resource or sampling parameters")
    if "containers.smp" in selected and cpus < 2:
        raise typer.BadParameter("containers.smp requires at least 2 CPUs; select single-worker workloads for 1 CPU")
    if any(
        not math.isfinite(value) or value <= 0
        for value in (startup_timeout, case_timeout, guest_timeout)
        if value is not None
    ):
        raise typer.BadParameter("deadlines must be positive")
    expected_ram_mib = memory_mib if expected_ram_mib is None else expected_ram_mib
    if not 256 <= expected_ram_mib <= memory_mib:
        raise typer.BadParameter("expected firmware RAM must be between 256 MiB and installed RAM")
    cfg = Artifacts.load(manifest)
    if "users.simd_fault" in selected and cfg.arch != "X86_64":
        raise typer.BadParameter("users.simd_fault requires x86_64")
    build = cfg.manifest.parent
    metadata = cfg.build
    qemu = resolve_qemu(cfg.arch, qemu)
    image = cfg.require("validation_kernel")
    initrd = cfg.require("validation_initramfs")
    if any(name.startswith("bench.") for name in selected) and metadata["type"] != "Release":
        raise typer.BadParameter("benchmarks require the production Release build policy")
    prior = load_json(baseline.read_text()) if baseline else None
    if prior:
        validate_report(prior)
    settings = dict(
        cpus=cpus,
        memory_mib=memory_mib,
        expected_ram_mib=expected_ram_mib,
        warmup=warmup,
        samples=samples,
        order=order,
        startup_timeout=startup_timeout,
        case_timeout=case_timeout,
        guest_timeout=guest_timeout,
    )
    environment = {
        "arch": cfg.arch,
        "build": metadata,
        "qemu": get_qemu_version(qemu),
        "host": platform.uname()._asdict(),
        "settings": settings,
        "accelerator": "tcg",
        "clock_policy": 1,
        "fixture_sha256": sha256(initrd),
        "dtb_sha256": sha256(dtb) if dtb else None,
    }
    environment["machine"] = resolve_machine(cfg.arch, smp=cpus, machine=machine)
    environment["cpu_model"] = cpu or ARCH_CONFIG[cfg.arch]["cpu"]
    report: dict[str, Any] = {
        "schema_version": 2,
        "finalized": False,
        "requested": selected,
        "guests": [],
        "comparison_environment": environment,
        "provenance": {
            "revision": command_output(["git", "rev-parse", "HEAD"]),
            "dirty": bool(command_output(["git", "status", "--porcelain"])),
            "image_sha256": sha256(image),
            "manifest": str(cfg.manifest),
        },
    }
    output = (output or build / "validation" / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=False)
    write_reports(report, output)
    previous_handler = signal.getsignal(signal.SIGTERM)

    def cancelled(_signum: int, _frame: Any) -> None:
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, cancelled)
    try:
        for name in selected:
            count = iterations
            if prior and not count:
                old = next((g for g in prior["guests"] if g["workload"] == name and g.get("measurement")), None)
                if old and saved_measurement(prior, old) is not None:
                    count = old["batches"][0]["operations"]
            execution = dict(settings, qemu=qemu, machine=machine, cpu=cpu, dtb=dtb)
            result = run_guest(cfg, name, output / name, execution, count)
            report["guests"].append(result)
            write_reports(report, output)
            typer.echo(f"{name}: {result['status']} ({result['observed']})")
            if result["observed"] == "cancelled":
                break
    except KeyboardInterrupt:
        report["cancelled"] = True
    finally:
        signal.signal(signal.SIGTERM, previous_handler)
        report["finalized"] = True
        report["not_run"] = selected[len(report["guests"]) :]
        if prior:
            report["comparison"] = comparison(prior, report)
        write_reports(report, output)
    typer.echo(str(output / "results.json"))
    raise typer.Exit(int(bool(report["not_run"]) or any(item["status"] != "passed" for item in report["guests"])))


if __name__ == "__main__":
    app()
