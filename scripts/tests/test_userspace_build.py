"""Exercise the real userspace CMake targets independently of the kernel."""

import hashlib
import json
import os
import shlex
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

from scripts.gen_initramfs import make_cpio_entry


def test_repository_formatter_preserves_vendored_sources():
    from format import classify_files

    assert classify_files(
        [Path("third_party/busybox/shell/ash.c"), Path("third_party/mlibc/CMakeLists.txt"), Path("build.py")]
    ) == ([], [], [Path("build.py")])


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


def test_validation_fixture_packages_the_real_runtime_programs(tmp_path):
    repository = Path(__file__).resolve().parents[2]
    inputs = [tmp_path / f"{name} program.elf" for name in ("parent", "child", "signal", "libc", "busybox")]
    for path in inputs:
        path.write_bytes(path.name.encode())
    child = bytearray(range(256))
    child[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<Q", child, 32, 64)
    inputs[1].write_bytes(child)
    output = tmp_path / "validation-initramfs.cpio"
    subprocess.run(
        [sys.executable, str(repository / "scripts/gen_validation_initramfs.py"), str(output), *map(str, inputs)],
        check=True,
        capture_output=True,
    )
    archive = output.read_bytes()
    assert archive.startswith(b"070701")
    assert archive.index(b"libc_validation.elf\0") < archive.index(b"libc program.elf") < archive.index(b"TRAILER!!!\0")
    assert archive.index(b"busybox.elf\0") < archive.index(b"busybox program.elf") < archive.index(b"TRAILER!!!\0")
    assert make_cpio_entry("validation_child.elf", bytes(child), ino=2) in archive
    for name, ino, offset, value in (
        ("bad_entry.elf", 7, 24, bytes(8)),
        ("bad_phentsize.elf", 8, 54, struct.pack("<H", 55)),
        ("bad_load.elf", 9, 96, struct.pack("<Q", 257)),
    ):
        malformed = bytearray(child)
        malformed[offset : offset + len(value)] = value
        assert make_cpio_entry(name, bytes(malformed), ino=ino) in archive


@pytest.mark.parametrize("arch,machine", [("X64", 62), ("ARM64", 183), ("RISCV64", 243)])
def test_vendored_runtime_build_is_native_and_incremental(tmp_path, arch, machine):
    required = ("clang", "clang++", "llvm-ar", "llvm-strip", "ld.lld", "cmake", "ninja", "sh", "bzip2")
    if any(shutil.which(tool) is None for tool in required):
        pytest.skip("LLVM, CMake, Ninja and BusyBox host-generator tools required")
    repository = Path(__file__).resolve().parents[2]
    root = tmp_path / "native runtime project"
    shutil.copytree(repository / "src/userspace", root / "src/userspace", symlinks=True)
    shutil.copytree(repository / "third_party", root / "third_party", symlinks=True)
    build = tmp_path / "runtime build"
    forbidden = tmp_path / "forbidden tools"
    forbidden.mkdir()
    for tool in ("make", "gmake", "meson", "curl", "wget"):
        path = forbidden / tool
        path.write_text("#!/bin/sh\necho 'Forbidden legacy build or download tool' >&2\nexit 97\n")
        path.chmod(0o755)
    env = dict(os.environ, PATH=f"{forbidden}{os.pathsep}{os.environ['PATH']}", UV_OFFLINE="1")

    def run(*argv):
        result = subprocess.run(argv, cwd=root, env=env, capture_output=True, text=True, check=False, timeout=300)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout + result.stderr

    def source_digest():
        digest = hashlib.sha256()
        for path in sorted(root.rglob("*")):
            if path.is_file():
                digest.update(str(path.relative_to(root)).encode() + b"\0" + path.read_bytes())
        return digest.hexdigest()

    before = source_digest()
    run(
        "cmake",
        "-S",
        str(root / "src/userspace/mlibc"),
        "-B",
        str(build),
        "-G",
        "Ninja",
        f"-DCMAKE_C_COMPILER={shutil.which('clang')}",
        f"-DCMAKE_CXX_COMPILER={shutil.which('clang++')}",
        f"-DCMAKE_ASM_COMPILER={shutil.which('clang')}",
        "-DCMAKE_SYSTEM_NAME=Generic",
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        f"-DMOSS_TARGET_ARCH={arch}",
    )
    command = ("cmake", "--build", str(build), "--target", "mlibc-validation", "busybox-validation", "-j", "4")
    run(*command)
    for path in (build / "libc_validation.elf", build / "busybox/busybox"):
        elf = path.read_bytes()
        assert elf[:6] == b"\x7fELF\x02\x01"
        assert struct.unpack_from("<H", elf, 18)[0] == machine
        assert struct.unpack_from("<Q", elf, 24)[0] >= 0x200000000
    entries = json.loads((build / "compile_commands.json").read_text())
    assert any("mlibc/sysdeps/moss/sysdeps.cpp" in item["file"] for item in entries)
    assert any("busybox/shell/ash.c" in item["file"] for item in entries)
    assert not (build / "_deps").exists()
    assert "no work to do" in run(*command)
    assert source_digest() == before

    # Edit the vendored source directly: CMake must recompile and relink consumers.
    source = root / "third_party/mlibc/sysdeps/moss/sysdeps.cpp"
    source.write_text(source.read_text() + "\n// Native dependency regression probe.\n")
    changed = source_digest()
    rebuilt = run(*command)
    assert "sysdeps.cpp" in rebuilt
    assert "libc_validation.unstripped.elf" in rebuilt
    assert "busybox_unstripped" in rebuilt
    assert "no work to do" in run(*command)
    assert source_digest() == changed
