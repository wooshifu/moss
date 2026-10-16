"""Drive the measurement collector through a real PTY, including failed guest runs."""

import json
import os
import pty
import select
import threading

import pytest

from scripts import rpi_perf


@pytest.mark.parametrize("failure", [None, "missing_sample", "duplicate_sample", "wrong_warmup", "guest_error"])
def test_measure_requires_complete_guest_results_and_success(tmp_path, monkeypatch, failure):
    master, slave = pty.openpty()
    monkeypatch.setattr(rpi_perf, "CASES", [("clock", 2, 1)])
    (tmp_path / "rpi-perf.elf").write_bytes(b"test probe identity")
    rows = [
        {"workload": "clock", "sample": i, "warmup": i < 2, "iterations": 2, "bytes": 1, "ns": 100, "switches": 0}
        for i in range(9)
    ]
    if failure == "missing_sample":
        rows.pop()
    elif failure == "duplicate_sample":
        rows[-1]["sample"] = rows[-2]["sample"]
    elif failure == "wrong_warmup":
        rows[0]["warmup"], rows[2]["warmup"] = False, True
    response = "\r\n" + "\r\n".join("RPI_PERF " + json.dumps(row) for row in rows)
    response += "\r\nRPI_PERF_DONE\r\nRPI_COMMAND_STATUS=" + ("1" if failure == "guest_error" else "0") + "\r\nmoss$ "
    sent = []

    def board():
        # Three seconds bounds a collector that failed to open/transmit to the PTY.
        if select.select([master], [], [], 3)[0]:
            sent.append(os.read(master, 4096))
            os.write(master, response.encode())

    thread = threading.Thread(target=board, daemon=True)
    thread.start()
    try:
        if failure:
            with pytest.raises((RuntimeError, ValueError)):
                rpi_perf.measure(os.ttyname(slave), tmp_path, None)
        else:
            rpi_perf.measure(os.ttyname(slave), tmp_path, None)
        thread.join(timeout=3)
        assert not thread.is_alive()
        assert sent == [b"/rpi-perf.elf clock 2 1; echo RPI_COMMAND_STATUS=$?\n"]
        result = json.loads(next(tmp_path.glob("measurements-*.json")).read_text())
        assert result["status"] == ("failed" if failure else "passed")
        assert next(tmp_path.glob("measurements-*.log")).read_bytes() == response.encode()
        if not failure:
            assert result["cases"][0]["median_ns"] == 50
            assert len(result["cases"][0]["samples"]) == 9
    finally:
        os.close(master)
        os.close(slave)


def pmu_lines(iterations=2):
    # A 1 MHz fixture clock gives exactly 20 ms for the calibration and 1 us per tick.
    lines = ['RPI_CALIBRATION {"frequency":1000000,"ticks":20000,"ns":20000000}']
    for sample in range(9):
        # Warmups intentionally contain outliers; only retained individual ticks define tails.
        ticks = [999] * iterations if sample < 2 else [1, 100][:iterations]
        lines.append("RPI_PMU " + json.dumps({"sample": sample, "counters": [10] * 7, "ticks": ticks}))
    return lines


def test_pmu_tails_use_individual_observations_and_exclude_warmups():
    result = rpi_perf.diagnostic_samples(pmu_lines(), 2)
    assert result["count"] == 14
    assert result["tail_ns"] == {"50": 1000, "95": 100000, "99": 100000, "100": 100000}
    assert result["counters_per_op"]["el0_instructions"] == 5
    assert len(result["samples"]) == 9


@pytest.mark.parametrize("failure", ["missing", "duplicate", "short", "negative", "wrap", "clock"])
def test_pmu_rejects_incomplete_or_invalid_measurements(failure):
    lines = pmu_lines()
    if failure == "missing":
        lines.pop()
    elif failure == "duplicate":
        lines[-1] = lines[-2]
    elif failure == "short":
        lines[-1] = lines[-1].replace("[1, 100]", "[1]")
    elif failure == "negative":
        lines[-1] = lines[-1].replace("[1, 100]", "[-1, 100]")
    elif failure == "wrap":
        lines[-1] = lines[-1].replace("[1, 100]", "[1000000, 100]")
    else:
        lines[0] = lines[0].replace("20000000", "10000000")
    with pytest.raises(ValueError):
        rpi_perf.diagnostic_samples(lines, 2)


@pytest.mark.parametrize("reading", ["0x4380006 0x6a9", "0x4380006 0x2a9", None])
def test_temperature_restores_cpu_after_success_invalid_sensor_or_debugger_error(monkeypatch, reading):
    commands = []

    class Debugger:
        def __enter__(self):
            return self

        def __exit__(self, *_):
            pass

        def command(self, command):
            commands.append(command)
            if command.startswith("read_memory"):
                if reading is None:
                    raise RuntimeError("read failed")
                return reading
            return ""

    monkeypatch.setattr(rpi_perf, "OpenOCD", Debugger)
    if reading == "0x4380006 0x6a9":
        assert rpi_perf.temperature()["millidegrees_c"] == 45622
    else:
        with pytest.raises((RuntimeError, ValueError)):
            rpi_perf.temperature()
    assert commands[-1] == "resume"


def test_probe_archive_replaces_services_once_and_preserves_other_entries():
    first = rpi_perf.make_cpio_entry("service.elf", b"old", ino=1, mode=0o100700)
    untouched = rpi_perf.make_cpio_entry("keep", b"unchanged", ino=2, mode=0o100444)
    archive = first + untouched + rpi_perf.make_cpio_trailer()
    expected = (
        rpi_perf.make_cpio_entry("service.elf", b"pinned", ino=1, mode=0o100700)
        + untouched
        + rpi_perf.make_cpio_entry("rpi-perf.elf", b"probe", ino=3)
        + rpi_perf.make_cpio_trailer()
    )
    assert rpi_perf.probe_archive(archive, b"probe", {b"service.elf": b"pinned"}) == expected
    with pytest.raises(ValueError, match="Missing services"):
        rpi_perf.probe_archive(untouched + rpi_perf.make_cpio_trailer(), b"probe", {b"service.elf": b"pinned"})
    with pytest.raises(ValueError, match="Duplicate service"):
        rpi_perf.probe_archive(first + archive, b"probe", {b"service.elf": b"pinned"})


@pytest.mark.parametrize("failure", [None, "missing", "wrong_cpu", "duplicate"])
def test_pinned_comparison_requires_one_correctly_placed_instance_of_every_service(failure):
    processes = [{"name": (service + ".elf")[:15], "cpu": 2} for service in rpi_perf.SERVICES]
    if failure == "missing":
        processes.pop()
    elif failure == "wrong_cpu":
        processes[0]["cpu"] = 0
    elif failure == "duplicate":
        processes.append(processes[0])
    lines = [*pmu_lines(), "RPI_PLACEMENT " + json.dumps({"processes": processes})]
    if failure:
        with pytest.raises(ValueError, match="Uncontrolled CPU placement"):
            rpi_perf.diagnostic_samples(lines, 2, 2)
    else:
        assert rpi_perf.diagnostic_samples(lines, 2, 2)["placement"]["processes"] == processes
