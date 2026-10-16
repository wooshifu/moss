#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Prepare and measure the production Pi userspace probe; deploy its manifest with rpi.py first."""

import argparse
import hashlib
import json
import math
import os
import select
import shutil
import statistics
import sys
import time
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.artifacts import Artifacts
from scripts.gen_initramfs import align4, make_cpio_entry, make_cpio_trailer
from scripts.rpi import OpenOCD, run, serial_port, tool, uart, write_all

ROOT = Path(__file__).resolve().parents[1]
# Exploratory batches: cheap calls need more iterations; fork/exec and page
# faults are deliberately bounded to keep the real board responsive.
CASES = [
    ("clock", 20000, 1),
    ("getpid_raw", 20000, 1),
    ("getpid_libc", 1000, 1),
    ("yield", 1000, 1),
    ("memcpy", 256, 1024 * 1024),
    ("open_close", 1000, 1),
    ("read_seek", 4096, 64),
    ("read_seek", 4096, 512),
    ("read_seek", 1024, 4096),
    ("read_seek", 64, 65536),
    ("write_seek", 1024, 4096),
    ("fault", 8, 1024 * 1024),
    ("fork_raw", 8, 1),
    ("fork_libc", 8, 1),
    ("exec", 8, 1),
    ("ipc0", 1000, 1),
    ("ipc0_deadline", 1000, 1),
    ("ipc1", 1000, 1),
    ("ipc1_deadline", 1000, 1),
]


# Short operations retain 7,168 individual observations; expensive exec retains
# 1,792. These are exploratory p99 estimates, not production tail guarantees.
PMU_CASES = [("empty", 1024, 1), ("instructions", 1024, 1)] + [
    (name, 256 if name == "exec" else 1024, size) for name, _, size in CASES
]
PMU_COUNTERS = [
    "el0_instructions",
    "el1_instructions",
    "el0_cycles",
    "el1_cycles",
    "l1d_refills",
    "l2d_refills",
    "cycles",
]


SERVICES = (
    "code-authority-service",
    "loader-service",
    "file-service",
    "namespace-service",
    "process-service",
    "pipe-service",
    "console-service",
)


def temperature():
    """Read the firmware-enabled sensor from idle CPU3, outside timed workloads."""
    started = time.time_ns()
    with OpenOCD() as ocd:
        ocd.command("targets bcm2837.cpu3")
        try:
            ocd.command("halt 2000")  # Bound the diagnostic halt to two seconds.
            # The probe uses CPUs 0/1; CPU3 is idle in EL1 between cases.
            # Moss identity-maps the BCM2837 MMIO aperture. Virtual reads avoid
            # OpenOCD disabling the MMU and flushing caches for a physical read.
            raw = [int(word, 0) for word in ocd.command("read_memory 0x3f212000 32 2").split()]
        finally:
            ocd.command("resume")
    if len(raw) != 2 or not raw[1] & (1 << 10):
        raise ValueError("BCM2837 temperature sensor has no valid sample")
    # Linux bcm2835_thermal + bcm2837.dtsi: VALID bit 10, ten-bit ADC,
    # temperature in milli-Celsius = 412000 - 538 * ADC. Do not alter control.
    return {
        "time_ns": started,
        "observation_ns": time.time_ns() - started,
        "access": "CPU3 kernel identity MMIO mapping",
        "control": raw[0],
        "status": raw[1],
        "millidegrees_c": 412000 - 538 * (raw[1] & 0x3FF),
    }


def diagnostic_samples(lines, iterations, service_cpu=None):
    calibration = [
        json.loads(line.removeprefix("RPI_CALIBRATION ")) for line in lines if line.startswith("RPI_CALIBRATION {")
    ]
    rows = [json.loads(line.removeprefix("RPI_PMU ")) for line in lines if line.startswith("RPI_PMU {")]
    if len(calibration) != 1:
        raise ValueError("Missing or duplicate timer calibration")
    timing = calibration[0]
    if any(type(timing.get(key)) is not int or timing[key] <= 0 for key in ("frequency", "ticks", "ns")):
        raise ValueError("Invalid timer calibration")
    converted = timing["ticks"] * 1e9 / timing["frequency"]
    # The 20 ms calibration should amortize two clock syscalls to much less
    # than 1%; reject a mismatch instead of reporting unvalidated nanoseconds.
    if abs(converted - timing["ns"]) > timing["ns"] / 100:
        raise ValueError("Generic timer disagrees with the kernel clock")
    if len(rows) != 9 or any(
        row.get("sample") != index
        or len(row.get("counters", [])) != len(PMU_COUNTERS)
        or len(row.get("ticks", [])) != iterations
        or any(type(n) is not int or n < 0 for n in row["counters"])
        or any(type(n) is not int or n < 0 or n >= timing["frequency"] for n in row["ticks"])
        for index, row in enumerate(rows)
    ):
        raise ValueError("Incomplete or invalid PMU/tail samples")
    # Drop exactly the two warmups; preserve all raw rows for recalculation.
    values = sorted(tick * 1e9 / timing["frequency"] for row in rows[2:] for tick in row["ticks"])
    placements = [
        json.loads(line.removeprefix("RPI_PLACEMENT ")) for line in lines if line.startswith("RPI_PLACEMENT {")
    ]
    if len(placements) > 1:
        raise ValueError("Duplicate placement snapshot")
    if service_cpu is not None:
        if len(placements) != 1:
            raise ValueError("Missing service placement snapshot")
        processes = placements[0]["processes"]
        for service in SERVICES:
            # Native TopInfo stores at most fifteen name bytes plus its NUL.
            expected = (service + ".elf")[:15]
            matches = [proc for proc in processes if proc["name"] == expected]
            if len(matches) != 1 or matches[0]["cpu"] != service_cpu:
                raise ValueError(f"Uncontrolled CPU placement: {service}")
    return {
        "placement": placements[0] if placements else None,
        "calibration": timing,
        "samples": rows,
        "count": len(values),
        "tail_ns": {str(p): values[math.ceil(p * len(values) / 100) - 1] for p in (50, 95, 99, 100)},
        "counters_per_op": {
            name: sum(row["counters"][i] for row in rows[2:]) / len(values) for i, name in enumerate(PMU_COUNTERS)
        },
    }


def probe_archive(archive: bytes, probe: bytes, replacements: dict[bytes, bytes]):
    position, last_ino, chunks = 0, 0, []
    remaining = dict(replacements)
    while True:
        if archive[position : position + 6] != b"070701":
            raise ValueError("Expected a newc production initramfs")
        # newc has a six-byte magic plus thirteen eight-digit hexadecimal fields.
        fields = [int(archive[position + 6 + i * 8 : position + 14 + i * 8], 16) for i in range(13)]
        ino, size, namesize = fields[0], fields[6], fields[11]
        name = archive[position + 110 : position + 110 + namesize - 1]
        if name == b"TRAILER!!!":
            break
        last_ino = max(last_ino, ino)
        end = align4(align4(position + 110 + namesize) + size)
        if name in replacements:
            if name not in remaining:
                raise ValueError("Duplicate service in production archive")
            chunks.append(make_cpio_entry(name.decode(), remaining.pop(name), ino=ino, mode=fields[1]))
        else:
            chunks.append(archive[position:end])
        position = end
    if remaining:
        raise ValueError("Missing services in production archive")
    return b"".join(chunks) + make_cpio_entry("rpi-perf.elf", probe, ino=last_ino + 1) + make_cpio_trailer()


def prepare(manifest: Path, output: Path, pmu: bool = False):
    artifacts = Artifacts.load(manifest)
    if artifacts.arch != "ARM64" or artifacts.build["type"] != "Release":
        raise ValueError("Use the ARM64 Release manifest")
    output.mkdir(parents=True, exist_ok=True)
    runtime = artifacts.manifest.parent / "src/userspace/mlibc"
    binary = output / "rpi-perf.elf"

    def link_program(source, binary):
        run(
            tool("clang"),
            "--target=aarch64-unknown-elf",
            "-mno-outline-atomics",
            "-O2",
            "-ffreestanding",
            "-fno-stack-protector",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'src/userspace'}",
            f"-I{ROOT / 'src/abi/include'}",
            "-isystem",
            str(runtime / "sysroot/usr/include"),
            "-nostdlib",
            "-static",
            "-fuse-ld=lld",
            # Match production runtime programs: link above inherited identity maps, on 4 KiB pages.
            "-Wl,--image-base=0x200000000,-z,max-page-size=4096",
            str(source),
            str(runtime / "mlibc/CMakeFiles/mlibc-crt1.dir/sysdeps/moss/crt1.S.obj"),
            str(runtime / "sysroot/usr/lib/libc.a"),
            str(runtime / "builtins/lib/generic/libclang_rt.builtins-aarch64.a"),
            "-o",
            str(binary),
        )
        run(tool("llvm-strip"), "--strip-all", str(binary))

    link_program(ROOT / "scripts/rpi_perf.c", binary)
    replacements = {}
    if pmu:
        # Fixed service placement makes reboot-to-reboot comparisons meaningful:
        # caller CPU0, IPC peer CPU1, production services CPU2, temperature CPU3.
        # Only the diagnostic archive is rebuilt; the normal service sources stay intact.
        for service in SERVICES:
            source = ROOT / "src/userspace" / ("moss_" + service.replace("-", "_") + ".c")
            wrapper = output / (service + ".c")
            wrapper.write_text(
                '#define main moss_perf_service_main\n#include "' + str(source) + '"\n#undef main\n'
                "int main(int argc, char **argv) {\n"
                "  unsigned mask = 1U << 2; /* Reserve CPU2 for services in the diagnostic image. */\n"
                "  if (syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(mask), (long)&mask)) return 1;\n"
                "  return moss_perf_service_main(argc, argv);\n}\n"
            )
            program = output / (service + ".elf")
            link_program(wrapper, program)
            replacements[program.name.encode()] = program.read_bytes()
    archive = artifacts.require("initramfs").read_bytes()
    (output / "initramfs.cpio").write_bytes(probe_archive(archive, binary.read_bytes(), replacements))
    (output / "probe-preparation.json").write_text(
        json.dumps(
            {
                "service_affinity_cpu": 2 if pmu else None,
                "probe_source_sha256": hashlib.sha256((ROOT / "scripts/rpi_perf.c").read_bytes()).hexdigest(),
                "services_sha256": {
                    name.decode(): hashlib.sha256(data).hexdigest() for name, data in replacements.items()
                },
            },
            indent=2,
        )
        + "\n"
    )
    (output / "rpi-perf.c").write_bytes((ROOT / "scripts/rpi_perf.c").read_bytes())
    data = json.loads(artifacts.manifest.read_text())
    for key in ("kernel", "debug_symbols"):
        source = artifacts.require(key)
        shutil.copyfile(source, output / source.name)
        data["artifacts"][key] = source.name
    data["artifacts"]["initramfs"] = "initramfs.cpio"
    for key in ("validation_kernel", "validation_initramfs", "validation_debug_symbols"):
        data["artifacts"][key] = None
    (output / "moss-artifacts.json").write_text(json.dumps(data, indent=2) + "\n")
    print(f"Prepared {output / 'moss-artifacts.json'}; deploy it using rpi.py deploy --check")


def measure(port: str | None, output: Path, selected: list[str] | None, pmu: bool = False):
    output.mkdir(parents=True, exist_ok=True)
    cases = [case for case in (PMU_CASES if pmu else CASES) if selected is None or case[0] in selected]
    if not cases or (selected and set(selected) - {case[0] for case in cases}):
        raise ValueError("Unknown workload")
    device = serial_port(port)
    report = {
        "port": device,
        "pmu": pmu,
        "collector_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "pmu_scope": "caller CPU0, all tasks/IRQs, excludes server CPU1; reader overhead included" if pmu else None,
        "probe_sha256": hashlib.sha256((output / "rpi-perf.elf").read_bytes()).hexdigest(),
        "probe_source_sha256": hashlib.sha256(
            (
                (output / "source/scripts/rpi_perf.c")
                if (output / "source/scripts/rpi_perf.c").is_file()
                else ROOT / "scripts/rpi_perf.c"
            ).read_bytes()
        ).hexdigest(),
        "cases": [],
        "status": "running",
    }
    preparation = output / "probe-preparation.json"
    if preparation.is_file():
        report["preparation"] = json.loads(preparation.read_text())
        report["probe_source_sha256"] = report["preparation"]["probe_source_sha256"]
    deployment = output / "deploy/result.json"
    if deployment.is_file():
        report["deployment_record"] = json.loads(deployment.read_text())
    result = output / f"measurements-{time.time_ns()}.json"
    try:
        with uart(device) as fd, result.with_suffix(".log").open("ab", buffering=0) as log:
            for name, iterations, size in cases:
                before = temperature() if pmu else None
                suffix = " pmu" if pmu else ""
                command = f"/rpi-perf.elf {name} {iterations} {size}{suffix}; echo RPI_COMMAND_STATUS=$?\n"
                if pmu:
                    # Full-line bursts lost input bytes on this Pi console. Limit
                    # diagnostic input to 500 bytes/s; this is outside guest timing.
                    for byte in command.encode():
                        write_all(fd, bytes([byte]))
                        time.sleep(0.002)
                else:
                    write_all(fd, command.encode())
                data = bytearray()
                # Three minutes bounds unusually slow service/fork cases; this is a host
                # liveness deadline, never a guest latency or regression threshold.
                deadline = time.monotonic() + 180
                while b"\nRPI_COMMAND_STATUS=" not in data.replace(b"\r\n", b"\n") or not data.endswith(b"moss$ "):
                    if time.monotonic() >= deadline:
                        raise TimeoutError(f"Timed out: {name}/{size}")
                    # One-second polls enforce the host deadline without busy waiting.
                    if select.select([fd], [], [], 1)[0]:
                        # Match the console's existing 4 KiB read buffer.
                        block = os.read(fd, 4096)
                        if not block:
                            raise ConnectionError("Serial adapter disconnected")
                        log.write(block)
                        data.extend(block)
                lines = data.decode(errors="replace").replace("\r\n", "\n").splitlines()
                rows = [json.loads(line.removeprefix("RPI_PERF ")) for line in lines if line.startswith("RPI_PERF {")]
                # Require the full protocol and child exit status; echoed commands cannot pass.
                if (
                    "RPI_COMMAND_STATUS=0" not in lines
                    or "RPI_PERF_DONE" not in lines
                    or any("RPI_PERF_ERROR" in line for line in lines)
                ):
                    raise RuntimeError(data.decode(errors="replace"))
                retained = [row for row in rows if not row["warmup"]]
                # Match the guest's two warmups and seven retained batches.
                if (
                    len(rows) != 9
                    or len(retained) != 7
                    or any(
                        row["ns"] <= 0
                        or row["sample"] != index
                        or row["warmup"] != (index < 2)
                        or row["iterations"] != iterations
                        or row["bytes"] != size
                        or row["workload"] != name
                        for index, row in enumerate(rows)
                    )
                ):
                    raise ValueError("Incomplete or invalid probe samples")
                values = [row["ns"] / iterations for row in retained]
                item = {
                    "workload": name,
                    "bytes": size,
                    "median_ns": statistics.median(values),
                    "min_ns": min(values),
                    "max_ns": max(values),
                    "samples": rows,
                }
                if pmu:
                    item["diagnostic"] = diagnostic_samples(
                        lines, iterations, report.get("preparation", {}).get("service_affinity_cpu")
                    )
                    item["temperature_before"], item["temperature_after"] = before, temperature()
                    tail = item["diagnostic"]["tail_ns"]
                    print(
                        f"  p50={tail['50'] / 1000:.3f} p99={tail['99'] / 1000:.3f} "
                        f"max={tail['100'] / 1000:.3f} us; {item['temperature_after']['millidegrees_c'] / 1000:.1f} C",
                        flush=True,
                    )
                report["cases"].append(item)
                print(f"{name:18} bytes={size:7} median={item['median_ns'] / 1000:10.3f} us/op", flush=True)
                result.write_text(json.dumps(report, indent=2) + "\n")
        report["status"] = "passed"
    except Exception as error:
        report["status"], report["error"] = "failed", str(error)
        raise
    finally:
        result.write_text(json.dumps(report, indent=2) + "\n")
        print(f"Results: {result}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "measure"))
    parser.add_argument("--manifest", type=Path, default=Path("build/arm64-release/moss-artifacts.json"))
    parser.add_argument("--output", type=Path, default=Path("build/rpi3b/perf"))
    parser.add_argument("--port")
    parser.add_argument("--pmu", action="store_true", help="Measure PMU/tails/temperature; requires diagnostic kernel")
    parser.add_argument("--workload", action="append", help="Measure only this workload; repeat to select more")
    args = parser.parse_args()
    if args.mode == "prepare":
        prepare(args.manifest, args.output, args.pmu)
    else:
        measure(args.port, args.output, args.workload, args.pmu)


if __name__ == "__main__":
    main()
