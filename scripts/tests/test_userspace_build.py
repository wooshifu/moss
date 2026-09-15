"""Exercise the real userspace CMake targets independently of the kernel."""

import json
import shlex
import shutil
import struct
import subprocess
from pathlib import Path

import pytest


@pytest.mark.parametrize(
    "arch,triple,machine",
    [
        ("ARM64", "aarch64-unknown-elf", 183),
        ("X64", "x86_64-unknown-linux-elf", 62),
        ("RISCV64", "riscv64-unknown-elf", 243),
    ],
)
def test_userspace_compile_database_and_incremental_dependencies(tmp_path, arch, triple, machine):
    required = ["clang", "cmake", "ninja", "ld.lld", "uv"]
    if any(shutil.which(tool) is None for tool in required):
        pytest.skip("system LLVM, CMake, Ninja and uv required")
    repository = Path(__file__).resolve().parents[2]
    root = tmp_path / "userspace project"
    shutil.copytree(repository / "src/userspace", root / "src/userspace")
    (root / "scripts").mkdir()
    shutil.copy2(repository / "scripts/gen_initramfs.py", root / "scripts/gen_initramfs.py")
    (root / "CMakeLists.txt").write_text(
        """cmake_minimum_required(VERSION 3.31)
project(userspace_fixture LANGUAGES C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Userspace must retain its standalone flags instead of inheriting kernel flags.
set(CMAKE_C_FLAGS "-DMOSS_KERNEL_ONLY=1 -O0")
set(CMAKE_C_FLAGS_DEBUG "-DMOSS_DEBUG_ONLY=1 -g")
add_compile_definitions(MOSS_KERNEL_DEFINITION=1)
add_subdirectory(src/userspace)
""",
        encoding="utf-8",
    )
    build = root / "build"

    def run(*argv):
        result = subprocess.run(argv, cwd=root, capture_output=True, text=True, check=False)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout + result.stderr

    run(
        "cmake",
        "-S",
        str(root),
        "-B",
        str(build),
        "-G",
        "Ninja",
        f"-DCMAKE_C_COMPILER={shutil.which('clang')}",
        f"-DCMAKE_C_COMPILER_TARGET={triple}",
        "-DCMAKE_SYSTEM_NAME=Generic",
        "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
        "-DCMAKE_BUILD_TYPE=Debug",
        f"-DMOSS_TARGET_ARCH={arch}",
    )
    build_argv = ["cmake", "--build", str(build), "--target", "initramfs", "--parallel", "2"]
    run(*build_argv)
    programs = {"hello", "shell", "top", "signal_test"}
    entries = json.loads((build / "compile_commands.json").read_text())
    assert {Path(entry["file"]).stem for entry in entries} == programs
    assert len(entries) == len(programs)
    for entry in entries:
        arguments = shlex.split(entry["command"])
        assert "-ffreestanding" in arguments
        assert "-fno-stack-protector" in arguments
        assert "-O2" in arguments
        assert "-O0" not in arguments and "-g" not in arguments
        assert not any("MOSS_KERNEL" in arg or "MOSS_DEBUG" in arg for arg in arguments)
    for program in programs:
        elf = (build / "userspace" / f"{program}.elf").read_bytes()
        assert elf[:6] == b"\x7fELF\x02\x01"  # ELF64, little endian
        assert struct.unpack_from("<H", elf, 18)[0] == machine
        assert struct.unpack_from("<Q", elf, 24)[0] >= 0x200000000
    archive = build / "initramfs.cpio"
    assert archive.read_bytes().startswith(b"070701")
    assert "no work to do" in run(*build_argv)

    # Header dependency tracking must rebuild all objects, relink their ELFs, and
    # regenerate the archive, including in a clean build without legacy .o files.
    header = root / "src/userspace/syscall.h"
    header.write_text(header.read_text() + "\n// Header dependency regression probe.\n", encoding="utf-8")
    rebuilt = run(*build_argv)
    for program in programs:
        assert f"Linking userspace {program}.elf" in rebuilt
    assert "Generating initramfs.cpio" in rebuilt
