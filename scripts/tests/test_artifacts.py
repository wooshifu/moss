import json
import struct
import subprocess
from dataclasses import replace
from pathlib import Path
from shutil import copy2
from types import SimpleNamespace

import pytest

from qemu import build_qemu_args, resolve_qemu
from scripts.artifacts import Artifacts, record_validation_provenance
from scripts.verify_linux_image import verify_arm64_header, verify_relocations, verify_riscv64_header


def manifest(tmp_path: Path) -> Path:
    # Deliberately not a CMake build directory.
    for name in ("Image", "symbols.elf", "initrd", "test.Image", "test.initrd", "test.symbols.elf"):
        (tmp_path / name).write_bytes(b"artifact")
    path = tmp_path / "moss-artifacts.json"
    path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "target": {"architecture": "ARM64", "boot_protocol": "linux-image"},
                "build": {"type": "Debug", "compiler": "Clang", "flags": {}},
                "artifacts": {
                    "kernel": "Image",
                    "debug_symbols": "symbols.elf",
                    "initramfs": "initrd",
                    "validation_kernel": "test.Image",
                    "validation_initramfs": "test.initrd",
                    "validation_debug_symbols": "test.symbols.elf",
                },
            }
        )
    )
    return path


def test_manifest_drives_normal_debug_and_validation_without_cmake(tmp_path):
    artifacts = Artifacts.load(manifest(tmp_path))
    normal = build_qemu_args(artifacts)
    debug = build_qemu_args(artifacts, debug_mode=True)
    validation = build_qemu_args(artifacts, validation=True)
    assert normal[normal.index("-kernel") + 1] == str(tmp_path / "Image")
    assert debug[debug.index("-kernel") + 1] == normal[normal.index("-kernel") + 1]
    assert debug[-2:] == ["-s", "-S"]
    assert validation[validation.index("-kernel") + 1] == str(tmp_path / "test.Image")
    assert validation[validation.index("-initrd") + 1] == str(tmp_path / "test.initrd")
    assert not any("semihost" in arg or "isa-debug-exit" in arg or "loader," in arg for arg in validation)


@pytest.mark.parametrize("host", ["linux", "darwin", "win32"])
@pytest.mark.parametrize(
    ("machine", "cpu", "memory_mib"),
    [("raspi3ap", "cortex-a53", 512), ("raspi3b", "cortex-a53", 1024), ("raspi4b", "cortex-a72", 2048)],
)
def test_raspi_defaults_and_explicit_dtb(tmp_path, monkeypatch, host, machine, cpu, memory_mib):
    artifacts = Artifacts.load(manifest(tmp_path))
    monkeypatch.setattr("qemu.sys.platform", host)
    default_dtb = Path(__file__).resolve().parents[1] / "qemu" / f"{machine}.dtb"
    for validation in (False, True):
        args = build_qemu_args(artifacts, machine=machine, validation=validation)
        assert args[args.index("-dtb") + 1] == str(default_dtb)
        assert args[args.index("-cpu") + 1] == cpu
        assert args[args.index("-smp") + 1] == "4"
        assert args[args.index("-m") + 1] == f"{memory_mib}M"
    custom_dtb = tmp_path / "custom.dtb"
    custom_dtb.write_bytes(b"custom")
    args = build_qemu_args(artifacts, machine=machine, dtb=custom_dtb)
    assert args[args.index("-dtb") + 1] == str(custom_dtb)
    for resources in ({"smp": 2}, {"memory_mib": memory_mib // 2}):
        with pytest.raises(ValueError, match=f"4 CPUs and {memory_mib} MiB"):
            build_qemu_args(artifacts, machine=machine, **resources)
    with pytest.raises(ValueError, match="ARM64"):
        build_qemu_args(replace(artifacts, arch="X64"), machine=machine)


@pytest.mark.parametrize(
    ("host", "directory", "writable", "free_mib", "extra", "machine", "automatic"),
    [
        ("linux", True, True, 2048, [], None, True),
        ("darwin", True, True, 2048, [], None, False),
        ("win32", True, True, 2048, [], None, False),
        ("linux", False, True, 2048, [], None, False),
        ("linux", True, False, 2048, [], None, False),
        ("linux", True, True, 2047, [], None, False),
        ("linux", True, True, 2048, ["-mem-path", "/custom/ram"], None, False),
        ("linux", True, True, 2048, ["-mem-path=/custom/ram"], None, False),
        ("linux", True, True, 2048, ["-object", "memory-backend-memfd,id=ram,size=2G"], None, False),
        ("linux", True, True, 2048, ["-numa", "node,memdev=ram"], None, False),
        ("linux", True, True, 2048, [], "virt,memory-backend=ram", False),
        ("linux", True, True, None, [], None, False),
    ],
)
def test_qemu_ram_backend_is_host_safe(
    tmp_path, monkeypatch, host, directory, writable, free_mib, extra, machine, automatic
):
    artifacts = Artifacts.load(manifest(tmp_path))
    monkeypatch.setattr("qemu.sys.platform", host)
    monkeypatch.setattr("qemu.Path.is_dir", lambda _: directory)
    monkeypatch.setattr("qemu.os.access", lambda _path, _mode: writable)

    def disk_usage(path):
        assert host == "linux" and path == Path("/dev/shm")
        if free_mib is None:
            raise OSError("shared memory unavailable")
        return SimpleNamespace(free=free_mib * 1024**2)

    monkeypatch.setattr("qemu.shutil.disk_usage", disk_usage)
    for validation in (False, True):
        args = build_qemu_args(artifacts, machine=machine, validation=validation, extra_args=extra)
        assert args[0] == "qemu-system-aarch64"
        assert args[args.index("-accel") + 1] == ("tcg,tb-size=64" if validation else "tcg")
        if automatic:
            assert args[-2:] == ["-mem-path", "/dev/shm"]
        else:
            assert "/dev/shm" not in args
        assert args.count("-mem-path") == (automatic or "-mem-path" in extra)
        if extra:
            assert args[-len(extra) :] == extra


@pytest.mark.parametrize("value", ["/tmp/image", "../image", "", None, 4])
def test_rejects_invalid_kernel_path(tmp_path, value):
    path = manifest(tmp_path)
    data = json.loads(path.read_text())
    data["artifacts"]["kernel"] = value
    path.write_text(json.dumps(data))
    with pytest.raises(ValueError):
        Artifacts.load(path)


def test_no_implicit_image_fallback_and_qemu_is_only_a_run_dependency(tmp_path, monkeypatch):
    artifacts = Artifacts.load(manifest(tmp_path))
    (tmp_path / "Image").unlink()
    with pytest.raises(ValueError, match="not built"):
        build_qemu_args(artifacts)
    monkeypatch.setattr("qemu.shutil.which", lambda _: None)
    with pytest.raises(ValueError, match="QEMU executable not found"):
        resolve_qemu("ARM64")
    # Reading build artifacts must not require an emulator.
    assert Artifacts.load(artifacts.manifest).arch == "ARM64"


def test_validation_provenance_binds_built_artifacts_not_current_checkout(tmp_path):
    source = tmp_path / "source"
    source.mkdir()

    def git(*args):
        return subprocess.check_output(
            ["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid", *args], cwd=source
        ).strip()

    git("init", "-q")
    (source / "kernel.c").write_text("int kernel = 1;\n")
    git("add", "kernel.c")
    git("commit", "-qm", "initial")
    built_revision = git("rev-parse", "HEAD").decode()
    artifacts = Artifacts.load(manifest(tmp_path))
    record_validation_provenance(artifacts, source)
    original = artifacts.validation_provenance()
    assert original["revision"] == built_revision and original["dirty"] is False
    assert original["source_status"] == "recorded"

    (source / "kernel.c").write_text("int kernel = 2;\n")
    git("commit", "-qam", "changed checkout")
    assert git("rev-parse", "HEAD").decode() != built_revision
    assert artifacts.validation_provenance() == original

    artifacts.require("validation_initramfs").write_bytes(b"changed fixture")
    with pytest.raises(ValueError, match="provenance does not match"):
        artifacts.validation_provenance()


def test_missing_provenance_is_unknown_and_dirty_sources_have_distinct_identity(tmp_path):
    artifacts = Artifacts.load(manifest(tmp_path))
    assert artifacts.validation_provenance()["revision"] is None
    assert artifacts.validation_provenance()["source_status"] == "unrecorded"
    source = tmp_path / "source"
    source.mkdir()
    subprocess.run(["git", "init", "-q", str(source)], check=True)
    subprocess.run(
        [
            "git",
            "-c",
            "user.name=Test",
            "-c",
            "user.email=test@example.invalid",
            "commit",
            "--allow-empty",
            "-qm",
            "base",
        ],
        cwd=source,
        check=True,
    )
    (source / "new.c").write_text("int value = 1;\n")
    record_validation_provenance(artifacts, source)
    before = artifacts.validation_provenance()
    assert before["dirty"] is True
    (source / "new.c").write_text("int value = 2;\n")
    record_validation_provenance(artifacts, source)
    after = artifacts.validation_provenance()
    assert before["revision"] == after["revision"]
    assert before["source_sha256"] != after["source_sha256"]
    artifacts.require("validation_kernel").write_bytes(b"changed image")
    with pytest.raises(ValueError, match="provenance does not match"):
        artifacts.validation_provenance()


def test_validation_snapshot_survives_in_place_rebuild(tmp_path):
    artifacts = Artifacts.load(manifest(tmp_path))
    identity = artifacts.validation_identity()
    snapshot = artifacts.snapshot_validation(tmp_path / "inputs")
    for name in ("validation_kernel", "validation_initramfs", "validation_debug_symbols"):
        artifacts.require(name).write_bytes(b"rebuilt")
    assert snapshot.validation_identity() == identity
    assert snapshot.validation_provenance()["source_status"] == "unrecorded"
    args = build_qemu_args(snapshot, validation=True)
    assert Path(args[args.index("-kernel") + 1]).read_bytes() == b"artifact"
    assert Path(args[args.index("-initrd") + 1]).read_bytes() == b"artifact"
    assert snapshot.require("validation_debug_symbols").read_bytes() == b"artifact"


def test_legacy_manifest_explicitly_has_no_validation_symbols(tmp_path):
    path = manifest(tmp_path)
    data = json.loads(path.read_text())
    del data["artifacts"]["validation_debug_symbols"]
    path.write_text(json.dumps(data))
    artifacts = Artifacts.load(path)
    assert artifacts.files["validation_debug_symbols"] is None
    assert "symbols_sha256" not in artifacts.snapshot_validation(tmp_path / "inputs").validation_provenance()


@pytest.mark.parametrize("value", ["../symbols.elf", "/tmp/symbols.elf", "", 4])
def test_validation_symbols_must_stay_inside_manifest(tmp_path, value):
    path = manifest(tmp_path)
    data = json.loads(path.read_text())
    data["artifacts"]["validation_debug_symbols"] = value
    path.write_text(json.dumps(data))
    with pytest.raises(ValueError, match="validation_debug_symbols"):
        Artifacts.load(path)


def test_validation_rejects_symbols_from_another_link(tmp_path):
    artifacts = Artifacts.load(manifest(tmp_path))
    source = tmp_path / "source"
    source.mkdir()
    record_validation_provenance(artifacts, source)
    artifacts.require("validation_debug_symbols").write_bytes(b"different link")
    with pytest.raises(ValueError, match="provenance does not match"):
        artifacts.snapshot_validation(tmp_path / "inputs")


@pytest.mark.parametrize("damaged", ["test.Image", "test.initrd", "test.symbols.elf"])
def test_validation_snapshot_rejects_build_during_copy(tmp_path, monkeypatch, damaged):
    artifacts = Artifacts.load(manifest(tmp_path))

    def raced_copy(source, target):
        if source.name == damaged:
            target.write_bytes(b"concurrent build")
        else:
            copy2(source, target)

    monkeypatch.setattr("scripts.artifacts.shutil.copy2", raced_copy)
    with pytest.raises(ValueError, match="provenance does not match"):
        artifacts.snapshot_validation(tmp_path / "inputs")


def test_image_header_requires_static_size_before_boot_relocation():
    data = bytearray(128)
    struct.pack_into("<I", data, 0, 0x14000010)
    struct.pack_into("<I", data, 56, 0x644D5241)
    assert not verify_arm64_header(data)
    struct.pack_into("<Q", data, 16, 4096)
    struct.pack_into("<Q", data, 24, 10)
    assert verify_arm64_header(data)
    struct.pack_into("<Q", data, 16, 64)
    assert not verify_arm64_header(data)


def test_riscv64_header_and_relative_relocation_contract():
    data = bytearray(128)
    struct.pack_into("<I", data, 0, 0x6F)
    struct.pack_into("<QQQI", data, 8, 0x200000, 4096, 0, 2)
    struct.pack_into("<I", data, 56, 0x05435352)
    assert verify_riscv64_header(data)
    struct.pack_into("<Q", data, 16, 0)
    assert not verify_riscv64_header(data)

    # One allocated ELF64 RELA section, with a bootstrap-supported relocation.
    elf = bytearray(152)
    struct.pack_into("<16sHHIQQQIHHHHHH", elf, 0, b"\x7fELF\x02\x01", 3, 243, 1, 64, 0, 64, 0, 64, 0, 0, 64, 1, 0)
    struct.pack_into("<IIQQQQIIQQ", elf, 64, 0, 4, 2, 128, 128, 24, 0, 0, 8, 24)
    struct.pack_into("<QQq", elf, 128, 256, 3, 512)
    verify_relocations(elf, 4096, 243)
    for target, kind, addend in [(0, 3, 512), (256, 2, 512), (256, 3, 4097), (4096, 3, 512)]:
        struct.pack_into("<QQq", elf, 128, target, kind, addend)
        with pytest.raises(ValueError):
            verify_relocations(elf, 4096, 243)


def test_workflows_reuse_matching_build_and_test_presets():
    root = Path(__file__).resolve().parents[2]
    for arch in ("arm64", "riscv64", "x64"):
        data = json.loads((root / "cmake" / "presets" / "arch" / f"{arch}.json").read_text())
        assert "qemu" not in json.dumps(data).lower()
        assert all("targets" not in preset for preset in data["buildPresets"])
        tests = {preset["name"]: preset for preset in data["testPresets"]}
        assert {workflow["name"] for workflow in data["workflowPresets"]} == {
            preset["name"] for preset in data["buildPresets"]
        }
        for workflow in data["workflowPresets"]:
            name = workflow["name"]
            assert workflow["steps"] == [
                {"type": "configure", "name": name},
                {"type": "build", "name": name},
                {"type": "test", "name": f"{name}-test"},
            ]
            assert tests[f"{name}-test"]["configurePreset"] == name
