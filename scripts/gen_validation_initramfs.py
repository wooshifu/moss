"""Build deterministic validation fixtures using the production CPIO encoder."""

import struct
import sys
from pathlib import Path

from gen_initramfs import make_cpio_entry, make_cpio_trailer


def generate(program: bytes, child: bytes, libc_program: bytes, busybox: bytes) -> bytes:
    if len(child) < 64 or child[:6] != b"\x7fELF\x02\x01":
        raise ValueError("validation child must be an ELF64 little-endian image")
    phoff = struct.unpack_from("<Q", child, 32)[0]
    if phoff < 64 or phoff > len(child) - 56:
        raise ValueError("validation child must contain a complete program header")
    bad_entry = bytearray(child)
    bad_entry[24:32] = bytes(8)  # ELF64 e_entry: outside every user executable segment.
    bad_phentsize = bytearray(child)
    struct.pack_into("<H", bad_phentsize, 54, 55)
    bad_load = bytearray(child)
    struct.pack_into("<Q", bad_load, phoff + 32, len(child) + 1)
    return b"".join(
        [
            make_cpio_entry("validation.elf", program, ino=1),
            make_cpio_entry("validation_child.elf", child, ino=2),
            make_cpio_entry("fixture.bin", bytes(range(256)) * 256, ino=3, mode=0o100444),
            make_cpio_entry("libc_validation.elf", libc_program, ino=4),
            make_cpio_entry("busybox.elf", busybox, ino=5),
            make_cpio_entry("bad_entry.elf", bytes(bad_entry), ino=6),
            make_cpio_entry("bad_phentsize.elf", bytes(bad_phentsize), ino=7),
            make_cpio_entry("bad_load.elf", bytes(bad_load), ino=8),
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
