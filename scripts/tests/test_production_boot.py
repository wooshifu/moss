import sys

import pytest

from scripts import check_production_boot as boot
from scripts.artifacts import Artifacts


@pytest.mark.parametrize(
    "mode",
    [
        "complete",
        "legacy_shell",
        "applets_failed",
        "echo_only",
        "exit",
        "late_panic",
        "gdb_complete",
        "gdb_unverified",
        "gdb_failure",
    ],
)
def test_production_probe_requires_exec_and_subsequent_shell_output(tmp_path, monkeypatch, mode):
    files = {name: tmp_path / name for name in ("kernel", "initramfs", "debug_symbols")}
    for path in files.values():
        path.write_bytes(b"image")
    cfg = Artifacts(tmp_path / "manifest.json", "ARM64", "linux-image", {"type": "Debug"}, files)
    script = "import signal, sys, time\n"
    script += "print('moss-init: supervisor ready', flush=True)\n"
    if mode != "legacy_shell":
        script += "print('BusyBox built-in shell (ash)', flush=True)\n"
    script += r"""
print('moss$ ', end='', flush=True)
assert input() == '/busybox.elf ash -c \'printf "MOSS_EXEC_READY\\n"\''
print('\nMOSS_EXEC_READY\nmoss$ ', end='', flush=True)
assert input().startswith('mkdir /shell-check && ')
print('moss$ ', end='', flush=True)
assert input().startswith('count=$(cat /shell-check/result | grep moss | wc -l); ')
"""
    if mode == "applets_failed":
        script += "print('cat: not found\\nmoss$ ', end='', flush=True)\ntime.sleep(30)\n"
    script += r"""
print('\nMOSS_BUSYBOX_READY\nmoss$ ', end='', flush=True)
assert input().startswith("sh -c ")
print('\nMOSS_NESTED_SHELL\nmoss$ ', end='', flush=True)
assert input() == 'echo MOSS_PRODUCTION_READY'
"""
    if mode == "late_panic":
        script += "signal.signal(signal.SIGTERM, lambda *_: (print('[P] late panic', flush=True), sys.exit(1)))\n"
    if mode == "echo_only":
        script += "print('echo MOSS_PRODUCTION_READY\\nmoss$ ', end='', flush=True)\n"
    elif mode != "exit":
        script += "print('\\nMOSS_PRODUCTION_READY\\nmoss$ ', end='', flush=True)\n"
    if mode != "exit":
        script += "assert input() == 'exit'\n"
        script += "print('moss-init: restarting shell\\nBusyBox built-in shell (ash)\\nmoss$ ', end='', flush=True)\n"
        script += "time.sleep(30)\n"
    monkeypatch.setattr(boot, "resolve_qemu", lambda _: "unused")
    monkeypatch.setattr(boot, "build_qemu_args", lambda *_a, **_kw: [sys.executable, "-c", script])
    popen = boot.subprocess.Popen

    def launch(args, **kwargs):
        if args[0] == "fake-gdb":
            probe = "print('MOSS_CONSOLE_FIRST_READ_PAUSED', flush=True)\n"
            if mode != "gdb_unverified":
                probe += "print('MOSS_CONSOLE_INPUT_QUEUED', flush=True)\n"
            probe += f"raise SystemExit({int(mode == 'gdb_failure')})\n"
            args = [sys.executable, "-c", probe]
        return popen(args, **kwargs)

    monkeypatch.setattr(boot.subprocess, "Popen", launch)
    result = boot.run(cfg, tmp_path / "run", timeout=1, gdb="fake-gdb" if mode.startswith("gdb_") else None)
    assert result["status"] == ("passed" if mode in ("complete", "gdb_complete") else "error")
    assert result["raw_exit"] is not None
    if mode == "echo_only":
        assert result["completed_steps"] == 10
    if mode == "legacy_shell":
        assert result["completed_steps"] == 1
    if mode == "applets_failed":
        assert result["completed_steps"] == 6
    if mode == "late_panic":
        assert "panicked" in result["observed"]
    if mode in ("gdb_unverified", "gdb_failure"):
        assert result["observed"] == "first-read input barrier not verified"
