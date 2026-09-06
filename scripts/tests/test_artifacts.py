import json
import struct
from pathlib import Path

import pytest

from scripts.artifacts import Artifacts
from scripts.run_qemu import build_qemu_args, resolve_qemu
from scripts.verify_linux_image import verify_arm64_header, verify_relocations, verify_riscv_header


def manifest(tmp_path: Path) -> Path:
    # Deliberately not a CMake build directory.
    for name in ("Image", "symbols.elf", "initrd", "test.Image", "test.initrd"):
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
    monkeypatch.setattr("scripts.run_qemu.shutil.which", lambda _: None)
    with pytest.raises(ValueError, match="QEMU executable not found"):
        resolve_qemu("ARM64")
    # Reading build artifacts must not require an emulator.
    assert Artifacts.load(artifacts.manifest).arch == "ARM64"


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


def test_riscv_header_and_relative_relocation_contract():
    data = bytearray(128)
    struct.pack_into("<I", data, 0, 0x6F)
    struct.pack_into("<QQQI", data, 8, 0x200000, 4096, 0, 2)
    struct.pack_into("<I", data, 56, 0x05435352)
    assert verify_riscv_header(data)
    struct.pack_into("<Q", data, 16, 0)
    assert not verify_riscv_header(data)

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
    for arch in ("arm64", "riscv", "x86_64"):
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
