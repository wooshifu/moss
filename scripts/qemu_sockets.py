"""Local QEMU control sockets: Unix on POSIX, loopback TCP on Windows."""

import socket
import subprocess
import sys
import time
from pathlib import Path


def server_socket(path: Path) -> str:
    if sys.platform != "win32":
        return f"unix:{path},server=on,wait=off"
    # QEMU for Windows has no Unix socket backend. Record the ephemeral port
    # beside the logical socket path so diagnostics can reconnect to it.
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    path.with_suffix(".tcp").write_text(str(port), encoding="ascii")
    return f"tcp:127.0.0.1:{port},server=on,wait=off"


def gdb_address(path: Path) -> str:
    marker = path.with_suffix(".tcp")
    return f"127.0.0.1:{marker.read_text(encoding='ascii')}" if marker.exists() else str(path)


def connect_socket(path: Path, process: subprocess.Popen, deadline: float) -> socket.socket:
    marker = path.with_suffix(".tcp")
    address = ("127.0.0.1", int(marker.read_text(encoding="ascii"))) if marker.exists() else str(path)
    family = socket.AF_INET if marker.exists() else getattr(socket, "AF_UNIX", None)
    if family is None:
        raise ValueError("QEMU TCP endpoint was not published")
    while process.poll() is None and time.monotonic() < deadline:
        client = socket.socket(family, socket.SOCK_STREAM)
        client.settimeout(max(0.001, deadline - time.monotonic()))
        try:
            client.connect(address)
            return client
        except (FileNotFoundError, ConnectionRefusedError):
            client.close()
            time.sleep(0.01)
        except BaseException:
            client.close()
            raise
    raise ValueError("QEMU control socket startup failed or timed out")
