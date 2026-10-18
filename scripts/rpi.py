#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["pydantic>=2.11", "typer>=0.24.1"]
# ///
"""Pi 3B+ serial console and JTAG RAM deployment (macOS/Linux; no SD writes)."""

from __future__ import annotations

import argparse
import contextlib
import copy
import fcntl
import glob
import hashlib
import json
import math
import os
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time
import tty
from pathlib import Path
from typing import TYPE_CHECKING

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.artifacts import Artifacts

if TYPE_CHECKING:
    from scripts.kernel_validation import Protocol

# ARM64 Image placement must be 2 MiB aligned. Keep the small handoff stub below it.
IMAGE_BASE = 0x200000
STUB_BASE = 0x100000
# These separate RAM windows fit the firmware's 948 MiB ARM allocation on this Pi 3B+.
INITRD_BASE = 0x20000000
DTB_BASE = 0x30000000
# The existing firmware and U-Boot consoles use 115200 baud, eight bits, no parity, one stop bit.
BAUD = termios.B115200
# OpenOCD's Tcl protocol uses SUB to frame requests; its standard local control port is 6666.
TCL_END = b"\x1a"
TCL_PORT = 6666
# Retain the ten-minute allowance for slow tools/Tcl requests. This is a
# per-request deadline; a chunked deployment may take longer in total.
TRANSFER_TIMEOUT = 600
# A 4 KiB chunk bounds console buffers and matches a typical host page size.
CHUNK = 4096
# At the measured 150–200 KiB/s, 256 KiB gives updates about every 1–2 seconds
# without paying an OpenOCD command round trip for every small console-sized block.
TRANSFER_CHUNK = 256 * 1024
# Match the production shell, then require a full output line so input echo cannot pass the check.
PROMPT = b"moss$ "
PROBE_COMMAND = b"echo MOSS_RPI_INPUT_OK\n"
PROBE_RESULT = b"\nMOSS_RPI_INPUT_OK\n"
# These suites need no host input injection, lifecycle release or benchmark
# calibration. Add other suites only with their corresponding host contracts.
VALIDATION_WORKLOADS = ("mm", "scheduler", "vfs", "users.frame", "users.uaccess", "users.ipc")


def transfer_progress(label: str, completed: int, total: int, started: float) -> None:
    elapsed = time.monotonic() - started
    speed = completed / elapsed if elapsed > 0 else 0
    eta = f"{(total - completed) / speed:.0f}s" if speed else "--"
    # Binary units match OpenOCD's KiB/s output.
    kib = 1024
    mib = kib * kib
    scale, unit = (mib, "MiB") if total >= mib else (kib, "KiB")
    status = (
        f"{label}: {completed / total:6.1%}  {completed / scale:.2f}/{total / scale:.2f} {unit}"
        f"  {speed / kib:.1f} KiB/s  elapsed {elapsed:.0f}s  ETA {eta}"
    )
    if sys.stderr.isatty():
        # Replace the current terminal line; redirected logs get complete lines instead.
        print(f"\r{status}\033[K", end="", file=sys.stderr, flush=True)
    else:
        print(status, file=sys.stderr, flush=True)


def serial_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    ports = sorted(
        {p for pattern in ("/dev/cu.usbserial*", "/dev/ttyUSB*", "/dev/ttyACM*") for p in glob.glob(pattern)}
    )
    if len(ports) != 1:
        raise ValueError(f"Use --port to select one serial adapter; found: {ports}")
    return ports[0]


@contextlib.contextmanager
def uart(port: str):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    original = None
    exclusive = False
    try:
        # Coordinate this tool's sessions; exclusive mode also rejects later opens by other tools.
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.ioctl(fd, termios.TIOCEXCL)
        exclusive = True
        original = termios.tcgetattr(fd)
        settings = copy.deepcopy(original)
        settings[:4] = [0, 0, termios.CS8 | termios.CREAD | termios.CLOCAL, 0]
        settings[4:6] = [BAUD, BAUD]
        settings[6][termios.VMIN] = settings[6][termios.VTIME] = 0
        # Preserve queued early output; never flush incoming bytes during connection.
        termios.tcsetattr(fd, termios.TCSANOW, settings)
        yield fd
    finally:
        try:
            if original is not None:
                termios.tcsetattr(fd, termios.TCSANOW, original)
        finally:
            try:
                if exclusive:
                    fcntl.ioctl(fd, termios.TIOCNXCL)
            finally:
                os.close(fd)


def write_all(fd: int, data: bytes) -> None:
    while data:
        # Five seconds bounds a stalled host serial driver without dropping a partial write.
        if not select.select([], [fd], [], 5)[1]:
            raise TimeoutError("Serial transmit stalled")
        try:
            data = data[os.write(fd, data) :]
        except BlockingIOError:
            continue


def console(fd: int, log: Path, *, timeout: float | None, send: str | None, expect: str | None, check: bool):
    interactive = sys.stdin.isatty() and not check and expect is None
    original = termios.tcgetattr(sys.stdin) if interactive else None
    received = bytearray()
    sent = False
    begin = time.monotonic()
    log.parent.mkdir(parents=True, exist_ok=True)
    print(f"Serial log: {log}; Ctrl-] exits, Ctrl-C goes to the Pi.", file=sys.stderr, flush=True)
    try:
        if original is not None:
            tty.setraw(sys.stdin)
        if send is not None:
            write_all(fd, send.encode() + b"\n")
        with log.open("ab", buffering=0) as output:
            while timeout is None or time.monotonic() - begin < timeout:
                inputs = [fd, sys.stdin.fileno()] if interactive else [fd]
                # Short select intervals make deadlines responsive without busy polling.
                ready, _, _ = select.select(inputs, [], [], 0.1)
                if fd in ready:
                    data = os.read(fd, CHUNK)
                    if not data:
                        raise OSError("Serial adapter disconnected")
                    output.write(data)
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
                    received.extend(data)
                    normalized = received.replace(b"\r\n", b"\n")
                    if check and not sent and PROMPT in normalized:
                        print("\nBoot check: shell ready; sending input probe...", file=sys.stderr, flush=True)
                        write_all(fd, PROBE_COMMAND)
                        sent = True
                    if check and sent and PROBE_RESULT in normalized and normalized.endswith(PROMPT):
                        return True
                    if expect is not None and expect.encode() in normalized:
                        return True
                    # Retain enough tail for split prompts/markers; raw logs retain all bytes.
                    del received[:-CHUNK]
                if interactive and sys.stdin.fileno() in ready:
                    data = os.read(sys.stdin.fileno(), CHUNK)
                    if not data or b"\x1d" in data:  # Ctrl-] is the terminal escape, ASCII GS.
                        return True
                    write_all(fd, data)
        if check or expect is not None:
            raise TimeoutError("Expected shell response was not received; inspect serial.log")
        return True
    finally:
        if original is not None:
            termios.tcsetattr(sys.stdin, termios.TCSANOW, original)


class OpenOCD:
    def __enter__(self):
        self.conn = socket.create_connection(("127.0.0.1", TCL_PORT), timeout=TRANSFER_TIMEOUT)
        return self

    def __exit__(self, *_):
        self.conn.close()

    def command(self, command: str) -> str:
        # catch propagates Tcl errors explicitly instead of treating textual errors as success.
        wrapped = f"set rc [catch {{{command}}} result]; list $rc $result"
        self.conn.sendall(wrapped.encode() + TCL_END)
        result = b""
        while not result.endswith(TCL_END):
            part = self.conn.recv(CHUNK)
            if not part:
                raise ConnectionError("OpenOCD closed its control connection")
            result += part
        text = result[:-1].decode(errors="replace")
        if not text.startswith("0 "):
            raise RuntimeError(f"OpenOCD failed: {command}\n{text}")
        return text[2:].strip().removeprefix("{").removesuffix("}")

    def load(self, path: Path, address: int):
        data = path.read_bytes()
        if not data:
            raise ValueError(f"Cannot load an empty image: {path}")
        print(f"JTAG load {path.name}: {len(data):,} bytes at {address:#x}", flush=True)
        # OpenOCD 0.12's AArch64/MEM-AP targets lack checksum_memory, used by verify_image.
        # Read physical bytes back instead; a failed readback remains available for diagnosis.
        readback = path.with_name(path.name + ".readback")
        with tempfile.TemporaryDirectory(prefix="moss-rpi-") as folder:
            block = Path(folder) / "block.bin"
            for phase in ("Upload", "Readback"):
                started = time.monotonic()
                transfer_progress(f"{phase} {path.name}", 0, len(data), started)
                try:
                    # Both phases use contiguous physical addresses. A command must
                    # finish before its bytes count as progress; percentages are not time estimates.
                    with readback.open("wb") if phase == "Readback" else contextlib.nullcontext() as output:
                        for offset in range(0, len(data), TRANSFER_CHUNK):
                            size = min(TRANSFER_CHUNK, len(data) - offset)
                            if output is None:
                                block.write_bytes(data[offset : offset + size])
                                self.command(f"load_image {tcl_path(block)} {address + offset:#x} bin")
                            else:
                                self.command(f"dump_image {tcl_path(block)} {address + offset:#x} {size}")
                                received = block.read_bytes()
                                output.write(received)
                                if len(received) != size:
                                    raise RuntimeError(f"JTAG readback is truncated; saved {readback}")
                            transfer_progress(f"{phase} {path.name}", offset + size, len(data), started)
                finally:
                    if sys.stderr.isatty():
                        print(file=sys.stderr, flush=True)
        if readback.read_bytes() != data:
            raise RuntimeError(f"JTAG readback differs from {path}; saved {readback}")
        readback.unlink()
        print(f"Verified {path.name}: all {len(data):,} bytes match", flush=True)


def tcl_path(path: Path) -> str:
    path = str(path.resolve())
    # These characters would escape the nested Tcl catch/braced path; reject rather than execute them.
    if any(c in path for c in "{}\n\r\x1a\\"):
        raise ValueError(f"Path is not safe for OpenOCD Tcl: {path!r}")
    return "{" + path + "}"


def run(*args: str) -> str:
    try:
        return subprocess.check_output(args, text=True, stderr=subprocess.PIPE, timeout=TRANSFER_TIMEOUT).strip()
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"{args[0]} failed:\n{error.stderr or error.output}") from error


def tool(name: str) -> str:
    # Homebrew LLVM is keg-only on macOS; prefer PATH but accept its conventional installation.
    found = shutil.which(name)
    for prefix in ("/opt/homebrew/opt/llvm/bin", "/usr/local/opt/llvm/bin"):
        if not found and (Path(prefix) / name).is_file():
            found = str(Path(prefix) / name)
    if not found:
        raise ValueError(f"Required host tool is missing: {name}")
    return found


def prepare_image(manifest: Path, dtb: Path, output: Path, *, bootargs: str = ""):
    cfg = Artifacts.load(manifest)
    if cfg.arch != "ARM64":
        raise ValueError("Pi 3B+ requires the ARM64 Linux Image artifact")
    kernel, initrd = cfg.require("kernel"), cfg.require("initramfs")
    header = kernel.read_bytes()[:64]  # Linux ARM64 Image header is exactly 64 bytes.
    if len(header) != 64 or header[56:60] != b"ARM\x64":
        raise ValueError("Kernel does not contain an ARM64 Linux Image header")
    offset, size = struct.unpack_from("<QQ", header, 8)  # text_offset/image_size follow the two entry instructions.
    if offset or size < kernel.stat().st_size or IMAGE_BASE + size > INITRD_BASE:
        raise ValueError("Image runtime extent does not fit this Pi deployment layout")
    model = run(tool("fdtget"), str(dtb), "/", "model")
    if not model.startswith("Raspberry Pi 3 Model B Plus"):
        raise ValueError(f"Expected the real Pi 3B+ firmware DTB, got {model!r}")
    cells = run(tool("fdtget"), "-tx", str(dtb), "/memory@0", "reg").split()
    if len(cells) != 2:
        raise ValueError("Expected Pi firmware's single 32-bit memory range")
    ram_base, ram_size = (int(v, 16) for v in cells)
    if ram_base != 0 or DTB_BASE + dtb.stat().st_size + CHUNK > ram_size:
        raise ValueError("Firmware RAM allocation does not fit the deployment layout")
    if INITRD_BASE + initrd.stat().st_size > DTB_BASE:
        raise ValueError("Initramfs overlaps the device-tree window")
    output.mkdir(parents=True, exist_ok=True)
    prepared = output / "moss.dtb"
    shutil.copyfile(dtb, prepared)
    fdtput = tool("fdtput")
    run(fdtput, "-p", "-t", "s", str(prepared), "/chosen", "stdout-path", "serial0:115200n8")
    run(fdtput, "-t", "s", str(prepared), "/chosen", "bootargs", f"console=ttyAMA0,115200 {bootargs}".rstrip())
    for name, value in (("linux,initrd-start", INITRD_BASE), ("linux,initrd-end", INITRD_BASE + initrd.stat().st_size)):
        run(fdtput, "-t", "x", str(prepared), "/chosen", name, f"{value:x}")
    source = Path(__file__).with_name("rpi_boot.S")
    stub_elf, stub_bin = output / "handoff.elf", output / "handoff.bin"
    run(
        tool("clang"),
        "--target=aarch64-none-elf",
        "-march=armv8-a",
        "-nostdlib",
        "-fuse-ld=lld",
        # LLD's default 2 MiB ELF base is above our 1 MiB stub; this ELF is a host build intermediate.
        f"-Wl,--image-base=0,-Ttext={STUB_BASE:#x},-e,_start",
        str(source),
        "-o",
        str(stub_elf),
    )
    run(tool("llvm-objcopy"), "-O", "binary", "--only-section=.text", str(stub_elf), str(stub_bin))
    symbols = run(tool("llvm-nm"), "--defined-only", str(stub_elf)).splitlines()
    barrier = next(int(line.split()[0], 16) for line in symbols if line.split()[-1] == "cache_off")
    return kernel, initrd, prepared, stub_bin, barrier


def collect_validation(
    fd: int, log: Path, state: Protocol, *, startup_timeout: float, case_timeout: float, timeout: float
):
    """Use the QEMU runner's fail-closed protocol over the actual UART."""
    from scripts.kernel_validation import Protocol

    pending, received = b"", bytearray()
    start = time.monotonic()
    termination, reason = "timeout", None
    print(f"Validation serial log: {log}", file=sys.stderr, flush=True)
    try:
        with log.open("wb", buffering=0) as output:
            while True:
                # Match the QEMU collector's bounded polling and evidence limits;
                # deadlines start after upload, so JTAG time cannot consume them.
                if select.select([fd], [], [], 0.02)[0]:
                    data = os.read(fd, CHUNK)
                    if not data:
                        raise OSError("Serial adapter disconnected")
                    output.write(data)
                    received.extend(data)
                    if len(received) > 32 * 2**20:  # Same 32 MiB log ceiling as run_guest().
                        raise ValueError("serial log exceeded 32 MiB")
                    pending += data
                    while b"\n" in pending:
                        line, pending = pending.split(b"\n", 1)
                        state.accept(line.rstrip(b"\r"))
                    if len(pending) > 65536:  # Same partial-line ceiling as run_guest().
                        raise ValueError("unbounded partial serial line")
                # Two chunks retain a panic marker split across reads without
                # rescanning a growing log on every poll. Recheck all bytes below.
                if state.outcome(None, None, received[-2 * CHUNK :])[1] == "unexpected_kernel_panic":
                    termination = "panic"
                    break
                if state.end:
                    termination = "protocol_end"
                    break
                now = time.monotonic()
                if not state.ready and now - start >= startup_timeout:
                    reason = "startup_timeout"
                elif state.case_started is not None and now - state.case_started >= case_timeout:
                    reason = "case_timeout"
                elif now - start >= timeout:
                    reason = "guest_timeout"
                if reason:
                    break
    except (OSError, ValueError) as error:
        termination, reason = "infrastructure", f"infrastructure: {error}"
    except KeyboardInterrupt:
        termination, reason = "cancelled", "cancelled"
    if termination == "protocol_end":
        # Match run_guest's final replay, including an unterminated protocol
        # fragment after end that the newline parser has not accepted yet.
        try:
            replay = Protocol(state.workload, state.cpus, state.memory_mib, state.warmup, state.samples)
            for line in received.splitlines():
                replay.accept(line)
        except ValueError as error:
            reason = f"infrastructure: {error}"
    status, observed = state.outcome(termination, reason, received)
    cases = list(state.cases)
    for name in state.expected[len(cases) :]:
        cases.append({"name": name, "status": "error" if name == state.active else "not_run", "reason": observed})
    return {
        "workload": state.workload,
        "status": status,
        "observed": observed,
        "cases": cases,
        "ready": state.ready,
        "worker": state.worker,
        "completion": state.end,
        "termination": termination,
        "elapsed_seconds": time.monotonic() - start,
        "serial_log": str(log),
    }


def validate(args):
    """Snapshot a matched validation set; never deploy the production image by mistake."""
    # Keep production deployment helpers usable by rpi_perf's stdlib-only
    # script environment; only validation needs the shared protocol packages.
    from scripts.kernel_validation import CATALOG_VERSION, Protocol

    output = args.output.resolve()
    # A fresh directory preserves failed runs and prevents stale logs/results
    # from being interpreted as evidence for a new upload.
    output.mkdir(parents=True, exist_ok=False)
    cfg = Artifacts.load(args.manifest).snapshot_validation(output / "artifacts")
    files = {
        name: str(cfg.require(name).relative_to(output / "artifacts"))
        for name in ("validation_kernel", "validation_initramfs", "validation_debug_symbols")
    }
    files.update(
        kernel=files["validation_kernel"],
        initramfs=files["validation_initramfs"],
        debug_symbols=files["validation_debug_symbols"],
    )
    args.manifest = output / "artifacts" / "moss-artifacts.json"
    args.manifest.write_text(
        json.dumps(
            dict(
                schema_version=1,
                target=dict(architecture=cfg.arch, boot_protocol=cfg.boot_protocol),
                build=cfg.build,
                artifacts=files,
            ),
            indent=2,
        )
        + "\n"
    )
    # Derive expected RAM from this board's firmware allocation, not QEMU's
    # default or the nominal 1 GiB (the GPU/firmware reserve is unavailable).
    cells = run(tool("fdtget"), "-tx", str(args.dtb), "/memory@0", "reg").split()
    if len(cells) != 2:
        raise ValueError("Expected Pi firmware's single 32-bit memory range")
    memory_mib = int(cells[1], 16) // 2**20  # Device-tree reg is in bytes; protocol expects MiB.
    # BCM2837 has four cores; require all four to execute validation work.
    # Functional suites use the existing runner's default sampling contract.
    state = Protocol(args.workload, 4, memory_mib, 5, 30)
    bootargs = f"moss.validation={args.workload} moss.cpus=4 moss.memory={memory_mib}"
    report = dict(
        schema_version=1,
        catalog_version=CATALOG_VERSION,
        finalized=False,
        provenance=cfg.validation_provenance(),
        workload=args.workload,
        status="not_run",
        parameters=dict(
            cpus=4,
            memory_mib=memory_mib,
            startup_timeout=args.startup_timeout,
            case_timeout=args.case_timeout,
            timeout=args.timeout,
        ),
    )
    result = output / "validation-result.json"
    result.write_text(json.dumps(report, indent=2) + "\n")
    try:
        report.update(deploy(args, state=state, bootargs=bootargs))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report.update(
            status="error",
            observed=f"deployment: {error}",
            cases=[dict(name=name, status="not_run", reason="deployment_failed") for name in state.expected],
        )
        raise
    except KeyboardInterrupt:
        report.update(
            status="error",
            observed="cancelled",
            cases=[dict(name=name, status="not_run", reason="cancelled") for name in state.expected],
        )
        raise
    finally:
        report["finalized"] = True
        result.write_text(json.dumps(report, indent=2) + "\n")
    if report["observed"] == "cancelled":
        raise KeyboardInterrupt
    if report["status"] != "passed":
        raise RuntimeError(f"Validation {args.workload}: {report['observed']}; inspect {result}")
    print(f"Validation {args.workload} passed; result: {result}", flush=True)


def deploy(args, *, state: Protocol | None = None, bootargs: str = ""):
    print("[1/5] Preparing kernel, initramfs and firmware DTB...", flush=True)
    output = args.output.resolve()
    kernel, initrd, dtb, stub, barrier = prepare_image(args.manifest, args.dtb, output, bootargs=bootargs)
    record = {
        "deployment": "JTAG RAM only",
        "image_base": IMAGE_BASE,
        "dtb_base": DTB_BASE,
        "initrd_base": INITRD_BASE,
        "boot_check": "not_run",
        "files": {},
    }
    for name, path in (("kernel", kernel), ("initramfs", initrd), ("dtb", dtb)):
        record["files"][name] = {"path": str(path.resolve()), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    result = output / "result.json"
    result.write_text(json.dumps(record, indent=2) + "\n")
    print("[2/5] Connecting to serial and JTAG; checking boot CPU...", flush=True)
    with uart(serial_port(args.port)) as fd, OpenOCD() as ocd:
        ocd.command("targets bcm2837.cpu0")
        if ocd.command("bcm2837.cpu0 curstate") != "halted":
            raise ValueError("Halt the boot core in U-Boot first using the jtag command")
        cpsr = ocd.command("reg cpsr")
        if int(cpsr.split()[-1], 16) & 0xF != 9:  # SPSR.M=9 is AArch64 EL2h, required by the handoff stub.
            raise ValueError(f"Expected the bootloader in EL2h, got {cpsr}")
        # The caller selects a rate verified for this probe, wiring and board.
        ocd.command(f"adapter speed {args.jtag_khz}")
        print("[3/5] Loading handoff stub and disabling bootloader caches...", flush=True)
        ocd.load(stub, STUB_BASE)
        ocd.command(f"bp {barrier:#x} 4 hw")  # An AArch64 instruction/breakpoint occupies four bytes.
        try:
            ocd.command(f"resume {STUB_BASE:#x}")
            ocd.command("wait_halt 5000")  # Five seconds bounds a cache-maintenance stub that should finish promptly.
            if int(ocd.command("reg pc").split()[-1], 16) != barrier:
                raise RuntimeError("CPU did not reach the cache-off handoff barrier")
        finally:
            ocd.command(f"rbp {barrier:#x}")
        # MEM-AP 0 DRAM writes fault on this Pi. Use the halted boot core with translation/caches off.
        # Every transfer is read back before any kernel instruction executes.
        print("[4/5] Uploading and verifying kernel, initramfs and DTB...", flush=True)
        for path, address in ((kernel, IMAGE_BASE), (initrd, INITRD_BASE), (dtb, DTB_BASE)):
            ocd.load(path, address)
        for name, value in (("x0", DTB_BASE), ("x1", 0), ("x2", 0), ("x3", 0)):
            ocd.command(f"reg {name} {value:#x}")  # Linux Image passes DTB in x0 and reserves x1..x3 as zero.
        # UART is already open, so no initial kernel output can be lost to host setup.
        if state is not None:
            print(f"[5/5] Starting Moss validation: {state.workload}...", flush=True)
        elif args.check:
            print(f"[5/5] Starting Moss; waiting for shell and input check (timeout {args.timeout:g}s)...", flush=True)
        else:
            print("[5/5] Starting Moss; opening serial console...", flush=True)
        # CPU0 is still halted. Remove bootloader/previous-run bytes before
        # resume so an old complete protocol cannot pass a new validation.
        termios.tcflush(fd, termios.TCIFLUSH)
        ocd.command(f"resume {IMAGE_BASE:#x}")
        try:
            if state is not None:
                validation = collect_validation(
                    fd,
                    output / "serial.log",
                    state,
                    startup_timeout=args.startup_timeout,
                    case_timeout=args.case_timeout,
                    timeout=args.timeout,
                )
                record["boot_check"] = f"validation_{validation['status']}"
                return validation
            console(fd, output / "serial.log", timeout=args.timeout, send=None, expect=None, check=args.check)
            record["boot_check"] = "shell_input_passed" if args.check else "interactive_session_ended"
        except Exception as error:
            record["boot_check"] = "failed"
            record["error"] = str(error)
            raise
        finally:
            result.write_text(json.dumps(record, indent=2) + "\n")
        if args.check:
            print(f"\nDeployment complete: Moss shell input check passed. Result: {result}", flush=True)


def jtag(args):
    commands = ["transport select jtag", "bindto 127.0.0.1", "telnet_port disabled", "reset_config trst_only"]
    if args.jlink:
        if not args.jlink.isdecimal():
            raise ValueError("J-Link serial must contain decimal digits only")
        commands.append(f"adapter serial {args.jlink}")
    argv = [tool("openocd"), "-f", "interface/jlink.cfg"]
    for cmd in commands:
        argv += ["-c", cmd]
    # BCM2837 has four Cortex-A53 cores; examine them independently so a parked core keeps running.
    argv += ["-f", "target/bcm2837.cfg"]
    # A live attach passed at 1 MHz; halt waits up to one second within U-Boot's early startup window.
    # 3333 is OpenOCD's conventional GDB port; subsequent cores use consecutive ports.
    for core in range(4):
        argv += ["-c", f"bcm2837.cpu{core} configure -gdb-port {3333 + core}"]
    for cmd in (
        "adapter speed 1000",
        "bcm2837.ap configure -gdb-port disabled",
        "bcm2837.cpu0 configure -gdb-port 3333",
        "init",
        "if {![bcm2837.cpu0 was_examined]} {shutdown error}",
        "halt 1000",
    ):
        argv += ["-c", cmd]
    # Retry failed scans for thirty seconds to catch the current U-Boot's early attach window.
    deadline = time.monotonic() + args.timeout
    while True:
        process = subprocess.Popen(argv)
        try:
            status = process.wait()
        except KeyboardInterrupt:
            process.terminate()
            process.wait()
            return
        if status == 0:
            return
        if time.monotonic() >= deadline:
            raise RuntimeError(f"OpenOCD stopped with status {status}")


def reset():
    with OpenOCD() as ocd:
        ocd.command("targets bcm2837.cpu0")
        ocd.command("halt 1000")  # One second matches the validated JTAG attach halt deadline.
        # BCM2837 PM_WDOG/RSTC live at peripheral base + 0x100024/0x10001c.
        # Writes require password 0x5a in the high byte. 65536 watchdog ticks
        # at 65536 Hz give one second; RSTC[5:4]=0b10 selects a full SoC reset.
        ocd.command("mww 0x3f100024 0x5a010000")
        ocd.command(
            "set old [lindex [read_memory 0x3f10001c 32 1] 0]; "
            "mww 0x3f10001c [expr {0x5a000000 | ($old & ~0x30) | 0x20}]"
        )
        # Reset invalidates OpenOCD's cached core state; require a fresh attach.
        ocd.conn.sendall(b"shutdown" + TCL_END)
    # Let the one-second watchdog expire, plus one second before reattachment.
    time.sleep(2)
    print("Board reset requested; restart the jtag command before deploying again.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    attach = sub.add_parser("jtag", help="Attach/halt U-Boot; keep this running in a separate terminal")
    attach.add_argument("--jlink", help="Select one J-Link by decimal USB serial")
    attach.add_argument("--timeout", type=float, default=30, help="Failed-attach retry window in seconds")
    reboot = sub.add_parser("reset", help="Reset the Pi through the running JTAG server; stops that server")
    reboot.set_defaults(timeout=None)
    for mode in ("console", "deploy", "validate"):
        cmd = sub.add_parser(mode)
        cmd.add_argument("--port", help="Serial device; auto-select when exactly one is connected")
        cmd.add_argument("--timeout", type=float, help="Console duration/boot check deadline in seconds")
        if mode == "console":
            cmd.add_argument("--log", type=Path, default=Path("build/rpi3b/console.log"))
            cmd.add_argument("--send", help="Send one command followed by LF")
            cmd.add_argument("--expect", help="Return successfully after receiving this text")
        else:
            cmd.add_argument("--manifest", type=Path, default=Path("build/arm64-debug/moss-artifacts.json"))
            cmd.add_argument("--dtb", type=Path, required=True, help="Real firmware DTB captured from this board")
            cmd.add_argument("--output", type=Path, default=Path("build/rpi3b"))
            # 1 MHz matches the existing conservative attach rate. Higher
            # rates require an actual stable scan/readback on this setup.
            cmd.add_argument("--jtag-khz", type=int, default=1000, help="Verified JTAG transfer rate in kHz")
            if mode == "deploy":
                cmd.add_argument(
                    "--check", action="store_true", help="Wait for shell, send echo, verify reply, then exit"
                )
            else:
                cmd.add_argument("--workload", choices=VALIDATION_WORKLOADS, required=True)
                # Same boot/guest budgets as run_guest(); allow 30 s for
                # expensive IPC/uaccess cases, matching its long-case budget.
                cmd.add_argument("--startup-timeout", type=float, default=30)
                cmd.add_argument("--case-timeout", type=float, default=30)
                cmd.set_defaults(check=False, timeout=60)
    args = parser.parse_args()
    try:
        if args.timeout is not None and (not math.isfinite(args.timeout) or args.timeout <= 0):
            raise ValueError("--timeout must be positive")
        if args.mode in ("deploy", "validate") and args.jtag_khz <= 0:
            raise ValueError("--jtag-khz must be positive")
        if args.mode == "jtag":
            jtag(args)
        elif args.mode == "reset":
            reset()
        elif args.mode == "deploy":
            if args.check and args.timeout is None:
                # Match the repository's production boot smoke-test budget.
                args.timeout = 90
            deploy(args)
        elif args.mode == "validate":
            if any(not math.isfinite(value) or value <= 0 for value in (args.startup_timeout, args.case_timeout)):
                raise ValueError("validation deadlines must be positive")
            validate(args)
        else:
            if args.expect is not None and args.timeout is None:
                args.timeout = 30  # Bound scripted serial requests; interactive sessions remain unlimited.
            with uart(serial_port(args.port)) as fd:
                console(fd, args.log, timeout=args.timeout, send=args.send, expect=args.expect, check=False)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130  # Conventional shell status for an interrupted host command; context managers restore the UART.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
