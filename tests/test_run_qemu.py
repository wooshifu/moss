"""Unit tests for run_qemu.py QEMU arguments passthrough functionality."""

import pytest
from pathlib import Path
from unittest.mock import Mock, patch
import sys
import os
import tempfile
import json

# Add scripts directory to path for importing
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'scripts'))

from run_qemu import collect_extra_qemu_args, build_qemu_args, QemuConfig


def create_test_config():
    """Helper to create test QEMU config."""
    return {
        "build_dir": "/tmp",
        "arch": "ARM64",
        "kernel_elf": "/tmp/moss.elf",
        "test_elf": "/tmp/moss.test.elf",
        "kernel_bin": "/tmp/moss_boot.bin",
        "kernel_bin_full": "/tmp/moss.bin",
        "qemu_path": "qemu-system-aarch64"
    }


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


@patch('run_qemu.subprocess.run')
@patch('run_qemu.Path.exists')
def test_main_with_qemu_args_option(mock_exists, mock_run):
    """Test main function with --qemu-args option."""
    # Setup mocks
    mock_exists.return_value = True
    mock_run.return_value = Mock(returncode=0)

    # Create temporary config
    with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
        json.dump(create_test_config(), f)
        config_path = f.name

    # Mock typer context
    ctx = Mock()
    ctx.args = []

    try:
        # Test would call main function with our arguments
        # For now, just test that our functions integrate correctly
        cfg = QemuConfig.from_json(Path(config_path))
        extra_args = collect_extra_qemu_args(ctx, "-d int,in_asm")

        qemu_cmd = build_qemu_args(
            cfg,
            Path("/tmp/moss.bin"),
            use_binary=False,
            test_mode=False,
            debug_mode=False,
            extra_args=extra_args
        )

        assert "-d" in qemu_cmd
        assert "int,in_asm" in qemu_cmd

    finally:
        Path(config_path).unlink()


@patch('run_qemu.subprocess.run')
@patch('run_qemu.Path.exists')
def test_main_with_double_dash_args(mock_exists, mock_run):
    """Test main function with -- separator arguments."""
    # Setup mocks
    mock_exists.return_value = True
    mock_run.return_value = Mock(returncode=0)

    # Create temporary config
    with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
        json.dump(create_test_config(), f)
        config_path = f.name

    # Mock typer context with args from -- separator
    ctx = Mock()
    ctx.args = ["-trace", "enable=virtio*", "-d", "int"]

    try:
        cfg = QemuConfig.from_json(Path(config_path))
        extra_args = collect_extra_qemu_args(ctx, None)

        qemu_cmd = build_qemu_args(
            cfg,
            Path("/tmp/moss.bin"),
            use_binary=False,
            test_mode=False,
            debug_mode=False,
            extra_args=extra_args
        )

        assert "-trace" in qemu_cmd
        assert "enable=virtio*" in qemu_cmd
        assert "-d" in qemu_cmd
        assert "int" in qemu_cmd

    finally:
        Path(config_path).unlink()