# Running Kernel Validation

The validation executable links the same production object modules and follows the same boot, memory, VFS, scheduler, userspace entry and exec paths as `moss.elf`. Its initramfs contains deterministic fixtures and real userspace validation programs. The framework is `ut_kernel`, from `unit_kenel`, not Unity.

The historical `containers.ipc_*` cases link `moss.ipc` only into the validation image. They exercise the old channel and service-manager algorithms, not the production IPC path. `users.ipc` and `moss-production-boot` exercise capability-backed control IPC and Memory Objects used by production.
`users.ipc/badged_sender` checks that the receiver gets the sender capability's badge, user-supplied badges are rejected, and attenuated or minted sender handles cannot mint new identities. Production boot exercises `/scratch` and a second native file with distinct badges, rejects an escaping path, checks a write across the shared-page boundary followed by a shorter replacement and zero-filled extension, rejects an over-budget resize without changing content, and checks that namespace restart preserves file contents while file-service restart discards the volatile second file. Ordinary shell file paths still use the kernel VFS.

The `drivers` registry cases likewise link the former `moss.drivers` matching/callback module only into validation. Production boot uses `moss.drivers.console` directly with the boot-owned interrupt controller and timer; the console readiness and RX cases still exercise that production module.

## Build and Test

Run from the repository root. Replace `arm64` with `x64` or `riscv64` for the other architectures.

```sh
uv run cmake --preset arm64-debug
uv run cmake --build --preset arm64-debug
uv run ctest --preset arm64-debug-test
```

CTest runs `moss-functional`, `moss-applications`, `moss-framework`, `moss-production-boot` and `moss-supervisor-reset`. The application test performs 10 full workflows, including a baseline and completion resource checkpoint; use `--iterations 1000` for the former extended routine profile. X64 also runs `moss-pvh-initrd`; ARM64 Debug runs `moss-console-input` and requires either `gdb-multiarch` or `gdb` at configure time; Release provides `moss-benchmark`. The `test-kernel` build target includes both functional and application tests; `benchmark-kernel` runs benchmarks in Release. The explicit `stability-kernel` target runs the minimum-30-minute core-path workload; it is not part of routine CTest. Full application stability uses the explicit command below. Use `kernel_validation.py run` for validation; the normal runner does not dispatch tests. Configure with `-DMOSS_BUILD_TESTS=OFF` to exclude validation images and validation userspace programs.

The default is QEMU TCG with four real online vCPUs and 2048 MiB RAM. CPU count is never silently clamped. The resource suite verifies work executed on every requested CPU and writes to owned memory beyond the old 256 MiB window. The present early mappings require RAM/device addresses below 4 GiB; the largest usable RAM size therefore depends on the firmware's physical layout. The default resource profile remains the baseline regression; additional machine/layout profiles verify image portability.

The `scheduler` workload requires at least two CPUs. Its enabled
`migration_current_owner` regression protects the previously observed window
where migration could take a still-executing thread before its context was
saved. It passes in the current nine-preset functional matrix; this bounded
regression does not prove every scheduler interleaving. See the retained
[diagnostic checkpoint](kernel-validation.md#scheduler-migration-diagnostic-checkpoint-2026-09-16).

`scheduler/ipc_priority_inheritance` uses the production run queues with
synthetic threads to check transitive RT and CFS nice donations, multiple
callers, receiver handoff, cycles, and restoration after call completion or
thread death. The receive path assigns a queued call to a waiting receiver
before waking it, then transfers the donation to the actual claimant.
`users.ipc` and production boot exercise that call lifecycle with ordinary
threads. They do not yet establish end-to-end inversion bounds under an
admitted real-time scheduling profile.

Each functional suite boots once, executes its cases sequentially, and stops after failure. Subsequent suites get fresh guests. Panic and timeout self-checks each use their own guest. Five warmups and thirty recorded benchmark batches share one guest per scenario, not one boot per sample.

```sh
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --workload mm
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --selftest
uv run scripts/kernel_validation.py run --manifest build/arm64-release/moss-artifacts.json --benchmark --order 2 --output build/results/order2
```

Use the actual `moss-artifacts.json` path printed by your preset if using an overridden build directory. With no `--output`, each run creates a unique directory under `<build>/validation/`. Explicit output directories must not already exist. The terminal prints the canonical report path.

Each run copies its validation image, initramfs and optional DTB into `inputs/`.
New manifests also provide `validation_debug_symbols`: the companion ELF from
the same test-kernel link, frozen into `inputs/validation_debug_symbols/`.
Image/fixture copies must match their captured build provenance before any guest
starts. All guests use these copies, so a concurrent rebuild cannot mix artifacts
within one report. The report retains the frozen paths and content hashes,
including `provenance.symbols_sha256`. A declared symbol file must exist and match
the build record; rebuild after configuring the new manifest. Older manifests
remain usable with explicitly unavailable validation symbols. Release keeps ELF
symbols in the companion while the boot image stays stripped; its existing `-g0`
policy means source lines and typed task inspection are unavailable.

Before terminating a failed, still-live guest (including expected fatal
self-checks), the runner stops it through QMP and retains all vCPU registers and
`info roms` in `<workload>/diagnostics/qmp.json`. GDB adds per-vCPU backtraces,
instructions, stack memory and, when DWARF is usable, current task IDs, states
and wait objects. `results.json` embeds the capture status, errors, paths and
actual symbol relocation offset in each guest's `diagnostics` field. ARM64/RV64
offsets come from the exact loaded Image in `info roms`; x64 uses its ELF addresses.
No fixed machine load address is substituted.

Capture has an independent **10-second** budget outside case/guest deadlines and
reported workload duration. GDB defaults to `gdb-multiarch`, then `gdb` on PATH;
use `--gdb /path/to/gdb` for another target-capable version. If full DWARF loading
fails or hangs, the remaining budget permits an ELF-only `-readnever` attempt.
Scripts, debugger logs and completed JSON snapshots are retained. Missing GDB,
unusable symbols or capture errors produce partial diagnostics without changing
the original verdict. Successful runs do not attach GDB. Ctrl-C/SIGTERM skips
capture and reaps the guest; exited guests have no live snapshot. Each guest uses
private Unix sockets, so concurrent presets do not share debugger ports.

`--cpus`, `--memory-mib`, `--warmup`, `--samples`, `--iterations`, and `--order` are explicit overrides. `--machine`, `--cpu`, `--qemu` and `--dtb` select the runtime environment. `--expected-ram-mib` explicitly checks firmware-visible RAM when firmware reserves part of the installed RAM; it defaults to `--memory-mib` and is recorded separately. Ordinary host deadlines remain 30 s for startup, 60 s per guest, and 5 s per case except `pfa`, `users.signals`, `users.lifecycle` and the event benchmarks (30 s). The distinct `users.applications` workload has the progress-based budget below. An explicit `--case-timeout` overrides the case default, including a shorter value; `--guest-timeout` overrides the total guest budget, subject to the existing stability minimum. Reports retain `case_timeout_seconds`, its `case_timeout_kind` (`total` or `no_progress`), `guest_timeout_seconds` and each case's host-observed `elapsed_seconds`; these are not kernel microbenchmarks. Functional/framework CTest budgets remain 2100 s; the 10-cycle application test has a 150 s budget. Ctrl-C or SIGTERM finalizes partial reports and terminates/reaps QEMU; workloads not started are recorded as such.

## Pipe Regression

The `users` suite also exercises blocking pipes through real syscalls: a delayed
writer must not produce premature EOF, a delayed reader must release a full
pipe's blocked writer, and CPU 0/CPU 1 exchange 256 request/reply records. The
child sleeps and checks its reported CPU assignment after setting affinity;
a successful affinity syscall alone is not placement evidence. The
`users.uaccess` partial-copy cases close the writer before checking pipe EOF;
they still verify fault handling and the exact bytes transferred.

`users.signals` covers `SIGPIPE`/`EPIPE`, caught-signal interruption of empty
reads and full writes, ignored/blocked signals that must not interrupt I/O,
positive partial-write counts, and wakeup respecting CPU affinity. The signal
sender and interrupted parent check their CPU 1/CPU 0 assignments through
`topinfo`. In the validation image, the sender observes the parent's actual
blocked read/write and FD after sleep handoff, not a guessed delay; the fixture
deliberately includes 30 ms of parent preparation. A separate pipe acknowledges
caught-signal interruption before the sender releases data I/O. The observer
does not treat preparation nanosleep as pipe readiness and is absent from the
production image. These are focused regressions, not complete pipe, job-control,
immediate-migration or deterministic SMP acceptance.

The `users.signals/pid_lifecycle` case creates 300 sequential children, crossing
both the 255-user-ASID capacity and the former 256-entry signal-state boundary.
Each child writes a distinct nonzero pattern at the same user virtual address,
alternates affinity between CPU 0 and CPU 1, verifies its reported CPU after a
sleep/wakeup, and checks the pattern across eight bounded redispatches. The
parent's value at that address must remain zero. The eight yields add repeated
TLB use without turning this boundary regression into an unbounded stress test.

```sh
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --workload users.signals
```

`vfs.smp/shared_references` separately runs 1000 cross-table clone/close cycles,
then same-table close/reuse while a CPU 1 pipe reader or writer is actually
blocked. It requires the original operation's data, eventual EOF, a still usable
replacement FD and exact resource recovery. The write case also checks that the
active File remains allocated until its I/O finishes. These checks do not cover
every multi-step descriptor publication/rollback or duplication interleaving.

The same SMP case now forces competition for the last FD before namespace
mutation. Failed `open(O_TRUNC)` must preserve the original inode and data;
successful open and competing dup cannot both claim the slot. The ordinary
FD-boundary case covers pending-slot visibility, clone, cancellation and
recovery after path errors and real File-pool exhaustion. Pipe-pair publication
and atomic source/target duplication remain distinct obligations.

```sh
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --workload vfs.smp
```

## Production Boot

`moss-production-boot` freezes the production kernel and initramfs, boots four
vCPUs with 2 GiB, rejects an unknown namespace path, opens `/scratch` and
writes and reads through the userspace file service, executes
`/busybox.elf ash -c` through the real shell, requires a second shell command
after child reaping, exits the shell and verifies the same file survives its
restart, kills and restarts the namespace service while requiring the file
contents to survive, then kills the file service and requires new service
instances with empty volatile contents. A prompt or echoed input alone cannot pass.
It also writes and reads a 300-byte value through the shared Memory Object
data path, beyond the 256-byte control-message limit.
Panics, validation output, unexpected exit
and timeout fail the probe. Image hashes and serial/QEMU logs are retained in
`<build>/production-boot/run-*/guest/` with `results.json`.

`moss-supervisor-reset` checks that the production Initial System Supervisor's
death resets the platform. It kills PID 1 after the shell prompt, then boots a
valid archive without `/init.elf` to force an early exec failure. Both cases
require the kernel diagnostic and an actual QEMU reset exit with `-no-reboot`.
Frozen images, serial logs and results are under `<build>/supervisor-reset/run-*/`.
The x64 path supports the ACPI FADT 8-bit System I/O reset register; ARM64 uses
PSCI and RV64 uses SBI SRST.

```sh
uv run scripts/check_production_boot.py --manifest build/arm64-debug/moss-artifacts.json
uv run scripts/check_supervisor_reset.py --manifest build/arm64-debug/moss-artifacts.json
uv run scripts/check_production_boot.py --manifest build/arm64-debug/moss-artifacts.json --gdb gdb-multiarch
uv run scripts/check_production_boot.py --manifest build/arm64-debug/moss-artifacts.json --gdb gdb-multiarch --registration-race
```

The optional GDB probe supports ARM64 Debug on the fixed QEMU virt platform.
Pass either `gdb-multiarch` or a native `gdb` with the required remote-target
support; CTest resolves those names in that order and fails configuration if
neither exists instead of silently omitting the controlled probe.
It freezes matching production symbols, stops at the first VFS console read
after the prompt, injects the first command and verifies its arrival through
UARTFR before resuming. It does not consume FIFO bytes or patch kernel state.
The same shell workflow must finish; missing barrier evidence, debugger failure
or the unchanged 30-second timeout fails the run. The generated GDB script and
debugger log accompany the ordinary serial log and report. This checks console
readiness, not general concurrent-reader or scheduler handoff correctness.

The `--registration-race` probe stops inside `console::getc_blocking()` after
its empty-ring check and before waiter registration, while the event lock is
held. It confirms a real PL011 byte is queued in UARTFR before resuming, then
requires the same shell workflow to finish. This covers RX arrival in that
window on ARM64 Debug virt; it does not force the IRQ handler itself to run
before waiter registration or cover multiple readers.

## x64 PVH Initrd Contract

`moss-pvh-initrd` freezes and hashes the production x64 kernel/initramfs, then
boots the same kernel with two initramfs sizes and two RAM layouts. It checks
that the logged interval matches the real file and moves when QEMU changes the
PVH module placement. Missing and zero-length module descriptors, malformed
newc data, and a valid archive without `/validation.elf` or `/busybox.elf` must
produce their exact errors before the boot-completed marker. The zero-length
case uses QEMU's GDB remote protocol only to alter the bootloader-owned module
descriptor at the real ELF entry; it does not patch kernel code or require an
external GDB executable. This follows the [Xen PVH boot
ABI](https://xenbits.xenproject.org/docs/unstable/misc/pvh.html).

```sh
uv run scripts/check_x64_pvh_initrd.py --manifest build/x64-debug/moss-artifacts.json
```

Per-case serial/QEMU logs and the aggregate hash/range verdict are retained in
`<build>/pvh-initrd/<timestamp>/`.

## Core-Path Recovery, Stability and IRQ Return Probes

`users.lifecycle/core_paths_recovery` runs one warmup plus 1,000 complete cycles
in one guest. Each cycle maps a private page, forks and verifies COW isolation,
delivers and returns from a signal handler, sleeps through a real timer wakeup,
transfers and checks pipe bytes, reads an inherited descriptor, executes a new
image, exits, waits/reaps, checks pipe EOF and releases all parent resources.
The warmup deliberately reads final pipe EOF before `waitpid`, proving that exit
closes the child's last inherited writer before the Zombie can be reaped. The
1,000 measured cycles retain wait-before-EOF ordering: repeating the blocking
EOF-first handoff would primarily add scheduler wake/block traffic to the
resource-recovery workload rather than strengthen the ownership assertion.
Every 100 cycles it verifies heap, physical pages, process/thread counts,
parent descriptors, their file references, user/stack pages and VFS inode/dentry/file
pool occupancy against the warm baseline. The
`checkpoints` array in `results.json` retains the raw resource records. Missing
or unequal recovery evidence cannot pass.

`users.applications/core_application_recovery` reuses that complete core cycle
and then forks, execs and reaps a new BusyBox ash running all nine selected applets.
It verifies the uname-derived `HOSTNAME=moss`, exact captured stdout, pipeline
results, directory/file cleanup and exit status. An independent counter advances
only after a successful application workflow; `application_cycles` must match
the core count. One warmup is excluded from both counts. Every **10** completed
cycles records exact resource recovery, giving a baseline and completion checkpoint
for CTest's 10-cycle run (or **101 checkpoints** with `--iterations 1000`). The original core-only
workload still records every 100 cycles and reports zero application cycles.

This is a separate routine CTest (`moss-applications`), not a longer deadline for
the original core case. It passes `--iterations 10`, so its 30 s **no-progress**
watchdog observes the baseline and completion checkpoint and its total budget is
90 s, with another 60 s for CTest cleanup/reporting. Direct invocation keeps the
1,000-cycle default unless `--iterations` supplies another checkpoint-aligned
count. A short explicit total budget still terminates a progressing guest. Direct CLI invocation without
`--workload` runs the original functional catalog; use the application selector
below or the full CTest preset to include repeated applications.

The pinned ash profile enables upstream `ASH_BASH_COMPAT`, which contains
pipefail. Scripts must successfully enable it; the text-pipeline probe additionally
requires an upstream exit 7 to propagate through a successful downstream cat.
Enabling this bundled upstream option does not establish support for every bash
extension. The workflow changes directories and exercises relative paths;
getcwd, Uname and the selected terminal/stdio adapter diagnostics are resolved.
These tests check stdout and status, not a clean-stderr contract or complete
POSIX behavior. Earlier failed runs remain evidence, not the latest source's
acceptance status. See the design document's latest evidence and remaining limits.

The static-libc descriptor check also enters the real native read/write/close
syscalls with out-of-range full-width FDs, including positive and negative
values sharing a live descriptor's low 32 bits. Rejection must preserve the
read output, file contents and offset, and the original descriptor itself;
direct FD-table tests alone do not cover syscall argument narrowing.

`users.libc/static_runtime` checks native/real-libc working-directory behavior,
including failed changes, buffer/pointer/path boundaries, fork and relative
exec, cross-CPU ancestor rename, and directories removed while retained.
`vfs/working_directory_lifecycle` repeats directory-reference recovery 1000 times
and checks exact root references, pool occupancy and heap recovery. Paths remain
bounded by the kernel's 1024-byte buffer. Symlinks, dirfd-relative APIs and dynamic
mount behavior are not established by these checks.

`users.libc/filesystem_permissions` uses the validation-only control hook to
drop a child to UID/GID 99. It checks denied file open/truncate, path search,
create/delete/rename and exec through the real static-libc syscall path, unchanged
data and stat output after rejection, allowed namespace changes in a writable
directory, creation ownership, inherited descriptors, and fork/exec credentials.
`vfs/access_permissions` repeats owner/group/other precedence and rejected
namespace mutations for 64 lifecycles with exact pool/heap recovery. These checks
do not add a production identity-management API, supplementary groups or set-ID
executables, nor establish complete POSIX permissions.

The `users.timers/cancel_in_flight` fixture waits until its CPU 1/CPU 2 actors
have masked local IRQs before arming the real periodic timer. The callback
must execute off those actor CPUs, so it cannot preempt a participant whose
progress it deliberately waits for. The real cancellation and rejected-restart
assertions, observation window and five-second host deadline are unchanged.

`timers/dispatch` checks ordinary setup and delayed registration of the second
timer. Its unchanged 100 ms dispatch observation window begins only after both
timers are armed; setup delay cannot consume it. Both phases still require
one-shot and periodic callbacks at or after their deadlines, minimum callback
counts, and no callbacks after cancellation.

`--stability` accepts one core or application lifecycle workload and requires both at least 10,000 completed
cycles and 30 minutes in the same guest. It continues cycling after 10,000 until
the duration is reached. Both guest elapsed time and host-observed case duration
must reach 30 minutes. The host sends a completion permit over the guest's serial
input only after its own 30-minute case deadline and 10,000 reported cycles.
The guest polls that permit through the shared nonblocking console RX owner,
not directly from a UART whose interrupt handler may already have drained it.
The guest continues full cycles until it receives that permit and meets its own
duration gate; calibrated clock drift cannot cause an early successful stop.
Reports record `stability_release_seconds`. The default 30-second case budget becomes a no-progress
deadline, refreshed only by ordered checkpoints with advancing guest time.
The overall guest budget is at least 30 minutes + startup budget + 60 seconds.
Core-only stability remains the default. Full-profile stability must select
`users.applications`; its interval-derived default total budget is **30060 s**,
because completing 10000 actual workflows may take longer than 30 minutes.
Neither the minimum duration nor the cycle/resource requirements are reduced.
The required stage matrix is all three architectures in both Debug and Release;
one passing configuration does not establish that matrix or concurrency closure.

```sh
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --workload users.lifecycle
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --workload users.applications
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --stability
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --stability --workload users.applications
uv run scripts/check_riscv64_dispatch.py --manifest build/arm64-debug/moss-artifacts.json --symbols build/arm64-debug/bin/moss.test.elf --gdb gdb-multiarch --runs 3
```

Despite its historical filename, the IRQ probe supports ARM64 exception return
after waitpid and RV64 initial user-stack publication. Both require Debug symbols
and a target-capable GDB. ARM64 executes the kernel's existing timer-compare MSR
under debugger single-step to make a real IRQ pending, restores scratch PC/registers,
then verifies IRQ delivery is deferred until EL0. The complete signal suite must
also pass. RV64 uses the existing pending-software-IRQ probe plus `users.vm`.
Frozen images, symbol hashes, GDB/serial/QEMU logs and results are retained under
`<build>/dispatch-irq/`; failure is not replaced by a later retry. GDB 10.2 works
for the ARM64 probe with ELF-only symbols but rejected the current RV64 QEMU
register description. A locally built GDB 17.2 with its matching data directory
completed three RV64 probe runs; supply that target-capable debugger via `--gdb`.
Debugger setup failures remain errors, not accepted kernel checks.

## Memory Ownership and Boot Inputs

The `heap` and `pfa` suites check real allocation/release and exhaustion. They
snapshot PFA metadata, the early table pool and current page-table trees; heap
writes must not change them, and PFA must never return their pages as free memory.
These are single-worker ownership checks, not concurrent allocator/COW acceptance.

Additional boot-input checks run independently of CMake configure/build:

```sh
uv run scripts/check_pfa_firmware.py --manifest build/riscv64-debug/moss-artifacts.json
uv run scripts/check_heap_layout.py --manifest build/arm64-debug/moss-artifacts.json
```

The firmware check requires QEMU and dtc's `fdtget`/`fdtput`. It reuses an unchanged
image with altered DTBs; ARM64 tests reserved regions, while RV64 also tests RAM
bank shape/order/capacity. The heap-layout check requires an existing Ninja build
and LLVM tools. It reuses that build's actual link command to create a disposable
image with an overlapping heap limit; original artifacts and sources are untouched.
Generated images, manifests, DTBs and reports remain under the supplied build directory.

Negative checks require the expected boot diagnostic and no `ready` event. Their
raw guest reports remain errors; the check script succeeds only when rejection is
verified. They do not count an unexecuted functional test as passed.

## Page Permission Boundaries

`mm.permissions` is part of the default functional run. It checks supervisor-only
table defaults, rejects USER attributes in the kernel mapping API, and walks the
production kernel identity/direct-map trees and the current process's hardware
page-table root. Kernel leaves must not carry USER; x86 user mappings must retain
USER throughout their permission chain. ARM64 also verifies the active TTBR1 root.
`kernel_wx` checks that executable kernel leaves are read-only and confined to
the linked text range (including boot text), all direct-map aliases are NX,
and no alias of text/rodata is writable. x86 also checks CR0.WP. The
`address_space_ownership` case creates, COW-clones and destroys real address
spaces, checking inherited kernel permissions, physical-page references,
page-count recovery and unchanged shared kernel tables.
`vma_boundaries` exercises production AddressSpace admission: complete user
address bounds, alignment, reversed ranges, permissions and the reserved
sigreturn page. Ordinary empty VMAs are rejected; the sole exception is the
fixed `HEAP_START` sentinel used for `brk == brk_base`. The case grows that VMA,
rejects an overlapping resize without invoking its mutation callback, shrinks
it back to empty, and rejects a second or displaced HEAP. It also checks access
across adjacent VMAs and rejection at a gap or incompatible permission.
It also rejects a writable executable VMA. `users.vm/access_permissions`
requires anonymous executable `mmap` requests to fail with EACCES, while
ordinary non-executable mappings remain usable. This closes the current
anonymous-memory execution path; code approval and immutable executable
Memory Objects remain separate work under
[ADR-0026](adr/0026-require-explicit-authority-for-executable-memory.md).

`users.vm/brk_lifecycle` enters through the real `brk`, `mmap`, `munmap`, fork,
fault and wait paths. It proves that the initial empty heap faults, a page fully
removed by a non-page-aligned shrink faults, and regrowth supplies a zero page.
It then places an anonymous mapping beyond a guard page and requires conflicting
heap growth to preserve the old break, mapping contents, and inaccessible gap.
Shrinking to the base and regrowing checks the first page again. A sub-page
shrink intentionally retains its containing hardware page; partial `munmap`
remains unsupported and must be rejected rather than partially committed.

Run the focused policy and lifecycle set with:

```sh
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json \
  --workload users.vm --workload mm.permissions --workload mm.transactions
```

This is structural U/S and W^X evidence for the present 0-4 GiB mapping contract,
not complete malicious-user fault containment or physical-hardware acceptance.
Ownership snapshots ignore only hardware Accessed/Dirty state; address, permission
and software ownership bits remain protected by the hash. Framework self-checks
verify this distinction without modifying an installed page table.

The `users.user_ranges` case uses actual architecture syscalls to reject invalid
debug pointers, kernel/overflow/reserved mmap ranges, unsupported flags and
read-only/PROT_NONE output. It creates two adjacent anonymous VMAs and exercises
copy-in, copy-out and pathname copying across their boundary, then removes one
VMA and requires EFAULT. Bounded strings without a NUL return ENAMETOOLONG instead
of silently using a truncated name. This follows the current Moss ABI:
clock_gettime writes one u64 nanosecond value; zero-count read/write return EINVAL.
These are policy checks, not recoverable CPU-fault tests: copy helpers and VFS
buffer accesses still need fault fixups and VM lifetime protection (MOSS-002).
The subsequent users case covers fork/exec/reaping; containers.smp covers CPU1.

## Static Runtime and Exec

The default functional selection includes `users.libc`, `users.exec` and
`users.busybox`. Run just these real user/kernel paths with:

```sh
uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json \
  --workload users.libc --workload users.exec --workload users.busybox
```

`users.exec` has 28 cases. It checks malformed entry/header/load rejection, file
and address wraparound, page-offset/alignment and reserved/overlap policy,
PT_INTERP/PT_DYNAMIC, orphan file-backed TLS, bad environment pointers, combined
argument/environment count and byte limits, exact accepted boundaries, and empty
vectors. Rejection cases use an argv marker that makes accidental exec observably
different from ENOEXEC. The accepted native limit is 128 strings and 16 KiB
including NULs; static mlibc validates the delivered arguments/environment.
`allocation_rollback` exhausts the real physical-page allocator once, then exposes
one additional page per root/stack/intermediate-table preparation stage. Each
failed syscall must return ENOMEM without replacing the old address space or
process name, changing the open file's position, or leaking heap/pages/files.

`mutable_snapshot_rollback` copies the real child ELF into writable ramfs and
separately exhausts the runtime heap at argument storage, mutable-image object,
shared control block, image bytes, address-space object, and a partially prepared
VMA list. Every stage preserves the old address-space identity/root hash/name,
stack canary, sequential file offset, and the complete lifecycle-resource baseline.
After pressure is released, the suite successfully fork/execs. `boundary_load_plan`
executes an independently constructed ELF with non-page-aligned RX/RW segments. It
checks file-prefix and cross-page bytes, BSS/page-tail zero fill, target code execution,
RW writes, RX write faults and RW execute faults. `users.libc` also
checks that successful exec retains an independent mutable-file snapshot, preserves
ordinary descriptors, and closes only descriptors marked close-on-exec. These
checks close MOSS-016 for the documented fixed-address static subset. They do not
cover multithreaded exec, dynamic linking/relocation, shared LOAD pages or kernel
TLS initialization; unsupported layouts are rejected instead of merged.

BusyBox tests execute the pinned real ash for exit status, command substitution
with exact captured stdout, environment export followed by external shell exec,
pipelines, redirection, directory lifecycle, copy/rename and the combined
application workflow. Repeated application acceptance is the separate
`users.applications` workload described above.

## User Floating-Point State

On x64, `users.fork_exec_exit_reap` also checks x87 data/control, MXCSR and
XMM15 across yield and fork, child state changes without parent contamination,
default state after exec, and real x87 invalid-operation termination followed by
parent continuation. Exec validates argc/argv. `containers.smp` additionally
checks inherited FP state on its actual CPU1 child and the surviving CPU0 parent.
These checks do not cover every extended register, signal frame or CPU feature.

The additional `users.simd_fault` workload requires a real unmasked SSE invalid
operation to terminate only the child. It is explicit, not part of the default
functional set, and a missing exception remains a failure:

```sh
uv run scripts/kernel_validation.py run --manifest build/x64-debug/moss-artifacts.json \
  --workload users.simd_fault
```

The current QEMU 11.1.1 TCG run fails this assertion: its CPU trace records #MF
but no #XM. QEMU's [upstream explanation](https://github.com/qemu/qemu/commit/418b0f93d12a1589d5031405de857844f32e9ccc)
distinguishes SSE status-flag emulation from trapping support. Do not replace the
arithmetic with a software interrupt or classify its early return as a pass.
SIMD-fault isolation remains unaccepted until exercised in a suitable environment.

## Container Ownership

The `containers` suite runs production `LockedList` and `LockedHashMap`: reachable
values survive insertion, unlink/replacement destroys each owned value once, and
a 1,024-node clear/reuse restores heap accounting. Lookup returns a value copy
(a retained `shared_ptr` for owned objects), not node storage. The `held_reader`
case keeps that owner across removal; `reentry` checks snapshot callbacks and
destructors accessing the same container after its lock is released. The fake
RCU reader/callback queue has been removed, not given a periodic drain.

`containers.smp` forks a real userspace child with inherited CPU1 affinity. CPU0
removes an object while CPU1 retains it, then barriers force two creators past
the same-key lookup and race insertion/removal. Assertions verify the actual CPU
IDs, a single published value, exactly one successful removal and final destruction.
Only CPU0 records assertions; the peer publishes results with acquire/release
handshakes. The parent reaps the child before destroying the fixture. A stuck
handshake fails the host case deadline; there are no timed sleeps or mock workers.

Both suites are in the default functional selection (nine suites, twenty-seven cases).
Run either alone with `--workload containers` or `--workload containers.smp`.
The SMP suite requires at least two CPUs; for `--cpus 1`, explicitly select
single-worker workloads. This is bounded container/SMP boot coverage, not full
scheduler, IRQ-context, driver or IPC lifecycle acceptance; MOSS-006 remains open.

## Single-Function Measurements

There are thirteen built-ins. The original five are `bench.allocate`, `bench.release`, `bench.combined`, `bench.read`, and `bench.getpid`. Allocation and release support orders 0 through 4. `bench.read` measures 256-byte reads from a real 64 KiB ramfs file. `bench.getpid` brackets real user-to-kernel-to-user calls from userspace, rather than calling a handler directly.

The eight additional version-1 scenarios use these fixed parameters and boundaries:

| Scenario | Measured operation and untimed checks |
| --- | --- |
| `bench.fault` | First byte write to each of up to 64 fresh 4 KiB anonymous pages. Mapping and absent-PTE validation precede timing; payload validation and unmapping follow it. Measures the faulting instruction and complete fault/return path, not exclusive handler time. |
| `bench.cow` | First write to each of up to 64 resident 4 KiB pages shared with a live forked child. Before timing, validate COW PTEs and at least two references; afterwards verify the parent's new bytes and child's unchanged bytes, reap and unmap. Fork/setup is not timed. |
| `bench.switch` | Real userspace `sched_yield` round trips, including syscall entry, scheduler and saved-context transfers away and back. A scheduler-counter check rejects a yield that does not switch. This is not the duration of one assembly context-switch routine. |
| `bench.wakeup` | Each real one-shot timer callback captures a counter immediately before the shared `task_wakeup` path; stop at the sleeping CPU-0 worker's first resumed counter read. The 1 ms pre-wakeup wait is excluded. |
| `bench.timer` | Lateness from each one-shot timer's 1 ms deadline to the callback's first counter read. The timer clock's exact rounded conversion is inverted to obtain the deadline counter; setup and the pre-deadline wait are excluded. |
| `bench.lifecycle` | Complete fork, child exec of `/validation_child.elf`, exit 37 and parent wait/reap, up to 64 cycles per batch. Child creation and teardown belong inside this measurement. |
| `bench.signal` | Individually bracket self `SIGUSR1` send to the userspace handler's first counter read. Verify exactly one handler call per send; handler return is excluded from the reported interval. |
| `bench.pipe` | One 1,024-byte write/read pair through real pipe syscalls. Check both transfer lengths, then verify final payload and close descriptors outside timing. Useful throughput is 1,024 bytes per pair, not double-counted; this is single-process transfer, not blocking producer/consumer throughput. |

Signal, switch and pipe batches have a 65,536-operation safety cap. Timer and
wakeup batches have a 64-event cap, so their summed measured latency need not
reach the pilot's approximate 1 ms target. New scenarios have a 30 s default
case deadline; explicit runner budgets still take precedence.

Raw batches identify `measurement_kind`: `event_sum` for signal, wakeup and timer,
`elapsed_batch` for the others. Event sums retain total event ticks and counts;
their reported values are means within each batch and a median across batches,
not per-event percentiles. Empty instrumentation costs are retained without
subtraction. User fixtures are warmed before exact pre/post resource accounting;
later batches must restore heap, physical pages, processes, threads, user/stack
pages, descriptors and File references. Timer fixtures also require exact
resource recovery and synchronous callback cancellation before stack teardown.

Add a callback in `src/test/validation.cpp` and register it during `moss_validation_boot`:

```cpp
bench::register_benchmark("bench.my_function", [](bench::Context& context) {
  // Create bounded, scenario-owned fixtures here, not during registration.
  context.measure_batches(
      [&](usize count) { return prepare_owned_inputs(count); },
      [&](usize index) { invoke_production_function(index); },
      [&](usize count) { return validate_and_release_owned_inputs(count); });
});
```

The three application-specific functions above are author-provided: prepare and cleanup return `bool`; the operation returns `void` and retains its result in the fixture for untimed validation. A failed prepare still invokes cleanup, so partially initialized fixtures must be safe to release. Cleanup failures invalidate the scenario. Refer to `allocation_benchmark` for a complete production example with bounded ownership and free-page accounting.

Add the stable scenario ID to the host `CATALOG` in `scripts/kernel_validation.py`. Similarly, add functional cases with `ut::register_test` inside an explicit `ut::register_suite`, then update that suite's host catalog. IDs and descriptor strings must have static lifetime and use ASCII letters/digits or `_-.=/`, at most 80 characters. Capacities are 160 cases, 32 suites and 16 benchmarks. Change the workload version when changing its definition or fixture semantics.

Counters are ordered and frequencies are validated: ARM64 CNTFRQ/CNTVCT, RISC-V 64 DTB timebase/time, and x86 CPUID.15 or three bounded PIT-channel-0 calibration samples. A bounded pilot selects an operation count, then all batches keep it fixed. The worker is pinned to CPU 0; other CPUs stay online and normal interrupts remain enabled during measured kernel operations.

Raw ticks and empty-loop/counter overhead are retained, without exact overhead subtraction. Reported medians summarize batch-average elapsed time per operation, including callees and residual loop/result-storage costs. They are not per-call latency percentiles, exclusive function CPU time, hardware CPU cycles, or native-hardware performance claims. Do not run other benchmark runners concurrently when collecting comparison data.

On Linux, `--host-cpus 8,9,10,11` additionally binds guest vCPUs 0–3 to those
distinct eligible host CPUs. This is separate from the kernel worker's CPU-0
affinity. The runner starts QEMU paused, uses its
[QMP control protocol](https://www.qemu.org/docs/master/interop/qmp-spec.html)
to discover vCPU thread IDs, verifies each belongs to its child QEMU process,
applies and reads back every affinity, then resumes the guest. Missing or failed
binding cannot silently launch an unpinned accepted measurement. Reports retain
the actual thread/CPU mappings; offline comparison validates that evidence.
Select CPUs appropriate to your host; this option does not isolate their SMT
siblings, reserve exclusive CPU time, or change the host frequency governor.
For pinned runs, the report also records each selected CPU's governor (or its
unavailability); different recorded governors are incompatible comparison
environments. Changing a governor is a separate, explicitly authorized host
operation, not an automatic runner side effect.

## Reports and Baselines

Every run retains `results.json`, per-case `junit.xml`, and original `<workload>/serial.log` and `qemu.log`. The JSON records build/compiler flags, revision, dirty state, image/fixture hashes, QEMU arguments, resources, clock calibration, raw batches, completion, host termination reason and raw child exit status. Expected fatal self-checks retain both the expected and observed outcome.

The validation build writes `<validation-image>.provenance.json` beside the image. Reports use that build-time record, not the current checkout's HEAD. An image/initramfs mismatch requires a rebuild; missing source records are marked `source_status: unrecorded`, not attributed to the current source. Keep source files stable while building: the source snapshot is recorded at link completion, not continuously during compilation.

```sh
uv run scripts/kernel_validation.py run --manifest build/arm64-release/moss-artifacts.json --benchmark --output build/results/baseline
uv run scripts/kernel_validation.py run --manifest build/arm64-release/moss-artifacts.json --benchmark --baseline build/results/baseline/results.json --output build/results/current
uv run scripts/kernel_validation.py compare build/results/baseline/results.json build/results/current/results.json
```

The baseline is read-only and explicit. Matching scenarios reuse its valid operation count. Offline comparison launches no QEMU, revalidates raw evidence, and refuses incomplete/invalid or incompatible measurements. Revision and image hashes can differ; workload version, parameters, fixtures, build policy and execution environment must match. The two-report `compare` command is informational; use the separate repeat-based gate below for a blocking decision.

### Repeated-Measurement Gate

`scripts/performance_gate.py` requires at least five independent baseline reports
from one exact source/image/fixture and three independent candidate reports from
one exact source/image/fixture. Each must complete the full current benchmark
catalog in Release. Collect them sequentially without competing validation or
benchmark jobs, using `run --baseline` to keep per-scenario operation counts fixed.
Repeated or relabelled raw measurements cannot manufacture independent runs.

Supply each path with repeated `--baseline` and `--current` arguments, plus a new
`--output` path. The gate retains the input paths and report hashes and never
overwrites prior results. Without `--max-noise`, it reports calibration values
but remains incomplete; no maximum acceptable noise has been silently chosen.
After reviewing repeated-run variability, explicitly supply an accepted maximum
run-to-run noise ratio with `--max-noise` in a separate output run.

The user confirmed **5% as the trial noise ceiling on 2026-09-15**. Use
`--max-noise 0.05`; the CLI still requires an explicit value for a gate decision.
This caps variability in both repeat groups, not the permitted slowdown.
For example, replay a complete pinned x64 Release campaign with a new output filename:

```sh
perf_evidence=build/performance-pinned.uoPXu1/x64-release
uv run scripts/performance_gate.py --max-noise 0.05 \
  --baseline "$perf_evidence/run-0/results.json" \
  --baseline "$perf_evidence/run-1/results.json" \
  --baseline "$perf_evidence/run-2/results.json" \
  --baseline "$perf_evidence/run-3/results.json" \
  --baseline "$perf_evidence/run-4/results.json" \
  --current "$perf_evidence/run-5/results.json" \
  --current "$perf_evidence/run-6/results.json" \
  --current "$perf_evidence/run-7/results.json" \
  --output "$perf_evidence/trial-noise-005-replay.json"
```

For each scenario, let `B` be its five-or-more baseline run medians. Observed noise
is `max(B) / min(B) - 1`; the empirical upper bound is
`max(B) * (1 + noise)`. All candidate run medians above that bound establish a
regression; all at or below it pass that scenario. Mixed results, incompatible
evidence, or baseline/candidate noise exceeding the explicit maximum remain
incomplete. This conservative empirical rule is not a statistical confidence
interval. The output records every run median, calibrated bound and decision.
Exit codes are `0` for all scenarios passed, `1` for a confirmed regression, and
`2` for incomplete evidence. A noise limit is not a fixed regression percentage;
the scenario regression bounds come from the measured baselines.

The gate's host tests include real command-line exit codes. The historical
five-scenario results do not cover the expanded thirteen-scenario catalog.
Current collection settings, raw evidence and acceptance outcomes are recorded
in the [expanded performance acceptance record](kernel-validation.md#expanded-performance-acceptance-on-2026-09-15).

Host-tool regression tests use `uv run pytest scripts/tests`. Those tests validate orchestration and parsing only; real kernel acceptance is the QEMU matrix above.
