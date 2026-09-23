"""Require a platform reset when the production Initial System Supervisor dies."""

import argparse
import hashlib
import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from contextlib import suppress
from dataclasses import replace
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from qemu import build_qemu_args, resolve_qemu
from scripts.artifacts import Artifacts
from scripts.gen_initramfs import make_cpio_entry, make_cpio_trailer

RESET_MARKER = b"initial system supervisor exited:"


def run_case(cfg: Artifacts, output: Path, *, early: bool, timeout: float = 20) -> dict:
    output.mkdir(parents=True, exist_ok=False)
    if early:
        # A valid production archive with no /init.elf makes the fixed boot
        # trampoline fail before the supervisor can start any service.
        source_initramfs = output / "missing-init.cpio"
        source_initramfs.write_bytes(make_cpio_entry("busybox.elf", b"unused", ino=1) + make_cpio_trailer())
    else:
        source_initramfs = cfg.require("initramfs")
    files = dict(cfg.files)
    hashes = {}
    for name, source in (("kernel", cfg.require("kernel")), ("initramfs", source_initramfs)):
        target = output / f"frozen-{name}-{source.name}"
        shutil.copy2(source, target)
        hashes[name] = hashlib.sha256(target.read_bytes()).hexdigest()
        if hashes[name] != hashlib.sha256(source.read_bytes()).hexdigest():
            raise ValueError(f"{name} changed during artifact copy")
        files[name] = target
    args = build_qemu_args(replace(cfg, files=files), qemu=resolve_qemu(cfg.arch), smp=4)
    listener = None
    if sys.platform == "win32":
        # QEMU stdio can discard line endings when driven through an anonymous
        # Windows pipe. The same loopback chardev as the production probe keeps
        # serial evidence exact.
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(5)
        chardev = args.index("-chardev") + 1
        args[chardev] = f"socket,id=char0,host=127.0.0.1,port={listener.getsockname()[1]},mux=on"

    result = dict(status="error", observed="timeout", mode="bootstrap" if early else "running", sha256=hashes)
    serial = output / "serial.log"
    child = None
    console = None
    started = time.monotonic()
    sent = False
    pending = b""
    try:
        with serial.open("wb") as out, serial.open("rb") as incoming, (output / "qemu.log").open("wb") as err:
            child = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=out, stderr=err)
            if listener:
                console = listener.accept()[0]
                console.setblocking(False)
            while time.monotonic() - started < timeout:
                try:
                    chunk = console.recv(2**20) if console else incoming.read(2**20)
                except BlockingIOError:
                    chunk = b""
                if console and chunk:
                    out.write(chunk)
                    out.flush()
                pending += chunk.replace(b"\r", b"")
                if len(pending) > 2**20:
                    result["observed"] = "serial output exceeded 1 MiB"
                    break
                if not early and not sent and b"moss$ " in pending:
                    if console:
                        console.sendall(b"/moss-domain.elf terminate supervisor\n")
                    else:
                        child.stdin.write(b"/moss-domain.elf terminate supervisor\n")
                        child.stdin.flush()
                    sent = True
                if child.poll() is not None:
                    # QEMU may exit between the last serial read and poll().
                    if console:
                        with suppress(BlockingIOError):
                            tail = console.recv(2**20)
                            out.write(tail)
                            pending += tail.replace(b"\r", b"")
                    else:
                        pending += incoming.read().replace(b"\r", b"")
                    if RESET_MARKER in pending and (early or sent) and child.returncode == 0:
                        result.update(status="passed", observed="platform_reset")
                    else:
                        result["observed"] = "guest exited without verified supervisor reset"
                    break
                time.sleep(0.02)
    finally:
        if child:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
            child.stdin.close()
            result["raw_exit"] = child.returncode
        if console:
            console.close()
        if listener:
            listener.close()
        result.update(elapsed_seconds=time.monotonic() - started, termination_sent=sent)
        (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    cfg = Artifacts.load(args.manifest)
    root = cfg.manifest.parent / "supervisor-reset"
    root.mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=root))
    results = [run_case(cfg, output / mode, early=early) for mode, early in (("running", False), ("bootstrap", True))]
    for result in results:
        print(f"{cfg.arch} {result['mode']}: {result['status']} ({result['observed']})")
    print(output)
    return 0 if all(result["status"] == "passed" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
