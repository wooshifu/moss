"""Build deterministic validation fixtures using the production CPIO encoder."""

import struct
import sys
from pathlib import Path

try:
    from .gen_initramfs import make_cpio_entry, make_cpio_trailer
except ImportError:  # Direct script execution puts this directory on sys.path.
    from gen_initramfs import make_cpio_entry, make_cpio_trailer


# ELF64 wire sizes, tags and field offsets come from the format, not host
# Python layouts. Header offsets used below are machine=18, entry=24,
# phoff=32, phentsize=54 and phnum=56; program-header offsets are type=0,
# flags=4, file offset=8, virtual address=16, filesz=32, memsz=40 and align=48.
ELF_HEADER_BYTES = 64
PROGRAM_HEADER_BYTES = 56
PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PT_TLS = 7
PF_X = 1
PF_W = 2
PF_R = 4

# Moss uses 4 KiB user pages on every supported architecture. Both synthetic
# segments start at deliberately non-page-aligned, instruction-aligned offsets.
# The non-power-of-two 37/211 and 83/257 tails distinguish file/BSS endpoints
# from page boundaries, making off-by-one rounding and zero-fill errors visible.
PAGE_BYTES = 4096
BOUNDARY_RX_PAGE = 0x0000000200020000
BOUNDARY_RX_PREFIX = 0x124
BOUNDARY_RX_VADDR = BOUNDARY_RX_PAGE + BOUNDARY_RX_PREFIX
BOUNDARY_RX_FILE_BYTES = PAGE_BYTES + 37
BOUNDARY_RX_MEMORY_BYTES = BOUNDARY_RX_FILE_BYTES + 211
BOUNDARY_RW_PAGE = 0x0000000200040000
BOUNDARY_RW_PREFIX = 0x2A0
BOUNDARY_RW_VADDR = BOUNDARY_RW_PAGE + BOUNDARY_RW_PREFIX
BOUNDARY_RW_FILE_BYTES = 83
BOUNDARY_RW_MEMORY_BYTES = PAGE_BYTES + 257

# These marker bytes are fixture protocol values chosen to remain distinct from
# zero-fill and from each other; validation_child.c checks the same values.
RX_PREFIX_MARKER = 0xA1
RX_FILE_MARKER = 0xB2
RW_PREFIX_MARKER = 0xC3
RW_FILE_MARKER = 0xD4

# Each sequence returns integer 42 using the target's ordinary calling ABI.
RETURN_42 = {
    62: bytes.fromhex("b82a000000c3"),  # x86-64: mov eax, 42; ret
    183: bytes.fromhex("40058052c0035fd6"),  # AArch64: mov w0, 42; ret
    243: bytes.fromhex("1305a00267800000"),  # RV64: addi a0, zero, 42; ret
}


def _elf_layout(image: bytes) -> tuple[int, int, int]:
    if len(image) < ELF_HEADER_BYTES or image[:6] != b"\x7fELF\x02\x01":
        raise ValueError("validation child must be an ELF64 little-endian image")
    phoff = struct.unpack_from("<Q", image, 32)[0]
    phentsize, phnum, machine = (
        struct.unpack_from("<HH", image, 54)[0],
        struct.unpack_from("<H", image, 56)[0],
        struct.unpack_from("<H", image, 18)[0],
    )
    if phentsize != PROGRAM_HEADER_BYTES or phoff < ELF_HEADER_BYTES or phnum == 0:
        raise ValueError("validation child must use complete ELF64 program headers")
    if phoff > len(image) or phnum > (len(image) - phoff) // PROGRAM_HEADER_BYTES:
        raise ValueError("validation child must contain its complete program-header table")
    return phoff, phnum, machine


def _program_header(image: bytes, phoff: int, index: int) -> tuple[int, int, int, int, int, int, int, int]:
    return struct.unpack_from("<IIQQQQQQ", image, phoff + index * PROGRAM_HEADER_BYTES)


def _first_load(image: bytes, phoff: int, phnum: int) -> int:
    for index in range(phnum):
        if _program_header(image, phoff, index)[0] == PT_LOAD:
            return index
    raise ValueError("validation child must contain a PT_LOAD segment")


def _append_program_header(image: bytearray, header: tuple[int, int, int, int, int, int, int, int]) -> None:
    phoff, phnum, _ = _elf_layout(image)
    occupied_offsets = [
        program[2] for index in range(phnum) if (program := _program_header(image, phoff, index))[2] != 0
    ]
    first_payload = min(occupied_offsets, default=len(image))
    header_offset = phoff + phnum * PROGRAM_HEADER_BYTES
    if header_offset + PROGRAM_HEADER_BYTES > first_payload:
        raise ValueError("validation child has no spare program-header slot")
    struct.pack_into("<IIQQQQQQ", image, header_offset, *header)
    struct.pack_into("<H", image, 56, phnum + 1)


def _append_load_segment(
    image: bytearray,
    *,
    vaddr: int,
    prefix: int,
    file_bytes: int,
    memory_bytes: int,
    flags: int,
    prefix_marker: int,
    file_marker: int,
    code: bytes,
) -> None:
    if vaddr % PAGE_BYTES != prefix or len(code) > file_bytes or file_bytes > memory_bytes:
        raise ValueError("invalid synthetic LOAD geometry")
    file_page = (len(image) + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1)
    image.extend(bytes(file_page - len(image)))
    image.extend(bytes([prefix_marker]) * prefix)
    file_offset = len(image)
    image.extend(code)
    image.extend(bytes([file_marker]) * (file_bytes - len(code)))
    _append_program_header(
        image,
        (PT_LOAD, flags, file_offset, vaddr, vaddr, file_bytes, memory_bytes, PAGE_BYTES),
    )


def _boundary_image(child: bytes) -> bytes:
    image = bytearray(child)
    _, _, machine = _elf_layout(image)
    try:
        code = RETURN_42[machine]
    except KeyError as error:
        raise ValueError(f"unsupported validation-child machine {machine}") from error
    _append_load_segment(
        image,
        vaddr=BOUNDARY_RX_VADDR,
        prefix=BOUNDARY_RX_PREFIX,
        file_bytes=BOUNDARY_RX_FILE_BYTES,
        memory_bytes=BOUNDARY_RX_MEMORY_BYTES,
        flags=PF_R | PF_X,
        prefix_marker=RX_PREFIX_MARKER,
        file_marker=RX_FILE_MARKER,
        code=code,
    )
    _append_load_segment(
        image,
        vaddr=BOUNDARY_RW_VADDR,
        prefix=BOUNDARY_RW_PREFIX,
        file_bytes=BOUNDARY_RW_FILE_BYTES,
        memory_bytes=BOUNDARY_RW_MEMORY_BYTES,
        flags=PF_R | PF_W,
        prefix_marker=RW_PREFIX_MARKER,
        file_marker=RW_FILE_MARKER,
        code=code,
    )
    return bytes(image)


def _malformed_images(child: bytes) -> list[tuple[str, bytes]]:
    phoff, phnum, _ = _elf_layout(child)
    load_index = _first_load(child, phoff, phnum)
    load_offset = phoff + load_index * PROGRAM_HEADER_BYTES

    bad_entry = bytearray(child)
    bad_entry[24:32] = bytes(8)  # ELF64 e_entry: outside every executable segment.
    bad_phentsize = bytearray(child)
    struct.pack_into("<H", bad_phentsize, 54, PROGRAM_HEADER_BYTES - 1)
    bad_load = bytearray(child)
    load_memsz = struct.unpack_from("<Q", bad_load, load_offset + 40)[0]
    struct.pack_into("<Q", bad_load, load_offset + 32, load_memsz + 1)

    truncated_phdr = bytearray(child)
    struct.pack_into("<Q", truncated_phdr, 32, len(child) - PROGRAM_HEADER_BYTES + 1)
    struct.pack_into("<H", truncated_phdr, 56, 1)
    bad_file_range = bytearray(child)
    # A naive p_offset+p_filesz check wraps to 8; subtract-before-add validation
    # must reject it without reading outside the archive.
    struct.pack_into("<Q", bad_file_range, load_offset + 8, (1 << 64) - 8)
    struct.pack_into("<QQ", bad_file_range, load_offset + 32, 16, 16)
    bad_user_range = bytearray(child)
    # Address zero lies in the kernel identity range, below the accepted user band.
    struct.pack_into("<QQ", bad_user_range, load_offset + 16, 0, 0)
    struct.pack_into("<Q", bad_user_range, 24, 0)
    bad_address_overflow = bytearray(child)
    # Keep file/page congruence valid while vaddr+memsz would wrap to 8 under
    # unchecked arithmetic. The only intended rejection is the user range.
    overflow_address = (1 << 64) - 8
    struct.pack_into("<Q", bad_address_overflow, load_offset + 8, PAGE_BYTES - 8)
    struct.pack_into("<QQ", bad_address_overflow, load_offset + 16, overflow_address, overflow_address)
    struct.pack_into("<QQ", bad_address_overflow, load_offset + 32, 0, 16)
    struct.pack_into("<Q", bad_address_overflow, 24, overflow_address)
    bad_page_offset = bytearray(child)
    original_vaddr = struct.unpack_from("<Q", bad_page_offset, load_offset + 16)[0]
    # Four bytes preserve instruction alignment but break file/virtual page congruence.
    struct.pack_into("<QQ", bad_page_offset, load_offset + 16, original_vaddr + 4, original_vaddr + 4)
    struct.pack_into("<Q", bad_page_offset, 24, original_vaddr + 4)
    bad_alignment = bytearray(child)
    struct.pack_into("<Q", bad_alignment, load_offset + 48, 3)
    bad_reserved = bytearray(child)
    heap_start = 0x0000000100000000
    struct.pack_into("<QQ", bad_reserved, load_offset + 16, heap_start, heap_start)
    struct.pack_into("<Q", bad_reserved, 24, heap_start)

    bad_overlap = bytearray(child)
    _append_program_header(bad_overlap, _program_header(child, phoff, load_index))
    too_many = bytearray(child)
    # 65 is one above the loader's documented 64-header bound; the existing
    # file gap still contains the entire synthetic table, so only the cap fails.
    struct.pack_into("<H", too_many, 56, 65)
    if phoff + 65 * PROGRAM_HEADER_BYTES > _program_header(child, phoff, load_index)[2]:
        raise ValueError("validation child cannot hold the oversized header fixture")
    bad_rwx = bytearray(child)
    struct.pack_into("<I", bad_rwx, load_offset + 4, PF_R | PF_W | PF_X)
    bad_dynamic = bytearray(child)
    _append_program_header(bad_dynamic, (PT_DYNAMIC, 0, 0, 0, 0, 0, 0, 1))
    bad_interp = bytearray(child)
    _append_program_header(bad_interp, (PT_INTERP, 0, 0, 0, 0, 0, 0, 1))
    bad_tls_file = bytearray(child)
    # Static runtimes may describe a file-backed TLS template covered by PT_LOAD.
    # This otherwise valid one-byte template is deliberately outside every LOAD,
    # so the loader must reject it instead of exposing unmapped file metadata.
    orphan_tls_address = 0x0000000200060000
    _append_program_header(bad_tls_file, (PT_TLS, PF_R, 0, orphan_tls_address, orphan_tls_address, 1, 16, 8))

    return [
        ("bad_entry.elf", bytes(bad_entry)),
        ("bad_phentsize.elf", bytes(bad_phentsize)),
        ("bad_load.elf", bytes(bad_load)),
        ("truncated_header.elf", child[: ELF_HEADER_BYTES - 1]),
        ("truncated_phdr.elf", bytes(truncated_phdr)),
        ("bad_file_range.elf", bytes(bad_file_range)),
        ("bad_user_range.elf", bytes(bad_user_range)),
        ("bad_address_overflow.elf", bytes(bad_address_overflow)),
        ("bad_page_offset.elf", bytes(bad_page_offset)),
        ("bad_alignment.elf", bytes(bad_alignment)),
        ("bad_reserved.elf", bytes(bad_reserved)),
        ("bad_overlap.elf", bytes(bad_overlap)),
        ("too_many_phdrs.elf", bytes(too_many)),
        ("bad_rwx.elf", bytes(bad_rwx)),
        ("bad_dynamic.elf", bytes(bad_dynamic)),
        ("bad_interp.elf", bytes(bad_interp)),
        ("bad_tls_file.elf", bytes(bad_tls_file)),
    ]


def generate(program: bytes, child: bytes, libc_program: bytes, busybox: bytes) -> bytes:
    malformed = _malformed_images(child)
    return b"".join(
        [
            make_cpio_entry("validation.elf", program, ino=1),
            make_cpio_entry("validation_child.elf", child, ino=2),
            make_cpio_entry("fixture.bin", bytes(range(256)) * 256, ino=3, mode=0o100444),
            make_cpio_entry("libc_validation.elf", libc_program, ino=4),
            make_cpio_entry("busybox.elf", busybox, ino=5),
            *(make_cpio_entry(name, image, ino=6 + index) for index, (name, image) in enumerate(malformed)),
            make_cpio_entry("boundary_load.elf", _boundary_image(child), ino=6 + len(malformed)),
            make_cpio_trailer(),
        ]
    )


if __name__ == "__main__":
    output, program, child, libc_program, busybox = map(Path, sys.argv[1:])
    output.write_bytes(
        generate(
            program.read_bytes(),
            child.read_bytes(),
            libc_program.read_bytes(),
            busybox.read_bytes(),
        )
    )
