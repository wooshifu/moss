"""Exercise the production supervisor and its real interactive shell."""

import argparse
import hashlib
import json
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import replace
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from qemu import build_qemu_args, resolve_dtb, resolve_qemu
from scripts.artifacts import Artifacts

# Match the aggregate limit in src/userspace/moss_file_protocol.h.
FILE_CONTENT_BUDGET_BYTES = 4096 * 4096


def run(
    cfg: Artifacts,
    output: Path,
    timeout: float = 50,
    *,
    gdb: str | None = None,
    registration_race: bool = False,
    machine: str | None = None,
    dtb: Path | None = None,
) -> dict:
    # The debugger barriers below use virt's Image load base and PL011 registers.
    if registration_race and not gdb:
        raise ValueError("registration race probe requires GDB")
    if gdb and (
        cfg.arch != "ARM64"
        or cfg.build["type"] != "Debug"
        or (machine and not machine.split(",")[0].startswith("virt"))
    ):
        raise ValueError("controlled first-read input requires ARM64 Debug on virt")
    dtb = resolve_dtb(cfg.arch, machine, dtb)
    output.mkdir(parents=True, exist_ok=False)
    files, hashes = dict(cfg.files), {"probe": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    sources = {key: cfg.require(key) for key in ("kernel", "initramfs") + (("debug_symbols",) if gdb else ())}
    if dtb is not None:
        sources["dtb"] = dtb
    for key, source in sources.items():
        before = hashlib.sha256(source.read_bytes()).hexdigest()
        target = output / f"{key}-{source.name}"
        shutil.copy2(source, target)
        if hashlib.sha256(target.read_bytes()).hexdigest() != before:
            raise ValueError("production artifact changed during copy")
        files[key], hashes[key] = target, before
    args = build_qemu_args(
        replace(cfg, files=files),
        qemu=resolve_qemu(cfg.arch),
        smp=4,
        machine=machine,
        dtb=files.get("dtb"),
    )
    console_listener = None
    if sys.platform == "win32" and "-chardev" in args:
        chardev = args.index("-chardev") + 1
        if args[chardev].startswith("stdio,id=char0,"):
            # QEMU's Windows stdio backend drops line terminators written
            # through an anonymous pipe.  A loopback chardev preserves the
            # serial byte stream and keeps the probe fully non-interactive.
            console_listener = socket.socket()
            console_listener.bind(("127.0.0.1", 0))
            console_listener.listen(1)
            console_listener.settimeout(5)
            port = console_listener.getsockname()[1]
            args[chardev] = f"socket,id=char0,host=127.0.0.1,port={port},mux=on"
    capture = output / "console-input.gdb"
    pause_marker = "MOSS_CONSOLE_REGISTRATION_PAUSED" if registration_race else "MOSS_CONSOLE_FIRST_READ_PAUSED"
    breakpoint = "break moss_validation_console_before_register" if registration_race else "rbreak console_read"
    if gdb:
        with socket.socket() as port:
            port.bind(("127.0.0.1", 0))
            address = f"127.0.0.1:{port.getsockname()[1]}"
        args += ["-S", "-gdb", f"tcp:{address}"]
        # The registration probe stops after the empty ring check, under the
        # event lock. Queue real UART input before resuming that same path.
        # The older first-read probe still covers input before lazy RX init.
        # Neither probe drains FIFO data or changes kernel state.
        capture.write_text(f"""set pagination off
set confirm off
symbol-file -readnever -o 0x40200000 {files["debug_symbols"]}
target remote {address}
{breakpoint}
continue
python
import gdb, time
print('{pause_marker}', flush=True)
deadline = time.monotonic() + {timeout!r}
while int(gdb.parse_and_eval('*(unsigned int*)0x9000018')) & 16:
    if time.monotonic() > deadline:
        raise gdb.GdbError('no UART input at first-read barrier')
    time.sleep(0.005)
print('MOSS_CONSOLE_INPUT_QUEUED', flush=True)
end
detach
quit
""")
    serial = output / "serial.log"
    result = dict(
        status="error",
        observed="timeout",
        arch=cfg.arch,
        build=cfg.build,
        sha256=hashes,
        qemu_args=args,
        input_timing=("registration_barrier" if registration_race else "first_read_barrier") if gdb else "prompt",
    )
    child, debugger, console, stage, pending = None, None, None, 0, b""
    service_pid = None
    namespace_pid = None
    pipe_pid = None
    console_pid = None
    process_pid = None
    old_child_started = False
    code_pid = None
    loader_pid = None
    bulk_data = b"0" * 300
    # One byte past a shared transfer page proves the file service retains
    # content across multiple positional calls.
    page_crossing_size = 4096 + 1
    page_crossing_data = b"0" * page_crossing_size
    # Require native process observation, namespace lookup and direct file
    # capabilities alongside shell workflows and independent service recovery.
    steps = [
        (b"moss-init: supervisor ready", None),
        (b"moss-init: code authority service started", None),
        (b"moss-init: file service started", None),
        (b"moss-init: namespace service started", None),
        (b"moss-init: loader service started", None),
        (b"moss-init: pipe service started", None),
        (b"moss-init: console service started", None),
        (b"moss-init: process service started", None),
        (b"moss-init: loader process bridge ready", None),
        (b"built-in shell (ash)", None),
        (b"moss$ ", b"/moss-process.elf probe\n"),
        (b"\nMOSS_PROCESS_READY\n", None),
        (b"moss$ ", b"/moss-file.elf read /missing\n"),
        (b"\nMOSS_FILE_ERROR\n", None),
        (b"moss$ ", b"/moss-file.elf read /loader-probe\n"),
        (b"\nMOSS_FILE_ERROR\n", None),
        (b"moss$ ", b"/moss-file.elf write hijack /loader-probe\n"),
        (b"\nMOSS_FILE_WRITE_OK\n", None),
        (b"moss$ ", b"/moss-file.elf read /loader-probe\n"),
        (b"\nMOSS_FILE_READ=hijack\n", None),
        (b"moss$ ", b"/moss-file.elf write native\n"),
        (b"\nMOSS_FILE_WRITE_OK\n", None),
        (b"moss$ ", b"/moss-file.elf size\n"),
        (b"\nMOSS_FILE_SIZE=6\n", None),
        (b"moss$ ", b"/moss-file.elf read\n"),
        (b"\nMOSS_FILE_READ=native\n", None),
        (b"moss$ ", b"/moss-file.elf write separate /note\n"),
        (b"\nMOSS_FILE_WRITE_OK\n", None),
        (b"moss$ ", b"/moss-file.elf read /note\n"),
        (b"\nMOSS_FILE_READ=separate\n", None),
        (b"moss$ ", b"/moss-file.elf read /../note\n"),
        (b"\nMOSS_FILE_READ=separate\n", None),
        # Root's parent is root, but a regular file cannot be traversed.
        (b"moss$ ", b"/moss-file.elf read /note/../scratch\n"),
        (b"\nMOSS_FILE_ERROR\n", None),
        (b"moss$ ", b"/moss-file.elf read\n"),
        (b"\nMOSS_FILE_READ=native\n", None),
        (b"moss$ ", b"/busybox.elf ash -c 'printf \"MOSS_EXEC_READY\\n\"'\n"),
        (b"\nMOSS_EXEC_READY\n", None),
        (
            b"moss$ ",
            b"mkdir /shell-check && printf 'moss\\nskip\\nmoss\\n' > /shell-check/input && "
            b"cp /shell-check/input /shell-check/copy && mv /shell-check/copy /shell-check/result\n",
        ),
        (
            b"moss$ ",
            b"count=$(cat /shell-check/result | grep moss | wc -l); "
            b'[ "$count" -eq 2 ] && [ "$(ls /shell-check | wc -l)" -eq 2 ] && '
            b"rm -rf /shell-check && [ ! -e /shell-check ] && printf 'MOSS_BUSYBOX_READY\\n'\n",
        ),
        (b"\nMOSS_BUSYBOX_READY\n", None),
        (b"moss$ ", b"sh -c 'printf \"MOSS_NESTED_SHELL\\n\"'\n"),
        (b"\nMOSS_NESTED_SHELL\n", None),
        (b"moss$ ", b"echo MOSS_PRODUCTION_READY\n"),
        (b"\nMOSS_PRODUCTION_READY\n", None),
        (b"moss$ ", b"sleep 1 &\n"),
        (b"moss$ ", b"exit\n"),
        (b"moss-init: restarting shell", None),
        (b"built-in shell (ash)", None),
        (b"moss$ ", b"sleep 2 && echo MOSS_SLEEP_READY\n"),
        (b"\nMOSS_SLEEP_READY\n", None),
        (b"moss$ ", b"/moss-domain.elf terminate code\n"),
        (b"moss-init: code authority service died", None),
        (b"moss-init: code authority service started", None),
        (b"moss-init: loader service started", None),
        (b"moss-init: restarting shell", None),
        (b"built-in shell (ash)", None),
        (b"moss$ ", b"/moss-file.elf read\n"),
        (b"\nMOSS_FILE_READ=native\n", None),
        (b"moss$ ", b"/moss-domain.elf terminate loader\n"),
        (b"moss-init: loader service died", None),
        (b"moss-init: loader service started", None),
        (b"moss-init: restarting shell", None),
        (b"built-in shell (ash)", None),
        (b"moss$ ", b"/moss-file.elf read\n"),
        (b"\nMOSS_FILE_READ=native\n", None),
        (b"moss$ ", b"/moss-process.elf crash-survivor &\n"),
        (b"MOSS_OLD_CHILD_STARTED", b"/moss-domain.elf arm process\n"),
        (b"moss-init: pending process call armed", b"/moss-domain.elf terminate process\n"),
        (b"moss-init: process service died", None),
        (b"moss-init: pending process call released", None),
        (b"moss-init: pipe service started", None),
        (b"moss-init: console service started", None),
        (b"moss-init: process service started", None),
        (b"moss-init: loader process bridge ready", None),
        (b"built-in shell (ash)", None),
        (b"moss$ ", b"sleep 3\n"),
        (b"moss$ ", b"/moss-process.elf probe\n"),
        (b"\nMOSS_PROCESS_READY\n", None),
        (b"moss$ ", b"/moss-domain.elf terminate namespace\n"),
        (b"moss-init: namespace service died", None),
        (b"moss-init: namespace service started", None),
        (b"moss-init: loader service started", None),
        (b"moss-init: pipe service started", None),
        (b"moss-init: console service started", None),
        (b"moss-init: process service started", None),
        (b"moss-init: loader process bridge ready", None),
        (b"built-in shell (ash)", None),
        (b"moss$ ", b"/moss-file.elf read\n"),
        (b"\nMOSS_FILE_READ=native\n", None),
        (b"moss$ ", b"/moss-file.elf read /note\n"),
        (b"\nMOSS_FILE_READ=separate\n", None),
        (b"moss$ ", b"/moss-domain.elf terminate file\n"),
        (b"moss-init: file service died", None),
        (b"moss-init: file service started", None),
        (b"moss-init: namespace service started", None),
        (b"moss-init: loader service started", None),
        (b"moss-init: pipe service started", None),
        (b"moss-init: console service started", None),
        (b"moss-init: process service started", None),
        (b"moss-init: loader process bridge ready", None),
        (b"built-in shell (ash)", None),
        (b"moss$ ", b"/moss-file.elf read\n"),
        (b"\nMOSS_FILE_READ=\n", None),
        (b"moss$ ", b"/moss-file.elf read /note\n"),
        (b"\nMOSS_FILE_ERROR\n", None),
        (b"moss$ ", b"/moss-file.elf write \"$(printf '%0300d' 0)\"\n"),
        (b"\nMOSS_FILE_WRITE_OK\n", None),
        (b"moss$ ", b"/moss-file.elf read\n"),
        (b"\nMOSS_FILE_READ=" + bulk_data + b"\n", None),
        (b"moss$ ", f"/moss-file.elf write \"$(printf '%0{page_crossing_size}d' 0)\" /note\n".encode()),
        (b"\nMOSS_FILE_WRITE_OK\n", None),
        (b"moss$ ", b"/moss-file.elf read /note\n"),
        (b"\nMOSS_FILE_READ=" + page_crossing_data + b"\n", None),
        (b"moss$ ", b"/moss-file.elf write short /note\n"),
        (b"\nMOSS_FILE_WRITE_OK\n", None),
        (b"moss$ ", b"/moss-file.elf read /note\n"),
        (b"\nMOSS_FILE_READ=short\n", None),
        (b"moss$ ", b"/moss-file.elf resize 7 /note\n"),
        (b"\nMOSS_FILE_RESIZE_OK\n", None),
        (b"moss$ ", b"/moss-file.elf size /note\n"),
        (b"\nMOSS_FILE_SIZE=7\n", None),
        (b"moss$ ", b"/moss-file.elf read /note\n"),
        (b"\nMOSS_FILE_READ=short\0\0\n", None),
        # /scratch and the private loader images still own storage, so /note
        # cannot claim the entire service budget; rejection preserves bytes.
        (b"moss$ ", f"/moss-file.elf resize {FILE_CONTENT_BUDGET_BYTES} /note\n".encode()),
        (b"\nMOSS_FILE_ERROR\n", None),
        (b"moss$ ", b"/moss-file.elf read /note\n"),
        (b"\nMOSS_FILE_READ=short\0\0\n", None),
        (b"moss$ ", b"/moss-file.elf read\n"),
        (b"\nMOSS_FILE_READ=" + bulk_data + b"\n", None),
        (b"moss$ ", b"/moss-file.elf append A /note\n"),
        (b"\nMOSS_FILE_APPEND_OK\n", None),
        (b"moss$ ", b"/moss-file.elf append B /note\n"),
        (b"\nMOSS_FILE_APPEND_OK\n", None),
        (b"moss$ ", b"/moss-file.elf read /note\n"),
        (b"\nMOSS_FILE_READ=short\0\0AB\n", None),
        (b"moss$ ", b"/moss-file.elf size /note\n"),
        (b"\nMOSS_FILE_SIZE=9\n", None),
        (b"moss$ ", b"/moss-process.elf fd-probe\n"),
        (b"\nMOSS_FD_READY\n", None),
        (b"moss$ ", b"/moss-process.elf pipe-probe\n"),
        (b"\nMOSS_PIPE_READY\n", None),
        (b"moss$ ", b"/moss-process.elf fd-pipe-probe\n"),
        (b"\nMOSS_FD_PIPE_READY\n", None),
        (b"moss$ ", b"/moss-process.elf console-fd-probe\n"),
        (b"MOSS_CONSOLE_OBJECT_WRITE\n", None),
        (b"MOSS_CONSOLE_READY\n", None),
        (b"moss$ ", b"/moss-domain.elf terminate pipe\n"),
        (b"moss-init: pipe service died", None),
        (b"moss-init: pipe service started", None),
        (b"moss-init: console service started", None),
        (b"moss-init: process service started", None),
        (b"moss-init: loader process bridge ready", None),
        (b"built-in shell (ash)", None),
        (b"moss$ ", b"/moss-process.elf fd-pipe-probe\n"),
        (b"\nMOSS_FD_PIPE_READY\n", None),
        (b"moss$ ", b"/moss-domain.elf terminate console\n"),
        (b"moss-init: console service died", None),
        (b"moss-init: pipe service started", None),
        (b"moss-init: console service started", None),
        (b"moss-init: process service started", None),
        (b"moss-init: loader process bridge ready", None),
        (b"built-in shell (ash)", None),
        (b"moss$ ", b"/moss-process.elf console-fd-probe\n"),
        (b"MOSS_CONSOLE_OBJECT_WRITE\n", None),
        (b"MOSS_CONSOLE_READY\n", None),
        (b"moss$ ", None),
    ]
    started = time.monotonic()
    previous_handler = signal.getsignal(signal.SIGTERM)

    def cancelled(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, cancelled)
    try:
        with (
            serial.open("wb") as out,
            serial.open("rb") as incoming,
            (output / "qemu.log").open("wb") as err,
            (output / "gdb.log").open("wb") as debug_log,
        ):
            child = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=out, stderr=err)
            if console_listener:
                console = console_listener.accept()[0]
                console.setblocking(False)
            if gdb:
                debugger = subprocess.Popen(
                    [gdb, "-nx", "-batch", "-x", str(capture)], stdout=debug_log, stderr=subprocess.STDOUT
                )
            while time.monotonic() - started < timeout:
                try:
                    chunk = console.recv(2**20) if console else incoming.read(2**20)
                except BlockingIOError:
                    chunk = b""
                if console and chunk:
                    out.write(chunk)
                    out.flush()
                chunk = chunk.replace(b"\r", b"")
                pending += chunk
                old_child_started |= b"MOSS_OLD_CHILD_STARTED" in pending
                if serial.stat().st_size > 32 * 2**20:
                    raise ValueError("production serial log exceeds 32 MiB")
                if any(marker in pending for marker in (b"[P]", b"KERNEL PANIC", b"KERNEL PAGE FAULT", b"@@MOSS")):
                    raise ValueError("production boot panicked or entered validation")
                if b"mlibc: fatal runtime error" in pending:
                    raise ValueError("production service hit a fatal runtime error")
                while stage < len(steps) and steps[stage][0] in pending:
                    if gdb and stage == 0 and pause_marker.encode() not in (output / "gdb.log").read_bytes():
                        break
                    marker, command = steps[stage]
                    if command == b"/moss-domain.elf terminate process\n" and not old_child_started:
                        break
                    after = pending.split(marker, 1)[1]
                    if marker in (
                        b"moss-init: file service started",
                        b"moss-init: namespace service started",
                        b"moss-init: pipe service started",
                        b"moss-init: console service started",
                        b"moss-init: process service started",
                        b"moss-init: code authority service started",
                        b"moss-init: loader service started",
                    ):
                        match = re.match(rb" pid=(\d+)\n", after)
                        if not match:
                            break
                        next_pid = int(match.group(1))
                        previous_pid = {
                            b"moss-init: file service started": service_pid,
                            b"moss-init: namespace service started": namespace_pid,
                            b"moss-init: pipe service started": pipe_pid,
                            b"moss-init: console service started": console_pid,
                            b"moss-init: process service started": process_pid,
                            b"moss-init: code authority service started": code_pid,
                            b"moss-init: loader service started": loader_pid,
                        }[marker]
                        if next_pid <= 1 or next_pid == previous_pid:
                            raise ValueError("service incarnation did not change")
                        if marker == b"moss-init: file service started":
                            service_pid = next_pid
                        elif marker == b"moss-init: namespace service started":
                            namespace_pid = next_pid
                        elif marker == b"moss-init: pipe service started":
                            pipe_pid = next_pid
                        elif marker == b"moss-init: console service started":
                            console_pid = next_pid
                        elif marker == b"moss-init: process service started":
                            process_pid = next_pid
                        elif marker == b"moss-init: loader service started":
                            loader_pid = next_pid
                        else:
                            code_pid = next_pid
                        after = after[match.end() :]
                    pending = after
                    if command:
                        if callable(command):
                            command = command(service_pid, namespace_pid)
                        if console:
                            console.sendall(command)
                        else:
                            child.stdin.write(command)
                            child.stdin.flush()
                    stage += 1
                if stage == len(steps) and (not debugger or debugger.poll() is not None):
                    trace = serial.read_bytes().replace(b"\r", b"")
                    child_start_offset = trace.find(b"MOSS_OLD_CHILD_STARTED")
                    process_loss_offset = trace.find(b"moss-init: process service died")
                    if child_start_offset < 0 or process_loss_offset < child_start_offset:
                        raise ValueError("adopted managed grandchild was not live before process service loss")
                    if b"MOSS_OLD_CHILD_SURVIVED" in trace:
                        raise ValueError("old managed child survived process service loss")
                    result.update(status="passed", observed="code_loader_process_namespace_and_file_services_recovered")
                    break
                if child.poll() is not None:
                    result["observed"] = "unexpected_exit"
                    break
                time.sleep(0.02)
    except (OSError, ValueError) as error:
        result["observed"] = str(error)
    except KeyboardInterrupt:
        result["observed"] = "cancelled"
    finally:
        signal.signal(signal.SIGTERM, previous_handler)
        if child:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
            child.stdin.close()
            result["raw_exit"] = child.returncode
        if console:
            console.close()
        if console_listener:
            console_listener.close()
        if debugger:
            if debugger.poll() is None:
                debugger.terminate()
                try:
                    debugger.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    debugger.kill()
                    debugger.wait()
            result["gdb_exit"] = debugger.returncode
            if debugger.returncode != 0 or b"MOSS_CONSOLE_INPUT_QUEUED" not in (output / "gdb.log").read_bytes():
                barrier = "registration input barrier" if registration_race else "first-read input barrier"
                result.update(status="error", observed=f"{barrier} not verified")
        evidence = serial.read_bytes() if serial.exists() else b""
        if any(marker in evidence for marker in (b"[P]", b"KERNEL PANIC", b"KERNEL PAGE FAULT", b"@@MOSS")):
            result.update(status="error", observed="production boot panicked or entered validation")
        if b"mlibc: fatal runtime error" in evidence:
            result.update(status="error", observed="production service hit a fatal runtime error")
        result.update(elapsed_seconds=time.monotonic() - started, completed_steps=stage)
        (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--gdb", help="ARM64 Debug: inject input at a controlled console read boundary")
    parser.add_argument(
        "--registration-race",
        action="store_true",
        help="Inject RX after the empty check and before waiter registration",
    )
    parser.add_argument("--machine", help="QEMU machine (defaults to the architecture's normal machine)")
    parser.add_argument("--dtb", type=Path, help="Override the machine's device tree")
    options = parser.parse_args()
    cfg = Artifacts.load(options.manifest)
    root = cfg.manifest.parent / "production-boot"
    root.mkdir(exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="run-", dir=root)) / "guest"
    result = run(
        cfg,
        directory,
        gdb=options.gdb,
        registration_race=options.registration_race,
        machine=options.machine,
        dtb=options.dtb,
    )
    print(f"{cfg.arch}: {result['status']} ({result['observed']})\n{directory / 'results.json'}")
    return int(result["status"] != "passed")


if __name__ == "__main__":
    raise SystemExit(main())
