# Native Windows validation

Validated on Windows with LLVM 23.1.3, CMake 4.4.3, QEMU 11.1.0,
uv 0.13.1 and Python 3.14. LLVM and QEMU were on `PATH`.

## Executed workflows

`uv run build.py --jobs 2 --verbose` executed all nine CMake workflows.
Every configure and build stage passed. The initial test stages failed;
after fixing Windows serial input, `moss-functional` was rerun and passed
for every preset:

| Architecture | Debug | Release | RelWithDebInfo |
| --- | --- | --- | --- |
| ARM64 | Build + functional passed | Build + functional passed | Build + functional passed |
| x64 | Build + functional passed | Build + functional passed | Build + functional passed |
| RISC-V 64 | Build + functional passed | Build + functional passed | Build + functional passed |

An additional complete ARM64 Debug workflow without GDB configured and
built successfully. CTest registered five suites; functional, applications,
framework and supervisor-reset passed. Production boot failed as described
below. The two optional GDB input probes were omitted.

## Remaining failures

The workflows are **not all green**. Production boot failed in ARM64's three
configurations, x64 Release, and RISC-V Release/RelWithDebInfo during the
initial run. The no-GDB ARM64 Debug run also reproduced this failure.
The guest process probe emits `MOSS_PROCESS_ERROR`, and the host eventually
reports a timeout waiting for its success marker.

A temporary instrumented ARM64 Release probe isolated a timing-sensitive
failure in `managed_fanout_probe`: all 20 children were enumerated, but some
were reaped with exit status 127. Adding per-child logging changed the
outcome. The exact cause is unresolved; no guest workload was weakened,
disabled or automatically retried to hide it.

## Host checks

- Full Python suite: 384 passed, 24 platform-dependent skips.
- After the final serial changes and two new socket tests, the affected
  protocol suite was rerun: 147 passed.
- Ruff, CMake formatting and whitespace checks passed.

Local detailed logs are under the ignored `build/` directory:
`windows-workflows.log`, `windows-functional-results.json`,
`windows-functional-recheck.log`, `windows-arm64-no-gdb-workflow.log`,
`windows-pytest-final.log` and `windows-protocol-recheck.log`.
The GitHub Actions Windows matrix retains failure reports as artifacts.
