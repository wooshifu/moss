"""Exercise the production CPIO parser against malformed boot archives."""

import shutil
import subprocess
from pathlib import Path

import pytest

from scripts.gen_initramfs import make_cpio_entry, make_cpio_trailer

ROOT = Path(__file__).resolve().parents[2]

# newc fixes six magic bytes followed by eleven eight-digit fields before
# c_namesize. Derive the mutation point from the format instead of a raw offset.
CPIO_MAGIC_BYTES = 6
CPIO_HEX_FIELD_BYTES = 8
CPIO_FIELDS_BEFORE_NAMESIZE = 11
CPIO_NAMESIZE_OFFSET = CPIO_MAGIC_BYTES + CPIO_FIELDS_BEFORE_NAMESIZE * CPIO_HEX_FIELD_BYTES
# UID is field index two after inode and mode. The parser does not consume its
# value, so corrupting it verifies that ignored metadata is still validated.
CPIO_UID_FIELD_INDEX = 2
CPIO_UID_OFFSET = CPIO_MAGIC_BYTES + CPIO_UID_FIELD_INDEX * CPIO_HEX_FIELD_BYTES
# The production generator enumerates real entries from one. Match that
# convention even though the production parser does not consume the inode.
FIXTURE_INODE = 1


def production_parser_source() -> str:
    text = (ROOT / "src/initramfs/src/initramfs.cppm").read_text()
    begin = text.index("namespace log =")
    end = text.index("/// Global initramfs instance")
    implementation = text[begin:end]
    return (
        r"""
#define MOSS_ARCH_X64 1
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using usize = std::size_t;
using PhysAddr = std::uintptr_t;
using VirtAddr = std::uintptr_t;

namespace moss::kernel::logging {
struct klog {
  template <typename... Args> static void info(const char *, Args...) noexcept {}
  template <typename... Args> static void warn(const char *, Args...) noexcept {}
};
}

namespace moss::kernel::initramfs {
"""
        + implementation
        + r"""
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  std::ifstream input(argv[1], std::ios::binary);
  std::vector<u8> bytes(std::istreambuf_iterator<char>(input), {});
  moss::kernel::initramfs::InitramfsArchive archive;
  const bool valid = archive.init(reinterpret_cast<PhysAddr>(bytes.data()), bytes.size());
  std::printf("%d %d %u %d\n", valid, archive.is_initialized(), archive.file_count(),
              archive.lookup("/busybox.elf") != nullptr);
}
"""
    )


@pytest.fixture(scope="module")
def parser_binary(tmp_path_factory):
    compiler = shutil.which("clang++")
    if not compiler:
        pytest.skip("Clang is needed to exercise the production initramfs parser")
    directory = tmp_path_factory.mktemp("initramfs-parser")
    source = directory / "parser.cpp"
    binary = directory / "parser"
    source.write_text(production_parser_source())
    subprocess.run(
        [compiler, "-std=c++23", "-O2", str(source), "-o", str(binary)],
        check=True,
        capture_output=True,
    )
    return binary


def parse(parser_binary: Path, tmp_path: Path, archive: bytes) -> tuple[int, int, int, int]:
    fixture = tmp_path / "initramfs.cpio"
    fixture.write_bytes(archive)
    result = subprocess.run([str(parser_binary), str(fixture)], check=True, capture_output=True, text=True)
    return tuple(map(int, result.stdout.split()))


def test_valid_archive_and_empty_archive_are_structurally_distinct(parser_binary, tmp_path):
    archive = make_cpio_entry("busybox.elf", b"ELF", ino=FIXTURE_INODE) + make_cpio_trailer()
    assert parse(parser_binary, tmp_path, archive) == (1, 1, 1, 1)
    # A trailer-only CPIO is structurally valid; boot policy, not the parser,
    # reports that it lacks the required init executable.
    assert parse(parser_binary, tmp_path, make_cpio_trailer()) == (1, 1, 0, 0)


def test_malformed_archives_leave_no_partial_index(parser_binary, tmp_path):
    entry = make_cpio_entry("busybox.elf", b"ELF", ino=FIXTURE_INODE)
    valid = entry + make_cpio_trailer()

    bad_magic = bytearray(valid)
    bad_magic[0] = ord("X")
    bad_hex = bytearray(valid)
    bad_hex[CPIO_NAMESIZE_OFFSET] = ord("G")
    bad_unused_hex = bytearray(valid)
    bad_unused_hex[CPIO_UID_OFFSET] = ord("G")
    unterminated_name = bytearray(valid)
    terminator = valid.index(b"busybox.elf\0") + len("busybox.elf")
    unterminated_name[terminator] = ord("X")

    for archive in (
        bytes(bad_magic),
        bytes(bad_hex),
        bytes(bad_unused_hex),
        bytes(unterminated_name),
        entry,
        valid[:-1],  # Removing even one required padding byte must reject the final record.
    ):
        assert parse(parser_binary, tmp_path, archive) == (0, 0, 0, 0)
