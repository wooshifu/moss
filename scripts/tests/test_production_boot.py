import sys
import time

import pytest

from scripts import check_production_boot as boot
from scripts.artifacts import Artifacts


@pytest.mark.parametrize(
    "mode",
    [
        "complete",
        "missing_bridge",
        "legacy_shell",
        "applets_failed",
        "echo_only",
        "exit",
        "late_panic",
        "sleep_runtime_failure",
        "old_child_survived",
        "pending_call_not_released",
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
    script += (
        "print('moss-init: supervisor ready\\nmoss-init: code authority service started pid=41\\n"
        "moss-init: file service started pid=42\\n"
        "moss-init: namespace service started pid=43\\n"
        "moss-init: loader service started pid=44\\n"
        "moss-init: pipe service started pid=45\\n"
        "moss-init: console service started pid=46\\n"
        "moss-init: process service started pid=47"
        + ("" if mode == "missing_bridge" else "\\nmoss-init: loader process bridge ready")
        + "', flush=True)\n"
    )
    if mode != "legacy_shell":
        script += "print('BusyBox built-in shell (ash)', flush=True)\n"
    script += r"""
print('moss$ ', end='', flush=True)
assert input() == '/moss-process.elf probe'
print('\nMOSS_PROCESS_READY\nmoss$ ', end='', flush=True)
assert input() == '/moss-file.elf read /missing'
print('\nMOSS_FILE_ERROR\nmoss$ ', end='', flush=True)
assert input() == '/moss-file.elf read /loader-probe'
print('\nMOSS_FILE_ERROR\nmoss$ ', end='', flush=True)
assert input() == '/moss-file.elf write hijack /loader-probe'
print('\nMOSS_FILE_WRITE_OK\nmoss$ ', end='', flush=True)
assert input() == '/moss-file.elf read /loader-probe'
print('\nMOSS_FILE_READ=hijack\nmoss$ ', end='', flush=True)
assert input() == '/moss-file.elf write native'
print('\nMOSS_FILE_WRITE_OK\nmoss$ ', end='', flush=True)
assert input() == '/moss-file.elf size'
print('\nMOSS_FILE_SIZE=6\nmoss$ ', end='', flush=True)
assert input() == '/moss-file.elf read'
print('\nMOSS_FILE_READ=native\nmoss$ ', end='', flush=True)
assert input() == '/moss-file.elf write separate /note'
print('\nMOSS_FILE_WRITE_OK\nmoss$ ', end='', flush=True)
assert input() == '/moss-file.elf read /note'
print('\nMOSS_FILE_READ=separate\nmoss$ ', end='', flush=True)
assert input() == '/moss-file.elf read /../note'
print('\nMOSS_FILE_READ=separate\nmoss$ ', end='', flush=True)
assert input() == '/moss-file.elf read /note/../scratch'
print('\nMOSS_FILE_ERROR\nmoss$ ', end='', flush=True)
assert input() == '/moss-file.elf read'
print('\nMOSS_FILE_READ=native\nmoss$ ', end='', flush=True)
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
        script += "assert input() == 'sleep 1 &'\n"
        if mode == "sleep_runtime_failure":
            script += "print('mlibc: fatal runtime error', flush=True)\n"
        script += "print('moss$ ', end='', flush=True)\n"
        script += "assert input() == 'exit'\n"
        script += "print('moss-init: restarting shell\\nBusyBox built-in shell (ash)\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == 'sleep 2 && echo MOSS_SLEEP_READY'\n"
        script += "print('\\nMOSS_SLEEP_READY\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-domain.elf terminate code'\n"
        script += (
            "print('moss-init: code authority service died\\n"
            "moss-init: code authority service started pid=48\\n"
            "moss-init: loader service started pid=49\\n"
            "moss-init: restarting shell\\nBusyBox built-in shell (ash)\\nmoss$ ', end='', flush=True)\n"
        )
        script += "assert input() == '/moss-file.elf read'\n"
        script += "print('\\nMOSS_FILE_READ=native\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-domain.elf terminate loader'\n"
        script += (
            "print('moss-init: loader service died\\n"
            "moss-init: loader service started pid=50\\n"
            "moss-init: restarting shell\\nBusyBox built-in shell (ash)\\nmoss$ ', end='', flush=True)\n"
        )
        script += "assert input() == '/moss-file.elf read'\n"
        script += "print('\\nMOSS_FILE_READ=native\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-process.elf crash-survivor &'\n"
        script += "print('MOSS_OLD_CHILD_STARTED\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-domain.elf arm process'\n"
        script += "print('moss-init: pending process call armed\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-domain.elf terminate process'\n"
        script += (
            "print('moss-init: process service died\\n"
            + ("" if mode == "pending_call_not_released" else "moss-init: pending process call released\\n")
            + "moss-init: pipe service started pid=51\\n"
            "moss-init: console service started pid=52\\n"
            "moss-init: process service started pid=53\\n"
            "moss-init: loader process bridge ready', flush=True)\n"
        )
        script += "print('BusyBox built-in shell (ash)\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == 'sleep 3'\n"
        if mode == "old_child_survived":
            script += "print('MOSS_OLD_CHILD_SURVIVED', flush=True)\n"
        script += "print('moss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-process.elf probe'\n"
        script += "print('\\nMOSS_PROCESS_READY\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-domain.elf terminate namespace'\n"
        script += (
            "print('moss-init: namespace service died\\nmoss-init: namespace service started pid=54\\n"
            "moss-init: loader service started pid=55\\n"
            "moss-init: pipe service started pid=56\\nmoss-init: console service started pid=57\\n"
            "moss-init: process service started pid=58\\n"
            "moss-init: loader process bridge ready', flush=True)\n"
        )
        script += "print('BusyBox built-in shell (ash)\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf read'\n"
        script += "print('\\nMOSS_FILE_READ=native\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf read /note'\n"
        script += "print('\\nMOSS_FILE_READ=separate\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-domain.elf terminate file'\n"
        script += (
            "print('moss-init: file service died\\nmoss-init: file service started pid=59\\n"
            "moss-init: namespace service started pid=60\\n"
            "moss-init: loader service started pid=61\\n"
            "moss-init: pipe service started pid=62\\nmoss-init: console service started pid=63\\n"
            "moss-init: process service started pid=64\\n"
            "moss-init: loader process bridge ready', flush=True)\n"
        )
        script += "print('BusyBox built-in shell (ash)\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf read'\n"
        script += "print('\\nMOSS_FILE_READ=\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf read /note'\n"
        script += "print('\\nMOSS_FILE_ERROR\\nmoss$ ', end='', flush=True)\n"
        script += 'assert input() == "/moss-file.elf write \\"$(printf \'%0300d\' 0)\\""\n'
        script += "print('\\nMOSS_FILE_WRITE_OK\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf read'\n"
        script += "print('\\nMOSS_FILE_READ=' + '0' * 300 + '\\nmoss$ ', end='', flush=True)\n"
        script += 'assert input() == "/moss-file.elf write \\"$(printf \'%04097d\' 0)\\" /note"\n'
        script += "print('\\nMOSS_FILE_WRITE_OK\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf read /note'\n"
        script += "print('\\nMOSS_FILE_READ=' + '0' * 4097 + '\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf write short /note'\n"
        script += "print('\\nMOSS_FILE_WRITE_OK\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf read /note'\n"
        script += "print('\\nMOSS_FILE_READ=short\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf resize 7 /note'\n"
        script += "print('\\nMOSS_FILE_RESIZE_OK\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf size /note'\n"
        script += "print('\\nMOSS_FILE_SIZE=7\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf read /note'\n"
        script += "print('\\nMOSS_FILE_READ=short\\0\\0\\nmoss$ ', end='', flush=True)\n"
        script += f"assert input() == '/moss-file.elf resize {boot.FILE_CONTENT_BUDGET_BYTES} /note'\n"
        script += "print('\\nMOSS_FILE_ERROR\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf read /note'\n"
        script += "print('\\nMOSS_FILE_READ=short\\0\\0\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf read'\n"
        script += "print('\\nMOSS_FILE_READ=' + '0' * 300 + '\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf append A /note'\n"
        script += "print('\\nMOSS_FILE_APPEND_OK\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf append B /note'\n"
        script += "print('\\nMOSS_FILE_APPEND_OK\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf read /note'\n"
        script += "print('\\nMOSS_FILE_READ=short\\0\\0AB\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf size /note'\n"
        script += "print('\\nMOSS_FILE_SIZE=9\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-process.elf fd-probe'\n"
        script += "print('\\nMOSS_FD_READY\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-process.elf pipe-probe'\n"
        script += "print('\\nMOSS_PIPE_READY\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-process.elf fd-pipe-probe'\n"
        script += "print('\\nMOSS_FD_PIPE_READY\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-process.elf console-fd-probe'\n"
        script += "print('MOSS_CONSOLE_OBJECT_WRITE\\n\\nMOSS_CONSOLE_READY\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-domain.elf terminate pipe'\n"
        script += (
            "print('moss-init: pipe service died\\nmoss-init: pipe service started pid=65\\n"
            "moss-init: console service started pid=66\\n"
            "moss-init: process service started pid=67\\n"
            "moss-init: loader process bridge ready\\nBusyBox built-in shell (ash)\\nmoss$ ', end='', flush=True)\n"
        )
        script += "assert input() == '/moss-process.elf fd-pipe-probe'\n"
        script += "print('\\nMOSS_FD_PIPE_READY\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-domain.elf terminate console'\n"
        script += (
            "print('moss-init: console service died\\nmoss-init: pipe service started pid=68\\n"
            "moss-init: console service started pid=69\\n"
            "moss-init: process service started pid=70\\n"
            "moss-init: loader process bridge ready\\nBusyBox built-in shell (ash)\\nmoss$ ', end='', flush=True)\n"
        )
        script += "assert input() == '/moss-process.elf console-fd-probe'\n"
        script += "print('MOSS_CONSOLE_OBJECT_WRITE\\n\\nMOSS_CONSOLE_READY\\nmoss$ ', end='', flush=True)\n"
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
    started = time.monotonic()
    result = boot.run(cfg, tmp_path / "run", timeout=4, gdb="fake-gdb" if mode.startswith("gdb_") else None)
    assert 0 <= result["elapsed_seconds"] <= time.monotonic() - started
    assert result["status"] == ("passed" if mode in ("complete", "gdb_complete") else "error")
    assert result["raw_exit"] is not None
    # Partial fake shells stop at their first missing marker; prompts and
    # command outputs each count as one completed probe step.
    if mode == "echo_only":
        assert result["completed_steps"] == 44
    if mode == "legacy_shell":
        assert result["completed_steps"] == 9
    if mode == "missing_bridge":
        assert result["completed_steps"] == 8
    if mode == "applets_failed":
        assert result["completed_steps"] == 40
    if mode == "late_panic":
        assert "panicked" in result["observed"]
    if mode == "sleep_runtime_failure":
        assert "fatal runtime error" in result["observed"]
    if mode in ("gdb_unverified", "gdb_failure"):
        assert result["observed"] == "first-read input barrier not verified"
