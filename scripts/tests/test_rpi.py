"""Exercise the real POSIX serial path without requiring a connected board."""

import argparse
import json
import os
import pty
import re
import select
import subprocess
import sys
import termios
import threading
from pathlib import Path

import pytest

from scripts import rpi
from scripts.artifacts import Artifacts
from scripts.kernel_validation import CATALOG, Protocol
from scripts.rpi import INITRD_BASE, TRANSFER_CHUNK, OpenOCD


def test_production_helpers_and_perf_tool_need_no_validation_packages():
    # -S removes site-packages, matching rpi_perf's empty PEP 723 dependency
    # environment. Importing UART/JTAG helpers must not pull in Pydantic/Typer.
    result = subprocess.run(
        [sys.executable, "-S", "-c", "from scripts import rpi, rpi_perf"],
        cwd=Path(__file__).resolve().parents[2],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr


def test_console_transmits_receives_and_restores_port(tmp_path):
    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    log = tmp_path / "serial.log"
    script = Path(__file__).resolve().parents[1] / "rpi.py"
    process = subprocess.Popen(
        [
            sys.executable,
            str(script),
            "console",
            "--port",
            os.ttyname(slave),
            "--send",
            "help",
            "--expect",
            "PTY_INPUT_OK",
            "--timeout",
            "3",
            "--log",
            str(log),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        # Three seconds includes interpreter startup; the PTY itself is immediately ready.
        assert select.select([master], [], [], 3)[0], "script never transmitted the command"
        assert os.read(master, 4096) == b"help\n"
        os.write(master, b"PTY_INPUT_OK\r\nmoss$ ")
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, stderr.decode()
        assert b"PTY_INPUT_OK" in stdout
        assert log.read_bytes() == b"PTY_INPUT_OK\r\nmoss$ "
        restored = termios.tcgetattr(slave)
        # macOS sets PENDIN when canonical mode is restored; it describes queued input, not configuration.
        restored[3] &= ~termios.PENDIN
        original[3] &= ~termios.PENDIN
        assert restored == original
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)


def test_openocd_does_not_treat_tcl_error_as_success():
    import socket

    client, server = socket.socketpair()
    ocd = OpenOCD()
    ocd.conn = client
    try:
        server.sendall(b"1 {invalid target address}\x1a")
        with pytest.raises(RuntimeError, match="invalid target address"):
            ocd.command("load_image {kernel.bin} 0x200000 bin")
    finally:
        client.close()
        server.close()


@pytest.mark.parametrize("readback_error", [None, "corrupt", "truncated"])
def test_jtag_load_transfers_contiguous_chunks_and_checks_readback(tmp_path, monkeypatch, capsys, readback_error):
    image = tmp_path / "init ramfs.bin"
    # Distinct trailing bytes exercise a second, non-full chunk and detect replaying the first one.
    data = b"k" * TRANSFER_CHUNK + b"last partial chunk"
    image.write_bytes(data)
    memory = {}
    writes = []
    reads = []
    ocd = OpenOCD()

    def command(request):
        match = re.fullmatch(r"(load_image|dump_image) \{([^}]+)\} (0x[0-9a-f]+) (bin|[0-9]+)", request)
        assert match, request
        operation, name, address, size = match.groups()
        block = Path(name)
        address = int(address, 16)
        if operation == "load_image":
            assert size == "bin"
            memory[address] = block.read_bytes()
            writes.append((address, len(memory[address])))
        else:
            received = memory[address]
            assert len(received) == int(size)
            reads.append((address, int(size)))
            if readback_error == "corrupt":
                received = b"!" + received[1:]
            elif readback_error == "truncated":
                received = received[:-1]
            block.write_bytes(received)
        return ""

    monkeypatch.setattr(ocd, "command", command)
    readback = image.with_name(image.name + ".readback")
    if readback_error:
        with pytest.raises(RuntimeError, match="JTAG readback"):
            ocd.load(image, INITRD_BASE)
        assert readback.exists(), "keep failed readback for diagnosis"
        assert readback.read_bytes() != data
    else:
        ocd.load(image, INITRD_BASE)
        assert not readback.exists()
        assert reads == writes
        assert b"".join(memory[address] for address, _ in writes) == data

    assert writes == [(INITRD_BASE, TRANSFER_CHUNK), (INITRD_BASE + TRANSFER_CHUNK, len(data) - TRANSFER_CHUNK)]
    stdout, stderr = capsys.readouterr()
    assert f"Upload {image.name}: 100.0%" in stderr
    assert f"Readback {image.name}:" in stderr
    assert "ETA" in stderr
    assert "\033" not in stderr, "redirected output must remain readable without terminal controls"
    assert ("Verified" in stdout) == (readback_error is None)
    if readback_error == "truncated":
        assert f"Readback {image.name}: 100.0%" not in stderr
    else:
        assert f"Readback {image.name}: 100.0%" in stderr


def test_jtag_load_rejects_empty_image(tmp_path):
    image = tmp_path / "empty.bin"
    image.touch()
    with pytest.raises(ValueError, match="empty image"):
        OpenOCD().load(image, INITRD_BASE)


# The supervisor tail reproduces the preserved Pi log, including its observed timings.
@pytest.mark.parametrize(
    "initial,reply,passed",
    [
        pytest.param(b"moss$ ", (b"echo MOSS_RPI_INPUT_OK\r\nmoss$ ",), False, id="echo_only"),
        pytest.param(
            b"moss$ ", (b"echo MOSS_RPI_INPUT_OK\r\nMOSS_RPI_INPUT_OK\r\nmoss$ ",), True, id="result_and_prompt"
        ),
        pytest.param(
            b"moss$ ",
            (b"echo MOSS_RPI_INPUT_OK\r\nMOSS_RPI_INPUT_OK\r\nmoss$ ready since_init_ms=117 delta_ms=22\r\n",),
            True,
            id="supervisor_after_prompt",
        ),
        pytest.param(
            b"moss$ ",
            (b"echo MOSS_RPI_INPUT_OK\r\nMOSS_RPI_IN", b"PUT_OK\r\nmoss", b"$ ready since_init_ms=117 delta_ms=22\r\n"),
            True,
            id="split_result_and_prompt",
        ),
        pytest.param(b"moss$ ", (b"MOSS_RPI_INPUT_OK\r\n",), False, id="result_without_prompt"),
        pytest.param(b"moss$ ", (b"moss$ \r\nMOSS_RPI_INPUT_OK\r\n",), False, id="prompt_before_result"),
        pytest.param(b"moss$ ", (b"supervisor: MOSS_RPI_INPUT_OK\r\nmoss$ ",), False, id="embedded_marker"),
        pytest.param(b"\r\nMOSS_RPI_INPUT_OK\r\nmoss$ ", (), False, id="result_before_probe"),
    ],
)
def test_boot_check_requires_command_output(tmp_path, initial, reply, passed):
    master, slave = pty.openpty()
    program = """
import sys
from pathlib import Path
from scripts.rpi import uart, console
with uart(sys.argv[1]) as fd:
    console(fd, Path(sys.argv[2]), timeout=1, send=None, expect=None, check=True)
"""
    # One second bounds invalid/missing replies; the PTY has no hardware boot delay.
    process = subprocess.Popen(
        [sys.executable, "-c", program, os.ttyname(slave), str(tmp_path / "check.log")],
        cwd=Path(__file__).resolve().parents[2],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        # Three seconds accommodates host scheduling, without changing the console deadline.
        host_timeout = 3
        # Wait for the child to open/configure the slave before delivering a boot prompt.
        assert select.select([process.stderr], [], [], host_timeout)[0], "console did not open"
        process.stderr.readline()
        os.write(master, initial)
        assert select.select([master], [], [], host_timeout)[0], "check did not send its probe"
        assert os.read(master, len(rpi.PROBE_COMMAND)) == rpi.PROBE_COMMAND
        assert select.select([process.stdout], [], [], host_timeout)[0], "console did not log the initial prompt"
        assert process.stdout.read(len(initial)) == initial
        for chunk in reply:
            os.write(master, chunk)
            # Wait for console to read/echo each fragment before sending the next;
            # otherwise the PTY could coalesce a purported split-response test.
            assert select.select([process.stdout], [], [], host_timeout)[0], "console did not read the reply"
            assert process.stdout.read(len(chunk)) == chunk
        # Five seconds covers the one-second console deadline and interpreter teardown.
        _, stderr = process.communicate(timeout=5)
        assert (process.returncode == 0) == passed, stderr.decode()
        if not passed:
            assert b"Expected shell response was not received" in stderr
        assert (tmp_path / "check.log").read_bytes() == initial + b"".join(reply)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)


def validation_stream(workload, *, failed=False):
    records = []

    def emit(event, **fields):
        records.append(dict(v=1, event=event, workload=workload, **fields))

    # Four physical A53 cores and 948 MiB match the recorded Pi firmware DTB;
    # page count stays below that allocation, as required by Protocol.
    emit("ready", detected_cpus=4, online_mask=15, work_mask=15, ram_bytes=948 * 2**20, managed_pages=200000)
    for name in CATALOG[workload]:
        emit("catalog", case=name)
    emit("worker", cpu=0, affinity=1)
    for name in CATALOG[workload]:
        emit("case_start", case=name)
        emit("case_end", case=name, passed=int(not failed), failed=int(failed))
        if failed:
            break
    emit("end", completed=1 if failed else len(CATALOG[workload]), selected=len(CATALOG[workload]), failed=int(failed))
    return b"".join(b"@@MOSS " + json.dumps(record).encode() + b"\r\n" for record in records)


@pytest.mark.parametrize("workload", ["users.frame", "scheduler", "vfs", "users.vm"])
@pytest.mark.parametrize(
    "scenario,status,observed",
    [
        ("complete", "passed", "pass"),
        ("assertion", "failed", "assertion"),
        ("text", "error", "startup_timeout"),
        ("incomplete", "error", "guest_timeout"),
        ("panic", "error", "unexpected_kernel_panic"),
        ("wrong_workload", "error", "infrastructure:"),
        ("after_end", "error", "infrastructure:"),
        ("partial_after_end", "error", "infrastructure:"),
    ],
)
def test_hardware_collector_requires_complete_protocol(tmp_path, workload, scenario, status, observed):
    master, slave = pty.openpty()
    serial = validation_stream(workload, failed=scenario == "assertion")
    if scenario == "text":
        serial = b"PASS all tests passed\r\n"
    elif scenario == "incomplete":
        serial = serial[: serial.rfind(b"@@MOSS ")]
    elif scenario == "panic":
        serial = b"KERNEL PANIC: regression\r\n"
    elif scenario == "wrong_workload":
        # Rejection is immediate at ready. Do not leave a PTY writer blocked
        # on a full catalog after the collector has correctly stopped reading.
        serial = serial.splitlines(keepends=True)[0].replace(workload.encode(), b"users.ipc")
    elif scenario == "after_end":
        serial += serial.splitlines(keepends=True)[0]
    elif scenario == "partial_after_end":
        serial += b"@@MOSS {"
    log = tmp_path / "serial.log"
    try:
        with rpi.uart(os.ttyname(slave)) as fd:
            # macOS PTYs can block a write larger than their input queue; feed
            # concurrently to exercise the actual fragmented serial path.
            sender = threading.Thread(target=rpi.write_all, args=(master, serial), daemon=True)
            sender.start()
            # Queued PTY bytes need no board boot allowance; a tenth of a second
            # bounds negative cases without slowing ordinary hardware deadlines.
            result = rpi.collect_validation(
                fd, log, Protocol(workload, 4, 948, 5, 30), startup_timeout=0.1, case_timeout=0.1, timeout=0.1
            )
            sender.join(timeout=1)  # Bytes are consumed; bound a broken test writer.
            assert not sender.is_alive()
        assert result["status"] == status
        assert result["observed"].startswith(observed)
        if scenario in ("wrong_workload", "after_end"):
            # Stop at the first protocol violation, retaining every byte read;
            # later UART bytes need not be consumed after a failed boundary.
            assert serial.startswith(log.read_bytes()) and log.stat().st_size > 0
        else:
            assert log.read_bytes() == serial
        if scenario == "assertion":
            assert [case["status"] for case in result["cases"]] == ["failed"] + ["not_run"] * (
                len(CATALOG[workload]) - 1
            )
        elif scenario in ("text", "panic"):
            assert all(case["status"] == "not_run" for case in result["cases"])
    finally:
        os.close(master)
        os.close(slave)


def artifact_manifest(tmp_path):
    files = {
        name: name + ".bin"
        for name in (
            "kernel",
            "initramfs",
            "debug_symbols",
            "validation_kernel",
            "validation_initramfs",
            "validation_debug_symbols",
        )
    }
    for name, file in files.items():
        (tmp_path / file).write_bytes(name.encode())
    manifest = tmp_path / "moss-artifacts.json"
    manifest.write_text(
        json.dumps(
            dict(
                schema_version=1,
                target=dict(architecture="ARM64", boot_protocol="linux-image"),
                build=dict(type="Debug"),
                artifacts=files,
            )
        )
    )
    return manifest


def test_validation_deploys_only_matched_snapshot_and_preserves_failure(tmp_path, monkeypatch):
    manifest = artifact_manifest(tmp_path)
    original = manifest.read_bytes()
    args = argparse.Namespace(
        manifest=manifest,
        output=tmp_path / "run",
        dtb=tmp_path / "firmware.dtb",
        workload="users.frame",
        startup_timeout=30,
        case_timeout=30,
        timeout=60,
    )
    monkeypatch.setattr(rpi, "tool", lambda name: name)
    # The validated Pi firmware RAM range is [0, 0x3b400000), i.e. 948 MiB.
    monkeypatch.setattr(rpi, "run", lambda *args: "0 3b400000")

    def deploy(args, *, state, bootargs):
        snapshot = Artifacts.load(args.manifest)
        assert snapshot.require("kernel").read_bytes() == b"validation_kernel"
        assert snapshot.require("initramfs").read_bytes() == b"validation_initramfs"
        assert snapshot.require("debug_symbols").read_bytes() == b"validation_debug_symbols"
        assert snapshot.validation_provenance() == dict(
            Artifacts.load(manifest).validation_provenance(), schema_version=1
        )
        assert "moss.validation=users.frame" in bootargs and "moss.memory=948" in bootargs
        assert state.cpus == 4
        # A transfer failure must leave finalized evidence and no passing cases.
        raise RuntimeError("readback differs")

    monkeypatch.setattr(rpi, "deploy", deploy)
    with pytest.raises(RuntimeError, match="readback differs"):
        rpi.validate(args)
    report = json.loads((args.output / "validation-result.json").read_text())
    assert report["finalized"] and report["status"] == "error"
    assert all(case["status"] == "not_run" for case in report["cases"])
    assert manifest.read_bytes() == original
    with pytest.raises(FileExistsError):
        rpi.validate(args)


@pytest.mark.parametrize("bootargs", ["", "moss.validation=users.frame moss.cpus=4 moss.memory=948"])
def test_prepare_image_preserves_explicit_validation_bootargs(tmp_path, monkeypatch, bootargs):
    import struct

    manifest = artifact_manifest(tmp_path)
    header = bytearray(64)  # Linux Image header; its image_size includes this whole fixture.
    header[56:60] = b"ARM\x64"
    struct.pack_into("<QQ", header, 8, 0, len(header))
    Artifacts.load(manifest).require("kernel").write_bytes(header)
    dtb = tmp_path / "firmware.dtb"
    dtb.write_bytes(b"firmware device tree fixture")
    commands = []

    def run(*args):
        commands.append(args)
        if args[0] == "fdtget":
            return "Raspberry Pi 3 Model B Plus Rev 1.3" if args[-1] == "model" else "0 3b400000"
        if args[0] == "llvm-nm":
            return "00100010 T cache_off"
        return ""

    monkeypatch.setattr(rpi, "tool", lambda name: name)
    monkeypatch.setattr(rpi, "run", run)
    _, _, prepared, _, _ = rpi.prepare_image(manifest, dtb, tmp_path / "prepared", bootargs=bootargs)
    assert next(args[-1] for args in commands if "bootargs" in args) == f"console=ttyAMA0,115200 {bootargs}".rstrip()
    assert all(str(prepared) in args and str(dtb) not in args for args in commands if args[0] == "fdtput")
    assert dtb.read_bytes() == b"firmware device tree fixture", "the original firmware tree is never edited"
