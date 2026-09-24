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
        "sleep_runtime_failure",
        "old_child_survived",
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
        "print('moss-init: supervisor ready\\nmoss-init: file service started pid=42\\n"
        "moss-init: namespace service started pid=43\\n"
        "moss-init: process service started pid=44', flush=True)\n"
    )
    if mode != "legacy_shell":
        script += "print('BusyBox built-in shell (ash)', flush=True)\n"
    script += r"""
print('moss$ ', end='', flush=True)
assert input() == '/moss-process.elf probe'
print('\nMOSS_PROCESS_READY\nmoss$ ', end='', flush=True)
assert input() == '/moss-file.elf read /missing'
print('\nMOSS_FILE_ERROR\nmoss$ ', end='', flush=True)
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
        script += "assert input() == '/moss-file.elf read'\n"
        script += "print('\\nMOSS_FILE_READ=native\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-process.elf crash-survivor &'\n"
        script += "print('MOSS_OLD_CHILD_STARTED\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-domain.elf terminate process'\n"
        script += "print('moss-init: process service died\\nmoss-init: process service started pid=45', flush=True)\n"
        script += "print('BusyBox built-in shell (ash)\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == 'sleep 3'\n"
        if mode == "old_child_survived":
            script += "print('MOSS_OLD_CHILD_SURVIVED', flush=True)\n"
        script += "print('moss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-process.elf probe'\n"
        script += "print('\\nMOSS_PROCESS_READY\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-domain.elf terminate namespace'\n"
        script += (
            "print('moss-init: namespace service died\\nmoss-init: namespace service started pid=46\\n"
            "moss-init: process service started pid=47', flush=True)\n"
        )
        script += "print('BusyBox built-in shell (ash)\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf read'\n"
        script += "print('\\nMOSS_FILE_READ=native\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf read /note'\n"
        script += "print('\\nMOSS_FILE_READ=separate\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-domain.elf terminate file'\n"
        script += (
            "print('moss-init: file service died\\nmoss-init: file service started pid=48\\n"
            "moss-init: namespace service started pid=49\\n"
            "moss-init: process service started pid=50', flush=True)\n"
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
        script += "assert input() == '/moss-file.elf resize 65536 /note'\n"
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
        script += "assert input() == '/moss-file.elf resize 61440 /note'\n"
        script += "print('\\nMOSS_FILE_RESIZE_OK\\nmoss$ ', end='', flush=True)\n"
        script += 'assert input() == "/moss-file.elf append \\"$(printf \'%04096d\' 0)\\" /note"\n'
        script += "print('\\nMOSS_FILE_ERROR\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-file.elf size /note'\n"
        script += "print('\\nMOSS_FILE_SIZE=61440\\nmoss$ ', end='', flush=True)\n"
        script += "assert input() == '/moss-process.elf fd-probe'\n"
        script += "print('\\nMOSS_FD_READY\\nmoss$ ', end='', flush=True)\n"
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
        assert result["completed_steps"] == 31
    if mode == "legacy_shell":
        assert result["completed_steps"] == 4
    if mode == "applets_failed":
        assert result["completed_steps"] == 27
    if mode == "late_panic":
        assert "panicked" in result["observed"]
    if mode == "sleep_runtime_failure":
        assert "fatal runtime error" in result["observed"]
    if mode in ("gdb_unverified", "gdb_failure"):
        assert result["observed"] == "first-read input barrier not verified"
