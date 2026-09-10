#!/usr/bin/env python3
"""Inject a real pending IRQ at RV64's initial user-stack publication.

This Debug-only QEMU/GDB regression uses the production scheduler and trap
entry, then requires the complete users.vm suite. It does not rebuild or patch
the kernel. Every run has frozen artifacts and retained logs; failure is final.
The load base belongs to the runner's virt/OpenSBI fixture, not the kernel.
"""

import argparse
import dataclasses
import hashlib
import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from qemu import build_qemu_args, resolve_qemu
from scripts.artifacts import Artifacts
from scripts.kernel_validation import Protocol

GDB_COMMANDS = r"""
set pagination off
set confirm off
set architecture riscv:rv64
symbol-file -o @LOAD_BASE@ @SYMBOLS@
set remotetimeout 5
set tcp auto-retry on
target remote 127.0.0.1:@PORT@
hbreak *_ZN4moss6kernel4archW4mossW4arch21set_user_kernel_stackEy
continue
set $dispatch_top = $a0
set $initial_pc_address = $a0-48
set $initial_pc = *(unsigned long long *)$initial_pc_address
set $publish_return = $ra
disable 1
thbreak *$publish_return
continue
printf "[dispatch-irq] stack published\n"
info registers pc sp tp sstatus sscratch
python
def value(name):
    return int(gdb.parse_and_eval("$" + name)) & ((1 << 64) - 1)
if value("sscratch") != value("dispatch_top") - 16 or value("initial_pc") == 0:
    raise gdb.GdbError("initial user-stack publication was not reached")
published_irq_masked = not (value("sstatus") & 2)
dispatch_cpu = gdb.selected_thread().global_num
end
set $sie = $sie | 2
set $sip = $sip | 2
python
if not (value("sip") & value("sie") & 2):
    raise gdb.GdbError("software IRQ did not become pending and enabled")
bp = gdb.Breakpoint("*switch_to_user", type=gdb.BP_HARDWARE_BREAKPOINT, temporary=True)
bp.thread = dispatch_cpu
end
continue
printf "[dispatch-irq] entering user return\n"
info registers pc a0 a1 sstatus sscratch
x/gx $a0+256
python
actual_pc = int(gdb.parse_and_eval("*(unsigned long long *)($a0+256)"))
if actual_pc != value("initial_pc"):
    raise gdb.GdbError("pending IRQ corrupted the initial user PC")
if not published_irq_masked or value("sstatus") & 2:
    raise gdb.GdbError("IRQs were enabled during the dispatch transition")
user_sp = value("a1")
bp = gdb.Breakpoint("*syscall_entry_point", type=gdb.BP_HARDWARE_BREAKPOINT, temporary=True)
bp.thread = dispatch_cpu
end
continue
printf "[dispatch-irq] deferred IRQ delivered\n"
info registers pc sp sepc scause sstatus sscratch
python
if value("scause") != ((1 << 63) | 1) or value("sstatus") & 256:
    raise gdb.GdbError("the pending software IRQ was not delivered from U-mode")
if value("sepc") != value("initial_pc") or value("sp") != user_sp:
    raise gdb.GdbError("the IRQ did not preserve the initial user PC/SP")
if value("sscratch") != value("dispatch_top") - 16:
    raise gdb.GdbError("the IRQ has the wrong per-thread kernel stack")
end
printf "MOSS_DISPATCH_IRQ_VERIFIED\n"
detach
quit
"""


def terminate(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def run(artifacts, symbols, directory, options):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    commands = (
        GDB_COMMANDS.replace("@LOAD_BASE@", hex(options.load_base))
        .replace("@SYMBOLS@", json.dumps(str(symbols)))
        .replace("@PORT@", str(port))
    )
    (directory / "capture.gdb").write_text(commands)
    invocation = build_qemu_args(
        artifacts,
        validation=True,
        qemu=resolve_qemu(artifacts.arch),
        smp=options.cpus,
        memory_mib=2048,
        cpu=options.cpu,
        extra_args=[
            "-append",
            f"moss.validation=users.vm moss.cpus={options.cpus} moss.memory=2048",
            "-S",
            "-gdb",
            f"tcp:127.0.0.1:{port}",
        ],
    )
    state = Protocol("users.vm", options.cpus, 2048, 0, 0)
    guest = debugger = None
    reason, termination = None, None
    started = time.monotonic()
    serial_path, gdb_path = directory / "serial.log", directory / "gdb.log"
    try:
        with (
            serial_path.open("wb") as serial_out,
            serial_path.open("rb") as serial_in,
            (directory / "qemu.log").open("wb") as errors,
            gdb_path.open("wb") as debug_out,
        ):
            guest = subprocess.Popen(invocation, stdin=subprocess.DEVNULL, stdout=serial_out, stderr=errors)
            debugger = subprocess.Popen(
                [options.gdb, "-nx", "-batch", "-x", str(directory / "capture.gdb")],
                stdin=subprocess.DEVNULL,
                stdout=debug_out,
                stderr=subprocess.STDOUT,
            )
            pending = b""
            while True:
                if serial_path.stat().st_size > 32 * 2**20:
                    raise ValueError("serial log exceeded 32 MiB")
                pending += serial_in.read(2**20)
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    state.accept(line.rstrip(b"\r"))
                if len(pending) > 65536:
                    raise ValueError("unbounded partial serial line")
                if debugger.poll() not in (None, 0):
                    reason = "dispatch_probe_failed"
                elif state.end and debugger.poll() == 0:
                    termination = "protocol_end"
                    break
                elif guest.poll() is not None:
                    reason = "unexpected_guest_exit"
                elif state.case_started and time.monotonic() - state.case_started >= 5:
                    reason = "case_timeout"
                elif time.monotonic() - started >= 60:
                    reason = "guest_timeout"
                if reason:
                    break
                time.sleep(0.01)
    except (OSError, ValueError) as error:
        reason = f"infrastructure: {error}"
    except KeyboardInterrupt:
        reason = "cancelled"
    finally:
        terminate(debugger)
        terminate(guest)
    serial = serial_path.read_bytes()
    if termination == "protocol_end":
        # Include records emitted during termination; never accept a late error.
        try:
            state = Protocol("users.vm", options.cpus, 2048, 0, 0)
            for line in serial.splitlines():
                state.accept(line)
        except ValueError as error:
            reason = f"infrastructure: {error}"
    if b"MOSS_DISPATCH_IRQ_VERIFIED" not in gdb_path.read_bytes():
        reason = reason or "missing_dispatch_evidence"
    status, observed = state.outcome(termination, reason, serial)
    return {
        "status": status,
        "observed": observed,
        "cases": state.cases,
        "qemu_args": invocation,
        "gdb_exit": debugger.returncode if debugger else None,
        "elapsed_seconds": time.monotonic() - started,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--symbols", required=True, type=Path, help="matching moss.test.elf")
    parser.add_argument("--gdb", default="gdb")
    parser.add_argument("--cpu", default="rv64")
    parser.add_argument("--cpus", choices=(1, 4), type=int, default=4)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--load-base", type=lambda number: int(number, 0), default=0x80200000)
    options = parser.parse_args()
    artifacts = Artifacts.load(options.manifest)
    if artifacts.arch != "RISCV" or artifacts.build["type"] != "Debug":
        parser.error("requires an RV64 Debug image (unoptimized publication boundary)")
    if options.runs < 1 or not shutil.which(options.gdb):
        parser.error("requires --runs >= 1 and a RISC-V-capable GDB")
    root = artifacts.manifest.parent / "dispatch-irq"
    root.mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=root))
    files = dict(artifacts.files)
    hashes = {}
    for key in ("validation_kernel", "validation_initramfs"):
        target = output / artifacts.require(key).name
        shutil.copy2(artifacts.require(key), target)
        files[key] = target
        hashes[key] = hashlib.sha256(target.read_bytes()).hexdigest()
    symbols = output / "moss.test.elf"
    shutil.copy2(options.symbols, symbols)
    hashes["symbols"] = hashlib.sha256(symbols.read_bytes()).hexdigest()
    hashes["probe"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    snapshot = dataclasses.replace(artifacts, files=files)
    results = []
    print(output, flush=True)
    for number in range(1, options.runs + 1):
        directory = output / f"{number:04}"
        directory.mkdir()
        result = run(snapshot, symbols, directory, options)
        results.append(result)
        (output / "results.json").write_text(json.dumps({"sha256": hashes, "runs": results}, indent=2) + "\n")
        print(f"{number}: {result['status']} ({result['observed']})", flush=True)
        if result["status"] != "passed":
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
