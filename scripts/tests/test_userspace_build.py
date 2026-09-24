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

from scripts import gen_validation_initramfs
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
    required = ["clang", "clang++", "llvm-ar", "llvm-strip", "cmake", "ninja", "ld.lld", "uv"]
    if any(shutil.which(tool) is None for tool in required):
        pytest.skip("system LLVM, CMake, Ninja and uv required")
    repository = Path(__file__).resolve().parents[2]
    root = tmp_path / "userspace project"
    shutil.copytree(repository / "src/userspace", root / "src/userspace")
    shutil.copytree(repository / "src/abi/include", root / "src/abi/include")
    (root / "third_party").symlink_to(repository / "third_party", target_is_directory=True)
    (root / "scripts").mkdir()
    shutil.copy2(repository / "scripts/gen_initramfs.py", root / "scripts/gen_initramfs.py")
    (root / "CMakeLists.txt").write_text(
        """cmake_minimum_required(VERSION 3.31)
project(userspace_fixture LANGUAGES C CXX ASM)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Userspace must retain its standalone flags instead of inheriting kernel flags.
set(CMAKE_C_FLAGS "-DMOSS_KERNEL_ONLY=1 -O0")
set(CMAKE_C_FLAGS_DEBUG "-DMOSS_DEBUG_ONLY=1 -g")
add_compile_definitions(MOSS_KERNEL_DEFINITION=1)
set(MOSS_BUILD_TESTS ON)
add_subdirectory(src/userspace)
add_custom_target(userspace-fixture DEPENDS
    ${CMAKE_BINARY_DIR}/userspace/validation.elf ${CMAKE_BINARY_DIR}/userspace/validation_child.elf)
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
        f"-DCMAKE_CXX_COMPILER={shutil.which('clang++')}",
        f"-DCMAKE_ASM_COMPILER={shutil.which('clang')}",
        f"-DCMAKE_C_COMPILER_TARGET={triple}",
        "-DCMAKE_SYSTEM_NAME=Generic",
        "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
        "-DCMAKE_BUILD_TYPE=Debug",
        f"-DMOSS_TARGET_ARCH={arch}",
    )
    build_argv = ["cmake", "--build", str(build), "--target", "userspace-fixture", "--parallel", "2"]
    run(*build_argv)
    assert not (build / "src/userspace/mlibc/busybox/busybox").exists()
    programs = {"validation", "validation_child"}
    entries = json.loads((build / "compile_commands.json").read_text())
    entries = [entry for entry in entries if "moss_userspace_" in entry["command"]]
    validation_sources = {source.stem for source in (root / "src/userspace/validation").glob("*.c")}
    expected_sources = programs | {"loader_probe", "validation_frame"} | validation_sources
    assert {Path(entry["file"]).stem for entry in entries} == expected_sources
    assert len(entries) == len(expected_sources)
    for entry in entries:
        arguments = shlex.split(entry["command"])
        assert "-ffreestanding" in arguments
        assert "-fno-stack-protector" in arguments
        assert "-O2" in arguments
        assert "-O0" not in arguments
        if Path(entry["file"]).suffix == ".c":
            assert "-g" not in arguments
        assert not any("MOSS_KERNEL" in arg or "MOSS_DEBUG" in arg for arg in arguments)
    for program in programs:
        elf = (build / "userspace" / f"{program}.elf").read_bytes()
        assert elf[:6] == b"\x7fELF\x02\x01"  # ELF64, little endian
        assert struct.unpack_from("<H", elf, 18)[0] == machine
        assert struct.unpack_from("<Q", elf, 24)[0] >= 0x200000000
    no_op = run(*build_argv)
    assert "no work to do" in no_op
    assert "Re-checking globbed directories" not in no_op

    # Header dependency tracking must rebuild all objects and relink their ELFs,
    # including in a clean build without legacy .o files.
    header = root / "src/userspace/syscall.h"
    header.write_text(header.read_text() + "\n// Header dependency regression probe.\n", encoding="utf-8")
    rebuilt = run(*build_argv)
    for program in programs:
        assert f"Linking userspace {program}.elf" in rebuilt


def test_validation_fixture_packages_the_real_runtime_programs(tmp_path):
    repository = Path(__file__).resolve().parents[2]
    inputs = [tmp_path / f"{name} program.elf" for name in ("parent", "child", "libc", "busybox")]
    for path in inputs:
        path.write_bytes(path.name.encode())
    # A complete synthetic x86-64 child leaves the same page-sized program-header
    # gap as the real linker output, so every derived fixture is structurally real.
    child = bytearray(2 * gen_validation_initramfs.PAGE_BYTES)
    # The production linker places fixed-address userspace at 8 GiB; mirroring
    # that band keeps range checks realistic while leaving boundary segments apart.
    code_address = 0x0000000200000000
    identification = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into(
        "<16sHHIQQQIHHHHHH",
        child,
        0,
        identification,
        2,  # ET_EXEC: the loader accepts only fixed-address executable images.
        62,  # EM_X86_64 selects the matching synthetic return sequence.
        1,  # ELF header version.
        code_address,
        gen_validation_initramfs.ELF_HEADER_BYTES,
        0,
        0,
        gen_validation_initramfs.ELF_HEADER_BYTES,
        gen_validation_initramfs.PROGRAM_HEADER_BYTES,
        2,  # One PT_LOAD plus one GNU_STACK record matches real child shape.
        0,
        0,
        0,
    )
    struct.pack_into(
        "<IIQQQQQQ",
        child,
        gen_validation_initramfs.ELF_HEADER_BYTES,
        gen_validation_initramfs.PT_LOAD,
        gen_validation_initramfs.PF_R | gen_validation_initramfs.PF_X,
        gen_validation_initramfs.PAGE_BYTES,
        code_address,
        code_address,
        16,  # A small nonempty payload is sufficient for derivation tests.
        16,
        gen_validation_initramfs.PAGE_BYTES,
    )
    struct.pack_into(
        "<IIQQQQQQ",
        child,
        gen_validation_initramfs.ELF_HEADER_BYTES + gen_validation_initramfs.PROGRAM_HEADER_BYTES,
        0x6474E551,  # PT_GNU_STACK wire tag emitted by ld.lld.
        gen_validation_initramfs.PF_R | gen_validation_initramfs.PF_W,
        0,
        0,
        0,
        0,
        0,
        0,
    )
    # 0x90 is the x86 NOP byte; execution is irrelevant here, but a nonzero
    # payload makes file-range mutation and packaging observable.
    child[gen_validation_initramfs.PAGE_BYTES : gen_validation_initramfs.PAGE_BYTES + 16] = bytes([0x90]) * 16
    inputs[1].write_bytes(child)
    output = tmp_path / "validation-initramfs.cpio"
    subprocess.run(
        [sys.executable, str(repository / "scripts/gen_validation_initramfs.py"), str(output), *map(str, inputs)],
        check=True,
        capture_output=True,
    )
    archive = output.read_bytes()
    assert archive.startswith(b"070701")
    assert make_cpio_entry("validation.elf", inputs[0].read_bytes(), ino=1) in archive
    for removed in ("hello", "shell", "top", "signal_test"):
        assert f"{removed}.elf\0".encode() not in archive
    assert archive.index(b"libc_validation.elf\0") < archive.index(b"libc program.elf") < archive.index(b"TRAILER!!!\0")
    assert archive.index(b"busybox.elf\0") < archive.index(b"busybox program.elf") < archive.index(b"TRAILER!!!\0")
    assert make_cpio_entry("validation_child.elf", bytes(child), ino=2) in archive
    malformed = gen_validation_initramfs._malformed_images(bytes(child))
    for index, (name, image) in enumerate(malformed):
        assert make_cpio_entry(name, image, ino=6 + index) in archive
    malformed_by_name = dict(malformed)
    load_offset = gen_validation_initramfs.ELF_HEADER_BYTES
    bad_file_range = malformed_by_name["bad_file_range.elf"]
    assert struct.unpack_from("<Q", bad_file_range, load_offset + 8)[0] == (1 << 64) - 8
    bad_address = malformed_by_name["bad_address_overflow.elf"]
    assert struct.unpack_from("<Q", bad_address, load_offset + 16)[0] == (1 << 64) - 8

    boundary = gen_validation_initramfs._boundary_image(bytes(child))
    boundary_ino = 6 + len(malformed)
    assert make_cpio_entry("boundary_load.elf", boundary, ino=boundary_ino) in archive
    phoff, phnum, _ = gen_validation_initramfs._elf_layout(boundary)
    assert phnum == 4  # Original LOAD/GNU_STACK plus RX and RW boundary segments.
    rx = gen_validation_initramfs._program_header(boundary, phoff, phnum - 2)
    rw = gen_validation_initramfs._program_header(boundary, phoff, phnum - 1)
    assert rx[0:2] == (
        gen_validation_initramfs.PT_LOAD,
        gen_validation_initramfs.PF_R | gen_validation_initramfs.PF_X,
    )
    assert rx[3:8] == (
        gen_validation_initramfs.BOUNDARY_RX_VADDR,
        gen_validation_initramfs.BOUNDARY_RX_VADDR,
        gen_validation_initramfs.BOUNDARY_RX_FILE_BYTES,
        gen_validation_initramfs.BOUNDARY_RX_MEMORY_BYTES,
        gen_validation_initramfs.PAGE_BYTES,
    )
    assert rw[0:2] == (
        gen_validation_initramfs.PT_LOAD,
        gen_validation_initramfs.PF_R | gen_validation_initramfs.PF_W,
    )
    assert rw[3:8] == (
        gen_validation_initramfs.BOUNDARY_RW_VADDR,
        gen_validation_initramfs.BOUNDARY_RW_VADDR,
        gen_validation_initramfs.BOUNDARY_RW_FILE_BYTES,
        gen_validation_initramfs.BOUNDARY_RW_MEMORY_BYTES,
        gen_validation_initramfs.PAGE_BYTES,
    )
    assert boundary[rx[2] - gen_validation_initramfs.BOUNDARY_RX_PREFIX] == gen_validation_initramfs.RX_PREFIX_MARKER
    assert boundary[rx[2] + len(gen_validation_initramfs.RETURN_42[62])] == gen_validation_initramfs.RX_FILE_MARKER
    assert boundary[rw[2] - gen_validation_initramfs.BOUNDARY_RW_PREFIX] == gen_validation_initramfs.RW_PREFIX_MARKER
    assert boundary[rw[2] + len(gen_validation_initramfs.RETURN_42[62])] == gen_validation_initramfs.RW_FILE_MARKER


@pytest.mark.parametrize("arch,machine", [("X64", 62), ("ARM64", 183), ("RISCV64", 243)])
def test_vendored_runtime_build_is_native_and_incremental(tmp_path, arch, machine):
    required = ("clang", "clang++", "llvm-ar", "llvm-strip", "ld.lld", "cmake", "ninja", "sh", "bzip2", "uv")
    if any(shutil.which(tool) is None for tool in required):
        pytest.skip("LLVM, CMake, Ninja and BusyBox host-generator tools required")
    repository = Path(__file__).resolve().parents[2]
    root = tmp_path / "native runtime project"
    shutil.copytree(repository / "src/userspace", root / "src/userspace", symlinks=True)
    shutil.copytree(repository / "src/abi/include", root / "src/abi/include", symlinks=True)
    shutil.copytree(repository / "third_party", root / "third_party", symlinks=True)
    (root / "scripts").mkdir()
    shutil.copy2(repository / "scripts/gen_initramfs.py", root / "scripts/gen_initramfs.py")
    shutil.copy2(repository / "scripts/gen_busybox_headers.py", root / "scripts/gen_busybox_headers.py")
    (root / "CMakeLists.txt").write_text(
        """cmake_minimum_required(VERSION 3.31)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(production_userspace LANGUAGES C CXX ASM)
set(MOSS_BUILD_TESTS OFF)
find_program(UV_EXECUTABLE uv REQUIRED)
add_subdirectory(src/userspace)
"""
    )
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
        str(root),
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
    command = ("cmake", "--build", str(build), "--target", "initramfs", "mlibc-validation", "-j", "4")
    run(*command)
    runtime = build / "src/userspace/mlibc"
    for path in (
        runtime / "libc_validation.elf",
        runtime / "init.elf",
        runtime / "code-authority-service.elf",
        runtime / "loader-service.elf",
        runtime / "file-service.elf",
        runtime / "namespace-service.elf",
        runtime / "moss-file.elf",
        runtime / "moss-domain.elf",
        runtime / "process-service.elf",
        runtime / "moss-process.elf",
        runtime / "pipe-service.elf",
        runtime / "console-service.elf",
        runtime / "busybox/busybox",
    ):
        elf = path.read_bytes()
        assert elf[:6] == b"\x7fELF\x02\x01"
        assert struct.unpack_from("<H", elf, 18)[0] == machine
        assert struct.unpack_from("<Q", elf, 24)[0] >= 0x200000000
    init = (runtime / "init.elf").read_bytes()
    code_service = (runtime / "code-authority-service.elf").read_bytes()
    loader_service = (runtime / "loader-service.elf").read_bytes()
    loader_probe = (build / "userspace/loader_probe.elf").read_bytes()
    assert len(loader_probe) <= 16 * 4096  # Current File Service complete-file budget.
    file_service = (runtime / "file-service.elf").read_bytes()
    namespace_service = (runtime / "namespace-service.elf").read_bytes()
    file_client = (runtime / "moss-file.elf").read_bytes()
    domain_client = (runtime / "moss-domain.elf").read_bytes()
    process_service = (runtime / "process-service.elf").read_bytes()
    process_client = (runtime / "moss-process.elf").read_bytes()
    pipe_service = (runtime / "pipe-service.elf").read_bytes()
    console_service = (runtime / "console-service.elf").read_bytes()
    busybox = (runtime / "busybox/busybox").read_bytes()
    archive = (build / "initramfs.cpio").read_bytes()
    assert make_cpio_entry("init.elf", init, ino=1) in archive
    assert make_cpio_entry("code-authority-service.elf", code_service, ino=2) in archive
    assert make_cpio_entry("file-service.elf", file_service, ino=3) in archive
    assert make_cpio_entry("namespace-service.elf", namespace_service, ino=4) in archive
    assert make_cpio_entry("moss-file.elf", file_client, ino=5) in archive
    assert make_cpio_entry("busybox.elf", busybox, ino=6) in archive
    assert make_cpio_entry("moss-domain.elf", domain_client, ino=7) in archive
    assert make_cpio_entry("process-service.elf", process_service, ino=8) in archive
    assert make_cpio_entry("moss-process.elf", process_client, ino=9) in archive
    assert make_cpio_entry("pipe-service.elf", pipe_service, ino=10) in archive
    assert make_cpio_entry("console-service.elf", console_service, ino=11) in archive
    assert make_cpio_entry("loader-service.elf", loader_service, ino=12) in archive
    assert make_cpio_entry("loader_probe.elf", loader_probe, ino=13) in archive
    for removed in ("hello", "shell", "top", "signal_test"):
        assert not (build / "userspace" / f"{removed}.elf").exists()
        assert f"{removed}.elf\0".encode() not in archive
    assert not (build / "validation-initramfs.cpio").exists()
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
    assert "Generating initramfs.cpio" in rebuilt
    assert "no work to do" in run(*command)
    assert source_digest() == changed
