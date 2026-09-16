"""Run the real decimal helpers and inspect assembled embedded-init programs."""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPOSITORY = Path(__file__).resolve().parents[2]

# Include the production header unchanged. Only its pure formatting functions
# execute on the host; Moss syscall output is covered by users.validation in QEMU.
FORMAT_PROBE = r"""
#include <limits.h>
#include <stdio.h>
#include "syscall.h"

static int check(unsigned long magnitude, long signed_value, int is_signed, int capacity) {
  unsigned char storage[64];
  char expected[32];
  for (unsigned i = 0; i < sizeof(storage); ++i)
    storage[i] = 0xa5;
  int full = is_signed ? snprintf(expected, sizeof(expected), "%ld", signed_value)
                       : snprintf(expected, sizeof(expected), "%lu", magnitude);
  int wanted = capacity > 0 ? (full < capacity ? full : capacity - 1) : 0;
  int actual = is_signed ? ltoa(signed_value, (char *)storage + 1, capacity)
                         : ultoa(magnitude, (char *)storage + 1, capacity);
  if (actual != wanted) {
    fprintf(stderr, "length: signed=%d capacity=%d wanted=%d actual=%d\n",
            is_signed, capacity, wanted, actual);
    return 1;
  }
  for (int i = 0; i < wanted; ++i)
    if (storage[i + 1] != (unsigned char)expected[i]) {
      fprintf(stderr, "digits: signed=%d capacity=%d offset=%d\n", is_signed, capacity, i);
      return 1;
    }
  if (capacity > 0 && storage[wanted + 1] != 0) {
    fprintf(stderr, "missing NUL: signed=%d capacity=%d\n", is_signed, capacity);
    return 1;
  }
  for (unsigned i = 0; i < sizeof(storage); ++i)
    if ((i == 0 || i >= (unsigned)(capacity > 0 ? capacity + 1 : 1)) && storage[i] != 0xa5) {
      fprintf(stderr, "guard overwritten: signed=%d capacity=%d offset=%u\n", is_signed, capacity, i);
      return 1;
    }
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  if (argv[1][0] == 's')
    return check(0, LONG_MIN, 1, 21) || check(0, LONG_MAX, 1, 21) || check(0, -1, 1, 21);
  if (argv[1][0] == 'u')
    return check(ULONG_MAX, 0, 0, 21) || check(10000000000000000000UL, 0, 0, 21);
  if (argv[1][0] == 'b')
    return check(0, 0, 0, 1) || check(0, 0, 1, 1) || check(1, 0, 0, 0) || check(0, -1, 1, 0);
  if (argv[1][0] == 'p')
    return check(12345678901234567890UL, 0, 0, 4) || check(0, -1234567890123456789L, 1, 4);
  for (int capacity = 0; capacity <= 24; ++capacity)
    if (check(12345678901234567890UL, 0, 0, capacity) || check(0, -1234567890123456789L, 1, capacity)
        || check(0, 0, 0, capacity) || check(0, 0, 1, capacity))
      return 1;
  return 0;
}
"""


@pytest.fixture(scope="module")
def format_probe(tmp_path_factory):
    compiler = shutil.which("clang")
    if compiler is None:
        pytest.skip("system Clang with UBSan required")
    root = tmp_path_factory.mktemp("userspace-numbers")
    source = root / "numbers.c"
    executable = root / "numbers"
    source.write_text(FORMAT_PROBE, encoding="utf-8")
    result = subprocess.run(
        [
            compiler,
            "-std=gnu11",
            "-O1",
            "-g",
            "-fno-builtin",
            "-fsanitize=undefined,bounds",
            "-fno-sanitize-recover=all",
            "-I",
            str(REPOSITORY / "src/userspace"),
            str(source),
            "-o",
            str(executable),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return executable


@pytest.mark.parametrize(
    "case", ["signed_extremes", "unsigned_extremes", "buffer_boundaries", "prefix_truncation", "all_buffer_sizes"]
)
def test_decimal_helpers_execute_under_ubsan(format_probe, case):
    result = subprocess.run([str(format_probe), case], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize(
    "arch,triple,count_pattern,status_pattern",
    [
        ("arm64", "aarch64-unknown-elf", r"mov\s+x2, #0x([0-9a-f]+)", r"mov\s+x0, #0x([0-9a-f]+)"),
        ("riscv64", "riscv64-unknown-elf", r"li\s+a2, 0x([0-9a-f]+)", r"li\s+a0, 0x([0-9a-f]+)"),
        ("x64", "x86_64-unknown-elf", r"movq\s+\$0x([0-9a-f]+), %rdx", r"movq\s+\$0x([0-9a-f]+), %rdi"),
    ],
)
@pytest.mark.parametrize("extra", ["", " extra"])
def test_embedded_init_assembled_lengths_and_failure_status(
    tmp_path, arch, triple, count_pattern, status_pattern, extra
):
    required = ["clang", "ld.lld", "llvm-objdump"]
    if any(shutil.which(tool) is None for tool in required):
        pytest.skip("system LLVM assembler, ELF linker and objdump required")
    source = (REPOSITORY / f"src/kernel/src/arch/{arch}_user_program.S").read_text(encoding="utf-8")
    announcement = "init: calling execve..."
    failure = "execve failed with error: " if arch == "x64" else "init: execve failed!"
    # Vary both strings in a temporary fixture to prove emitted lengths follow
    # the data labels when text changes, rather than just matching today's text.
    source = source.replace(f'"{announcement}\\n"', f'"{announcement}{extra}\\n"')
    source = (
        source.replace(f'"{failure}"', f'"{failure}{extra}"')
        if arch == "x64"
        else source.replace(f'"{failure}\\n"', f'"{failure}{extra}\\n"')
    )
    assembly = tmp_path / "init.S"
    object_file = tmp_path / "init.o"
    elf = tmp_path / "init.elf"
    assembly.write_text(source, encoding="utf-8")

    def run(*argv):
        result = subprocess.run(argv, capture_output=True, text=True, check=False)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout

    run("clang", f"--target={triple}", "-c", str(assembly), "-o", str(object_file))
    run("ld.lld", "-e", "_user_program_start", str(object_file), "-o", str(elf))
    disassembly = run("llvm-objdump", "-d", "--no-show-raw-insn", "--print-imm-hex", str(elf))
    number_pattern = {
        "arm64": r"mov\s+x8, #0x([0-9a-f]+)",
        "riscv64": r"li\s+a7, 0x([0-9a-f]+)",
        "x64": r"movq\s+\$0x([0-9a-f]+), %rax",
    }[arch]
    counts = []
    statuses = []
    for entry in re.split(r"\b(?:svc\s+#0|ecall|syscall)\b", disassembly):
        numbers = re.findall(number_pattern, entry)
        if not numbers:
            continue
        number = int(numbers[-1], 16)
        # Inspect only write traps: execve's third argument is also zero in
        # the count register, but it is an envp pointer, not an output length.
        if number == 33:
            counts.append(int(re.findall(count_pattern, entry)[-1], 16))
        elif number == 1:
            statuses.append(int(re.findall(status_pattern, entry)[-1], 16))
    expected = [(announcement + extra + "\n").encode(), (failure + extra + ("" if arch == "x64" else "\n")).encode()]
    image = elf.read_bytes()
    assert all(payload in image for payload in expected)
    assert counts == [len(payload) for payload in expected], disassembly
    assert statuses == [1], disassembly
