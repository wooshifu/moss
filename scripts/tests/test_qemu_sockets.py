"""Exercise Windows control endpoints with real loopback connections."""

import socket
import time
from types import SimpleNamespace

import pytest

from scripts import qemu_sockets as qs


def test_windows_endpoint_connects_and_supplies_gdb_address(tmp_path, monkeypatch):
    monkeypatch.setattr(qs, "sys", SimpleNamespace(platform="win32"))
    path = tmp_path / "gdb.sock"
    argument = qs.server_socket(path)
    address = qs.gdb_address(path)
    assert argument == f"tcp:{address},server=on,wait=off"
    host, port = address.split(":")
    assert host == "127.0.0.1"
    with socket.socket() as listener:
        listener.bind((host, int(port)))
        listener.listen()
        with (
            qs.connect_socket(path, SimpleNamespace(poll=lambda: None), time.monotonic() + 2) as client,
            listener.accept()[0] as server,
        ):
            client.sendall(b"QMP")
            assert server.recv(3) == b"QMP"


def test_exited_guest_does_not_wait_for_control_connection(tmp_path, monkeypatch):
    monkeypatch.setattr(qs, "sys", SimpleNamespace(platform="win32"))
    path = tmp_path / "control.sock"
    qs.server_socket(path)
    with pytest.raises(ValueError, match="startup failed"):
        qs.connect_socket(path, SimpleNamespace(poll=lambda: 1), time.monotonic() + 10)
