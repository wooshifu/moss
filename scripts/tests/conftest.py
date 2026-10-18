"""Portable resources shared by host-side protocol tests."""

import os
import shutil
import socket
import sys
import tempfile
from contextlib import contextmanager
from pathlib import Path

import pytest


@pytest.fixture
def python_command(tmp_path):
    """Keep simulated guests below Windows' command-line length limit."""

    def command(source):
        script = tmp_path / "guest.py"
        script.write_text(source, encoding="utf-8")
        return [sys.executable, str(script)]

    return command


@pytest.fixture
def qmp_listener():
    @contextmanager
    def listen(path):
        family = socket.AF_INET if sys.platform == "win32" else socket.AF_UNIX
        with socket.socket(family, socket.SOCK_STREAM) as listener:
            listener.bind(("127.0.0.1", 0) if family == socket.AF_INET else str(path))
            if family == socket.AF_INET:
                path.with_suffix(".tcp").write_text(str(listener.getsockname()[1]), encoding="ascii")
            listener.listen()
            yield listener

    return listen


@pytest.fixture(scope="session", autouse=True)
def windows_sanitizer_runtime():
    # LLVM's Windows sanitizer DLLs live outside bin; native host probes need
    # their directory in PATH even when clang itself is already discoverable.
    compiler = shutil.which("clang")
    if sys.platform != "win32" or compiler is None:
        yield
        return
    original = os.environ.get("PATH", "")
    runtimes = Path(compiler).parent.parent.glob("lib/clang/*/lib/windows")
    os.environ["PATH"] = os.pathsep.join([*(str(path) for path in runtimes), original])
    try:
        yield
    finally:
        os.environ["PATH"] = original


@pytest.fixture
def qmp_sockets():
    # Unix socket addresses have a bounded pathname. pytest's per-test names
    # exceed that bound under macOS's long temp root; mirror the runner's short
    # temporary directory and retain automatic cleanup on assertion failures.
    with tempfile.TemporaryDirectory(prefix="moss-qmp-") as directory:
        yield Path(directory)
