"""Portable resources shared by host-side protocol tests."""

import tempfile
from pathlib import Path

import pytest


@pytest.fixture
def qmp_sockets():
    # Unix socket addresses have a bounded pathname. pytest's per-test names
    # exceed that bound under macOS's long temp root; mirror the runner's short
    # temporary directory and retain automatic cleanup on assertion failures.
    with tempfile.TemporaryDirectory(prefix="moss-qmp-") as directory:
        yield Path(directory)
