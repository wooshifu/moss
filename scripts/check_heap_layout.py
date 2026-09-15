#!/usr/bin/env python3
"""Relink a disposable invalid heap layout and require rejection before ready.

Uses an existing Ninja build and llvm-objcopy; production artifacts are untouched.
"""

import argparse
import hashlib
import json
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from .artifacts import Artifacts
except ImportError:
    from artifacts import Artifacts


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()
    artifacts = Artifacts.load(args.manifest)
    base = artifacts.manifest.parent
    original = artifacts.require("validation_kernel")
    original_hash = hashlib.sha256(original.read_bytes()).hexdigest()
    output = Path(tempfile.mkdtemp(prefix="heap-layout-", dir=base))
    commands = subprocess.check_output(
        ["ninja", "-C", str(base), "-t", "commands", "bin/moss.test.elf"], text=True, timeout=15
    )
    # Reuse the actual link inputs/flags, not a second kernel build definition.
    tokens = shlex.split(commands.splitlines()[-1])
    assert tokens[:2] == [":", "&&"] and "-o" in tokens, "unsupported Ninja link command"
    link = tokens[2 : tokens.index("&&", 2)]
    elf = output / "moss.test.elf"
    link[link.index("-o") + 1] = str(elf)
    link.append("-Wl,--defsym=_heap_end_addr=_kernel_end_addr+4096")
    subprocess.run(link, cwd=base, check=True, timeout=60)
    image = elf
    if artifacts.arch != "X64":
        objcopy = shutil.which("llvm-objcopy")
        assert objcopy, "llvm-objcopy is required"
        image = output / "moss.test.bin"
        subprocess.run([objcopy, "-O", "binary", str(elf), str(image)], check=True, timeout=15)
    data = json.loads(artifacts.manifest.read_text())
    data["artifacts"]["validation_kernel"] = str(image.relative_to(base))
    data["artifacts"]["debug_symbols"] = str(elf.relative_to(base))
    manifest = base / f"{output.name}.json"
    manifest.write_text(json.dumps(data, indent=2) + "\n")
    report_dir = output / "report"
    run = subprocess.run(
        [
            sys.executable,
            str(Path(__file__).with_name("kernel_validation.py")),
            "run",
            "--manifest",
            str(manifest),
            "--workload",
            "heap",
            "--output",
            str(report_dir),
            "--startup-timeout",
            "3",
            "--guest-timeout",
            "15",
        ],
        check=False,
    )
    report = json.loads((report_dir / "results.json").read_text())
    serial = (report_dir / "heap" / "serial.log").read_text()
    assert report["provenance"]["image_sha256"] == hashlib.sha256(image.read_bytes()).hexdigest()
    assert hashlib.sha256(original.read_bytes()).hexdigest() == original_hash
    assert run.returncode != 0 and report["guests"][0]["ready"] is None, report_dir
    assert "PFA: invalid linker memory layout" in serial, report_dir
    assert "Error: Memory management setup failed" in serial, report_dir
    print(f"invalid heap layout: verified ({report_dir})")


if __name__ == "__main__":
    main()
