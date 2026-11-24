import json
import struct
from pathlib import Path
from types import SimpleNamespace

import pytest

from scripts import coverage


def raw_profile(counters: list[int]) -> bytes:
    header = [0] * coverage.RAW_HEADER_QWORDS
    header[0] = coverage.RAW_MAGIC
    header[1] = coverage.RAW_VERSION
    header[5] = len(counters)
    header[18] = coverage.RAW_VALUE_KIND_LAST
    return struct.pack("<19Q", *header) + struct.pack(f"<{len(counters)}Q", *counters)


def arena(*records: tuple[str, list[int]]) -> bytes:
    data = bytearray()
    for program_id, counters in records:
        raw = raw_profile(counters)
        name = program_id.encode()
        data += struct.pack(
            "<IIIIQ32s",
            coverage.RECORD_MAGIC,
            coverage.RECORD_VERSION,
            len(counters),
            len(name),
            len(raw),
            name,
        )
        data += raw
    return bytes(data)


def write_arena(root: Path, workload: str, data: bytes, errors: int = 0) -> Path:
    directory = root / "validation" / workload / "coverage"
    directory.mkdir(parents=True)
    path = directory / "coverage.arena"
    path.write_bytes(data)
    (directory / "coverage.used").write_bytes(struct.pack("<Q", len(data)))
    (directory / "coverage.errors").write_bytes(struct.pack("<Q", errors))
    return path


def test_parse_arena_round_trip(tmp_path):
    path = tmp_path / "coverage.arena"
    path.write_bytes(arena(("kernel", [1, 2, 3]), ("init", [0, 7])))
    records = coverage.parse_arena(path)
    assert [(record.program_id, record.counter_count) for record in records] == [("kernel", 3), ("init", 2)]
    assert records[0].raw[coverage.RAW_HEADER_SIZE :] == struct.pack("<3Q", 1, 2, 3)


@pytest.mark.parametrize("mutate", ["magic", "truncate", "count"])
def test_parse_arena_rejects_damage(tmp_path, mutate):
    data = bytearray(arena(("kernel", [1, 2])))
    if mutate == "magic":
        data[0] ^= 1
    elif mutate == "truncate":
        data = data[:-4]
    else:
        struct.pack_into("<I", data, 8, 3)
    path = tmp_path / "coverage.arena"
    path.write_bytes(data)
    with pytest.raises(coverage.CoverageError):
        coverage.parse_arena(path)


def test_capture_health_rejects_runtime_errors(tmp_path):
    path = write_arena(tmp_path, "self", arena(("kernel", [1])), errors=1)
    with pytest.raises(coverage.CoverageError, match="runtime reported"):
        coverage.check_capture_health(path)


def test_generate_merges_records_and_ignores_python_tools(tmp_path, monkeypatch):
    build = tmp_path / "build"
    (build / "bin").mkdir(parents=True)
    (build / "bin" / "moss.test.symbols.elf").write_bytes(b"elf")
    (build / "moss-artifacts.json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "target": {"architecture": "ARM64", "boot_protocol": "linux-image"},
                "build": {"type": "RelWithDebInfo", "coverage": True, "compiler": "Clang", "flags": {}},
                "artifacts": {
                    "kernel": "bin/moss.bin",
                    "debug_symbols": "bin/moss.elf",
                    "initramfs": "initramfs.cpio",
                    "validation_kernel": "bin/moss.test.bin",
                    "validation_initramfs": "validation-initramfs.cpio",
                    "validation_debug_symbols": "bin/moss.test.symbols.elf",
                },
            }
        )
    )
    write_arena(build, "self", arena(("kernel", [1, 2])))
    write_arena(build, "users.libc", arena(("kernel", [3, 4])))
    calls = []

    def fake_run(command):
        calls.append(command)
        if command[1] == "show":
            output_dir = next(arg for arg in command if arg.startswith("-output-dir=")).removeprefix("-output-dir=")
            Path(output_dir).mkdir(parents=True)
        if command[1] == "export":
            return SimpleNamespace(stdout=json.dumps({"data": [{"totals": {"lines": {"count": 2, "covered": 2}}}]}))
        return SimpleNamespace(stdout="TOTAL\n")

    monkeypatch.setattr(coverage, "require_tool", lambda name: name)
    monkeypatch.setattr(coverage, "run", fake_run)
    summary = coverage.generate(build, tmp_path / "report")
    assert summary["arenas"] == 2
    assert summary["programs"]["kernel"]["records"] == 2
    assert summary["programs"]["kernel"]["totals"]["lines"]["covered"] == 2
    assert any(command[1] == "merge" for command in calls)
    assert (tmp_path / "report" / "summary.json").is_file()


def test_generate_rejects_unknown_program(tmp_path, monkeypatch):
    build = tmp_path / "build"
    (build / "bin").mkdir(parents=True)
    (build / "bin" / "moss.test.symbols.elf").write_bytes(b"elf")
    (build / "moss-artifacts.json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "target": {"architecture": "ARM64", "boot_protocol": "linux-image"},
                "build": {"type": "RelWithDebInfo", "coverage": True, "compiler": "Clang", "flags": {}},
                "artifacts": {
                    "kernel": "bin/moss.bin",
                    "debug_symbols": "bin/moss.elf",
                    "initramfs": "initramfs.cpio",
                    "validation_kernel": "bin/moss.test.bin",
                    "validation_initramfs": "validation-initramfs.cpio",
                    "validation_debug_symbols": "bin/moss.test.symbols.elf",
                },
            }
        )
    )
    write_arena(build, "self", arena(("missing", [1])))
    with pytest.raises(coverage.CoverageError, match="no unstripped ELF"):
        coverage.generate(build, tmp_path / "report")
