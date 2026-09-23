"""Run real Moss suites/scenarios, validate completion, and retain original evidence."""

import hashlib
import json
import math
import os
import platform
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Annotated, Any, Literal

import typer
from pydantic import BaseModel, ConfigDict

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from qemu import (
    ARCH_CONFIG,
    RASPI_CONFIG,
    TCG_CACHE_MIB,
    build_qemu_args,
    get_qemu_version,
    resolve_dtb,
    resolve_machine,
    resolve_qemu,
    resolve_resources,
)
from scripts.artifacts import Artifacts
from scripts.qemu_diagnostics import capture_failure, qmp_session

CATALOG = {
    "drivers": [
        "registration",
        "matching_failure",
        "ownership",
        "registration_rollback",
        "irq_registration_rollback",
        "console_ring",
    ],
    "resources": ["cpu_memory"],
    "mm": [
        "initialization_publication",
        "unsupported_contracts",
        "pageblock_units",
        "mmu_granule",
        "orders_alignment",
        "reuse",
    ],
    "pfa": ["release_contract", "exhaustion"],
    "heap": ["alignment", "invalid_requests", "release_contract", "reuse", "exhaustion"],
    "containers": [
        "queue_reuse",
        "capability_process_handles",
        "ownership",
        "release_reuse",
        "map_ownership",
        "held_reader",
        "reentry",
    ],
    "containers.smp": ["interleaving"],
    "vfs.smp": ["shared_references"],
    "interrupts.smp": ["irq_context_retirement"],
    "mm.lifetime": ["held_readers", "hardware_root", "kernel_root"],
    "mm.concurrent": ["cow_fault", "demand_fault", "fault_unmap", "fault_fork", "fork_unmap"],
    "mm.uaccess": ["copy_unmap", "copy_fork"],
    "mm.tlb_broadcast": [
        "local_remap",
        "remote_remap",
        "locked_remap",
        "ipi_remap",
        "pruned_remap",
        "concurrent_remap_0",
        "concurrent_remap_1",
    ],
    "mm.tlb_join.request_first": ["registration"],
    "mm.tlb_join.cpu_first": ["registration"],
    "vfs": [
        "read_position_eof",
        "errors_readonly",
        "fd_boundaries",
        "pipe_reuse",
        "pipe_fd_rollback",
        "directory_capacity",
        "writable_lifecycle",
        "rename_lifecycle",
        "rename_boundaries",
        "access_permissions",
        "working_directory_lifecycle",
    ],
    "timers": ["clocksource_high_frequency", "contracts", "dispatch", "capacity"],
    "scheduler": [
        "pelt_large_runtime",
        "pelt_partitioned_runtime",
        "pelt_half_life",
        "pelt_continuous_normalization",
        "kernel_stack_initialization",
        "cfs_self_selection",
        "rr_self_selection",
        "ipc_priority_inheritance",
        "migration_current_owner",
    ],
    "process": ["heap_rollback"],
    "users": [
        "syscall_values",
        "user_ranges",
        "fork_exec_exit_reap",
        "pipe_output_rollback",
        "yield_reuse",
        "wait_status_rollback",
        "pipe_waits_for_writer",
        "pipe_cross_cpu_roundtrip",
        "pipe_waits_for_reader",
        "cross_cpu_exit_reap",
        "fork_fd_allocation_rollback",
        "mmap_heap_rollback",
        "fork_process_allocation_rollback",
        "fork_metadata_allocation_rollback",
    ],
    "users.vm": [
        "private_cow",
        "readonly_cow",
        "access_permissions",
        "brk_lifecycle",
        "kernel_text",
        "kernel_rodata",
        "kernel_data",
        "kernel_page_table",
        "kernel_mmio",
    ],
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
        "cow_copy_fault",
        "cow_partial_read",
        "cow_user_fault",
    ],
    "users.lifecycle": ["core_paths_recovery"],
    "users.applications": ["core_application_recovery"],
    "users.libc": ["static_runtime", "filesystem_permissions"],
    "users.exec": [
        "rejects_invalid_entry",
        "rejects_phentsize",
        "rejects_load_size",
        "rejects_truncated_header",
        "rejects_truncated_phdr",
        "rejects_file_range",
        "rejects_user_range",
        "rejects_address_overflow",
        "rejects_page_offset",
        "rejects_alignment",
        "rejects_reserved_range",
        "rejects_page_overlap",
        "rejects_program_header_limit",
        "rejects_wx",
        "rejects_dynamic",
        "rejects_interp",
        "rejects_orphan_tls_file",
        "bad_env_vector",
        "bad_env_string",
        "argument_count_limit",
        "combined_count_limit",
        "string_byte_limit",
        "exact_combined_count",
        "exact_string_bytes",
        "empty_vectors",
        "allocation_rollback",
        "mutable_snapshot_rollback",
        "boundary_load_plan",
        "source_version",
        "registration_gate",
        "shared_thread_gate",
        "startup_capability",
    ],
    "users.busybox": [
        "ash_exit",
        "ash_substitution",
        "ash_exec_environment",
        "text_pipeline",
        "directory_lifecycle",
        "file_redirection",
        "file_copy",
        "file_rename",
        "application_workflow",
        "head",
        "cut",
        "sort",
        "uniq",
        "tr",
        "tee",
        "cmp",
        "basename",
        "dirname",
        "rmdir",
        "uname",
        "kill",
        "find",
        "find_rejects_unsupported",
    ],
    "users.timers": [
        "relative_sleep",
        "absolute_sleep",
        "invalid_arguments",
        "short_reuse",
        "cancel_in_flight",
        "early_wakeup",
        "arm_failure_recovery",
        "relative_interrupted",
        "clock_relative_interrupted",
        "clock_absolute_interrupted",
    ],
    "users.ipc": [
        "roundtrip",
        "deadline",
        "peer_death",
        "signal_cancel",
        "capability_transfer",
        "delivery_rollback",
        "memory_object",
        "badged_sender",
        "nested_roundtrip",
        "domain_control",
        "domain_selection",
        "domain_wait_any",
    ],
    "users.signals": [
        "basic_handler",
        "nested_signals",
        "sigchld",
        "wait_registration",
        "wait_interrupted",
        "wait_restarted",
        "cpu_bound_irq",
        "stop_continue",
        "wait_job_status",
        "no_cldstop",
        "wait_process_group",
        "wait_group_change",
        "no_cldwait",
        "sigaction_race",
        "sigaction_discard",
        "signal_exit_status",
        "sigprocmask",
        "sigaltstack",
        "sig_ign",
        "invalid_arguments",
        "frame_validation",
        "altstack_overflow",
        "altstack_boundaries",
        "inheritance",
        "pid_lifecycle",
        "pipe_sigpipe",
        "pipe_interrupted",
        "pipe_restarted",
        "pipe_noninterrupting_signals",
        "pipe_partial_interrupt",
        "signal_wakeup_affinity",
        "console_interrupted",
        "console_partial_interrupt",
        "console_restarted",
        "console_multi_reader",
    ],
    "users.console_irq": ["irq_before_registration"],
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
        "user_copy_version",
        "raw_copy_fixup",
        "map_preserves_existing",
        "map_allocation_rollback",
        "map_rejects_blocks",
        "clone_preserves_destination",
        "clone_allocation_rollback",
        "address_space_heap_rollback",
        "address_space_control_rollback",
        "vma_heap_rollback",
        "asid_leases",
        "unmap_reclaims_tables",
    ],
    "self": ["accounting_registration", "registry_limits", "cleanup_guards", "heap_bounds"],
    "self.fail": ["intentional_assertion", "not_run"],
    "self.panic": ["intentional_panic"],
    "self.timeout": ["intentional_timeout"],
    **{
        f"bench.{name}": [f"bench.{name}"]
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
    },
}
FUNCTIONAL = [
    "drivers",
    "resources",
    "mm",
    "mm.permissions",
    "mm.transactions",
    "mm.lifetime",
    "mm.concurrent",
    "mm.uaccess",
    "mm.tlb_broadcast",
    "pfa",
    "heap",
    "containers",
    "containers.smp",
    "vfs.smp",
    "interrupts.smp",
    "vfs",
    "timers",
    "scheduler",
    "process",
    "users",
    "users.vm",
    "users.frame",
    "users.uaccess",
    "users.signals",
    "users.lifecycle",
    "users.timers",
    "users.ipc",
    "users.libc",
    "users.exec",
    "users.busybox",
]
BENCHMARKS = [name for name in CATALOG if name.startswith("bench.")]
TLB_JOIN = ["mm.tlb_join.request_first", "mm.tlb_join.cpu_first"]


def functional_workloads(arch: str) -> list[str]:
    """Select only workloads supported by each architecture's interrupt path."""
    return [
        *FUNCTIONAL,
        *(["users.console_irq"] if arch in ("ARM64", "X64") else []),
        *(TLB_JOIN if arch in ("X64", "RISCV64") else []),
    ]


BENCHMARK_KINDS = {
    f"bench.{name}": "event_sum" if name in ("signal", "wakeup", "timer") else "elapsed_batch"
    for name in ("fault", "cow", "switch", "wakeup", "timer", "lifecycle", "signal", "pipe")
}
SELFTESTS = ["self", "self.fail", "self.panic", "self.timeout"]
# Five is the midpoint of CTest's ten-cycle application profile: it preserves
# the full workload while proving liveness before a slow TCG run can consume a
# whole 30-second no-progress window. Core recovery remains sampled every 100
# cycles to keep its 1,000/10,000-cycle runs from becoming protocol-bound.
LIFECYCLE_INTERVAL = {"users.lifecycle": 100, "users.applications": 5}
RESOURCE_FIELDS = (
    "heap_bytes",
    "free_pages",
    "processes",
    "threads",
    "descriptors",
    "file_refs",
    "user_pages",
    "stack_pages",
    "vfs_inodes",
    "vfs_dentries",
    "vfs_files",
)


class Record(BaseModel):
    model_config = ConfigDict(extra="allow", strict=True)
    v: Literal[1]
    event: Literal[
        "ready",
        "catalog",
        "worker",
        "case_start",
        "case_end",
        "checkpoint",
        "clock",
        "calibration",
        "batch",
        "fatal",
        "end",
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

    def __init__(
        self,
        workload: str,
        cpus: int,
        memory_mib: int,
        warmup: int,
        samples: int,
        stability: bool = False,
        lifecycle_cycles: int | None = None,
    ):
        self.workload = workload
        self.expected = CATALOG[workload]
        self.cpus, self.memory_mib = cpus, memory_mib
        self.warmup, self.samples = warmup, samples
        self.stability = stability
        # Stability is a fixed 10,000-cycle acceptance contract. Routine
        # lifecycle callers may request a smaller multiple of their checkpoint
        # cadence, such as CTest's ten application workflows.
        self.lifecycle_cycles = 10000 if stability else lifecycle_cycles or 1000
        if workload in LIFECYCLE_INTERVAL and self.lifecycle_cycles % LIFECYCLE_INTERVAL[workload]:
            raise ValueError("lifecycle cycle count must align with checkpoint interval")
        self.ready: dict | None = None
        self.catalog: list[str] = []
        self.worker: dict | None = None
        self.active: str | None = None
        self.case_started: float | None = None
        self.last_progress: float | None = None
        self.cases: list[dict] = []
        self.checkpoints: list[dict] = []
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
            if not 0 < integer(record, "managed_pages") * 4096 <= ram:
                raise ValueError("allocator page count is outside firmware RAM")
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
            if not self.worker or self.active or len(self.cases) >= len(self.expected):
                raise ValueError("invalid case start")
            if record.get("case") != self.expected[len(self.cases)]:
                raise ValueError("unexpected case identity/order")
            self.active = record["case"]
            self.case_started = time.monotonic()
            self.last_progress = self.case_started
        elif event == "case_end":
            if not self.active or record.get("case") != self.active:
                raise ValueError("case completion without matching start")
            count = integer(record, "failed")
            passed = integer(record, "passed")
            if (
                not count
                and self.workload in LIFECYCLE_INTERVAL
                and (
                    not self.checkpoints
                    or self.checkpoints[-1]["cycles"] < self.lifecycle_cycles
                    or (
                        not self.stability
                        and len(self.checkpoints) != self.lifecycle_cycles // LIFECYCLE_INTERVAL[self.workload] + 1
                    )
                    or self.checkpoints[-1]["elapsed_ns"] < (1800 * 10**9 if self.stability else 0)
                    or any(
                        sample[key] != self.checkpoints[0][key]
                        for sample in self.checkpoints
                        for key in RESOURCE_FIELDS
                    )
                )
            ):
                raise ValueError("missing or unequal lifecycle resource recovery")
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
        elif event == "checkpoint":
            if self.workload not in LIFECYCLE_INTERVAL or not self.active or record.get("case") != self.active:
                raise ValueError("unexpected resource checkpoint")
            if (
                integer(record, "cycles", 0, 2**31 if self.stability else self.lifecycle_cycles)
                != len(self.checkpoints) * LIFECYCLE_INTERVAL[self.workload]
            ):
                raise ValueError("resource checkpoint sequence")
            if integer(record, "application_cycles") != (
                record["cycles"] if self.workload == "users.applications" else 0
            ):
                raise ValueError("application_cycles differ from complete core cycles")
            elapsed = integer(record, "elapsed_ns")
            if (not self.checkpoints and elapsed != 0) or (
                self.checkpoints and elapsed <= self.checkpoints[-1]["elapsed_ns"]
            ):
                raise ValueError("resource checkpoint time did not advance")
            for key in RESOURCE_FIELDS:
                integer(record, key)
            self.checkpoints.append(record)
            self.last_progress = time.monotonic()
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
            if self.workload in BENCHMARK_KINDS and record.get("measurement_kind") != BENCHMARK_KINDS[self.workload]:
                raise ValueError("missing or invalid measurement kind")
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
            for marker in (b"KERNEL PAGE FAULT", b"HALTED", b"RISC-V 64 EXCEPTION", b"UNHANDLED USER EXCEPTION")
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
        if any(marker in serial for marker in (b"[P]", b"KERNEL PAGE FAULT", b"HALTED", b"RISC-V 64 EXCEPTION")):
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


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def pin_vcpus(path: Path, process: subprocess.Popen, host_cpus: list[int], deadline: float) -> list[dict]:
    """Pin only this paused QEMU's vCPU threads, verify, then resume via QMP."""
    with qmp_session(path, process, deadline) as command:
        cpus = command("query-cpus-fast")
        if not isinstance(cpus, list) or len(cpus) != len(host_cpus) or not all(isinstance(cpu, dict) for cpu in cpus):
            raise ValueError("QMP vCPU count mismatch")
        if len({integer(cpu, "thread-id", 1) for cpu in cpus}) != len(cpus):
            raise ValueError("QMP vCPUs do not have independent host threads")
        cpus = sorted(cpus, key=lambda cpu: integer(cpu, "cpu-index"))
        bindings = []
        for index, cpu in enumerate(cpus):
            tid = integer(cpu, "thread-id", 1)
            if cpu["cpu-index"] != index or not Path(f"/proc/{process.pid}/task/{tid}").exists():
                raise ValueError("QMP vCPU identity is not owned by this guest")
            os.sched_setaffinity(tid, {host_cpus[index]})
            if os.sched_getaffinity(tid) != {host_cpus[index]}:
                raise ValueError("vCPU host affinity was not applied")
            bindings.append(dict(cpu=index, thread_id=tid, host_cpu=host_cpus[index]))
        command("cont")
        return bindings


def run_guest(cfg: Artifacts, workload: str, directory: Path, settings: dict, iterations: int) -> dict:
    directory.mkdir()
    lifecycle_cycles = 10000 if settings.get("stability") else iterations or 1000
    if workload in LIFECYCLE_INTERVAL and lifecycle_cycles % LIFECYCLE_INTERVAL[workload]:
        raise ValueError("lifecycle iterations must align with the checkpoint interval")
    case_timeout = settings.get("case_timeout")
    if case_timeout is None:
        # Exhaustive RAM access and repeated process lifecycles can exceed 5 s
        # under host pressure. BusyBox's file-rename workflow took 6.32 s on
        # ARM64 Debug while compiling; retain the same bounded 30 s deadline.
        case_timeout = (
            30.0
            if workload
            in ("pfa", "users.signals", "users.console_irq", "users.busybox", *LIFECYCLE_INTERVAL, *BENCHMARK_KINDS)
            else 5.0
        )
    progress_deadline = settings.get("stability", False) or workload == "users.applications"
    guest_timeout = settings.get("guest_timeout")
    if guest_timeout is None:
        # A distinct application workload, not a relaxation of the original
        # core case's 30 s total deadline. Bound each verified interval by 30 s
        # and the whole run by its finite number of intervals plus warmup/boot.
        guest_timeout = 60.0
        if workload == "users.applications":
            intervals = lifecycle_cycles // LIFECYCLE_INTERVAL[workload]
            guest_timeout = (intervals + 1) * case_timeout + settings["startup_timeout"]
    if settings.get("stability"):
        guest_timeout = max(guest_timeout, 1800 + settings["startup_timeout"] + 60)
    state = Protocol(
        workload,
        settings["cpus"],
        settings.get("expected_ram_mib", settings["memory_mib"]),
        settings["warmup"],
        settings["samples"],
        settings.get("stability", False),
        lifecycle_cycles,
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
            "stability": int(settings.get("stability", False)),
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
    stability_release_seconds = None
    start = time.monotonic()
    qmp_directory = None
    host_bindings = None
    failure_capture = {"status": "not_needed"}
    diagnostic_elapsed = 0.0
    serial_inputs = []
    try:
        # Short, private Unix paths avoid port races across concurrent presets.
        qmp_directory = tempfile.TemporaryDirectory(prefix="moss-qmp-")
        sockets = Path(qmp_directory.name)
        args += [
            "-qmp",
            f"unix:{sockets / 'control.sock'},server=on,wait=off",
            "-gdb",
            f"unix:{sockets / 'gdb.sock'},server=on,wait=off",
        ]
        if settings.get("host_cpus"):
            args += ["-S"]
        with (
            serial_path.open("wb") as serial_out,
            error_path.open("wb") as diagnostics,
            serial_path.open("rb") as serial_in,
        ):
            process = subprocess.Popen(
                args,
                stdin=subprocess.PIPE
                if settings.get("stability") or workload in ("users.signals", "users.console_irq")
                else subprocess.DEVNULL,
                stdout=serial_out,
                stderr=diagnostics,
            )
            if settings.get("host_cpus"):
                host_bindings = pin_vcpus(
                    sockets / "control.sock", process, settings["host_cpus"], start + settings["startup_timeout"]
                )
            while True:
                if serial_path.stat().st_size > 32 * 2**20:
                    raise ValueError("serial log exceeded 32 MiB")
                pending += serial_in.read(2**20)
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    line = line.rstrip(b"\r")
                    state.accept(line)
                    if (
                        workload == "users.signals"
                        and state.active == "console_partial_interrupt"
                        and not any(item["case"] == state.active for item in serial_inputs)
                    ):
                        # The fixture consumes one byte before forking, then checks
                        # that a caught signal returns the second as a partial read.
                        process.stdin.write(b"kk")
                        process.stdin.flush()
                        serial_inputs.append({"case": state.active, "hex": "6b6b"})
                    if (
                        workload == "users.signals"
                        and state.active == "console_multi_reader"
                        and line == b"MOSS_CONSOLE_MULTI_READY"
                    ):
                        process.stdin.write(b"ab")
                        process.stdin.flush()
                        serial_inputs.append({"case": state.active, "hex": "6162"})
                    if (
                        workload == "users.signals"
                        and state.active == "console_restarted"
                        and line == b"MOSS_CONSOLE_RESTART_READY"
                    ):
                        process.stdin.write(b"r")
                        process.stdin.flush()
                        serial_inputs.append({"case": state.active, "hex": "72"})
                    if (
                        workload == "users.console_irq"
                        and state.active == "irq_before_registration"
                        and line == b"MOSS_CONSOLE_IRQ_READY"
                    ):
                        process.stdin.write(b"r")
                        process.stdin.flush()
                        serial_inputs.append({"case": state.active, "hex": "72"})
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
                if (
                    settings.get("stability")
                    and stability_release_seconds is None
                    and state.case_started is not None
                    and now - state.case_started >= 1800
                    and state.checkpoints
                    and state.checkpoints[-1]["cycles"] >= 10000
                ):
                    # This only permits completion; the guest must still meet
                    # its own duration, cycle and exact resource checks.
                    process.stdin.write(b"S")
                    process.stdin.flush()
                    stability_release_seconds = now - state.case_started
                if not state.ready and now - start >= settings["startup_timeout"]:
                    reason = "startup_timeout"
                elif (
                    state.case_started
                    and now - (state.last_progress if progress_deadline else state.case_started) >= case_timeout
                ):
                    reason = "no_progress_timeout" if progress_deadline else "case_timeout"
                elif now - start >= guest_timeout:
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
            try:
                live_status, _ = state.outcome(termination, reason, serial_path.read_bytes())
                if reason != "cancelled" and (live_status != "passed" or state.failed or state.fatal):
                    capture_started = time.monotonic()
                    try:
                        failure_capture = capture_failure(
                            cfg, process, sockets, directory / "diagnostics", settings["cpus"], settings.get("gdb")
                        )
                    except (OSError, ValueError, KeyboardInterrupt) as error:
                        failure_capture = {"status": "error", "errors": [str(error)]}
                    finally:
                        diagnostic_elapsed = time.monotonic() - capture_started
                        failure_capture["elapsed_seconds"] = diagnostic_elapsed
            finally:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        elif reason or not state.end:
            failure_capture = {"status": "unavailable", "errors": ["guest is not alive"]}
        if process:
            raw_exit = process.returncode
            if process.stdin:
                process.stdin.close()
        if qmp_directory:
            qmp_directory.cleanup()
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
                settings.get("stability", False),
                lifecycle_cycles,
            )
            for line in serial.splitlines():
                replay.accept(line)
        except ValueError as error:
            reason = f"infrastructure: {error}"
        if (
            not reason
            and settings.get("stability")
            and not state.failed
            and (not state.cases or state.cases[0]["elapsed_seconds"] < 1800)
        ):
            reason = "infrastructure: stability guest completed before 30 host minutes"
        if not reason and settings.get("stability") and not state.failed and stability_release_seconds is None:
            reason = "infrastructure: stability guest completed without host release"
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
        "checkpoints": state.checkpoints,
        "raw_exit": raw_exit,
        "termination": termination,
        "completion": state.end,
        "elapsed_seconds": time.monotonic() - start - diagnostic_elapsed,
        "case_timeout_seconds": case_timeout,
        "case_timeout_kind": "no_progress" if progress_deadline else "total",
        "guest_timeout_seconds": guest_timeout,
        "stability_release_seconds": stability_release_seconds,
        "serial_inputs": serial_inputs,
        "qemu_args": args,
        "serial_log": str(serial_path),
        "qemu_log": str(error_path),
        "parameters": {"order": settings["order"], "iterations": iterations},
        "host_bindings": host_bindings,
        "diagnostics": failure_capture,
    }
    if workload in BENCHMARK_KINDS:
        result["parameters"]["measurement_kind"] = BENCHMARK_KINDS[workload]
    if workload == "bench.pipe":
        result["parameters"]["transfer_bytes"] = 1024
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
        if BENCHMARK_KINDS.get(workload) == "event_sum":
            result["measurement"]["interpretation"] = (
                "mean of individually bracketed event latencies per batch; not a percentile"
            )
        if workload == "bench.pipe":
            result["measurement"]["bytes_per_second"] = 1024 * 1e9 / statistics.median(values)
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
            "RISCV64": ("dtb.timebase-frequency",),
            "X64": ("cpuid.15", "pit.channel0"),
        }
        if item["clock"]["source"] not in sources[arch]:
            return None
        if item.get("termination") != "protocol_end":
            return None
        settings = environment["settings"]
        host_cpus = settings.get("host_cpus")
        if host_cpus is not None:
            if (
                not isinstance(host_cpus, list)
                or len(host_cpus) != integer(settings, "cpus", 1)
                or any(type(cpu) is not int or cpu < 0 for cpu in host_cpus)
                or len(set(host_cpus)) != len(host_cpus)
            ):
                return None
            bindings = item.get("host_bindings")
            if not isinstance(bindings, list) or len(bindings) != len(host_cpus):
                return None
            if any(
                not isinstance(b, dict) or integer(b, "cpu") != i or integer(b, "host_cpu") != cpu
                for i, (b, cpu) in enumerate(zip(bindings, host_cpus, strict=True))
            ):
                return None
            if len({integer(b, "thread_id", 1) for b in bindings}) != len(bindings):
                return None
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
    stability: bool = False,
    baseline: Annotated[Path | None, typer.Option()] = None,
    cpus: int = 4,
    host_cpus: str | None = None,
    memory_mib: int | None = None,
    expected_ram_mib: int | None = None,
    warmup: int = 5,
    samples: int = 30,
    iterations: int = 0,
    order: int = 0,
    startup_timeout: float = 30,
    case_timeout: float | None = None,
    guest_timeout: float | None = None,
    gdb: Annotated[
        str | None, typer.Option(help="Failure-capture debugger; defaults to gdb-multiarch or gdb on PATH")
    ] = None,
) -> None:
    """One fresh guest per suite/scenario; samples reuse that guest."""
    selected = (
        workload
        if workload is not None
        else (["users.lifecycle"] if stability else BENCHMARKS if benchmark else SELFTESTS if selftest else FUNCTIONAL)
    )
    if stability and (len(selected) != 1 or selected[0] not in LIFECYCLE_INTERVAL or benchmark or selftest or baseline):
        raise typer.BadParameter("stability requires one core or application lifecycle suite")
    if not selected or len(set(selected)) != len(selected) or any(name not in CATALOG for name in selected):
        raise typer.BadParameter("select unique, nonempty known workload IDs")
    host_cpu_ids = None
    if host_cpus is not None:
        try:
            host_cpu_ids = [int(cpu) for cpu in host_cpus.split(",")]
            if (
                not hasattr(os, "sched_setaffinity")
                or len(host_cpu_ids) != cpus
                or len(set(host_cpu_ids)) != cpus
                or not set(host_cpu_ids) <= os.sched_getaffinity(0)
            ):
                raise ValueError("one distinct eligible host CPU is required per vCPU")
        except ValueError as error:
            raise typer.BadParameter(f"invalid host CPUs: {error}") from error
    if not (
        1 <= cpus <= 16 and 0 <= warmup <= 100 and 1 <= samples <= 1000 and 0 <= iterations <= 65536 and 0 <= order <= 4
    ):
        raise typer.BadParameter("invalid resource or sampling parameters")
    if "containers.smp" in selected and cpus < 2:
        raise typer.BadParameter("containers.smp requires at least 2 CPUs; select single-worker workloads for 1 CPU")
    if "vfs.smp" in selected and cpus < 2:
        raise typer.BadParameter("vfs.smp requires at least 2 CPUs; select single-worker workloads for 1 CPU")
    if "interrupts.smp" in selected and cpus < 2:
        raise typer.BadParameter("interrupts.smp requires at least 2 CPUs")
    if "mm.lifetime" in selected and cpus < 2:
        raise typer.BadParameter("mm.lifetime requires at least 2 CPUs; select single-worker workloads for 1 CPU")
    if "mm.concurrent" in selected and cpus < 2:
        raise typer.BadParameter("mm.concurrent requires at least 2 CPUs; select single-worker workloads for 1 CPU")
    if "mm.uaccess" in selected and cpus < 2:
        raise typer.BadParameter("mm.uaccess requires at least 2 CPUs; select single-worker workloads for 1 CPU")
    if "mm.tlb_broadcast" in selected and cpus < 2:
        raise typer.BadParameter("mm.tlb_broadcast requires at least 2 CPUs")
    if "scheduler" in selected and cpus < 2:
        raise typer.BadParameter("scheduler migration probe requires at least 2 CPUs")
    if "users.timers" in selected and cpus < 3:
        raise typer.BadParameter("users.timers cancellation probe requires at least 3 CPUs")
    if any(
        not math.isfinite(value) or value <= 0
        for value in (startup_timeout, case_timeout, guest_timeout)
        if value is not None
    ):
        raise typer.BadParameter("deadlines must be positive")
    cfg = Artifacts.load(manifest)
    if workload is None and not (benchmark or selftest or stability):
        selected = functional_workloads(cfg.arch)
    if "users.console_irq" in selected and cfg.arch not in ("ARM64", "X64"):
        raise typer.BadParameter("users.console_irq requires ARM64 or x64 interrupt-driven console")
    if any(name in TLB_JOIN for name in selected):
        if cfg.arch not in ("X64", "RISCV64"):
            raise typer.BadParameter("TLB CPU-join workloads require x64 or riscv64")
        if cpus < 2:
            raise typer.BadParameter("TLB CPU-join workloads require at least 2 CPUs")
    cpu, memory_mib = resolve_resources(cfg.arch, machine, cpu, memory_mib)
    if expected_ram_mib is None:
        expected_ram_mib = RASPI_CONFIG.get((machine or "").split(",")[0], {}).get("firmware_ram_mib", memory_mib)
    if not 256 <= expected_ram_mib <= memory_mib:
        raise typer.BadParameter("expected firmware RAM must be between 256 MiB and installed RAM")
    dtb = resolve_dtb(cfg.arch, machine, dtb)
    if "users.simd_fault" in selected and cfg.arch != "X64":
        raise typer.BadParameter("users.simd_fault requires x64")
    build = cfg.manifest.parent
    metadata = cfg.build
    qemu = resolve_qemu(cfg.arch, qemu)
    if any(name.startswith("bench.") for name in selected) and metadata["type"] != "Release":
        raise typer.BadParameter("benchmarks require the production Release build policy")
    prior = load_json(baseline.read_text()) if baseline else None
    if prior:
        validate_report(prior)
    output = (output or build / "validation" / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=False)
    cfg = cfg.snapshot_validation(output / "inputs")
    if dtb:
        expected_dtb = sha256(dtb)
        frozen_dtb = output / "inputs" / "board.dtb"
        shutil.copy2(dtb, frozen_dtb)
        if sha256(frozen_dtb) != expected_dtb:
            raise ValueError("device tree changed while capturing validation inputs")
        dtb = frozen_dtb
    settings = dict(
        cpus=cpus,
        host_cpus=host_cpu_ids,
        memory_mib=memory_mib,
        expected_ram_mib=expected_ram_mib,
        warmup=warmup,
        samples=samples,
        order=order,
        startup_timeout=startup_timeout,
        case_timeout=case_timeout,
        guest_timeout=guest_timeout,
        stability=stability,
    )
    governors = {}
    for host_cpu in host_cpu_ids or ():
        governor = Path(f"/sys/devices/system/cpu/cpu{host_cpu}/cpufreq/scaling_governor")
        governors[str(host_cpu)] = governor.read_text().strip() if governor.exists() else None
    environment = {
        "arch": cfg.arch,
        "build": metadata,
        "qemu": get_qemu_version(qemu),
        "host": platform.uname()._asdict()
        | {
            "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
            "cpu_governors": governors,
        },
        "settings": settings,
        "accelerator": "tcg",
        "tcg_cache_mib": TCG_CACHE_MIB,
        "clock_policy": 1,
        "fixture_sha256": sha256(cfg.require("validation_initramfs")),
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
        "provenance": {**cfg.validation_provenance(), "manifest": str(cfg.manifest)},
    }
    report["inputs"] = {name: str(cfg.require(name)) for name in ("validation_kernel", "validation_initramfs")} | {
        "dtb": str(dtb) if dtb else None,
        "validation_debug_symbols": str(cfg.require("validation_debug_symbols"))
        if cfg.files.get("validation_debug_symbols") is not None
        else None,
    }
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
            execution = dict(settings, qemu=qemu, machine=machine, cpu=cpu, dtb=dtb, gdb=gdb)
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
