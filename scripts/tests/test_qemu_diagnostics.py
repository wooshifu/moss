"""Failure-capture contracts; target registers and GDB are also checked in real QEMU."""

import ast
import json
import socket
import subprocess
import sys
import threading
from pathlib import Path
from types import SimpleNamespace

import pytest

from scripts import qemu_diagnostics as qd
from scripts.artifacts import Artifacts
from scripts.tests.test_artifacts import manifest


@pytest.mark.parametrize(
    "arch,roms,expected",
    [
        ("ARM64", 'addr=0000000040200000 size=0x100 mem=ram name="/image"', 0x40200000),
        ("RISCV64", 'addr=0000000080200000 size=0x100 mem=ram name="/image"', 0x80200000),
        ("ARM64", 'addr=0000000000080000 size=0x100 mem=ram name="/image"', 0x80000),
        ("X64", "", 0),
    ],
)
def test_load_offset_comes_from_exact_loaded_image(arch, roms, expected):
    assert qd.image_load_offset(arch, Path("/image"), roms) == expected


@pytest.mark.parametrize("roms", ["", 'addr=40200000 name="/other"', 'addr=1 name="/image"\naddr=2 name="/image"'])
def test_missing_or_ambiguous_load_base_is_not_guessed(roms):
    with pytest.raises(ValueError, match="cannot identify"):
        qd.image_load_offset("ARM64", Path("/image"), roms)


@pytest.mark.parametrize("mode", ["complete", "no_gdb", "gdb_timeout", "no_load_offset"])
def test_capture_stops_guest_keeps_all_cpus_and_bounds_debugger(tmp_path, monkeypatch, mode):
    cfg = Artifacts.load(manifest(tmp_path))
    actions = []
    sockets = tmp_path / "sockets"
    sockets.mkdir()
    monkeypatch.setattr(qd, "CAPTURE_TIMEOUT", 0.3)
    monkeypatch.setattr(qd.shutil, "which", lambda _: None)
    children = []
    original_run, original_spawn = subprocess.run, subprocess.Popen

    def spawn(*args, **kwargs):
        child = original_spawn(*args, **kwargs)
        children.append(child)
        return child

    monkeypatch.setattr(qd.subprocess, "Popen", spawn)

    def debugger_run(args, **kwargs):
        assert actions[-1] == "info registers:3"  # Stop and QMP snapshot precede any debugger work.
        script = qd.Path(args[-1]).read_text()
        assert "continue" not in script and "detach" not in script
        if mode == "gdb_timeout":
            return original_run([sys.executable, "-c", "import time; time.sleep(30)"], **kwargs)
        configuration = json.loads(ast.literal_eval(script.split("config = json.loads(")[1].split("\n")[0][:-1]))
        qd.Path(configuration["output"]).write_text(json.dumps({"cpus": [0, 1, 2, 3], "tasks": [], "errors": []}))
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(qd.subprocess, "run", debugger_run)
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(str(sockets / "control.sock"))
        listener.listen()

        def server():
            with listener.accept()[0] as connection, connection.makefile("rwb") as stream:
                connection.settimeout(2)
                stream.write(b'{"QMP": {}}\n')
                stream.flush()
                while line := stream.readline():
                    request = json.loads(line)
                    name, arguments = request["execute"], request.get("arguments", {})
                    actions.append(arguments.get("command-line", name))
                    output = {}
                    if name == "stop":
                        stream.write(b'{"event": "STOP"}\n')
                    elif name == "query-status":
                        output = {"running": False, "status": "paused"}
                    elif name == "query-cpus-fast":
                        output = [{"cpu-index": i, "thread-id": 100 + i} for i in range(4)]
                    elif arguments.get("command-line") == "info roms":
                        output = (
                            ""
                            if mode == "no_load_offset"
                            else f'addr=40200000 mem=ram name="{cfg.require("validation_kernel")}"'
                        )
                    elif arguments.get("command-line") == "info registers":
                        index = arguments["cpu-index"]
                        actions[-1] += f":{index}"
                        output = f"CPU {index} PC=1234"
                    stream.write(json.dumps({"return": output, "id": name}).encode() + b"\n")
                    stream.flush()

        thread = threading.Thread(target=server)
        thread.start()
        try:
            result = qd.capture_failure(
                cfg,
                SimpleNamespace(poll=lambda: None),
                sockets,
                tmp_path / "capture",
                4,
                None if mode == "no_gdb" else "gdb",
            )
        finally:
            thread.join(timeout=3)
        assert not thread.is_alive()
    assert actions[1] == "stop" and "cont" not in actions
    assert len(result["cpus"]) == 4
    assert all(cpu["registers"] for cpu in result["cpus"])
    assert qd.Path(result["qmp_log"]).exists()
    assert result["status"] == ("captured" if mode == "complete" else "partial")
    if mode == "gdb_timeout":
        assert children and all(child.poll() is not None for child in children)
        assert result["elapsed_seconds"] < 1
        assert qd.Path(result["gdb"]["log"]).exists()


def test_missing_qmp_is_bounded_and_reported(tmp_path, monkeypatch):
    cfg = Artifacts.load(manifest(tmp_path))
    monkeypatch.setattr(qd, "CAPTURE_TIMEOUT", 0.05)
    monkeypatch.setattr(qd.shutil, "which", lambda _: None)
    result = qd.capture_failure(cfg, SimpleNamespace(poll=lambda: None), tmp_path, tmp_path / "capture", 4)
    assert result["status"] == "partial" and result["errors"]
    assert result["elapsed_seconds"] < 0.5
