"""Bounded, read-only failure capture from a stopped guest; never changes its verdict."""

import json
import re
import shutil
import socket
import subprocess
import time
from contextlib import contextmanager
from pathlib import Path

from scripts.artifacts import Artifacts

# This extra budget starts after failure, outside all test/benchmark measurements.
CAPTURE_TIMEOUT = 10.0


@contextmanager
def qmp_session(path: Path, process: subprocess.Popen, deadline: float):
    family = getattr(socket, "AF_UNIX", None)
    if family is None:
        raise ValueError("QMP Unix sockets are not supported by this Python build")
    with socket.socket(family, socket.SOCK_STREAM) as client:
        while True:
            if process.poll() is not None or time.monotonic() >= deadline:
                raise ValueError("QMP startup failed or timed out")
            client.settimeout(max(0.001, deadline - time.monotonic()))
            try:
                client.connect(str(path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.01)
        with client.makefile("rwb") as stream:

            def receive():
                client.settimeout(max(0.001, deadline - time.monotonic()))
                line = stream.readline(2**20)
                if not line.endswith(b"\n") or time.monotonic() >= deadline:
                    raise ValueError("incomplete or timed-out QMP response")
                return json.loads(line)

            if "QMP" not in receive():
                raise ValueError("missing QMP greeting")

            def command(name, arguments=None):
                request = {"execute": name, "id": name}
                if arguments is not None:
                    request["arguments"] = arguments
                stream.write(json.dumps(request).encode() + b"\n")
                stream.flush()
                while True:
                    response = receive()
                    if "event" in response:
                        continue
                    if response.get("id") != name or "return" not in response:
                        raise ValueError(f"QMP {name} failed: {response}")
                    return response["return"]

            command("qmp_capabilities")
            yield command


def image_load_offset(arch: str, image: Path, roms: str) -> int:
    if arch == "X64":
        return 0  # PVH loads the ELF's declared addresses, without PIE relocation.
    addresses = re.findall(r'addr=([0-9a-fA-F]+)[^\r\n]* name="' + re.escape(str(image)) + r'"', roms)
    if len(addresses) != 1:
        raise ValueError("cannot identify the loaded Image in QEMU info roms")
    return int(addresses[0], 16)


GDB_CAPTURE = r"""
set pagination off
set confirm off
set may-call-functions off
set print elements 16
set print repeats 2
set print max-depth 3
set remotetimeout 2
python
import gdb, json
config = json.loads(@CONFIG@)
snapshot = {"cpus": [], "tasks": [], "errors": []}
def run(command):
    try:
        output = gdb.execute(command, to_string=True)
        print(command + "\n" + output, flush=True)
        return output
    except gdb.error as error:
        snapshot["errors"].append(command + ": " + str(error))
        print("unavailable: " + command + ": " + str(error), flush=True)
        return None
gdb.execute("target remote " + config["socket"])
gdb.execute("symbol-file " + ("-readnever " if config["minimal"] else "")
            + "-o " + hex(config["offset"]) + " " + json.dumps(config["symbols"]))
run("info threads")
for thread in gdb.selected_inferior().threads():
    thread.switch()
    snapshot["cpus"].append({"thread": thread.num,
        "registers": run("info registers"), "backtrace": run("bt 12"),
        "instructions": run("x/8i $pc"), "stack": run("x/16gx $sp")})
# Use this ELF's DWARF field layout, not hard-coded offsets or guest function calls.
try:
    tasks = gdb.parse_and_eval("'_ZN4moss6kernel7processW4mossW7process12CfsScheduler22current_running_tasks_E'")
    slots = tasks["data_"] if int(tasks["data_"]) else tasks["boot_buf_"]
    for cpu in range(config["cpus"]):
        pointer = slots[cpu]["value"]
        task = {"vcpu": cpu, "address": str(pointer)}
        if int(pointer):
            value = pointer.dereference()
            for field in ("tid", "owner_pid", "cpu", "state", "sleep_handoff", "wait_queue"):
                task[field] = str(value[field])
        snapshot["tasks"].append(task)
except gdb.error as error:
    snapshot["errors"].append("task state unavailable: " + str(error))
run("p '_ZN4moss6kernel7processW4mossW7process11g_schedulerE'")
with open(config["output"], "w") as stream:
    json.dump(snapshot, stream, indent=2)
print("MOSS_DIAGNOSTICS_DONE", flush=True)
gdb.execute("disconnect")
end
quit
"""


def capture_failure(
    cfg: Artifacts, process: subprocess.Popen, sockets: Path, directory: Path, cpus: int, gdb: str | None = None
) -> dict:
    started = time.monotonic()
    deadline = started + CAPTURE_TIMEOUT
    result = {"status": "partial", "errors": [], "symbol_load_offset": None, "cpus": []}
    directory.mkdir()
    qmp_path = directory / "qmp.json"
    result["qmp_log"] = str(qmp_path)
    try:
        with qmp_session(sockets / "control.sock", process, min(deadline, started + 2)) as command:
            command("stop")
            result["guest_status"] = command("query-status")
            if result["guest_status"]["running"]:
                raise ValueError("guest did not stop for failure capture")
            result["roms"] = command("human-monitor-command", {"command-line": "info roms"})
            online = command("query-cpus-fast")
            if len(online) != cpus:
                raise ValueError("QMP vCPU count mismatch during failure capture")
            for cpu in online:
                index = cpu["cpu-index"]
                registers = command("human-monitor-command", {"command-line": "info registers", "cpu-index": index})
                result["cpus"].append({**cpu, "registers": registers})
        result["symbol_load_offset"] = image_load_offset(cfg.arch, cfg.require("validation_kernel"), result["roms"])
    except (OSError, ValueError, KeyError, TypeError) as error:
        result["errors"].append(str(error))
    # Keep the raw CPU snapshot even if DWARF parsing, symbol discovery or GDB fails.
    qmp_path.write_text(json.dumps(result, indent=2) + "\n")
    debugger = gdb or shutil.which("gdb-multiarch") or shutil.which("gdb")
    symbols = cfg.files.get("validation_debug_symbols")
    if debugger and symbols and result["symbol_load_offset"] is not None:
        for minimal in (False, True):
            prefix = "gdb-minimal" if minimal else "gdb"
            script, log = directory / f"{prefix}.gdb", directory / f"{prefix}.log"
            output = directory / f"{prefix}.json"
            configuration = dict(
                socket=str(sockets / "gdb.sock"),
                symbols=str(symbols),
                offset=result["symbol_load_offset"],
                cpus=cpus,
                minimal=minimal,
                output=str(output),
            )
            script.write_text(GDB_CAPTURE.replace("@CONFIG@", repr(json.dumps(configuration))))
            result[prefix] = {"script": str(script), "log": str(log), "snapshot": str(output)}
            try:
                with log.open("wb") as stream:
                    remaining = max(0.001, deadline - time.monotonic())
                    execution = subprocess.run(
                        [debugger, "-nx", "-batch", "-x", str(script)],
                        stdin=subprocess.DEVNULL,
                        stdout=stream,
                        stderr=subprocess.STDOUT,
                        timeout=remaining if minimal else remaining / 2,
                        check=False,
                    )
                result[prefix]["raw_exit"] = execution.returncode
                if execution.returncode == 0 and output.exists():
                    snapshot = json.loads(output.read_text())
                    result["errors"].extend(snapshot["errors"])
                    result["status"] = (
                        "captured" if len(snapshot["cpus"]) == cpus and not result["errors"] else "partial"
                    )
                    break
                result["errors"].append(f"{prefix} exited {execution.returncode}")
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                result["errors"].append(f"{prefix}: {error}")
            finally:
                result[prefix]["snapshot"] = str(output) if output.exists() else None
            if time.monotonic() >= deadline:
                break
    else:
        result["errors"].append("GDB, matching validation symbols or Image load offset unavailable")
    result["elapsed_seconds"] = time.monotonic() - started
    return result
