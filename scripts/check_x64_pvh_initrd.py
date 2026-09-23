#!/usr/bin/env python3
"""Verify x64 PVH boot metadata and its production failure modes."""

import argparse
import hashlib
import json
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from dataclasses import replace
from pathlib import Path
from typing import Any

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from qemu import build_qemu_args, resolve_qemu
from scripts.artifacts import Artifacts
from scripts.gen_initramfs import make_cpio_entry, make_cpio_trailer

# One MiB is large enough to move QEMU's page-aligned module interval while
# remaining negligible beside the minimum supported 256-MiB guest RAM.
INITRD_PADDING_BYTES = 1024 * 1024
# Debug builds normally reach the shell in a few seconds under TCG. Fifteen
# seconds preserves a wide host-load margin without turning a hang into a pass.
CASE_TIMEOUT_SECONDS = 15.0
# Polling every 20 ms keeps marker detection responsive without busy-spinning
# while TCG owns the CPU.
POLL_INTERVAL_SECONDS = 0.02
# Give QEMU two seconds to flush and exit after SIGTERM before forcing SIGKILL;
# this is cleanup time and does not extend a guest's acceptance deadline.
STOP_GRACE_SECONDS = 2.0
# Boot logs are normally well below one MiB; this ceiling bounds a runaway
# logger while retaining enough context for a failure report.
MAX_SERIAL_BYTES = 16 * 1024 * 1024
# Both profiles exceed Moss's 256-MiB floor. The 256-MiB difference moves
# QEMU's top-of-RAM module placement while keeping each acceptance boot small.
BASE_MEMORY_MIB = 512
RELOCATED_MEMORY_MIB = 768

# QEMU starts paused at the reset vector, then reaches the ELF entry after
# firmware has installed the PVH registers. Five seconds is a generous TCG
# bound for that short transition without inheriting the full boot timeout.
GDB_BREAK_TIMEOUT_SECONDS = 5.0
# RSP uses an eight-bit additive checksum and identifies hardware breakpoints
# as type one; QEMU's x86 target accepts a one-byte breakpoint kind.
GDB_CHECKSUM_MODULUS = 1 << 8
GDB_HARDWARE_BREAKPOINT_TYPE = 1
GDB_X86_BREAKPOINT_KIND = 1
# ELF64 stores e_entry at byte 24 of its little-endian file header.
ELF64_ENTRY_OFFSET = 24
ELF64_ENTRY_END = ELF64_ENTRY_OFFSET + 8
# AMD64's GDB remote register numbering assigns RSI register number four. The
# 32-bit PVH stub deliberately preserves EBX in ESI until the 64-bit ELF entry.
AMD64_RSI_REGISTER_NUMBER = 4
# The 56-byte PVH start_info includes the module list and memory-map pointer.
# Module size is the second 64-bit field in each 32-byte hvm_modlist_entry.
PVH_START_INFO_BYTES = 56
# Xen fixes HVM_START_MAGIC_VALUE to this value for PVH start_info discovery.
PVH_MAGIC = 0x336EC578
PVH_MODULE_LIST_OFFSET = 16
PVH_MODULE_SIZE_OFFSET = 8
PVH_MODULE_SIZE_BYTES = 8
# These match HvmStartInfo and PvhMemoryEntry in the production boot parser.
PVH_MEMORY_MAP_OFFSET = 40
PVH_MEMORY_ENTRY_BYTES = 24
PVH_BOOT_ADDRESS_LIMIT = 1 << 32
# Match the production PVH admission bound before requesting the GDB packet.
PVH_MAX_MEMORY_ENTRIES = 128
# Adding two bytes to the final 64-bit address must be rejected as overflow.
PVH_OVERFLOW_BASE = (1 << 64) - 1
PVH_OVERFLOW_BYTES = 2
# Only the bootstrap CPU participates in these boot-contract checks. A single
# vCPU also makes the temporary entry breakpoint unambiguous and keeps CI cheap.
PVH_TEST_VCPUS = 1
# The production generator enumerates real entries from one. Match that
# convention even though neither parser nor boot policy consumes the inode.
FIXTURE_INODE = 1

INITRD_RANGE = re.compile(rb"initramfs: found at 0x([0-9a-f]+)-0x([0-9a-f]+) \(([0-9]+) bytes\)")
BOOT_COMPLETED = b"[boot] MOSS kernel boot completed"
PANIC_MARKERS = (b"KERNEL PANIC", b"KERNEL PAGE FAULT", b"=== x64 PANIC ===")


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def stop(child: subprocess.Popen[bytes]) -> None:
    if child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=STOP_GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()


class GdbRemote:
    """Minimal acknowledged-mode GDB remote client for one paused QEMU CPU."""

    def __init__(self, connection: socket.socket):
        self.connection = connection

    def _receive_exact(self, length: int) -> bytes:
        data = bytearray()
        while len(data) < length:
            chunk = self.connection.recv(length - len(data))
            if not chunk:
                raise ConnectionError("QEMU closed the GDB socket mid-packet")
            data.extend(chunk)
        return bytes(data)

    def _receive_packet(self) -> bytes:
        while self._receive_exact(1) != b"$":
            continue
        payload = bytearray()
        while True:
            byte = self._receive_exact(1)
            if byte == b"#":
                break
            payload.extend(byte)
        # RSP encodes its one-byte checksum as exactly two hexadecimal digits.
        checksum_text = self._receive_exact(2)
        if int(checksum_text, 16) != sum(payload) % GDB_CHECKSUM_MODULUS:
            self.connection.sendall(b"-")
            raise ConnectionError("QEMU sent an invalid GDB packet checksum")
        self.connection.sendall(b"+")
        return bytes(payload)

    def command(self, payload: str) -> bytes:
        encoded = payload.encode("ascii")
        checksum = sum(encoded) % GDB_CHECKSUM_MODULUS
        self.connection.sendall(b"$" + encoded + f"#{checksum:02x}".encode("ascii"))
        if self._receive_exact(1) != b"+":
            raise ConnectionError("QEMU did not acknowledge the GDB command")
        return self._receive_packet()


def elf64_entry(kernel: Path) -> int:
    header = kernel.read_bytes()[:ELF64_ENTRY_END]
    if len(header) < ELF64_ENTRY_END or not header.startswith(b"\x7fELF\x02\x01"):
        raise ValueError("PVH acceptance kernel is not a little-endian ELF64 image")
    return struct.unpack_from("<Q", header, ELF64_ENTRY_OFFSET)[0]


def mutate_pvh_boot_info(
    kernel: Path, gdb_socket: Path, child: subprocess.Popen[bytes], mutation: str
) -> dict[str, int]:
    deadline = time.monotonic() + GDB_BREAK_TIMEOUT_SECONDS
    while not gdb_socket.exists():
        if child.poll() is not None:
            raise RuntimeError("QEMU exited before publishing its GDB socket")
        if time.monotonic() >= deadline:
            raise TimeoutError("QEMU did not publish its GDB socket")
        time.sleep(POLL_INTERVAL_SECONDS)

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(max(POLL_INTERVAL_SECONDS, deadline - time.monotonic()))
        connection.connect(str(gdb_socket))
        remote = GdbRemote(connection)
        entry = elf64_entry(kernel)
        breakpoint = f"{GDB_HARDWARE_BREAKPOINT_TYPE},{entry:x},{GDB_X86_BREAKPOINT_KIND}"
        if remote.command(f"Z{breakpoint}") != b"OK":
            raise RuntimeError("QEMU rejected the temporary PVH-entry breakpoint")
        stopped = remote.command("c")
        if not stopped.startswith((b"S", b"T")):
            raise RuntimeError(f"QEMU did not stop at the PVH entry: {stopped!r}")
        if remote.command(f"z{breakpoint}") != b"OK":
            raise RuntimeError("QEMU could not remove the temporary PVH-entry breakpoint")

        encoded_pointer = remote.command(f"p{AMD64_RSI_REGISTER_NUMBER:x}")
        try:
            start_info = int.from_bytes(bytes.fromhex(encoded_pointer.decode("ascii")), "little")
        except ValueError as error:
            raise RuntimeError(f"QEMU returned an invalid PVH pointer: {encoded_pointer!r}") from error
        encoded_start = remote.command(f"m{start_info:x},{PVH_START_INFO_BYTES:x}")
        try:
            start = bytes.fromhex(encoded_start.decode("ascii"))
        except ValueError as error:
            raise RuntimeError(f"QEMU could not read PVH start info: {encoded_start!r}") from error
        if len(start) != PVH_START_INFO_BYTES:
            raise RuntimeError("QEMU returned truncated PVH start info")
        magic, module_count = struct.unpack_from("<I8xI", start)
        (module_list,) = struct.unpack_from("<Q", start, PVH_MODULE_LIST_OFFSET)
        if magic != PVH_MAGIC:
            raise RuntimeError("QEMU did not provide PVH start info")
        if mutation == "invalid-module":
            if not module_count or not module_list:
                raise RuntimeError("QEMU did not provide the expected PVH module descriptor")
            size_address = module_list + PVH_MODULE_SIZE_OFFSET
            encoded_size = remote.command(f"m{size_address:x},{PVH_MODULE_SIZE_BYTES:x}")
            try:
                original_size = int.from_bytes(bytes.fromhex(encoded_size.decode("ascii")), "little")
            except ValueError as error:
                raise RuntimeError(f"QEMU could not read the PVH module size: {encoded_size!r}") from error
            if not original_size:
                raise RuntimeError("QEMU unexpectedly supplied an empty PVH module")
            zero_size = bytes(PVH_MODULE_SIZE_BYTES).hex()
            if remote.command(f"M{size_address:x},{PVH_MODULE_SIZE_BYTES:x}:{zero_size}") != b"OK":
                raise RuntimeError("QEMU rejected the invalid-module mutation")
            details = {"module_size_before": original_size}
        elif mutation in ("reserved-overlap", "reserved-overflow"):
            map_address, count = struct.unpack_from("<QI", start, PVH_MEMORY_MAP_OFFSET)
            if not map_address or not 1 < count <= PVH_MAX_MEMORY_ENTRIES:
                raise RuntimeError("QEMU did not provide a usable PVH memory map")
            encoded_map = remote.command(f"m{map_address:x},{count * PVH_MEMORY_ENTRY_BYTES:x}")
            try:
                memory_map = bytes.fromhex(encoded_map.decode("ascii"))
            except ValueError as error:
                raise RuntimeError(f"QEMU could not read the PVH memory map: {encoded_map!r}") from error
            if len(memory_map) != count * PVH_MEMORY_ENTRY_BYTES:
                raise RuntimeError("QEMU returned truncated PVH memory map")
            entries = [
                struct.unpack_from("<QQII", memory_map, index * PVH_MEMORY_ENTRY_BYTES) for index in range(count)
            ]
            reserved = next((index for index, (_, _, kind, _) in enumerate(entries) if kind != 1), None)
            if reserved is None:
                raise RuntimeError("QEMU did not provide a non-RAM entry for the memory-map check")
            entry_address = map_address + reserved * PVH_MEMORY_ENTRY_BYTES
            reserved_kind = entries[reserved][2]
            if mutation == "reserved-overlap":
                usable = [
                    (size, base)
                    for base, size, kind, _ in entries
                    if kind == 1 and base < PVH_BOOT_ADDRESS_LIMIT and 0 < size <= PVH_BOOT_ADDRESS_LIMIT - base
                ]
                if not usable:
                    raise RuntimeError("QEMU did not provide RAM for the overlap check")
                # Reuse a real descriptor while making it cover the largest RAM
                # bank; the allocator must never publish that bank as free pages.
                size, base = max(usable)
                details = {
                    "ram_base": base,
                    "ram_size": size,
                    "reserved_entry": reserved,
                    "reserved_type": reserved_kind,
                }
            else:
                base, size = PVH_OVERFLOW_BASE, PVH_OVERFLOW_BYTES
                details = {
                    "overflow_base": base,
                    "overflow_size": size,
                    "reserved_entry": reserved,
                    "reserved_type": reserved_kind,
                }
            replacement = struct.pack("<QQII", base, size, reserved_kind, 0)
            if remote.command(f"M{entry_address:x},{len(replacement):x}:{replacement.hex()}") != b"OK":
                raise RuntimeError(f"QEMU rejected the {mutation} mutation")
        else:
            raise ValueError(f"unknown PVH mutation: {mutation}")
        if remote.command("D") != b"OK":
            raise RuntimeError("QEMU could not detach and resume the mutated guest")
        return details


def run_case(
    cfg: Artifacts,
    *,
    kernel: Path,
    initrd: Path | None,
    memory_mib: int,
    expected: bytes,
    positive: bool,
    qemu: str,
    output: Path,
    mutation: str | None = None,
    gdb_socket: Path | None = None,
) -> dict[str, Any]:
    output.mkdir()
    files = dict(cfg.files)
    files.update(kernel=kernel, initramfs=initrd)
    args = build_qemu_args(replace(cfg, files=files), smp=PVH_TEST_VCPUS, memory_mib=memory_mib, qemu=qemu)
    if mutation:
        if gdb_socket is None:
            raise ValueError("PVH mutation requires a GDB socket")
        args += ["-S", "-gdb", f"unix:{gdb_socket},server=on,wait=off"]
    serial = output / "serial.log"
    qemu_log = output / "qemu.log"
    started = time.monotonic()
    child: subprocess.Popen[bytes] | None = None
    evidence = b""
    observed = "timeout"
    mutation_error = None
    mutation_details = None
    try:
        with serial.open("wb") as out, qemu_log.open("wb") as err:
            child = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=out, stderr=err)
            if mutation:
                try:
                    mutation_details = mutate_pvh_boot_info(kernel, gdb_socket, child, mutation)
                except (ConnectionError, OSError, RuntimeError, TimeoutError, ValueError) as error:
                    mutation_error = str(error)
                    observed = "mutation_error"
            while time.monotonic() - started < CASE_TIMEOUT_SECONDS:
                evidence = serial.read_bytes()
                if len(evidence) > MAX_SERIAL_BYTES:
                    observed = "serial_limit"
                    break
                if expected in evidence:
                    observed = "expected_marker"
                    break
                if child.poll() is not None:
                    observed = "unexpected_exit"
                    break
                if mutation_error:
                    break
                time.sleep(POLL_INTERVAL_SECONDS)
    finally:
        if child is not None:
            stop(child)
        evidence = serial.read_bytes() if serial.exists() else evidence

    match = INITRD_RANGE.search(evidence)
    initrd_range = None
    if match:
        start, end, size = int(match[1], 16), int(match[2], 16), int(match[3])
        initrd_range = {"start": start, "end": end, "size": size}
    completed = BOOT_COMPLETED in evidence
    panicked = any(marker in evidence for marker in PANIC_MARKERS)
    passed = observed == "expected_marker" and (completed and not panicked if positive else not completed)
    result = {
        "status": "passed" if passed else "error",
        "observed": observed,
        "expected": expected.decode(),
        "positive": positive,
        "memory_mib": memory_mib,
        "initrd": str(initrd) if initrd else None,
        "initrd_range": initrd_range,
        "boot_completed": completed,
        "panicked": panicked,
        "mutation_error": mutation_error,
        "mutation_details": mutation_details,
        "elapsed_seconds": time.monotonic() - started,
        "qemu_args": args,
        "raw_exit": child.returncode if child else None,
    }
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--qemu")
    parser.add_argument("--output", type=Path)
    options = parser.parse_args()

    cfg = Artifacts.load(options.manifest)
    if cfg.arch != "X64" or cfg.boot_protocol != "xen-pvh":
        parser.error("this acceptance check requires an X64 xen-pvh artifact manifest")
    qemu = resolve_qemu(cfg.arch, options.qemu)
    root = options.output or cfg.manifest.parent / "pvh-initrd" / str(time.time_ns())
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=False)

    source_kernel = cfg.require("kernel")
    source_initrd = cfg.require("initramfs")
    source_hashes = {"kernel": sha256(source_kernel), "initramfs": sha256(source_initrd)}
    kernel = root / "moss.elf"
    base = root / "base.cpio"
    shutil.copy2(source_kernel, kernel)
    shutil.copy2(source_initrd, base)
    if sha256(kernel) != source_hashes["kernel"] or sha256(base) != source_hashes["initramfs"]:
        raise ValueError("boot artifacts changed while freezing PVH acceptance inputs")

    padded = root / "padded.cpio"
    padded.write_bytes(base.read_bytes() + bytes(INITRD_PADDING_BYTES))
    invalid = root / "invalid.cpio"
    bad_archive = bytearray(make_cpio_trailer())
    bad_archive[0] = ord("X")  # Corrupt the first byte of the fixed six-byte newc magic.
    invalid.write_bytes(bad_archive)
    no_init = root / "no-init.cpio"
    no_init.write_bytes(
        make_cpio_entry("fixture.txt", b"not an init executable", ino=FIXTURE_INODE) + make_cpio_trailer()
    )

    scenarios = (
        ("base-512", base, BASE_MEMORY_MIB, b"moss$ ", True, None),
        ("padded-512", padded, BASE_MEMORY_MIB, b"moss$ ", True, None),
        ("base-768", base, RELOCATED_MEMORY_MIB, b"moss$ ", True, None),
        ("missing-module", None, BASE_MEMORY_MIB, b"BOOT ERROR: PVH initrd module is required", False, None),
        ("invalid-module", base, BASE_MEMORY_MIB, b"BOOT ERROR: invalid PVH initrd module", False, "invalid-module"),
        (
            "reserved-overlap",
            base,
            BASE_MEMORY_MIB,
            b"Error: Memory management setup failed",
            False,
            "reserved-overlap",
        ),
        (
            "reserved-overflow",
            base,
            BASE_MEMORY_MIB,
            b"BOOT ERROR: invalid PVH memory map entry",
            False,
            "reserved-overflow",
        ),
        ("invalid-archive", invalid, BASE_MEMORY_MIB, b"Error: Invalid initramfs archive", False, None),
        ("missing-init", no_init, BASE_MEMORY_MIB, b"Error: Required init executable is missing", False, None),
    )
    results: dict[str, dict[str, Any]] = {}
    # AF_UNIX paths are bounded independently of the report output location.
    with tempfile.TemporaryDirectory(prefix="moss-pvh-gdb-") as socket_dir:
        for name, initrd, memory_mib, expected, positive, mutation in scenarios:
            result = run_case(
                cfg,
                kernel=kernel,
                initrd=initrd,
                memory_mib=memory_mib,
                expected=expected,
                positive=positive,
                qemu=qemu,
                output=root / name,
                mutation=mutation,
                gdb_socket=Path(socket_dir) / f"{name}.sock" if mutation else None,
            )
            results[name] = result
            print(f"{name}: {result['status']} ({result['observed']})", flush=True)

    base_512 = results["base-512"]["initrd_range"]
    padded_512 = results["padded-512"]["initrd_range"]
    base_768 = results["base-768"]["initrd_range"]
    range_checks = bool(base_512 and padded_512 and base_768)
    if range_checks:
        for item, expected_size in ((base_512, base.stat().st_size), (padded_512, padded.stat().st_size)):
            range_checks &= item["size"] == expected_size and item["end"] - item["start"] == expected_size
        range_checks &= (base_512["start"], base_512["end"]) != (padded_512["start"], padded_512["end"])
        range_checks &= base_512["start"] != base_768["start"]

    final_source_hashes = {"kernel": sha256(source_kernel), "initramfs": sha256(source_initrd)}
    frozen_hashes = {"kernel": sha256(kernel), "initramfs": sha256(base)}
    artifacts_unchanged = final_source_hashes == source_hashes and frozen_hashes == source_hashes
    report = {
        "schema_version": 1,
        "manifest": str(cfg.manifest),
        "qemu": qemu,
        "source_sha256": source_hashes,
        "final_source_sha256": final_source_hashes,
        "frozen_sha256": frozen_hashes,
        "artifacts_unchanged": artifacts_unchanged,
        "range_checks": range_checks,
        "cases": results,
    }
    (root / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    passed = range_checks and artifacts_unchanged and all(result["status"] == "passed" for result in results.values())
    print(root / "results.json")
    return int(not passed)


if __name__ == "__main__":
    raise SystemExit(main())
