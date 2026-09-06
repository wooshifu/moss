#!/usr/bin/env python3
"""Exercise the production allocator with altered firmware maps, using one image.

Requires QEMU and the dtc package's fdtget/fdtput. Results and generated DTBs are
retained beside the supplied manifest; no kernel or CMake configuration changes.
"""

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

try:
    from .artifacts import Artifacts
    from .run_qemu import build_qemu_args, resolve_machine, resolve_qemu
except ImportError:
    from artifacts import Artifacts
    from run_qemu import build_qemu_args, resolve_machine, resolve_qemu

CASES = (
    "unaligned_reserved",
    "ram_hole",
    "ram_many_banks",
    "ram_metadata_later",
    "ram_adjacent",
    "ram_capacity",
    "reserved_end_max",
    "reserved_wrap",
    "reserved_capacity",
)


def command(*args: str) -> str:
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT, timeout=15).strip()


def cells(*values: int) -> list[str]:
    return [f"{word:x}" for value in values for word in (value >> 32, value & 0xFFFFFFFF)]


def put(dtb: Path, node: str, name: str, *values: str) -> None:
    command("fdtput", "-p", "-t", "x", str(dtb), node, name, *values)


def reserve(dtb: Path, index: int, start: int, size: int) -> None:
    put(dtb, "/reserved-memory", "#address-cells", "2")
    put(dtb, "/reserved-memory", "#size-cells", "2")
    put(dtb, "/reserved-memory", "ranges")
    put(dtb, f"/reserved-memory/test{index}@{start:x}", "reg", *cells(start, size))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--case", choices=CASES)
    args = parser.parse_args()
    artifacts = Artifacts.load(args.manifest)
    if artifacts.arch not in ("ARM64", "RISCV"):
        parser.error("DTB fixtures apply to ARM64/RV64; x86 uses the default PVH PFA suite")
    if artifacts.arch == "ARM64" and args.case and args.case.startswith("ram_"):
        parser.error("QEMU's ARM boot loader regenerates /memory; use RV64 for the RAM-bank fixture")
    for tool in ("fdtget", "fdtput"):
        if not shutil.which(tool):
            parser.error(f"{tool} is required (dtc package)")
    image = artifacts.require("validation_kernel")
    original_hash = hashlib.sha256(image.read_bytes()).hexdigest()
    output = artifacts.manifest.parent / "pfa-firmware" / str(time.time_ns())
    output.mkdir(parents=True, exist_ok=False)
    original = output / "original.dtb"
    machine = resolve_machine(artifacts.arch, smp=4)
    invocation = build_qemu_args(
        artifacts, validation=True, qemu=resolve_qemu(artifacts.arch), machine=f"{machine},dumpdtb={original}"
    )
    subprocess.run(invocation, check=True, capture_output=True, timeout=15)
    assert command("fdtget", "-t", "x", str(original), "/", "#address-cells") == "2"
    assert command("fdtget", "-t", "x", str(original), "/", "#size-cells") == "2"
    memory = "/" + next(
        name for name in command("fdtget", "-l", str(original), "/").split() if name.startswith("memory@")
    )
    words = [int(word, 16) for word in command("fdtget", "-t", "x", str(original), memory, "reg").split()]
    assert len(words) == 4
    start, size = (words[0] << 32) | words[1], (words[2] << 32) | words[3]
    # ARM's loader replaces /memory nodes (hw/arm/boot.c:arm_load_dtb).
    # Test RAM-bank shape on RV64; ARM still exercises unaligned reserved holes.
    selected = [name for name in CASES if artifacts.arch != "ARM64" or not name.startswith("ram_")]
    for name in selected:
        if args.case and args.case != name:
            continue
        dtb = output / f"{name}.dtb"
        shutil.copyfile(original, dtb)
        expected_ram = size
        reserved_bytes = 0
        if name == "unaligned_reserved":
            if artifacts.arch == "RISCV":
                expected_ram = size - 3 * 4096 - 27
                put(dtb, memory, "reg", *cells(start + 3 * 4096, expected_ram))
            reserve(dtb, 0, start + 16 * 2**20 + 19, 4097)
            reserve(dtb, 1, start + 16 * 2**20 + 4090, 8192)
            reserve(dtb, 2, start + 64 * 2**20 + 37, 8193)
        elif name in ("ram_hole", "ram_metadata_later"):
            split = start + 512 * 2**20 + 123
            second = split + 0x18000
            expected_ram = size - 3 * 4096 - 0x18000 - 9
            put(
                dtb,
                memory,
                "reg",
                *cells(start + 3 * 4096, split - start - 3 * 4096, second, start + size - second - 9),
            )
            if name == "ram_metadata_later":
                # No metadata-sized space in the kernel's bank; use the next.
                reserve(dtb, 0, start, split - start)
                reserved_bytes = split - start - 3 * 4096
        elif name in ("ram_many_banks", "ram_capacity", "ram_adjacent"):
            banks = 9 if name == "ram_capacity" else 8
            width = size // banks
            ranges = []
            for index in range(banks):
                begin = start + index * width + 123
                end = start + (index + 1) * width + 123 if index + 1 < banks else start + size - 9
                if name != "ram_adjacent":
                    begin += 3 * 4096
                ranges.append((begin, end - begin))
            expected_ram = sum(length for _, length in ranges)
            # Firmware order must not select a preferred bank or hide gaps.
            put(dtb, memory, "reg", *(word for pair in reversed(ranges) for word in cells(*pair)))
        elif name == "reserved_end_max":
            # Valid 64-bit interval, but rounding its exclusive end would wrap.
            reserve(dtb, 0, start + 2**20, (1 << 64) - 1 - start - 2**20)
        elif name == "reserved_wrap":
            reserve(dtb, 0, (1 << 64) - 4096, 8192)
        else:
            # The firmware table has 32 slots; the DTB itself also owns a slot.
            for index in range(32):
                reserve(dtb, index, start + 64 * 2**20 + index * 8192, 4096)
        report_dir = output / name
        run = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).with_name("kernel_validation.py")),
                "run",
                "--manifest",
                str(artifacts.manifest),
                "--workload",
                "pfa",
                "--dtb",
                str(dtb),
                "--output",
                str(report_dir),
                "--startup-timeout",
                "3",
                "--guest-timeout",
                "15",
            ],
            check=False,
        )
        report = json.loads((report_dir / "results.json").read_text())
        assert report["provenance"]["image_sha256"] == original_hash
        if name.startswith("reserved_") or name == "ram_capacity":
            serial = (report_dir / "pfa" / "serial.log").read_text()
            expected_error = (
                "Error: Memory management setup failed" if name == "reserved_end_max" else "Error: Hardware init failed"
            )
            assert run.returncode != 0 and expected_error in serial, report_dir
            assert report["guests"][0]["ready"] is None, report_dir
        else:
            assert run.returncode == 0 and report["guests"][0]["status"] == "passed", report_dir
            ready = report["guests"][0]["ready"]
            assert ready["ram_bytes"] == expected_ram, ("firmware changed the RAM fixture", report_dir, ready)
            if name.startswith("ram_"):
                # The image, metadata and boot allocations consume <32 MiB in
                # this 2-GiB fixture; losing even one 256-MiB bank must fail.
                assert ready["managed_pages"] * 4096 > expected_ram - reserved_bytes - 32 * 2**20, (
                    "allocator ignored usable RAM banks",
                    report_dir,
                    ready,
                )
        print(f"{name}: verified ({report_dir})", flush=True)
    assert hashlib.sha256(image.read_bytes()).hexdigest() == original_hash


if __name__ == "__main__":
    main()
