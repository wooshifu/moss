"""Unit tests for run_qemu.py QEMU arguments passthrough functionality."""

import pytest
from pathlib import Path
from unittest.mock import Mock, patch
import sys
import os

# Add scripts directory to path for importing
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'scripts'))

from run_qemu import collect_extra_qemu_args, build_qemu_args, QemuConfig


def test_collect_extra_qemu_args_from_qemu_args_option():
    """Test collecting arguments from --qemu-args option."""
    # Mock Typer context
    ctx = Mock()
    ctx.args = []

    result = collect_extra_qemu_args(ctx, "-d int,in_asm")
    assert result == ["-d", "int,in_asm"]


def test_collect_extra_qemu_args_from_context_args():
    """Test collecting arguments from context (-- separator)."""
    ctx = Mock()
    ctx.args = ["-d", "int,in_asm", "-trace", "enable=virtio*"]

    result = collect_extra_qemu_args(ctx, None)
    assert result == ["-d", "int,in_asm", "-trace", "enable=virtio*"]


def test_collect_extra_qemu_args_combined():
    """Test collecting arguments from both sources."""
    ctx = Mock()
    ctx.args = ["-trace", "enable=virtio*"]

    result = collect_extra_qemu_args(ctx, "-d int,in_asm")
    assert result == ["-d", "int,in_asm", "-trace", "enable=virtio*"]


def test_collect_extra_qemu_args_empty():
    """Test with no extra arguments."""
    ctx = Mock()
    ctx.args = []

    result = collect_extra_qemu_args(ctx, None)
    assert result == []


def test_build_qemu_args_with_extra_args():
    """Test that extra arguments are appended to QEMU command."""
    # Create minimal config
    cfg = QemuConfig(
        build_dir="/tmp",
        arch="ARM64",
        kernel_elf="/tmp/moss.elf",
        test_elf="/tmp/moss.test.elf",
        kernel_bin="/tmp/moss_boot.bin",
        kernel_bin_full="/tmp/moss.bin",
        qemu_path="qemu-system-aarch64"
    )

    kernel_file = Path("/tmp/moss.bin")
    extra_args = ["-d", "int,in_asm", "-trace", "enable=virtio*"]

    result = build_qemu_args(
        cfg,
        kernel_file,
        use_binary=False,
        test_mode=False,
        debug_mode=False,
        extra_args=extra_args
    )

    # Check that extra args are at the end
    assert result[-4:] == ["-d", "int,in_asm", "-trace", "enable=virtio*"]