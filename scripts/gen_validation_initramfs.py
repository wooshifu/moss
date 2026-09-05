"""Build deterministic validation fixtures using the production CPIO encoder."""

import sys
from pathlib import Path

from gen_initramfs import make_cpio_entry, make_cpio_trailer


def generate(program: bytes, child: bytes) -> bytes:
    return b"".join(
        [
            make_cpio_entry("shell.elf", program, ino=1),
            make_cpio_entry("validation_child.elf", child, ino=2),
            make_cpio_entry("fixture.bin", bytes(range(256)) * 256, ino=3, mode=0o100444),
            make_cpio_trailer(),
        ]
    )


if __name__ == "__main__":
    output, program, child = map(Path, sys.argv[1:])
    output.write_bytes(generate(program.read_bytes(), child.read_bytes()))
