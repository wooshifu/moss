# Raspberry Pi 3B+ serial console and JTAG deployment

`scripts/rpi.py` runs with uv on macOS/Linux; validation reuses the project's
Pydantic/Typer-based kernel protocol checker.
It provides an interactive 115200 8N1 console and loads the production ARM64
Image/initramfs into RAM through OpenOCD. It does not modify the SD card.

The board must already boot 64-bit firmware with `enable_jtag_gpio=1`,
`enable_uart=1`, and `dtoverlay=disable-bt`. Connect the UART adapter's RX to
physical pin 8, TX to pin 10, and GND to pin 6. Leave adapter VCC disconnected.
The adapter must use 3.3 V logic. JTAG uses the standard ARM 20-pin interface:

| Signal | ARM 20-pin | Pi physical pin |
| --- | ---: | ---: |
| VTref | 1 | 1 (3.3 V) |
| GND | 4 | 6 |
| nTRST | 3 | 15 |
| TDI | 5 | 37 |
| TMS | 7 | 13 |
| TCK | 9 | 22 |
| RTCK | 11 | 16 |
| TDO | 13 | 18 |

Leave JTAG pin 19 (5 V output) and pin 15 (system reset) disconnected.
This tool uses JTAG logic reset only, without cycling board power.

Host prerequisites are `uv`, OpenOCD with J-Link support, LLVM Clang/objcopy/nm,
LLD, and `fdtget`/`fdtput` from dtc. Build the normal generic kernel:

```sh
uv run cmake --preset arm64-debug -DMOSS_BUILD_TESTS=OFF
uv run cmake --build --preset arm64-debug
```

Keep the JTAG server running in one terminal:

```sh
uv run scripts/rpi.py jtag
# Select a specific adapter when needed:
# uv run scripts/rpi.py jtag --jlink 50120677
```

It retries failed attachment during its initial 30-second window, halts CPU0,
and exposes each core separately at `127.0.0.1:3333` through `3336` (CPU0 through
CPU3). The secondary cores keep running in their firmware wait loops. The current
old U-Boot restarts during USB enumeration; attaching early pauses it before that
failure.

Deployment requires a **real firmware-generated DTB from this board**, including
its RAM allocation and CPU release addresses. The captured tree for the current
hardware session is `build/rpi3b/firmware.dtb`. It is an ignored local artifact,
not supplied by a fresh checkout. The QEMU DTBs in `scripts/qemu/` are synthetic
fixtures and must not be substituted. To capture a new tree, use the address and
size from the firmware UART message `Device tree loaded to ... (size ...)` with
OpenOCD's `dump_image` while U-Boot is halted.

In another terminal, deploy and check real shell input automatically:

```sh
uv run scripts/rpi.py deploy --dtb build/rpi3b/firmware.dtb --jtag-khz 1000 --check
```

Deployment prints five stages, with separate upload and readback percentages for
each file, transferred bytes, speed, elapsed time and estimated time remaining.
Transfers update after each completed 256 KiB block. A terminal refreshes the
current progress line; redirected output keeps each update on its own line.
`Verified` appears only after the complete readback matches the source bytes.
The default transfer rate is the conservative 1 MHz attach rate. Use
`--jtag-khz` to select a rate checked on the actual probe/wiring/board;
each deployment still verifies all bytes, and failed readbacks are retained.

The loader requires a halted U-Boot CPU0 in AArch64 EL2h. Its small handoff stub
cleans and invalidates caches, disables EL2 translation/caches, and stops at a
host breakpoint before any image memory is overwritten. The loader checks the
Image header and RAM layout, prepares `/chosen` with the initramfs and serial
console, and reads back every uploaded byte before entering the Image with the
DTB in `x0`. CPU secondary startup uses the firmware spin tables in that tree.

`--check` waits up to 90 seconds after entry for `moss$ `, sends an `echo` command,
and requires its output and the next prompt. Exit status zero proves shell input
and response for this run. `build/rpi3b/result.json` records image hashes and the
result; `serial.log` contains received bytes. This smoke check does not establish
complete device support or stability.

## Physical kernel suite validation

Build with `MOSS_BUILD_TESTS=ON`, then start `jtag` with CPU0 halted in U-Boot.
Use a new output directory for every run:

```sh
uv run cmake --preset arm64-debug -DMOSS_BUILD_TESTS=ON
uv run cmake --build --preset arm64-debug
uv run scripts/rpi.py validate \
  --manifest build/arm64-debug/moss-artifacts.json \
  --dtb build/rpi3b/firmware.dtb --jtag-khz 1000 \
  --workload users.frame --output build/rpi3b/frame-run-1
```

The runner snapshots matching validation Image/initramfs/symbols and their
provenance into a relative-path manifest. It adds validation boot arguments,
opens UART before resume, discards prior-run bytes while CPU0 is halted and
uses the same cache-off/full-readback deployment. It requires all four cores
to execute work and derives expected memory from the actual firmware DTB.

`validation-result.json` records strict ready/catalog/worker/case/end checks;
`result.json` records uploaded hashes and `serial.log` retains raw bytes.
Textual PASS is insufficient. A protocol violation, panic, timeout or failed
assertion returns nonzero, with later cases marked `not_run`. Deployment
errors are also retained. Boot/total deadlines default to 30/60 seconds and
each case to 30 seconds, measured after transfer; these are host watchdogs,
not performance budgets. Reset and attach again before the next suite.

The supported suites are `mm`, `scheduler`, `users.frame`, `users.uaccess` and
`users.ipc`. Other suites requiring serial input injection, lifecycle release
or benchmark calibration are not advertised by this runner. Physical suite
validation is distinct from QEMU `--machine raspi3b` and from shell `--check`.
Each completed feature additionally needs the corresponding real-board
consumer/failure checks under [ADR-0036](adr/0036-develop-a-general-purpose-hybrid-kernel.md).

For interactive input, omit `--check` in a terminal, or connect to the running
kernel afterward:

```sh
uv run scripts/rpi.py console
uv run scripts/rpi.py console --send 'echo hello' --expect hello --timeout 10
```

Use `--port /dev/cu.usbserial-14440` when multiple serial adapters are connected.
Ctrl-] exits the host terminal; Ctrl-C and other keys go to the Pi. The script
restores host terminal/UART settings when it exits. Logs append across sessions.

The loader deploys through CPU0 and requires the firmware boot state. To repeat
a deployment while Moss is running, reset through the existing JTAG server:

```sh
uv run scripts/rpi.py reset
```

This arms the board's watchdog and stops OpenOCD, whose cached state becomes
invalid across reset. Restart `jtag` in one terminal, then run `deploy --dtb ...`
in the other. No physical power cycle or SD write is needed. The reset discards
Moss's RAM filesystem. Power loss also removes the uploaded Moss image;
persistent SD deployment remains a separate operation.

Validated on the connected Pi 3B+ on 2026-10-16: two full deployments passed,
including a script-triggered reset and fresh attach. All four cores came online,
userspace services and BusyBox shell started, and `deploy --check` verified command output.
The separate console also wrote and read a RAM file. BusyBox `mkdir -p` emitted
mlibc's missing `Umask` sysdep diagnostics during that check; this is not a claim
of complete userspace compatibility or long-term stability. The 18 host tests
and QEMU Pi 3 production boot/service recovery check passed as well.

For the Release hardware measurements, kernel/userspace findings, optimization
order and repeatable probe commands, see [Pi 3B+ performance analysis](rpi3b-performance.md).
