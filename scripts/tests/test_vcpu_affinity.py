"""Host QMP/affinity contract only; real guest pinning is tested separately."""

import json
import os
import socket
import threading
import time
from pathlib import Path
from types import SimpleNamespace

import pytest

from scripts.kernel_validation import pin_vcpus, saved_measurement
from scripts.tests.test_kernel_validation import benchmark_report


@pytest.mark.parametrize("damage", [None, "missing", "short", "boolean", "duplicate", "foreign_cpu", "invalid_tid"])
def test_saved_pinning_requires_complete_binding_evidence(damage):
    report = benchmark_report()
    settings = report["comparison_environment"]["settings"]
    settings["host_cpus"] = [8, 9, 10, 11]
    guest = report["guests"][0]
    guest["host_bindings"] = [dict(cpu=i, thread_id=100 + i, host_cpu=8 + i) for i in range(4)]
    if damage == "missing":
        guest.pop("host_bindings")
    elif damage == "short":
        settings["host_cpus"].pop()
        guest["host_bindings"].pop()
    elif damage == "boolean":
        guest["host_bindings"][0]["cpu"] = False
    elif damage == "duplicate":
        guest["host_bindings"][1]["thread_id"] = 100
    elif damage == "foreign_cpu":
        guest["host_bindings"][0]["host_cpu"] = 12
    elif damage == "invalid_tid":
        guest["host_bindings"][0]["thread_id"] = -1
    assert (saved_measurement(report, guest) is not None) == (damage is None)


@pytest.mark.parametrize("damage", [None, "missing_cpu", "foreign_thread", "affinity_not_applied"])
def test_qmp_resumes_only_after_owned_thread_binding(qmp_sockets, monkeypatch, damage):
    path = qmp_sockets / "qmp.sock"
    actions = []
    affinity = {0}

    def apply(tid, cpus):
        assert tid == os.getpid()
        actions.append("pin")
        if damage != "affinity_not_applied":
            affinity.clear()
            affinity.update(cpus)

    # This is a Linux-host protocol simulation, not a real pinning test. Supply
    # both the affinity API and its ownership namespace on non-Linux hosts too;
    # only this fixture's one reported thread belongs to the fake guest.
    monkeypatch.setattr(os, "sched_setaffinity", apply, raising=False)
    monkeypatch.setattr(os, "sched_getaffinity", lambda tid: affinity, raising=False)
    original_exists = Path.exists
    task_root = f"/proc/{os.getpid()}/task/"

    def exists(candidate):
        if str(candidate).startswith(task_root):
            return str(candidate) == f"{task_root}{os.getpid()}"
        return original_exists(candidate)

    monkeypatch.setattr(Path, "exists", exists)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        listener.bind(str(path))
        listener.listen()

        def server():
            with listener.accept()[0] as connection, connection.makefile("rwb") as stream:
                connection.settimeout(3)  # Fail-safe longer than the two-second protocol deadline below.
                stream.write(b'{"QMP": {}}\n')
                stream.flush()
                while line := stream.readline():
                    request = json.loads(line)
                    name = request["execute"]
                    actions.append(name)
                    result = {}
                    if name == "query-cpus-fast":
                        result = [
                            # Arbitrary foreign identity: the fixture owns only os.getpid().
                            {"cpu-index": 0, "thread-id": 999999999 if damage == "foreign_thread" else os.getpid()}
                        ]
                        if damage == "missing_cpu":
                            result = []
                        stream.write(b'{"event": "RESUME"}\n')
                    stream.write(json.dumps({"return": result, "id": name}).encode() + b"\n")
                    stream.flush()

        thread = threading.Thread(target=server)
        thread.start()
        process = SimpleNamespace(pid=os.getpid(), poll=lambda: None)
        try:
            if damage:
                with pytest.raises(ValueError):
                    # CPU 8 is an arbitrary simulated binding, distinct from
                    # the initial {0}; two seconds bound the local QMP exchange.
                    pin_vcpus(path, process, [8], time.monotonic() + 2)
                assert "cont" not in actions
            else:
                assert pin_vcpus(path, process, [8], time.monotonic() + 2) == [
                    {"cpu": 0, "thread_id": os.getpid(), "host_cpu": 8}
                ]
                assert actions == ["qmp_capabilities", "query-cpus-fast", "pin", "cont"]
        finally:
            thread.join(timeout=4)  # Let the three-second socket guard unwind before checking for leaks.
        assert not thread.is_alive()
