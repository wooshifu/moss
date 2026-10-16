"""Exercise the real POSIX serial path without requiring a connected board."""

import os
import pty
import re
import select
import subprocess
import sys
import termios
from pathlib import Path

import pytest

from scripts.rpi import INITRD_BASE, TRANSFER_CHUNK, OpenOCD


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


@pytest.mark.parametrize(
    "reply,passed",
    [(b"echo MOSS_RPI_INPUT_OK\r\nmoss$ ", False), (b"echo MOSS_RPI_INPUT_OK\r\nMOSS_RPI_INPUT_OK\r\nmoss$ ", True)],
)
def test_boot_check_requires_command_output(tmp_path, reply, passed):
    master, slave = pty.openpty()
    program = """
import sys
from pathlib import Path
from scripts.rpi import uart, console
with uart(sys.argv[1]) as fd:
    console(fd, Path(sys.argv[2]), timeout=1, send=None, expect=None, check=True)
"""
    # One second bounds an echo-only failure; the PTY has no hardware boot delay.
    process = subprocess.Popen(
        [sys.executable, "-c", program, os.ttyname(slave), str(tmp_path / "check.log")],
        cwd=Path(__file__).resolve().parents[2],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        # Wait for the child to open/configure the slave before delivering a boot prompt.
        assert select.select([process.stderr], [], [], 3)[0], "console did not open"
        process.stderr.readline()
        os.write(master, b"moss$ ")
        assert select.select([master], [], [], 3)[0], "check did not send its probe"
        assert os.read(master, 4096) == b"echo MOSS_RPI_INPUT_OK\n"
        os.write(master, reply)
        _, stderr = process.communicate(timeout=5)
        assert (process.returncode == 0) == passed, stderr.decode()
        if not passed:
            assert b"Expected shell response was not received" in stderr
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)
