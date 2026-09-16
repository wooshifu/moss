# Kernel Validation Design

Status: the initial delivery was implemented and validated on 2026-09-06 for ARM64, x64, and RISC-V 64 under QEMU TCG with four vCPUs and 2 GiB. This includes the required SMP and enlarged-memory repairs. The expanded core acceptance scope confirmed on 2026-09-14 below is required work, not a completed acceptance claim. See the [usage guide](kernel-validation-usage.md) and [acceptance record](kernel-validation-acceptance.md) for executable commands, evidence, and remaining limits.

## Confirmed Decisions

- [ADR-0001](adr/0001-validate-real-kernel-functions.md): validate real kernel functions on ARM64, x64, and RISC-V 64, with reliable failure reporting. The current delivery includes the necessary kernel repairs for all six required core paths.
- [ADR-0002](adr/0002-dedicated-kernel-validation-image.md): run validation workloads in a dedicated image that reuses production kernel startup and subsystem implementations.
- [ADR-0003](adr/0003-suite-level-kernel-test-isolation.md): use one fresh QEMU instance per functional suite, with separate instances for destructive cases and explicit reporting of cases not run after a failure.
- [ADR-0004](adr/0004-compare-kernel-performance-in-qemu.md): compare kernel revisions under fixed QEMU conditions and report results with their environment.
- [ADR-0006](adr/0006-validate-general-purpose-kernel-capabilities.md): assess the agreed general-purpose kernel capability profile through practical applications; networking and persistent storage are excluded from this delivery.

## Required System Acceptance Scope

Confirmed on 2026-09-15, with the user's later network and persistent-storage exclusions applied: this delivery's general-purpose kernel profile covers reliable command-line applications, requiring end-to-end correctness, stability and performance evidence. Missing production capabilities needed by these in-scope workflows and their acceptance tests are part of this goal. Network and persistent-storage implementation, capability tests and performance acceptance are outside this delivery, including `wget`, HTTP download, cross-reboot data retention, durability guarantees and power-loss recovery.

The six core paths and their existing architecture/build requirements remain prerequisites. Implementation was authorized through a persistent goal on 2026-09-15; begin at the agreed real-kernel and userspace-syscall test boundaries. Application porting choices and additional system-workload thresholds still require specification before the corresponding work. This scope decision does not establish system acceptance or extend QEMU timing evidence to physical hardware.

Also confirmed on 2026-09-15: application compatibility means rebuilding existing C/POSIX command-line applications for Moss, allowing C-library or platform adaptation while preserving the required application behavior. The initial application set is specified below; versions, enabled features and required interfaces remain to be selected. Direct execution of existing Linux binaries is outside this target, and passing selected applications does not establish complete POSIX conformance.

### Initial Real-Application Acceptance

Confirmed on 2026-09-15: use the following selected BusyBox commands as the first application acceptance target.

The user subsequently fixed the source version at **BusyBox 1.37.0**. The official release archive and verified SHA-256 are pinned in [ADR-0006](adr/0006-validate-general-purpose-kernel-capabilities.md#busybox-version-confirmed-on-2026-09-15); build-time checksum enforcement is still required. The user also selected **statically linked mlibc with Moss-specific adaptation**, including the required startup/TLS, RV64 soft-float and system-interface work; dynamic linking and Linux binary compatibility remain excluded. The runtime choice is recorded in ADR-0006 and is not evidence of an implemented port.

| Applications | Required Observable Behavior |
| --- | --- |
| `ash` | Execute scripts, pipelines and redirection; verify output and exit status. |
| `ls`, `cat`, `mkdir`, `cp`, `mv`, `rm`, `grep`, `wc` | Perform file/directory operations and text processing; verify results and error returns. |

Verify local file contents, directory changes and error returns within the same boot. The previously proposed download and local-file/reboot workflows are no longer required; application acceptance does not require data to survive reboot.

These workflows supplement the existing kernel functional and stability checks. Successful compilation or a shell prompt alone does not satisfy the application contracts. The build configuration, concrete fixtures, nonpersistent file environment, runtime profiles and additional acceptance thresholds remain to be specified. The selected commands are requirements, not evidence of a completed Moss port.

Excluding persistent storage does not exclude VFS, file descriptors, pipes or the selected commands' file and directory operations. Missing nonpersistent behavior required by those applications remains an implementation and test gap, not a passing or silently skipped case.

### Required System Acceptance Matrix

Confirmed on 2026-09-15: the complete in-scope BusyBox end-to-end workloads must pass on ARM64, x64 and RISC-V 64, each in Debug and Release. All six configurations must satisfy the same required application behavior; missing or failing configurations leave system acceptance incomplete.

Performance acceptance uses Release builds and the fixed, comparable execution environments in ADR-0004. Compare kernel revisions within each architecture and environment. Functional success does not establish a performance pass, and these matrix requirements do not constitute executed acceptance evidence.

### Persistent Storage Exclusion

The user's scope correction on 2026-09-15 supersedes the earlier storage acceptance decisions. This delivery no longer requires orderly-reboot persistence, simulated sudden power loss, the file/parent-directory synchronization durability contract or automatic startup/mount disk recovery. No recovery mechanism, recovery deadline or persistent-storage benchmark is required for this profile. These exclusions do not waive the existing core correctness, failure-rollback, resource-recovery or stability requirements.

### Implementation Start: Pipe Read Regression

On 2026-09-15 the initial host-tool baseline passed 185 tests. The first added userspace regression, `users/pipe_waits_for_writer`, reads before waiting for its delayed child writer and verifies payload and final EOF through real syscalls. ARM64 Debug built successfully; `build/arm64-debug/validation/1789454919689644052/results.json` records the six preceding cases passing and the new case failing with mask `0x2c` (premature empty read, absent payload and subsequently unread bytes). This is failing regression evidence, not a completed repair or cross-architecture acceptance. The earlier run `1789454898098738309` is retained as a catalog-mismatch infrastructure failure before the host catalog was updated. After that update, all 75 host runner tests passed. The delayed-writer case does not by itself establish deterministic SMP interleaving coverage.

### First Pipe Blocking Repair on 2026-09-15

The production pipe ring now serializes data and endpoint changes, registers stack-owned waiters without allocating, and sleeps until data or capacity changes. Closing the peer wakes waiters. Scheduler sleep preparation records early wakeups until bootstrap confirms that the sleeping continuation has been saved; only then can a waking CPU enqueue it. The two partial-copy tests now close their writer before asserting EOF, retaining their original copied-byte and fault assertions.

`users/pipe_waits_for_reader` first failed on ARM64 Debug with mask `0x28` in report `1789455444615672684`, demonstrating that a full pipe returned zero instead of waiting. After the write-side repair, the three new cases cover delayed producer, delayed consumer and 256 request/reply exchanges with requested CPU 0/CPU 1 affinity masks. The later affinity regression below showed that setting those masks did not establish execution placement; the initial cross-CPU claim is withdrawn. All six builds and all 18 functional/framework/production-boot CTests passed. Each functional report contains 17 suites, 78 passing cases and 1,000 completed lifecycle cycles with matching resource checkpoints. The 185 host tests, Ruff and scoped whitespace checks also passed.

| Preset | Functional Report | Framework Report | Production Boot Run |
| --- | --- | --- | --- |
| arm64-debug | `1789455573383768230` | `1789455647006109213` | `run-__eot54o` |
| arm64-release | `1789455626720370315` | `1789455693718662387` | `run-0j4lgdyo` |
| x64-debug | `1789455574552054255` | `1789455656831157486` | `run-7sgtf9lz` |
| x64-release | `1789455695765266799` | `1789455751847363396` | `run-234z85xj` |
| riscv64-debug | `1789455625565267886` | `1789455716757254608` | `run-8qerxif7` |
| riscv64-release | `1789455696937005596` | `1789455758572289242` | `run-pce3j5ax` |

Validation reports are `build/<preset>/validation/<ID>/results.json`; boot reports are `build/<preset>/production-boot/<run>/guest/results.json`. An attempted uaccess run during an unfinished rebuild was rejected by artifact-provenance validation before guest execution; it is not kernel-failure evidence, and subsequent runs used completed builds. The original assertion and catalog failures remain unchanged. This slice does not close signal-interrupted waits, `SIGPIPE`/`EPIPE`, descriptor lifetime races, all nonblocking/partial-transfer boundaries or deterministic sleep-handoff interleavings. BusyBox, long-run stability and performance acceptance remain incomplete; the short functional matrix cannot substitute for those gates.

### Pipe Signals and Wakeup Affinity on 2026-09-15

Five additional `users.signals` cases exercise real syscalls: `pipe_sigpipe`, `pipe_interrupted`, `pipe_noninterrupting_signals`, `pipe_partial_interrupt` and `signal_wakeup_affinity`. The pipe checks require `EPIPE` plus caught, blocked/unblocked, ignored and default-terminating `SIGPIPE`; `EINTR` before transferring bytes; uninterrupted I/O for ignored or masked signals; and an exact 4,096-byte result and payload when an 8,192-byte write is interrupted after filling the ring. Children release the I/O after sending the signal so the original broken behavior can report assertions rather than depend on a hang. These contracts follow the documented [pipe](https://man7.org/linux/man-pages/man7/pipe.7.html), [read](https://man7.org/linux/man-pages/man2/read.2.html) and [write](https://man7.org/linux/man-pages/man2/write.2.html) behavior; they do not claim full POSIX or Linux ABI compatibility.

ARM64 Debug recorded the original `SIGPIPE` failure in `1789456045759857243` and the original `EINTR` failure in `1789456422563483701`. The production pipe path now generates `SIGPIPE`, preserves positive partial-write counts, and checks actionable signals across the existing sleep handoff. Pending signals and masks are atomic; all signal senders use the shared wakeup implementation. Ignored and masked signals do not turn pipe waits into `EINTR`.

The affinity regression exposed another gap: `sched_setaffinity` changed the mask, but wakeup still enqueued on an excluded CPU. Merely changing the signal sender's preferred CPU did not fix this. The original failure is `1789456941231887352`; the subsequent failed matrices are ARM64 Debug `1789456985734774941`, ARM64 Release `1789457009836682242`, x64 Debug `1789457010987969190` and RV64 Debug `1789457042023561329`. Diagnostic report `1789457117150662666` explicitly records the CPU 1 task waking on CPU 0. These failures remain intact.

The shared scheduler wakeup now selects an allowed configured CPU before enqueueing; affinity syscall reads/writes use the same task lock. The pipe exchange and signal-wait tests also query the existing `topinfo` CPU assignment after a real sleep, rather than treating a successful mask update as placement evidence. ARM64 Debug focused reports `1789457336808136351` (`users.signals`) and `1789457337963810611` (`users`) pass with these stronger checks. Final functional/framework results and the outstanding production-boot failure are recorded below.

That matrix also exposed an intermittent x64 Release `users.timers/cancel_in_flight` timeout in `1789457484850663979`. Focused runs reproduced it in `1789457630223051549` and `1789457711838846157`; diagnostic logging changed its reproducibility and was removed, not retained as a fix. Code inspection found that the fixture armed the global timer before confirming both actors were ready, and allowed timer IRQs to preempt actors on whose progress its held callback depended. The fixture now waits for both actors to mask local IRQs and publish readiness before arming. It retains the real callback, concurrent cancellation/restart checks, observation window and unchanged five-second host deadline, and additionally checks that the callback ran off the two actor CPUs. Thirty-two consecutive uninstrumented x64 Release runs passed, from `1789457902680823873` through `1789457927925760198`. This supports the corrected controlled scenario, not a general IRQ-liveness claim or proof of every historical timeout's exact interrupt sequence.

All six final builds and functional/framework CTests passed. Every functional report contains 17 suites, 83 passing cases, no unrun selection and 1,000 completed lifecycle cycles; all 11 resource checkpoints match. The host suite passed 185 tests, with Ruff, scoped formatting and whitespace checks passing. Production boot passed five configurations, but ARM64 Debug timed out, so the combined matrix is **17/18 CTests, not complete**.

| Preset | Functional Report | Framework Report | Production Boot Run |
| --- | --- | --- | --- |
| arm64-debug | `1789457928940602495` | `1789457961362529416` | `run-6xmampzx` — timeout |
| arm64-release | `1789457960270722304` | `1789457996497981033` | `run-88lzqsxq` — passed |
| x64-debug | `1789457930127242749` | `1789457973172765675` | `run-99bwqsz5` — passed |
| x64-release | `1789458009959291170` | `1789458023407986352` | `run-8opsp1xt` — passed |
| riscv64-debug | `1789457931256431707` | `1789457974175559517` | `run-z9yxxaub` — passed |
| riscv64-release | `1789458011126339065` | `1789458022720042019` | `run-vq3tkgrl` — passed |

The ARM64 Debug production failure repeated in `run-_7auxu8n`. Both reports stopped at the first shell prompt, with `completed_steps: 1`, no command echo and the unchanged 30-second timeout. Initial inspection suspected the interval between checking the RX ring and publishing `blocked_reader_`, and noted that console sleep does not use the new handoff. That was a hypothesis, not the established cause of these timeouts; the controlled investigation below instead reproduced input loss during lazy UART initialization. Both original failures remain intact.

This slice does not establish immediate migration after changing a running task's affinity, deterministic run-ownership/sleep-handoff interleavings, full stop/continue or `SA_RESTART` behavior, complete signal semantics for waitpid/nanosleep, descriptor-lifetime concurrency, or all zero-length/nonblocking/partial-copy boundaries. The console repair, BusyBox adaptation, renewed long-run evidence for the final kernel and performance acceptance remain required. Networking and persistent storage remain excluded.

### Console Readiness Regression on 2026-09-15

An ordinary ARM64 Debug rerun, `run-c3drxllt`, passed and did not establish that the intermittent failure was gone. GDB showed that a controlled stop between the ring-empty check and waiter registration still had local IRQs masked in that invocation; subsequently injected input was delivered after registration and completed normally. The investigation did not establish that proposed lost-wakeup sequence as the historical cause.

Stopping at the first real VFS console read, after the shell prompt but before lazy RX initialization, reproduced the failure. The automated probe `run-tyymymge` verified UART input queued at that boundary, then detached without modifying kernel state or syscall results. The old kernel timed out after 30 seconds with only the first prompt completed. The installed QEMU is 7.2.22: its [PL011 implementation](https://github.com/qemu/qemu/blob/v7.2.22/hw/char/pl011.c) resets receive FIFO counters when the FIFO-enable bit changes, and clears pending interrupt bits on UARTICR writes. Moss performed both operations on the first read, after advertising the prompt. UARTFR still reported nonempty in the captured session, but this flag alone does not prove retained data: that emulator version leaves FIFO flags stale when resetting its counters.

The repair initializes console RX before devfs is published and removes the read-side lazy initializer. The initial green controlled report is `run-5j65_t9w`, alongside ordinary boot `run-tkmturxc`. `moss-console-input` now retains this controlled ARM64 Debug regression in CTest; it requires a target-capable `gdb-multiarch`, a verified first-read pause and queued hardware input, successful debugger exit, and the same actual ELF execution and subsequent shell command as ordinary production boot. Host checks reject missing input-barrier evidence or debugger failure even if simulated shell output appears successful.

Because UART input is now IRQ-owned before userspace starts, the stability permit poll uses `console_try_getc`, the same nonblocking receive path used by console reads, instead of reinitializing or reading the UART behind its IRQ handler. RV64 retains its direct polling backend. This ownership adjustment still requires renewed long-run execution; neither a short matrix nor a controlled startup pass establishes the 30-minute stability gate. The existing single-reader console model and its broader sleep/SMP semantics are not closed by this readiness repair.

All six updated builds passed. The first updated six-configuration matrix passed **18/19 CTests**: all production boots, all framework runs and the controlled ARM64 Debug console test passed, but RV64 Debug timed out in `users/pipe_cross_cpu_roundtrip` at the unchanged five-second deadline. Its following `pipe_waits_for_reader` case was not run. Every other functional report passed all 83 cases; every configuration completed 1,000 lifecycle cycles with 11 matching resource checkpoints. The host suite passed 188 tests. The six preset runners were launched concurrently; this execution context is recorded, not accepted as proof that host contention caused the timeout.

| Preset | Functional Report | Framework Report | Production Boot Run |
| --- | --- | --- | --- |
| arm64-debug | `1789459178318350203` | `1789459274858760415` | `run-sli6_fsq` |
| arm64-release | `1789459178368669396` | `1789459252792112582` | `run-g96lj0g_` |
| x64-debug | `1789459178348852097` | `1789459284035919003` | `run-e6gr3a_k` |
| x64-release | `1789459178387323634` | `1789459258029446292` | `run-xqr5d2s6` |
| riscv64-debug | `1789459178382645742` — pipe timeout | `1789459279164249494` | `run-empcqxmx` |
| riscv64-release | `1789459178371219897` | `1789459257445993855` | `run-a_6sgna_` |

The matrix's controlled console report is ARM64 Debug `run-ne96ua0k`. Eight additional ordinary boots and eight controlled first-read boots all passed, with reports under `build/arm64-debug/production-boot/console-repeat-on0t7lpn/{0..7}-{prompt,controlled}/results.json`. A focused RV64 Debug `users` rerun passed in `1789459331563422576`, but does not repair or explain the retained matrix timeout. Its root cause, the remaining concurrency gaps, real BusyBox adaptation, final-kernel long runs and performance acceptance remain open.

A subsequent RV64 Debug rerun without other owned QEMU workloads also passed all three CTests: functional `1789459423499251796` (17 suites, 83 cases), framework `1789459445453383340`, and production boot `run-fkms1stb`. This is additional passing evidence under a different host execution schedule, not a root-cause fix or grounds to erase the concurrent matrix's failure. No deadlines were relaxed. All owned debugging and validation processes were cleaned up after these runs.

### Cross-CPU Exit and Bootstrap Address-Space Regressions on 2026-09-15

Repeating the RV64 Debug `users` workload reproduced the retained pipe timeout. Read-only GDB captures in `1789460207813261307` and `1789460207996290098` showed all 256 request/reply records consumed, no pipe waiters, an exited child with status 37, and its parent sleeping in `sys_wait4` with SIGCHLD pending and empty runqueues. The old wait path scanned for zombies before registering and publishing sleep, so an intervening exit could lose its wakeup. The repair uses the existing prepared-sleep handoff, registers the waiter, rechecks child status, then commits sleep; process-state publication is atomic. `waitpid` and `wait4` share this repair, including the existing copyout-before-reap contract.

The new `users/cross_cpu_exit_reap` regression reuses the real two-pipe exchange with one record per child, repeating 128 complete cross-CPU exit/reap cycles. Before repair, 11 of 12 runs timed out under six concurrent runners; `1789460303031352275` is one retained failure. The minimized diagnostic `1789460360697760870` confirmed the same sleeping-parent/zombie-child state. No production tracing hooks or deadline changes are used by this test.

The first wait repair alone was insufficient: 14 of 24 uninstrumented runs still timed out in the new short-cycle case. Diagnostic `1789460589682178050` instead showed a runnable parent and a CPU repeatedly taking supervisor instruction faults with SATP still selecting the exited child's address space. Physical-memory capture `1789460655571679332` distinguishes the child's ASID 1 from the live parent's ASID 2 and the kernel's ASID 0; the child had already released its page tables. Bootstrap had retained a task's address space after yielding or sleeping, even though that task could resume and exit on another CPU. These failures remain intact and are not classified as host slowness or successful wait repair.

Returning to bootstrap now selects the kernel page tables before switching context. Sleep, yield, pipe/wait handoff, console blocking, exit and the timer benchmark use that shared transition; exit also reuses its kernel-page-table selection before freeing user mappings. GDB's [physical-memory inspection mode](https://www.qemu.org/docs/master/system/gdb.html#examining-physical-memory) was used only after the runner recorded failure, without writing guest memory or registers. Temporary capture scripts remain under the explicitly diagnostic `build/pipe-debug-Gi1sPf/`, not in production or the acceptance runner.

After both repairs, 24 uninstrumented RV64 Debug `users` runs passed with six runners in parallel, from `1789460775865123660` through `1789460808646496969`. Every run includes the original 256-record exchange and the new 128-cycle case: 3,072 additional complete cross-CPU exit/reap cycles in total. The host suite passes all 188 tests; scoped ClangFormat, Ruff and whitespace checks pass.

All six final builds and **19/19 CTests passed**, with the six preset runners launched concurrently. Every functional report has 17 suites, 84 passing cases, no unrun selection and 1,000 complete lifecycle cycles with all 11 resource checkpoints matching. Production boots and framework checks passed in each configuration; the controlled ARM64 Debug first-console-read report is `run-_hd9124r`.

| Preset | Functional Report | Framework Report | Production Boot Run |
| --- | --- | --- | --- |
| arm64-debug | `1789460891876773230` | `1789461057521758442` | `run-1ev9bwto` |
| arm64-release | `1789460891874773179` | `1789461039366601724` | `run-reyszyh1` |
| x64-debug | `1789460891880230391` | `1789461059254120776` | `run-4wgs1m5s` |
| x64-release | `1789460891857909392` | `1789461044153154605` | `run-vq0tl_eh` |
| riscv64-debug | `1789460891861873133` | `1789461063530507502` | `run-h_y92_v3` |
| riscv64-release | `1789460891865183275` | `1789461049341210723` | `run-ay1npmf7` |

No owned QEMU or debugger processes remain after validation. This regression does not establish complete timer/signal interruption semantics, immediate affinity migration, every scheduler interleaving or final performance/stability acceptance. Nanosleep and console still require controlled early-wakeup checks of their sleep-publication sequences; selecting kernel page tables is not a substitute for the prepared-sleep handoff. The additional page-table transition requires renewed performance measurements, not reuse of old benchmark acceptance. BusyBox 1.37.0 adaptation, final-kernel long runs and performance gates remain required; network and persistent storage remain excluded.

### Controlled Remote Nanosleep Expiry and Arm-Failure Recovery (2026-09-15)

The new `users.timers/early_wakeup` regression forks a real child pinned to CPU1. A validation-only override of the production image's no-op `moss_validation_sleep_armed` observation point holds that child after arming its actual `HrTimer`, with local IRQs still masked. CPU0 processes the real expiry. The probe waits for removal from the timer queue and callback completion before allowing the syscall to save its context; it neither invokes a synthetic wakeup nor substitutes a timer or scheduler. Both relative and absolute sleep must complete with the required elapsed time, exactly two probe visits, and the child's expected exit status.

The first probe attempts pinned the sleeper to CPU0 and could not deliver expiry while that CPU's IRQs were masked. Their timeouts are retained as test-driver failures, not evidence of the kernel race: RV64 Debug `1789461855864867950`, `1789461894579964026`, `1789461972107288072`, and diagnostic runs `1789462005877215687`, `1789462067939067541`. An intermediate secondary-CPU timer driver also failed (`1789462178881896315`, diagnostic `1789462180045859458`): the production IRQ routing processes the global `HrTimer` queue on CPU0, while secondary CPUs run scheduler ticks. Reading that route led to the smaller CPU1-sleeper test above, with no extra driver.

That corrected test failed before the repair in ordinary RV64 Debug run `1789462311494204496`. The timeout-only, read-only GDB capture `1789462312657370909/users.timers/gdb.txt` showed one probe visit, a completed wakeup, the child **Ready but absent from every runqueue**, its parent waiting for exit, and idle CPUs. The old `sleep_until` could enqueue the early-woken child and subsequently dequeue it before switching out, losing its only wakeup.

Both sleep syscalls now use the scheduler's existing `prepare_sleep` / `commit_sleep` handoff: dequeue before arming, defer remote wakeup publication until the continuation is saved, and cancel synchronously before releasing the stack timer. An unsuccessful arm self-wakes through that same handoff before returning its error. `users.timers/arm_failure_recovery` reuses the kernel timer-capacity fixture to fill the real queue, requires `ENOMEM` from both sleep interfaces, releases the timers with exact heap recovery, and then requires normal relative and absolute sleep to work again. No new sleep-state protocol or synthetic scheduler was introduced.

Initial repaired RV64 Debug timer suites passed in `1789462376502948760` and, with the arm-failure case, `1789462507373137104`. Twelve further ordinary runner invocations, six concurrent, all passed with the final fixture: `1789462570292922996`, `1789462570322497989`, `1789462570328939902`, `1789462570329414582`, `1789462570337134405`, `1789462570363791267`, `1789462580539307115`, `1789462581732943622`, `1789462582163490012`, `1789462582415597803`, `1789462582687859573`, `1789462583672158137`. Every invocation includes 1000 ordinary short sleeps plus the controlled expiry and capacity/recovery cases. Host checks passed: 188 pytest tests, Ruff, scoped ClangFormat and `git diff --check`.

The six builds succeeded, but the subsequent complete matrix was **18/19 CTests passed**, not an acceptance pass: x64 Debug's lifecycle warmup failed before the first counted cycle. All six timer suites, framework checks and production boots passed; the other five configurations passed all 17 functional suites / 86 cases and 1000 lifecycle cycles. Reports are finalized with no unrun selections. ARM64 Debug's controlled first-console-read boot passed in `run-syzz_nw8`.

| Preset | Functional Report | Framework Report | Production Boot Run |
| --- | --- | --- | --- |
| arm64-debug | `1789462569056607301` | `1789462706163427155` | `run-4arr38i3` |
| arm64-release | `1789462569090072465` | `1789462692434866270` | `run-0fqrbhch` |
| x64-debug | `1789462569052849409` (lifecycle failed) | `1789462711081411394` | `run-7bywlqf4` |
| x64-release | `1789462569102531451` | `1789462701287904004` | `run-hl_l34lc` |
| riscv64-debug | `1789462569123837000` | `1789462712762602636` | `run-4fz0zn78` |
| riscv64-release | `1789462569162690817` | `1789462689287977301` | `run-ale3eshc` |

A focused 12-run x64 Debug lifecycle reproduction, six concurrent, produced the same initial-cycle failure in `1789462789979465304` (11 other runs passed). The original lifecycle boolean did not identify which step failed. The user workload now retains a step mask and unexpected raw child wait status in its failure report without reducing its required checks, cleanup or cycle counts. The diagnosis and repair below address this failure; passing timer suites and incidental successful lifecycle reruns alone did not explain it.

The separate application preparation fixed BusyBox 1.37.0 and recorded the user's mlibc-static/Moss-runtime decision plus source pins in ADR-0006. A compiler-only check of the pinned mlibc RV64 `setjmp.S` rejected `rv64imac/lp64` because its `fsd`/`fld` instructions require the D extension; the same source assembled with `rv64gc/lp64d`. This proves an adaptation requirement, not a working mlibc build or permission to change Moss's ABI. Runtime/BusyBox integration remains unimplemented.

Remaining gates include the console sleep handoff, the benchmark fixture's manual sleep publication, signal-interrupted sleep/wait semantics, other scheduler interleavings, real BusyBox workflows, final-kernel long runs and renewed performance calibration/gates. This slice does not establish those capabilities; networking and persistent storage remain excluded.

### IRQ-Safe Bootstrap Task Selection (2026-09-15)

The detailed lifecycle report reproduced four failures in 24 ordinary x64 Debug runs: `1789462973869224673`, `1789462974763496317`, `1789462980041254065`, and `1789462981142585161`. Each reported mask `0x5c000218`: child exit 92 at the sleep check, with subsequent missing payload/shared-offset results. Temporary failure-only timestamps in `1789463133566673921` showed a requested 1 ms sleep returning after 666,112 ns, with all three clock/sleep syscall return codes zero. Further diagnostic runs `1789463267440151389` and `1789463305204215946` observed the syscall continuation resumed before its deadline while the real timer was still active and no signals were pending. Their logging delayed userspace enough to pass its elapsed-time check; these are diagnostic evidence, not acceptance passes.

GDB capture `1789463769334195242/users.lifecycle/gdb.txt` established the mechanism. CPU0's `cpu_startup_entry` had selected child TID 1005 with IRQs enabled. A real timer interrupt could consume that selection and run the child until it slept, then return to the interrupted bootstrap loop. That loop subsequently dispatched its stale cached pointer: GDB caught `context_switch_to_task` receiving the child in `Sleeping` state, absent from the runqueue, with no pending signals, while the caller still had IF enabled. The diagnostic temporarily expanded this real selection-to-dispatch window; it did not inject a wakeup. Its debugger-delayed pass is not ordinary runtime acceptance.

The production repair disables local IRQs at the start of each bootstrap scheduling iteration, before task selection. The existing idle primitive enables IRQs while waiting and masks them again before returning; dispatched tasks restore their own execution state. Disabling IRQs only inside `context_switch_to_task` was too late to protect its caller's selection. This closes the observed local-interrupt race, not every cross-CPU ownership or migration interleaving.

The existing real `users.lifecycle` workload now also observes the common dispatch boundary and asserts that local IRQs are masked, requiring an actual observer visit before completion. Two initial test-driver attempts (`1789463949795806859`, `1789464088605476454`) completed 1000 cycles but failed the visit requirement: the weak default definition in an imported C++ module prevented the test override from taking effect. Moving that default definition into the existing process implementation unit leaves a strong observer in the validation ELF and a weak no-op in the production ELF, verified from both symbol tables. With only the IRQ repair removed, ordinary run `1789464356334948403` failed the IRQ-boundary assertion despite completing all 1000 cycles and matching resource checkpoints. All temporary delays, tracing and debugger attachment were removed before renewed acceptance runs.

After restoring the repair, all 24 ordinary x64 Debug lifecycle runs passed with six runners in parallel, from `1789464397405051658` through `1789464439570576736`. Their finalized reports contain 24,000 complete cycles and 264 resource checkpoints matching their respective baselines. All six builds passed, as did 188 host tests, Ruff, scoped ClangFormat and whitespace checks. These short runs do not replace the required single-kernel 30-minute / 10,000-cycle gates.

The renewed six-configuration matrix passed **19/19 CTests** with the preset runners launched concurrently. Every finalized functional report contains 17 suites, 86 passing cases and no unrun cases, with 1000 complete lifecycle cycles and all 11 resource checkpoints matching. Framework self-validation and normal production boot passed in every configuration; ARM64 Debug's controlled console-input boot passed in `run-qwz64fd0`.

| Preset | Functional Report | Framework Report | Production Boot Run |
| --- | --- | --- | --- |
| arm64-debug | `1789464471223481807` | `1789464641690334546` | `run-y4impe3r` |
| arm64-release | `1789464471220252816` | `1789464634365655395` | `run-4ifm29tu` |
| x64-debug | `1789464471222932767` | `1789464648850377704` | `run-owhhxkmn` |
| x64-release | `1789464471200313667` | `1789464625691973949` | `run-jg9bz_yv` |
| riscv64-debug | `1789464471224515998` | `1789464647001584078` | `run-3jahigxr` |
| riscv64-release | `1789464471220181406` | `1789464635878730092` | `run-ftrpqofu` |

The earlier failed matrix and diagnostic/test-driver failures remain retained. This closes the observed timer early-wakeup and bootstrap IRQ-selection regressions, not the remaining console/benchmark handoff, signal semantics, general scheduler ownership, BusyBox, long-run or performance gates. Owned acceptance/debugging processes exited; the separate interactive ARM64 Release QEMU session and pre-existing Android emulator were left untouched.

### Staged Static mlibc Runtime (2026-09-15)

The standalone build in `src/userspace/mlibc/CMakeLists.txt` now produces the pinned mlibc v7.0.0 static library and headers for ARM64, x64 and RV64. CMake verifies commit-addressed archive SHA-256 values before extraction; Meson 1.10.2 uses only the populated dependency sources (`--wrap-mode=nodownload`). Besides mlibc's ADR-0006 pin, the enforced dependency pins are:

| Source | Revision | Archive SHA-256 |
| --- | --- | --- |
| managarm/frigg | `b0dbea66bc19f7c5546f0039a3be842feb02678c` | `10cbee1dab6e7b0a1ca8d6d59d9eeffab1ed3cf6b0031551fd63a6861cc07a41` |
| managarm/libsmarter | `f7d061bc37d485418344452c7ceb28d5df3ba85d` | `1426e24f3c3c2a08983fad71d461faa415f0ccc1ca432dd80bebe1dac9cc7bce` |
| osdev0/freestnd-c-hdrs | `d33711241b46ecb8f2ad33927fcefdcb3ac0162e` | `d3f0c1e0720dec9da97175eebdc51ac4b9bdd776b5687b2a1ccd97f6e861498e` |
| osdev0/freestnd-cxx-hdrs | `a6b351e0ab3e74e5789b01fa1447e4cd62373da7` | `dc4a44daef5d50a6f9aea0cb7bd2164c208f159d273c51f3ff8334b5bd65ef95` |

The port reuses Moss's raw syscall wrappers and adapts startup to its existing register-based `argc`/`argv` entry, without introducing Linux syscall numbers or a dynamic loader. ARM64/RV64 set their native userspace thread pointers. The pinned RV64 `setjmp.S` now saves/restores floating-point registers only for the double-float ABI; the actual library build remains `rv64imac/lp64`. Bare-metal x64 Clang ignores `ifunc`, so the build rejects that false-positive feature check and retains upstream's generic `strcmp`. These are source/build adaptations, not proof of runtime compatibility on all three architectures.

Reproduce the standalone builds from the repository root with CMake, Clang/LLD, LLVM ar/strip, Ninja and uv installed. These commands now include the compiler-runtime integration described below:

```sh
uv run cmake -S src/userspace/mlibc -B build/mlibc-arm64 -G Ninja -DMOSS_TARGET_ARCH=ARM64
uv run cmake --build build/mlibc-arm64 --target mlibc-validation
uv run cmake -S src/userspace/mlibc -B build/mlibc-x64 -G Ninja -DMOSS_TARGET_ARCH=X64
uv run cmake --build build/mlibc-x64 --target mlibc-validation
uv run cmake -S src/userspace/mlibc -B build/mlibc-riscv64 -G Ninja -DMOSS_TARGET_ARCH=RISCV64
uv run cmake --build build/mlibc-riscv64 --target mlibc-validation
```

The initial ARM64/x64 links used this host's `/usr/lib/gcc-cross/aarch64-linux-gnu/10/libgcc.a` and `/usr/lib/llvm-23/lib/clang/23/lib/linux/libclang_rt.builtins-x86_64.a`, with respective SHA-256 values `3b5ad18a9da140eb5a03f7c56d9e6fa5bc945319a27085c84fe27093d7b89538` and `f3e0fa742b886498399f8a138ef03d9a74cad3821ac64f741aae2fe1e010cd97`. Those host-specific dependencies have since been replaced by the pinned source build below. Outputs remain `build/mlibc-<arch>/sysroot/usr/{include,lib}` and `libc_validation.elf`. The initial three library builds and ARM64/x64 probe links passed (`build/mlibc-<arch>/build-final-port.log`), while RV64 linking failed on soft-float builtins including `__muldf3`, `__addtf3` and conversion/comparison helpers (`build/mlibc-riscv64/link-all-missing.log`). That stage established no RV64 runtime pass. Use a fresh build directory after changing architecture/compiler or cross-file options; Meson's cached options are not automatically replaced by a changed cross file.

The real `users.libc/static_runtime` case forks through the existing raw-syscall validation program, execs `/libc_validation.elf runtime`, and checks the exact wait status `37 << 8`. The child uses real mlibc startup, initialized/zero TLS, malloc/free, errno on `close(-1)`, getpid, sched_yield with volatile TLS rechecks, setjmp/longjmp and write before returning 37 through libc exit. This is a small single-threaded runtime check, not pthread/futex correctness or cross-process TLS isolation acceptance.

Its first ARM64 Debug execution exposed a production ELF-loader bug: valid LLD `PT_LOAD` virtual addresses were not page aligned, while `AddressSpace::add_vma` requires page-aligned ranges. `execve` passed those unaligned addresses and ignored the failed mappings, so the entry instruction faulted without a VMA. The repair rounds the VMA start down, validates that the file contains the required leading page prefix and segment bytes, and includes that prefix in the file backing with backing offset zero. It does not redefine the pager's source-offset contract. The retained sequence under `build/mlibc-arm64/` is:

| Fixture Directory | Report ID | Observed Result |
| --- | --- | --- |
| `probe-pr5k31fb` | `1789465778349291467` | Entry instruction abort; no VMA; child status `0xf500` |
| `probe-d32crkho` | `1789465925182696351` | Incomplete alignment repair used the wrong backing offset; entry still faulted |
| `probe-8t9_eyeu` | `1789466075431021382` | Correct mapping reached mlibc; explicit unimplemented TLS interface exited 127 |
| `probe-6ip1egz6` | `1789466168680372082` | Native ARM64 TLS enabled the initial runtime pass |
| `probe-63yxwa_s` | `1789466411897922139` | Last of 12 ordinary passes with volatile TLS and setjmp/longjmp checks |
| `probe-yvecfgi0` | `1789466785110400523` | Passed again after the final port build cleanup |

The 12-run batch used four ordinary runners concurrently, with finalized passing reports from `1789466393731925426` through `1789466411897922139`. Reports are `<fixture>/validation/<ID>/results.json`; serial logs and frozen kernel/initramfs inputs are retained there. These initial fixtures used the local preparation script `build/mlibc-arm64/run_probe.py` and the existing CPIO encoder, followed by the ordinary runner. Their relocated manifests record image/fixture hashes but `source_status: unrecorded`; they remain focused runtime evidence, not provenance-complete system acceptance. At that stage the probe was not packaged by the root build or selected by default. The subsequent integration below supersedes those packaging and selection limits without relabeling these earlier reports.

After the loader change, all six kernel builds and **19/19 core CTests** passed again. Each finalized functional report has 17 suites, 86 passing cases, no unrun cases, 1000 complete lifecycle cycles and 11 matching resource checkpoints. The host suite passed **188 tests**. This existing core matrix does not include the staged mlibc workload. Framework and normal production boot passed on every configuration; ARM64 Debug's controlled console boot passed in `run-mbfyp4s5`.

| Preset | Functional Report | Framework Report | Production Boot Run |
| --- | --- | --- | --- |
| arm64-debug | `1789466392497536102` | `1789466571754286247` | `run-jw2fp0hj` |
| arm64-release | `1789466392527970865` | `1789466557480153941` | `run-bxmo44m9` |
| x64-debug | `1789466392497620882` | `1789466569009114206` | `run-p9yz0n42` |
| x64-release | `1789466392517244850` | `1789466567348520024` | `run-iuj61zjk` |
| riscv64-debug | `1789466392509951697` | `1789466566951399079` | `run-2pp8huif` |
| riscv64-release | `1789466392500715403` | `1789466558827196115` | `run-sdszu1f5` |

At the end of this initial slice, runtime work still included x64 FS-base/TLS, RV64 soft-float compiler-runtime integration/execution, futex/terminal interfaces, the required file/process/signal adapters, environment/argument handling, and normal fixture/provenance integration. Unimplemented sysdeps explicitly return `ENOSYS`; startup supplies an empty environment with at most 16 arguments, open only supports access-mode bits, and mmap only supports the adapted anonymous/private case. Raw write and uncontended allocation do not establish stdio, locking or application completeness. `execve` still needs transactional failure handling and fuller ELF validation. BusyBox 1.37.0 has not yet been built or run on Moss; the outstanding core, long-run and performance gates remain unchanged, with networking and persistent storage excluded.

### Pinned Compiler Runtime and Default Application Probe (2026-09-15)

The port now builds compiler-rt builtins from the official [LLVM 20.1.8 release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-20.1.8) for each target ABI, replacing the host cross-libgcc/installed-builtins paths. CMake enforces SHA-256 `15277402f6fd63397c0917a5c7171cda82d16d226094b828c1ed0f58f73b9c69` for `compiler-rt-20.1.8.src.tar.xz` and `3319203cfd1172bbac50f06fa68e318af84dcb5d65353310c0586354069d6634` for `cmake-20.1.8.src.tar.xz`. Both downloads matched the official repository's release-asset API digests. This is digest verification, not a locally performed signature verification.

The build uses upstream's standalone `lib/builtins` CMake project, the selected Clang target/ABI flags, and the actual mlibc target headers. `LLVM_RUNTIMES_BUILD=ON` avoids discovering unrelated host LLVM libraries; sanitizers, dynamic libraries and upstream runtime test executables are not built. The initial x64 builtins build failed because `cpu_model/x86.c` requires `assert.h`; building mlibc first and using its sysroot headers fixed that dependency. ARM64, x64 and RV64 library/probe links now all succeed from pinned sources. The retained initial build logs are `build/mlibc-<arch>/build-compiler-rt.log`; x64's header-dependency repair is in `build/mlibc-x64/build-compiler-rt-target-headers.log`.

RV64's original missing-builtins failure was reproduced in `build/mlibc-riscv64/link-start-softfloat.log`. Linking the unmodified probe with upstream builtins enabled its first real Moss pass in `build/mlibc-riscv64/probe-mjgtlwvi/validation/1789467276832757689`. The probe then gained volatile float/double/long-double arithmetic and integer-conversion checks with independently specified exact results (failure status 96). Before the build integration it failed linking those actual operations in `build/mlibc-riscv64/link-arithmetic-red.log`; afterward it passed on RV64 (`probe-juc0dpfu/validation/1789467444496878591`) and ARM64 (`probe-0x7087sg/validation/1789467450182271959`). The RV64 ELF declares the soft-float ABI and `rv64imac`-compatible attributes without F/D, with no required unresolved symbols. These initial relocated-fixture runs retain their original provenance limitations.

With `MOSS_BUILD_TESTS` enabled, the ordinary userspace build now builds mlibc/compiler-rt and packs the actual probe into `validation-initramfs.cpio` using the existing fixture generator. No temporary manifest or separate runner is needed. `users.libc` is now part of the default functional selection; its initial x64 failure was retained in that selection throughout the repairs below. The public fixture-generator regression first failed on the missing input contract, then passed with the new ELF entry. Reproduce the maintained path with:

```sh
uv run cmake --build --preset riscv64-debug
uv run scripts/kernel_validation.py run --manifest build/riscv64-debug/moss-artifacts.json --workload users.libc
uv run ctest --preset riscv64-debug-test -R moss-functional --output-on-failure
```

The same commands apply to the other five presets. Per-preset probe/sysroot outputs are under `build/<preset>/src/userspace/mlibc/`; the normal manifest, frozen inputs and source provenance accompany the results. This runtime check is still not BusyBox acceptance, multithreaded TLS isolation, full floating-point conformance or completion of the required system adapters.

During simultaneous cross-builds, the existing host cleanup test `test_host_child_lifecycle_reaps_every_spawn[ignore_term]` reported `startup_timeout` instead of reaching its intended `case_timeout` path (188 tests passed, one failed). Its retained final serial log contains valid startup/case records and no stderr, consistent with delayed startup/observation under host load; an unchanged isolated rerun passed. The failure logs are preserved at `build/host-startup-failure-IAobZr/guest/`. That host fixture now uses the normal 30-second startup allowance and a 35-second outer allowance, while retaining the 0.1-second case deadline, two-second terminate/kill requirement and all reaping assertions. Production runner defaults, kernel case deadlines and performance thresholds were not changed.

The first default matrix built all six configurations, but only **17/19 CTests** passed: `users.libc` failed in x64 Debug/Release because `sys_tcb_set` was still unimplemented. ARM64/RV64 passed all 18 suites/87 cases. The other 17 suites/86 cases and 1000 lifecycle cycles with 11 matching resource checkpoints passed on all six configurations. These ordinary reports have recorded source provenance; they do not turn the two x64 failures into passes:

| Preset | Initial Integrated Functional Report |
| --- | --- |
| arm64-debug | `1789467827877712241` |
| arm64-release | `1789467827876012100` |
| x64-debug | `1789467827771414294` |
| x64-release | `1789467827762014250` |
| riscv64-debug | `1789467827948158212` |
| riscv64-release | `1789467827981137106` |

### Native TLS, Fork State and Scheduler Self-Selection (2026-09-15)

x64 now implements Moss `SYS_ARCH_PRCTL`'s FS-base set/get operations. The setter rejects bases outside the Moss user domain (except zero), the getter uses checked user copying, and unsupported operations fail explicitly. The scheduler saves/restores `IA32_FS_BASE` alongside the existing FP context; first entry and exec install the new context's base. The initial native-TLS probe passed in x64 Debug report `1789468393359627650`.

The probe now forks through mlibc, checks inherited initialized/zero TLS and `errno`, changes the child's values, alternates repeated yields in both processes, re-execs itself, checks fresh TLS initialization and verifies the child's exit status with mlibc `waitpid`. Missing mlibc fork adaptation first failed with status 97 in `1789468500359309885`; the added fork/waitpid/execve adapters call real Moss syscalls. Unsupported nonempty exec environments and wait resource-usage requests still return `ENOSYS`; environment preservation is not established.

An initial version of the fork test passed on ARM64 and x64 while the compiler reused pre-fork TLS addresses in preserved general-purpose registers. That was insufficient evidence of native thread-pointer inheritance. The maintained noinline helper resolves TLS afresh after fork. With that strengthened test, x64 report `1789468654891702255` faulted on `movq %fs:0,%rax` at address zero, and ARM64 report `1789468655043534182` faulted on a fresh `TPIDR_EL0`-relative access at address `0x10`. Fork now captures live x64 FS base and ARM64 `TPIDR_EL0` into the child's initial context. RV64 already inherits its `tp` register from the actual trap frame, confirmed before this repair in `1789468777929411934`. ARM64/RV64 passed the strengthened check after the repair in `1789468843856379153` and `1789468837486850913` respectively. This does not establish pthread TLS isolation or ARM64 FP/NEON inheritance across fork.

The stronger real application path also exposed intermittent x64 `case_timeout`, retained in reports `1789468843152896752`, `1789468934612801263` and `1789469452114633170`. A ten-run unchanged repetition had three failures (`build/mlibc-x64-tls-repeats.log`); successful cases took about 0.1 seconds, so the five-second case deadline was not relaxed. x64 Release also failed in `1789469531554375349`. Initial diagnostic replays completed successfully and did not explain those failures. The installed `gdb-cli` failed before connecting because its quoted target/source arguments were rejected by local GDB; native GDB then captured the actual failing state using the frozen failed inputs.

The failing CPU was in `CfsRunqueue::rb_insert_fixup`, called from `sys_sched_yield`. In `build/mlibc-debug-b9jpmots/gdb.log` the intrusive node at `0x3ab6c8` had its parent, left and right pointers all pointing to itself. The corresponding production tick path enqueued the current thread, then did nothing if selection chose that same thread: it remained Ready and linked while continuing to execute. A later yield inserted the same node again. These diagnostic replays, scripts and logs under `build/` are not acceptance runs.

The new default `scheduler` suite exercises the production tick on isolated real scheduler queues with IRQs masked and the original current-task binding restored afterward. It creates a CFS expired-slice/self-selection case and an RR self-selection case without dispatching synthetic contexts. Before the fix, ordinary x64 report `1789469810773794095` failed the CFS Running/unqueued assertions; the suite stopped and RR was correctly reported unrun. A shared tick rescheduling path now dequeues the selected thread even for self-selection, restores Running and clears its reschedule request. It is used by CFS, RT-preemption and RR branches; it does not silently ignore duplicate enqueues. The unchanged CFS/RR assertions and real mlibc probe passed together in `1789469877591515827`. Ten further ordinary x64 Debug `users.libc` runs passed at the unchanged five-second deadline (`build/mlibc-x64-tls-repeats-fixed.log`, reports `1789469915106292313` through `1789469977131537131`).

After these repairs, all six kernel builds and **19/19 CTests** passed. Each finalized default functional report has **19 suites, 89 passing cases, no unrun cases**, 1000 complete lifecycle cycles and 11 resource checkpoints matching its baseline for heap bytes, physical pages, processes, threads, user/stack pages, descriptors and file references. This matrix includes the real static mlibc probe and both scheduler self-selection cases. The reports share recorded build-time source SHA-256 `34905e00601e1547736a66666c9d315dcc77d99655549e36b8a0042a522bfb14` and retain their exact frozen images/fixtures. Framework checks and normal production boot passed in every configuration; ARM64 Debug's controlled console boot also passed in `run-lp2a1vrs`.

| Preset | Functional Report | Framework Report | Production Boot Run |
| --- | --- | --- | --- |
| arm64-debug | `1789469937044447370` | `1789470121522810808` | `run-6se25wbh` |
| arm64-release | `1789469938187010134` | `1789470116570552594` | `run-5zdurt_q` |
| x64-debug | `1789469939418320117` | `1789470126790294232` | `run-6_72cts1` |
| x64-release | `1789469940607971111` | `1789470116570762284` | `run-truv9aw2` |
| riscv64-debug | `1789469941757084978` | `1789470126282667238` | `run-149qnbvl` |
| riscv64-release | `1789469943048905658` | `1789470116570724304` | `run-vzbb8p_b` |

Build/CTest logs are retained as `build/mlibc-final-<preset>-{build,ctest}.log` (the x64 Debug build is `build/mlibc-x64-scheduler-self-green-build.log`). The host suite passed **189 tests** in `build/mlibc-host-tests-with-scheduler.log`; changed C/C++ files, CMake files and Python files passed their format/lint checks, and `git diff --check` passed. The routine lifecycle check is still the declared raw core-path cycle, not 1000 BusyBox or mlibc application lifecycles.

This was a self-selection ownership repair, not closure of all scheduler migration or sleep-publication races. At that snapshot BusyBox 1.37.0 was still unbuilt; terminal/futex interfaces, required file operations and further process/signal/runtime contracts remained incomplete. Transactional failed exec, the remaining core regressions, final six-configuration long runs and calibrated performance acceptance remain required. Networking and persistent storage stay excluded.

### Real BusyBox Build, Shell Probe and Fork Allocation State (2026-09-15)

The ordinary test-fixture build now downloads the pinned official BusyBox **1.37.0** archive with the ADR-0006 SHA-256, builds it against the target's static mlibc/compiler-rt, and packs `/busybox.elf` into the existing validation initramfs. `users.busybox` is in the default functional selection. Its two initial cases execute the actual ash: `ash_exit` checks exit status 37 and empty stdout; `ash_substitution` checks a forked printf's output through ash's command-substitution pipe, validates the resulting variable, emits the exact bytes `moss\n` through a second pipe to the validation parent, and checks status 37. Builtin-app delegation and no-fork applet shortcuts are disabled. This is shell startup, command substitution and stdio evidence, **not** the complete required pipeline/redirection/file-command application acceptance.

`src/userspace/configure_busybox.cmake` starts with upstream `allnoconfig`, explicitly enables the agreed ash/ls/cat/mkdir/cp/mv/rm/grep/wc profile and ash's echo/printf/test builtins, then resolves and verifies it with `oldconfig`. The initial `KCONFIG_ALLCONFIG` attempt did not enable the requested booleans: the pinned upstream `allnoconfig` resets them. The maintained build does not rely on that failed method. The three exact, idempotent source adaptations disable unavailable mount/statfs and optional malloc-tuning headers for Moss and build the affinity helper only for its actual taskset/nproc consumers, neither selected here. The mlibc port installs upstream's public `sys/sysmacros.h`; enabling all Linux extensions was tried and rejected because it requires a Linux kernel UAPI tree. The final port does not add that dependency. `crt1.o` is supplied only at the final link, not in flags reused for upstream's relocatable object links (which initially produced duplicate `_start`).

The retained preflight and maintained build logs are under `build/busybox-build-qHYXkz/`: `build-{initial,selected,device-header,moss-platform,affinity-selection,malloc-platform,final-crt}.log`, `mlibc-linux-extensions.log`, and `maintained-<arch>-build.log`. All three Debug target BusyBox links succeeded; the ARM64 ELF has no interpreter segment and uses the Moss user address domain. Source archives remain under `build/busybox-preflight-Nyaahw/`; these scratch artifacts and debug replays are not substitute acceptance reports.

Initial runtime reports retained the failures rather than omitting the new suite. ARM64 Debug `1789471252024721984` was a missing userspace-suite allowlist/control error, not application evidence. After that fix, `1789471380312588428` reached BusyBox and failed on missing effective-UID adaptation. The strengthened libc identity test failed first in `1789471520736748445`; the real UID/GID/effective-ID adapters passed it in `1789471607468752051`, while BusyBox then failed on missing parent-PID adaptation. The kernel's effective-ID queries read the current Process's fields, not hard-coded root values. Parent-PID adaptation enabled the first `ash_exit` pass in `1789471728464458799`; the test checks the validation process's actual root-credential startup contract, not general setuid or credential-inheritance semantics.

mlibc signal sets and `sigaction` structures are now explicitly translated: its 128-byte mask uses bit `signo-1`, whereas Moss's native word uses bit `signo` for signals 1..31; `SA_ONSTACK` also has a different numeric value. Higher mask bits do not add unsupported signals. The runtime check covers action round trips, unsupported flags without disposition changes, KILL/STOP mask filtering, actual blocked `raise` delivery on unmask, handler-time masks and restoration. The first signal probe failed in `1789472168670062522`. `1789472257971956201` passed that probe but BusyBox's linked pthread initializer failed: upstream requires `ENOSYS` to opt out of its reserved cancellation signal. The adapter now explicitly rejects that unimplemented signal with `ENOSYS`; ordinary unsupported signal numbers/flags are still rejected, and pthread cancellation is not claimed. Both probes passed in `1789472617928823899`.

The pipe adapters convert Moss's two native longs into POSIX's two ints; the libc regression guards the adjacent word, verifies dup/dup2, byte transfer, EOF, invalid descriptors and rejection of unsupported pipe flags. Non-character files are identified as non-terminals from real `fstat` data; character-device terminal queries still return `ENOSYS`. `1789472831381884797` retains the initial missing-pipe failures. After these adapters, `1789472943858275708` passed libc but ash command substitution failed. A native GDB replay captured BusyBox printf calling `fcntl(fd=1, F_GETFL=3)` before output (`fcntl-debug.log`). Moss now appends `SYS_FCNTL=130`, preserving existing syscall numbers, and returns status/access flags from the actual open File. Tests check read/write/duplicate descriptors, invalid fds, rejection of unsupported commands and preservation of flags. Descriptor flags, `F_SETFL` and close-on-exec remain unimplemented.

With F_GETFL implemented, `1789473355519698501` exposed a real fork defect: the shell child's first new allocator mapping failed. GDB replay `mmap-debug.log` captured PID 4 calling anonymous/private mmap with valid length 524288, protection 3, flags 34, fd -1 and offset 0, but its address-space `mmap_next` and `brk_current` were both zero; the kernel returned `EINVAL` for address zero. Fork copied VMAs/page tables but omitted their allocation cursors. The maintained libc regression now explicitly maps, checks zeroed bytes and unmaps new memory after fork, and checks the inherited program break; it failed before the repair in `1789473585939158413`. Fork now copies `mmap_next`, `brk_base` and `brk_current` with the cloned address space. The regression and real ash substitution passed together in `1789473750240658022`. No allocator size, application assertion or timeout was relaxed. Debug QEMU children were individually terminated/reaped; no instrumentation was added to production code.

After these repairs, all six builds and **22/22 CTests** passed: 19 functional/framework/production/controlled-console checks plus the three Release benchmark smoke runs. Each finalized functional report has **20 suites and 91 passing cases**, no unrun cases, and 1000 complete core lifecycle cycles with 11 matching resource checkpoints. The final shell substitution case additionally checks its stdout bytes in the validation parent, beyond the earlier shell-internal comparison. All six reports record build-time source SHA-256 `374dc478c742568afd26e3269fcf63dcef109aaeaa39112f24bd0c3c3a96787f` and frozen image/fixture inputs. This fingerprint predates this final results paragraph; no code changed after that matrix. All six BusyBox ELFs were checked as static EXEC images without an interpreter or needed dynamic libraries; RV64 declares soft-float-compatible `rv64imac` attributes without F/D. The paired Debug/Release userspace artifacts intentionally use the same target application flags; the kernel configurations remain distinct.

| Preset | Functional Report | Framework Report | Production Boot Run | Benchmark Smoke Report |
| --- | --- | --- | --- | --- |
| arm64-debug | `1789474365423174593` | `1789474544190894893` | `run-28zjrnfv` | — |
| arm64-release | `1789474365476873267` | `1789474530829721620` | `run-he2_19hq` | `1789474577693064530` |
| x64-debug | `1789474365421529492` | `1789474553989297475` | `run-3sj2pdny` | — |
| x64-release | `1789474365445379663` | `1789474533100762031` | `run-4aho8ylz` | `1789474582446348767` |
| riscv64-debug | `1789474365436351869` | `1789474554103296185` | `run-7j3puxd8` | — |
| riscv64-release | `1789474365498711307` | `1789474530829721900` | `run-3ja2miqb` | `1789474582043000589` |

ARM64 Debug's controlled console check passed in `run-u_kblk5k`. The host suite passed **189 tests** (`build/busybox-build-qHYXkz/host-tests.log`); changed C/C++, CMake and Python files passed format/lint checks and `git diff --check`. Build/CTest logs are `build/busybox-build-qHYXkz/final-<preset>-{build,ctest}.log`. The Release benchmark runs exercise the existing measurements, but were not calibrated baseline/candidate comparisons in the fixed performance-gate environment and do **not** establish a regression threshold or performance acceptance.

At that snapshot the complete selected file workflows, exec environment/argument preservation and transactional failure handling, terminal/futex support and other required kernel contracts were still incomplete. The shell logs still reported missing getcwd/character-terminal support. The routine raw kernel lifecycle cycle remains separate from a complete BusyBox application lifecycle. Final long-run and calibrated performance gates remain open, with network and persistent storage excluded.

### Static Exec Arguments, Environment and Preparation Rollback (2026-09-15)

The production exec path now snapshots argv and envp, validates the static ELF profile, prepares an independent address space and writes its complete startup stack through kernel physical mappings before committing. Preparation failure destroys only the new image; the old image, process name and signal state are not replaced early. At commit the hardware switches to the new root before ownership transfer destroys the old root. ARM64 ASID release already broadcasts invalidation before reuse; RV64 flushes on SATP switches and x64 uses CR3 without PCID.

The native contract allows **128 combined argument/environment strings and 16 KiB including their terminators**. Empty vectors and exact limits are supported; excess returns `E2BIG`, and bad user pointers return `EFAULT`. The C entry registers remain native Moss argc/argv and now envp; the stack also contains `argc, argv..., NULL, envp..., NULL, AT_NULL, 0`. Static mlibc consumes this vector directly, replacing its earlier local 16-argument/empty-environment startup shim. No dynamic loader or Linux binary compatibility was added. The static profile accepts at most 64 program headers and page-separated LOAD ranges; it rejects invalid header sizing/file extents, unsupported dynamic/interpreter segments, W+X, reserved stack/heap/signal collisions, LOAD page overlaps and an entry outside executable LOAD memory. This is bounded profile validation, not general ELF conformance.

Two real pre-repair failures are retained: ARM64 Debug `1789475178368791743` failed the strengthened libc environment re-exec check, and `1789475387675938273` failed rejection of a fixture with ELF entry zero. Both passed after preparation/commit and startup adaptation in `1789475827166022110`, together with the existing BusyBox probes. The new `users.exec` suite then added 12 cases: three malformed ELF fixtures, bad env vector/string pointers, argv count/shared count/string-byte overflow, exact combined count, exact string bytes, empty vectors and real allocation rollback. The fixture host test verifies the original child remains intact and each malformed image contains only its specified mutation.

The first exact-count test failed in `1789476150745428683` because it incorrectly expected mlibc's public `environ` to retain 64 duplicate names. The pinned upstream startup inserts entries through `putenv`, which coalesces those names. The fixture was corrected to 64 distinct variables, retaining the exact **64 argv + 64 envp** requirement; no kernel limit or assertion was relaxed. `1789476424547001010` passed all 12 exec cases, libc and the three BusyBox cases on ARM64 Debug. The new `ash_exec_environment` exports a value with spaces and externally execs `/busybox.elf ash`, where the value and exit status are checked.

The allocation case exhausts the real PFA using the existing owned-page fixture, exposing zero through four available pages on four-level targets (zero through three on three-level targets). Each failed exec must return ENOMEM, keep its old address-space identity/root hash and process name, preserve the user's stack canary and open-file offset, and return all temporary heap/page resources. The existing process/thread/mapped-page/descriptor/reference snapshot must match after release. A subsequent real fork/exec must succeed. These checks cover the root, stack and intermediate-page-table allocation stages; they do not claim exhaustive heap-failure, arbitrary-ELF, multithreaded-exec or executable-backing-lifetime coverage.

The finalized six-configuration matrix passed all builds and **22/22 CTests**: functional/framework/production checks, the ARM64 Debug controlled-console check and three Release benchmark smoke runs. Every functional report contains **21 suites and 104 passing cases**, no unrun cases, 1000 complete core lifecycle cycles and 11 matching resource checkpoints. All six record build-time source SHA-256 `b0af2fecbf636939511f9bc69fd903e39a279a8c6e164e87b80efe75e4e62efa`; the current documentation updates followed those builds, with no later code changes.

| Preset | Functional Report | Framework Report | Production Boot Run | Benchmark Smoke Report |
| --- | --- | --- | --- | --- |
| arm64-debug | `1789476525656949807` | `1789476713898372305` | `run-gzsz_lwk` | — |
| arm64-release | `1789476525661493339` | `1789476710050438268` | `run-3ut88at1` | `1789476755284697629` |
| x64-debug | `1789476525653353595` | `1789476720441332221` | `run-v32glx2m` | — |
| x64-release | `1789476525665059180` | `1789476710051066048` | `run-m0utjdto` | `1789476759930844409` |
| riscv64-debug | `1789476525682786018` | `1789476726198432150` | `run-12wpe2a9` | — |
| riscv64-release | `1789476525689977401` | `1789476710051101478` | `run-mf0a1473` | `1789476760410432210` |

ARM64 Debug console input passed in `run-5rogyqc8`. The host suite passed **189 tests**; changed C/C++ and Python files passed formatting/lint checks and `git diff --check`. Logs are `build/exec-validation-Rl7UeX/final-<preset>-{build,ctest}.log` and `host-tests.log`; the first environment red build is `build/exec-env-red-build.log`. Each Release smoke report passed 13 scenarios, but these runs had no calibrated baseline/candidate comparison or fixed vCPU host binding and are not performance acceptance. Complete selected file workflows, getcwd/terminal and other required interfaces, remaining core regressions, final six-configuration long runs and calibrated performance gates remain open. Networking and persistent storage remain excluded.

### BusyBox Text Pipeline and Mutable Directory Lifetime (2026-09-15)

The pinned BusyBox **1.37.0**, statically linked with mlibc and native Moss sysdeps, now runs five application probes. `text_pipeline` runs real `cat | grep | wc -l` processes under ash, checks `pipefail` and the numeric result, and returns exact captured output. `directory_lifecycle` creates two sibling directories with `mkdir`, lists them with `ls -1`, removes the tree with `rm -rf`, and checks that its pathname is gone. All probes check child exit status as well as stdout. The minimal pinned ls configuration has sorting disabled: it displays the two names in reverse enumeration order; the test does not assume alphabetical sorting.

Production VFS now provides native stat/lstat, mkdir, rmdir and directory enumeration. Stat has a **48-byte native layout**, carrying actual inode ownership, link count and distinct ramfs/devfs identities. Reserved bytes are explicitly zero-initialized. Native getdents **131** returns one fixed **280-byte record**, or zero at EOF; the mlibc adapter checks the dirent prefix and type mapping at compile time. Failed buffer validation/copyout does not advance the shared file cursor. Stable inode-based cookies avoid skipping the next sibling when deletion compacts the child array. This bounded implementation has no hard links; hard links would require per-dentry cookies.

Directory mutation and lookup/retention share a namespace lock. Open files retain their dentry and inode; dentries retain their parent. Removing a directory unpublishes its name and sets its link count to zero, while a held FD still sees the original inode. The final reference returns the inode/dentry slots to reusable pools. The directory cache handles removed slots without stopping collision lookup early. Inode and file pools also have allocator locks. This does not by itself establish all shared-FD/SMP lifetime contracts; controlled concurrent directory/descriptor interleavings remain to be completed.

New tests exercise actual production objects, not a substitute filesystem:

- The static libc runtime performs **300 directory lifecycles**, exceeding the 256-slot pools. It checks metadata, duplicate creation, nonempty deletion, held-FD identity after unlink, distinct identity after pathname reuse and restoration of the root link count.
- Its directory reader checks invalid and high-word FDs, regular-file rejection, undersized/negative sizes, null and cross-VMA output buffers, retry after EFAULT, shared dup cursors, deletion during traversal, repeated EOF and the devfs character-device entry through real `opendir/readdir`.
- `vfs.directory_capacity` fills all **64 children**, then checks **300 failed creates** and **300 failed opens with a full FD table**. Actual inode/dentry/file pool occupancy and retained reference counts must not change on failure. It also checks duplicate/nonempty failures, unlinked-but-open ownership, pathname reuse and exact final pool/link-count recovery. Directory write modes return EISDIR; access mode 3 returns EINVAL.

Original failures and fixture corrections are retained under `build/arm64-debug/validation/`:

| Report | Evidence |
| --- | --- |
| `1789477171993792229` | BusyBox mkdir failed with missing Mkdir sysdep. |
| `1789477840957144131` | The libc directory lifecycle failed with missing rmdir support. |
| `1789478232003074652` | BusyBox ls failed with missing OpenDir support. |
| `1789478557728150331` | Accidental run after a failed build; stale artifacts, not getdents acceptance. The build failed on the required switch default. |
| `1789478890423400124` | Two-sibling rm left a child behind and returned ENOTEMPTY; array-index cursor defect. |
| `1789478971299715546`, `1789479048976882988` | Removal succeeded after the cursor repair, but the test incorrectly expected sorted ls output. Diagnostic output confirmed the pinned unsorted profile. |
| `1789479191656952180` | The boundary fixture attempted unsupported partial munmap. It was corrected to unmap a separate complete VMA, retaining the cross-boundary EFAULT assertion. |
| `1789479473302724717` | Native open incorrectly accepted directory write modes and invalid access mode 3. |

Focused ARM64 Debug report `1789479398082479989` passed VFS, libc, BusyBox and the existing core lifecycle after the enumeration and capacity work; the later full matrix includes the access-mode repair. Logs are retained in `build/busybox-files-H8qLGi/`.

The finalized six-configuration matrix passed every build and **22/22 CTests**. Each functional report contains **21 suites, 107 passing cases, no unrun cases**, 1000 complete core lifecycle cycles and 11 matching resource checkpoints. All six record build-time source SHA-256 `ceca855282b7ff4b8adff9f585df74e72d4d76228385b80e9845352f042408bd`; these documentation updates followed the builds, with no later code changes.

| Preset | Functional Report | Framework Report | Production Boot Run | Benchmark Smoke Report |
| --- | --- | --- | --- | --- |
| arm64-debug | `1789479536768487260` | `1789479605704184406` | `run-3dlm494r` | — |
| arm64-release | `1789479631267085961` | `1789479793163805029` | `run-xkcvo19e` | `1789479836554960598` |
| x64-debug | `1789479632449783763` | `1789479818651719282` | `run-6dil1mbq` | — |
| x64-release | `1789479633849104190` | `1789479797292340360` | `run-otpyha24` | `1789479845471480031` |
| riscv64-debug | `1789479635062727775` | `1789479818744690693` | `run-zne_ex52` | — |
| riscv64-release | `1789479636011397554` | `1789479799182240104` | `run-4abioxk3` | `1789479845471479931` |

ARM64 Debug controlled console input passed in `run-ewcd7ooa`. The host suite passed **189 tests** (`final-host-tests.log`); changed C/C++ and Python passed format/lint checks and `git diff --check`. Build and CTest logs are `final-<preset>-{build,ctest}.log` in the same directory. Each Release benchmark smoke report passed 13 scenarios. These runs overlapped other validation activity, had no fixed vCPU host binding or calibrated baseline/candidate comparison, and are **not performance acceptance**.

This was a mutable-directory and text-pipeline slice, **not complete selected-application acceptance**. At this snapshot, regular ramfs file data was still read-only: creation/redirection, writable `cp/mv/rm` workflows, working-directory and general dot/dot-dot/mount traversal remained open. The writable-file follow-up is recorded below. Whole-VMA-only munmap, character-terminal/futex support and other required interfaces also remain incomplete. The routine 1000-cycle raw core workload is not 1000 complete BusyBox lifecycles. Final six-configuration long runs and calibrated baseline/candidate performance gates are still required. Networking and persistent storage remain excluded.

### Writable Files, Redirection and Executable Backing Ownership (2026-09-15)

BusyBox **1.37.0**, still statically linked against the pinned mlibc with native Moss sysdeps, now passes seven application probes. New `file_redirection` checks creation, append, truncation, exact `cat` output and removal. New `file_copy` copies real file contents with `cp`, checks the destination bytes with `cat`, removes both files with `rm` and checks that both names are absent. These are within one boot; they do not exercise persistent storage. `mv` and complete selected-application lifecycle acceptance remain open.

The shared ramfs/VFS implementation now supports mutable regular-file creation, writes, append, truncation and unlink. It zero-fills holes, rejects signed file-size overflow, commits only the successfully copied prefix, and leaves size/cursor/allocation ownership unchanged when a growing write copies no bytes or allocation fails. Inode-owned buffers are freed on the final reference, so an unlinked open file retains its original data while pathname reuse produces a distinct inode. CPIO-backed original files remain immutable. Access modes are enforced by the shared read/write entrypoints. The native open path checks descriptor capacity before creation/truncation; this is a single-table-thread preflight, not a concurrent reservation guarantee.

Descriptor flags now support `F_DUPFD`, `F_DUPFD_CLOEXEC`, `F_GETFD`, `F_SETFD` and `F_GETFL`. Close-on-exec is per descriptor, inherited by fork, cleared by dup2 replacement, and applied only after successful exec commit. Failed exec leaves the flags and open descriptors intact. A mutable executable is copied into an address-space-owned immutable snapshot before ELF validation/preparation; fork shares ownership of that snapshot. Later overwrites, truncation or unlink of the original file cannot corrupt lazy executable backing. Immutable CPIO executable backing is still borrowed.

Regression-led repairs also cover two shared boundaries:

- Pinned mlibc's stdio flush attempted a zero-distance seek after ash reused a cached file-like FILE across redirection to a pipe. The real seek correctly returned ESPIPE, but the resulting FILE error changed ash builtin exit status even when unlink/stat were correct. An exact, idempotent pinned-source adaptation skips only unnecessary repositioning. The regression also checks input-buffer repositioning and that an explicit pipe lseek still returns ESPIPE.
- Ramfs seek now checks addition overflow before changing the cursor; native lseek validates the full-width whence before narrowing. Native open validates mode only with O_CREAT: a two-argument open must not inspect a residual third argument register. Creation still rejects invalid full-width mode values before creating a name.

New tests use the production implementation and real userspace runtime:

- `vfs.writable_lifecycle` checks metadata, exclusive creation, shared dup offsets, sparse zero-fill, read/write access modes, append, complete and partial user-copy failures, allocation failure, seek/write overflow and unchanged failure state. A full descriptor table cannot create or truncate a file. Unlink with held FDs, pathname reuse and truncation through another FD retain correct ownership. **1000 complete create/write/unlink/read-held-file/close cycles** must restore exact heap-byte and inode/dentry/file pool occupancy after every cycle.
- The libc descriptor test checks minimum-FD allocation, full-width and invalid arguments, dup2 flag behavior, fork inheritance, failed-exec preservation and successful-exec closure. Its stream test covers file-to-pipe FILE reuse and repeated flushes.
- The libc executable test copies and executes its own image from ramfs, overwrites the existing backing bytes, truncates and unlinks the file, then forks and checks previously untouched read-only canary pages and TLS. This checks lazy backing ownership across fork, not every metadata-OOM or parent-exit interleaving.
- The libc fixture keeps `libc_validation.unstripped.elf` for host diagnosis; only DWARF is stripped from the packaged ELF. The initial roughly 7.9 MB debug-heavy file exceeded the current contiguous-buffer/8 MiB heap budget during copying. ARM64's packaged fixture is roughly 850 KB. All six stripped/unstripped pairs have identical entry point, LOAD and TLS program-header records.

Original failures remain in their report directories; scratch logs are under `build/busybox-write-A8CUsm/`:

| Report | Evidence |
| --- | --- |
| `1789480209004877026` | BusyBox redirection failed because writable open/create was unsupported. |
| `1789480637159894314` | ash required F_DUPFD_CLOEXEC; descriptor support was incomplete. |
| `1789481091834637060` | cp supplied source mode including file-type bits; native open rejected it. Creation now stores supported permission bits. |
| `1789481206217812621` | cp/cat succeeded, but rm required missing Unlinkat support. |
| `1789481308655666264`, `1789481474421897319` | rm returned success, but ash's following absence check failed. |
| `1789481609486819463`, `1789481678047508883` | The first diagnostic used suppressed info logging; the second proved unlink returned zero and stat returned ENOENT, while stdio produced ESPIPE. Temporary kernel diagnostics were subsequently removed. |
| `1789482147853718242` | Independent libc file-to-pipe flush regression failed with exit 143 before the shared stdio repair. |
| `1789482422397247606` | Two seek-overflow assertions failed; the executable-copy fixture also failed before its DWARF-size correction. |
| `1789482774819270581`, `1789482774807836286`, `1789482774856713027`, `1789482774830482596`, `1789482774879199027`, `1789482774889739742` | First full matrix: the old 256-byte unterminated-path fixture no longer covered the expanded 1024-byte native path limit. It was resized while retaining ENAMETOOLONG. RV64 also exposed the unused-open-mode register bug in device setup. These are failed/incomplete results, not accepted matrix runs. |
| `1789483148381801549` | Explicit non-creating open with invalid-looking unused mode reproduced the mode-register bug on ARM64 (exit 140); the corrected user-range fixture passed. |

Intermediate passing reports include `1789480842684338736` (redirection), `1789481032171309674` (descriptor flags), `1789482225054528674` (stdio/copy), `1789482488159731473` (writable VFS/seek) and `1789482562060172558` (executable backing). They establish only their selected workloads and earlier source snapshots. An early writable-exec build failed on a stale inode-size reference and is preserved in `redirection-build.log`; it was repaired before running the corresponding fixture. The first full matrix logs are `first-matrix-<preset>-{build,ctest}.log`.

The final frozen source passed all six builds and **22/22 CTests**. Every functional report contains **21 suites, 110 passing cases, no unrun cases**, 1000 complete core lifecycle cycles and **11 equal resource checkpoints**. All six record build-time source SHA-256 `c3336c762b2f0e721928113b3b61adce3b4769c6853d4a4142abcf7365e337ed`. This documentation update followed those builds; no later code changes were made.

| Preset | Functional Report | Framework Report | Production Boot Run | Benchmark Smoke Report |
| --- | --- | --- | --- | --- |
| arm64-debug | `1789483228717065573` | `1789483412317364045` | `run-zom43kl6` | — |
| arm64-release | `1789483228715610683` | `1789483391539509890` | `run-7j50l947` | `1789483431591585466` |
| x64-debug | `1789483228731744200` | `1789483416426478057` | `run-ds_yuuy9` | — |
| x64-release | `1789483228729584229` | `1789483404314739815` | `run-v91wux9u` | `1789483450324487329` |
| riscv64-debug | `1789483228772069618` | `1789483404167012370` | `run-2zwynw0b` | — |
| riscv64-release | `1789483228751700729` | `1789483394216612991` | `run-r6piisyf` | `1789483438909840234` |

ARM64 Debug console input passed in `run-mquvgvum`. The host suite passed **189 tests** (`fixed-host-tests.log`). Changed C/C++, CMake and Python passed format/lint checks and `git diff --check`. Final logs are `final-<preset>-{build,ctest}.log`. Each Release benchmark smoke passed 13 scenarios, but runs overlapped other validation activity and lacked fixed vCPU host binding and calibrated baseline/candidate gates: **these are not performance acceptance**.

Remaining limits are explicit: contiguous ramfs buffers and executable snapshots consume the bounded kernel heap; the namespace lock serializes file I/O; File reference counting and shared-FD installation/closure still need controlled SMP lifetime closure. Fallible file-buffer allocation does not close existing panic-on-metadata-allocation paths. Rename/mv, working-directory/path traversal semantics, access/terminal/futex and other profile-required interfaces remain unfinished. The new VFS loops and existing raw core loops are not 1000 full BusyBox lifecycles. Complete app workflows, remaining core concurrency/rollback obligations, each configuration's single-kernel **30-minute and 10000-cycle** long run, and calibrated Release performance gates remain required. Networking and persistent storage remain excluded.

### Rename Ownership and Combined Application Workflow (2026-09-15)

The pinned BusyBox/mlibc port now implements native `rename` **53** through mlibc's `Rename` sysdep. The initial call-chain hypothesis mentioned Renameat, but inspecting the pinned stdio implementation and the actual failure identified Rename as the required entry. Renameat and directory-FD-relative operations have not been added by this slice.

Ramfs reuses the source dentry instead of copying the file. Under the existing namespace lock, it validates source membership, type compatibility, destination capacity and directory ancestry before changing entries. Its commit phase requires no allocation or user copying. It preserves source inode identity and metadata, updates parent references/link counts and cache keys, and drops the overwritten target's namespace ownership while held FDs retain that target's old inode/data. Moving a directory preserves its child entries. Failure/type/ancestry requirements are drawn from the selected [POSIX rename contracts](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html); this is not a claim of complete POSIX or filesystem permission conformance.

The tests now include:

- `vfs.rename_lifecycle`: **1000 complete cycles** creating and populating two files, overwriting the destination across directories, verifying source identity and both open FDs' distinct original data, checking same-path rename, then unlinking and closing everything. Each cycle restores exact heap-byte and inode/dentry/file pool occupancy.
- `vfs.rename_boundaries`: directory subtree movement and empty-target replacement with an open target FD; parent link counts; rejection of ancestry cycles, nonempty targets, file/directory mismatches, mount roots, cross-filesystem moves, trailing slashes, final dot/dot-dot, missing names and non-directory path prefixes. A directory filled to **64 children** rejects **300 cross-directory insertions** without losing the source. Same-directory renaming and replacement still succeed at capacity. Cleanup restores the original pools and heap usage.
- The libc runtime checks both user pathname buffers before mutation, rejects unreadable/unterminated names and 256-byte components, and accepts an exactly 255-byte component. A pipe-coordinated parent/child test uses CPU 0/CPU 1 affinity: after the parent closes its old-target FD and replaces the pathname, the child alone retains and reads the removed target, while a fresh open sees the source inode and new bytes. This verifies a controlled ownership handoff, not arbitrary simultaneous File refcount updates.
- BusyBox `file_rename` checks cross-directory file movement, forced overwrite, directory movement, exact output and cleanup. `application_workflow` combines all nine selected applets: ash creates input through redirection, mkdir/cp/mv prepare the files, cat/grep/wc verify the pipeline result, ls checks the destination, and rm removes the directories. All **nine application probes** check child exit status and exact captured stdout. The combined workflow is currently one complete run per functional guest, not 1000 application lifecycles.

Original ARM64 Debug evidence is retained under `build/arm64-debug/validation/`:

| Report | Evidence |
| --- | --- |
| `1789483876751884555` | Real BusyBox mv failed with missing Rename sysdep/ENOSYS. |
| `1789484248333222330` | Basic mv, existing libc and VFS passed after implementation. |
| `1789484504905688427` | All 1000 rename cycles passed, but one boundary assertion failed: an intermediate non-directory component was incorrectly reported as ENOENT. |
| `1789484657412243884` | The shared path walker gained optional failure detail; rename/create parent lookup now preserves ENOTDIR. VFS, libc and BusyBox passed. |
| `1789484812703654899` | Cross-CPU retained-target ownership, combined application workflow, VFS, raw userspace and exec probes passed. The final matrix also includes the later exact component-length checks. |

The first boundary-test build used a braced range requiring unavailable `std::initializer_list`; it was corrected to fixed arrays, with the failed build retained as `rename-boundary-red-build.log`. No runtime was accepted from that failed build.

The final six configurations passed every build and **22/22 CTests**, with **21 suites, 114 passing cases and no unrun cases per configuration**. Each existing core lifecycle guest completed 1000 cycles with 11 identical resource checkpoints. All six report build-time source SHA-256 `94f1e526c14392e07901ec26048d92f1ef9d888622b5503312166a2eb27c44b5`; this documentation was updated afterward without further code changes.

| Preset | Functional Report | Framework Report | Production Boot Run | Benchmark Smoke Report |
| --- | --- | --- | --- | --- |
| arm64-debug | `1789484941814851746` | `1789485133987590289` | `run-y9z_sasy` | — |
| arm64-release | `1789484941807919063` | `1789485105689980798` | `run-zipmih24` | `1789485150615391833` |
| x64-debug | `1789484941814292726` | `1789485124812822062` | `run-a30koliz` | — |
| x64-release | `1789484941843106459` | `1789485105689213707` | `run-y1evj5yy` | `1789485150751622113` |
| riscv64-debug | `1789484941842527829` | `1789485115389562826` | `run-6kt7bw71` | — |
| riscv64-release | `1789484941840378438` | `1789485105689733917` | `run-ehnvpot6` | `1789485144676735674` |

ARM64 Debug console input passed in `run-bguzqzxk`. The host suite passed **189 tests**. Changed C/C++ and Python passed formatting/lint checks and `git diff --check`. Logs are `build/busybox-rename-oOzGlS/final-<preset>-{build,ctest}.log` and `final-host-tests.log`. Each Release benchmark smoke passed 13 scenarios, but overlapped other validation activity and had no fixed vCPU host binding or calibrated comparison: **not performance acceptance**.

Remaining profile work includes working-directory/general path traversal, access/permission and terminal/runtime interfaces, shared-FD/refcount concurrency and metadata-allocation failure closure. Repeated full BusyBox lifecycle acceptance, remaining six-path obligations, each configuration's single-kernel **at least 30 minutes and 10000 cycles**, and calibrated Release performance gates remain open. These results do not establish that the full goal is complete. Networking and persistent storage remain excluded.

### Cross-Table File Reference Ownership (2026-09-15)

The shared File counter now uses the existing atomic implementation. Allocation creates one caller-owned reference; open/pipe installation takes its descriptor reference and releases the temporary owner on both success and failure. The atomic decrement itself identifies the sole `1 -> 0` closer. No closer performs a subsequent count load that could observe a different closer's zero and finalize the endpoint twice. File/inode/dentry cleanup still uses the existing release path and namespace/pool locks.

The new `vfs.smp/shared_references` regression reuses the existing two-process validation driver, observing actual CPU 0/CPU 1 execution. Two independent FD tables each retain 64 descriptors for one real pipe writer. For **1000 cycles**, both CPUs clone their tables, join before checking 256 references, close the clones concurrently, then join before checking 128 references. Each cycle verifies a real byte round trip and exact heap/pool recovery; the final concurrent closure checks EOF and restoration of the original inode/dentry/file pool occupancy. The peer stays alive until resource checks finish. A failed ownership assertion terminates the failed guest without trying to release potentially corrupted objects.

These barriers align contention and make observations quiescent; they do **not** force every instruction-level interleaving. The 1000 cycles are complete cloned-FD-table lifecycles, not 1000 full BusyBox executions. Each passing run records **5136 assertions**. The host rejects a one-CPU selection before loading artifacts; the minimum two-CPU ARM64 Debug run also passed in `1789486176221150581`.

Preserved ARM64 Debug evidence under `build/arm64-debug/validation/`:

| Report | Evidence |
| --- | --- |
| `1789485860789844203` | New regression failed after concurrent close, with 134 passing assertions and one failure. |
| `1789485927470164194` | Diagnostic repeat failed on cycle index 1: expected **128** retained references, observed **144**, directly exposing lost decrements. |
| `1789485980719515180` | Atomic ownership repair passed the new SMP case, VFS, raw userspace, libc, BusyBox and core lifecycle workloads. |

The initial test-only build lacked a forward declaration for `end_case`; `ref-red-build.log` retains that compile failure. It was corrected before either failing runtime report, without changing production reference counting. The later diagnostic test adds the observed/expected counts without weakening the assertion.

The final source snapshot passed all six builds and **22/22 CTests**. Each configuration has **22 suites, 115 passing cases, no unrun cases**, and 1000 raw core lifecycle cycles with 11 identical resource checkpoints. Every functional report records source SHA-256 `14dff27c9a84fc68860439eed76f323327f13c972acf932a82a55408ed404e6f`; only documentation changed after these tests.

| Preset | Functional Report | Framework Report | Production Boot Run | Benchmark Smoke Report |
| --- | --- | --- | --- | --- |
| arm64-debug | `1789486081780943416` | `1789486252358509243` | `run-5wfespyq` | — |
| arm64-release | `1789486081760011667` | `1789486241555898089` | `run-0_og2ejk` | `1789486287108743401` |
| x64-debug | `1789486081765671169` | `1789486265643850303` | `run-127kpksa` | — |
| x64-release | `1789486081765508789` | `1789486242040770783` | `run-h4ec8d9c` | `1789486280029208268` |
| riscv64-debug | `1789486081783562147` | `1789486251106831301` | `run-7mjfr3i5` | — |
| riscv64-release | `1789486081790935260` | `1789486241553715538` | `run-d324qpml` | `1789486283094934800` |

ARM64 Debug console input passed in `run-yh89nn13`. Host tests passed **190/190**; changed C/C++ formatting, Python lint/format and `git diff --check` passed. Logs are retained under `build/file-references-Vz0BsO/`, including `ref-*-{build,test}.log`, `final-<preset>-{build,ctest}.log` and `final-host-tests.log`. All final-matrix jobs reached terminal success before documentation was updated. Each Release benchmark smoke passed 13 scenarios, but overlapped other testing, had no fixed host vCPU binding and did not perform a calibrated baseline/candidate comparison: **not performance acceptance**.

This repairs cross-table File reference ownership, not concurrent mutation of a single FdTable. Its slots and borrowed `get_file()` results remain unsynchronized; the one-mutator ceiling is explicit in the source. Metadata-allocation failure closure, remaining path/permission/runtime contracts, 1000 full BusyBox lifecycles, each configuration's single-kernel **at least 30 minutes and 10000 cycles**, and calibrated Release performance gates remain required. The overall goal is still active. Networking and persistent storage remain excluded.

### Repeated Real-Application Lifecycles (2026-09-16)

`users.applications/core_application_recovery` now executes **1000 full BusyBox workflows** in one kernel lifetime, after one excluded warmup. Each cycle reuses the existing six-path core lifecycle and then forks/execs/reaps a fresh ash. The shared application script runs all nine selected applets, verifies pipeline/file/directory results, removes its temporary namespace, and checks exact stdout plus exit status 37. This is not one shell looping internally. The separate application counter advances only after successful workflow completion; kernel and host require it to equal the complete core-cycle count.

Every 10 cycles, both sides check exact heap bytes, free physical pages, process/thread counts, user/stack pages, descriptors, File references and VFS inode/dentry/file pool occupancy. Missing fields, unmatched application counts, malformed/reordered progress and any changed resource field cannot pass. Host tests first exposed six missing-evidence cases that the old parser accepted (`protocol-red.log`); the expanded protocol and resource validation now reject them. Routine application runs require 101 checkpoints. The original `users.lifecycle` remains a separate 1000-cycle core-only workload with its unchanged 30 s total case deadline and 11 checkpoints.

The first development experiment inserted the application into the old core case and hit its 30 s deadline: ARM64 Debug report **`1789486799538741250`** remains an error, not an accepted run. Logs showed repeated workload startups rather than a proven deadlock. They also exposed an existing false-positive risk: scripts ignored `set -o pipefail` failure by continuing after a semicolon. Changing that failure to stop the script made both BusyBox and lifecycle tests fail in **`1789487029929062960`**. Inspection of the pinned source and generated config identified the disabled upstream `ASH_BASH_COMPAT` option, which gates pipefail. The profile now enables it, and `text_pipeline` verifies that an upstream exit 7 propagates through a successful downstream cat. The BusyBox-only repair passed in **`1789487261763805156`**. No additional external applets, dynamic linking or Linux binary ABI were introduced.

Repeated applications have their own routine **`moss-applications` CTest**, included by `test-kernel`. The original functional/framework/startup/benchmark budgets are unchanged. Its 30 s no-progress watchdog advances only on valid checkpoints; its default total budget is 101 intervals × 30 s plus 30 s startup = **3060 s**, and CTest allows **3120 s** including cleanup/reporting. A deliberately short, explicitly configured 30 s observation run **`1789487630786581952`** stopped at the total deadline after matching 0/10/20-cycle resource checkpoints; it remains incomplete evidence. Host subprocess tests exercise continuing progress beyond a case-sized duration, a stalled guest, a short total deadline, and the unchanged two-clock/host-release stability requirements. One expanded host fixture initially failed while constructing a large inline subprocess script; it now generates its checkpoint stream inside the child instead (`separate-protocol-tests.log`, `watchdog-tests.log`).

All six final builds and **28/28 CTests passed**, with **234/234 host tests**. For each configuration, the original functional report contains **22 suites and 115 passing cases**, and the additional application report contains one passing case: **23 suites/116 cases in aggregate**, with no unrun cases. Each application guest completed **1000 core cycles and 1000 application cycles**, with **101 identical resource checkpoints**. All final validation reports use source SHA-256 `21ebd2741a21229a867504c5d068f11973706e302c702cba877cb24359e6ec9a`. Documentation changed only after all final jobs were terminal.

| Preset | Functional Report | Application Report | Application Case Seconds | Framework Report | Production Boot Run | Benchmark Smoke Report |
| --- | --- | --- | --- | --- | --- | --- |
| arm64-debug | `1789487899611333094` | `1789488075227532864` | 1126.23 | `1789489211678252463` | `run-edygmffe` | — |
| arm64-release | `1789487899621228558` | `1789488057213566778` | 832.21 | `1789488900000290220` | `run-nczv0e_8` | `1789488940636137923` |
| x64-debug | `1789487899585786313` | `1789488082644067855` | 1097.93 | `1789489187298002439` | `run-pe9qrq3f` | — |
| x64-release | `1789487899642315468` | `1789488057212843738` | 802.13 | `1789488870186018590` | `run-mczfwz14` | `1789488907027409189` |
| riscv64-debug | `1789487899614519086` | `1789488075228708694` | 1148.73 | `1789489229546804994` | `run-psu7e34g` | — |
| riscv64-release | `1789487899620528018` | `1789488057212917498` | 764.76 | `1789488832754163869` | `run-8iwgfivb` | `1789488872821302432` |

ARM64 Debug console input passed in `run-4nr1c9qm`. Logs, including original failures and incomplete runs, are under `build/application-cycles-utJkGF/`; final jobs use `final-<preset>-{build,ctest}.log` and `final-host-tests.log`. C/C++, Python and CMake formatting/lint checks and `git diff --check` passed. Each Release smoke passed 13 benchmark scenarios while overlapping other tests, without host vCPU binding or a calibrated baseline/candidate comparison: **not performance acceptance**. The application durations above are observed workload runtimes under that concurrent environment, not latency guarantees.

These runs do not close working-directory/general path, permission, terminal or metadata-allocation failure contracts. In particular, getcwd/terminal/stdio diagnostics remain on stderr; selected stdout/status checks passing do not establish that these missing interfaces work or that all application error paths are covered. Shared-table FD concurrency also remains open. The required full-profile long run must select `--stability --workload users.applications` and meet **both at least 30 minutes and 10000 cycles** in each configuration; core-only `--stability` evidence is not a substitute. Those six long runs and calibrated Release performance gates remain uncompleted. The overall goal stays active; networking and persistent storage remain excluded.

### Working Directories and Relative Paths — Acceptance Incomplete (2026-09-16)

The native `chdir`/`getcwd` slots and mlibc sysdeps are now implemented. The current
one-FdTable-per-process model retains a real directory reference in that table:
fork takes another reference, successful or failed exec preserves it, and process
cleanup releases it independently of descriptors. Directory identity survives
ancestor renames and unlink; `getcwd` rejects a detached path with ENOENT, while
the retained directory and parent remain usable until their owners release them.
No success-only root-directory stub is used.

The shared VFS walk handles relative names, `.`/`..`, repeated separators,
component-length errors and non-directory intermediate/trailing components.
Boot-time mounts retain their namespace parent so `/dev/..` and `/dir/../dev`
walk correctly. Open/create, stat, mkdir, rename, unlink, rmdir and exec use the
process directory. Chdir checks the final directory and every searched ancestor
against effective UID and primary GID. The [getcwd contract](https://pubs.opengroup.org/onlinepubs/9699919799/functions/getcwd.html)
informs canonical absolute output and insufficient-buffer handling; this bounded
Moss implementation is not a claim of complete POSIX pathname conformance.

Regression evidence is retained under `build/cwd-runtime-zhEyJn/`:

- The initial root getcwd check failed in **`1789489743276908637`** and
  **`1789489759735155087`**; source inspection found no GetCwd/Chdir adaptation and
  native ENOSYS handlers. Root getcwd and existing BusyBox probes then passed in
  **`1789489999543488110`**. An intermediate compilation error using the wrong
  OutputBuffer signature remains in `root-green-build.log`.
- Relative-directory/file operations failed in **`1789490043631435106`**. The
  implementation exposed an existing rename-boundary assertion in
  **`1789490254715435140`**: the caller discarded the walk's ENOTDIR result.
  Preserving that error, without changing the assertion, passed in
  **`1789490303429126436`**.
- Buffer size, invalid user pointers, oversized paths, failed-chdir preservation,
  fork/exec inheritance, renamed and detached directories passed focused checks.
  The ownership/permission test initially produced **2000 failed permission
  assertions** in **`1789490460053818199`**. After search-access enforcement,
  **`1789490507174816733`** passed. The two-CPU userspace interleaving passed in
  **`1789490711029711428`**: CPU 1 reports its initial directory and waits; CPU 0
  renames the ancestor and releases it; CPU 1 observes the new name, changes its
  own directory and performs relative exec. Both processes verify actual CPU
  placement through the existing native TopInfo interface.

`vfs/working_directory_lifecycle` performs **1000** clone/rename/unlink/release
cycles, checks detached-parent lifetime, and restores exact pool occupancy,
heap bytes and root-directory reference count (**13000 assertions**). The shared
BusyBox workflow now uses real `cd`/`pwd`, relative redirection, cp/mv/cat/ls and
`..`, then returns to root and removes its temporary directories. All nine
selected applets and the original stdout/status checks remain required.

All six builds and functional reports passed: **22 suites/116 cases per
configuration**, no unrun cases. Host tests passed **234/234**. These reports use
source SHA-256 **`5cf7bb296daec080ccf1380a7434baf17576a6be808ea7044e32db5e4b88cb91`**.
Formatting/lint checks and `git diff --check` passed.

| Preset | Functional Report — Passed | Initial Application Report |
| --- | --- | --- |
| arm64-debug | `1789490784011930139` | `1789490871123993052` — no-progress timeout |
| arm64-release | `1789490804491637882` | `1789490967340800690` — 1000 cycles passed |
| x64-debug | `1789490803900109181` | `1789490994909898500` — 1000 cycles passed |
| x64-release | `1789490804223725384` | `1789490978738955388` — 1000 cycles passed |
| riscv64-debug | `1789490803900754291` | `1789490994059088125` — no-progress timeout |
| riscv64-release | `1789490804814940734` | `1789490973499863797` — 1000 cycles passed |

**The expanded routine application matrix has not passed.** ARM64 Debug and
RV64 Debug reached only the baseline checkpoint before the unchanged 30-second
no-progress deadline. Their logs show repeated ash startups, not proof of a
deadlock or completed cycles. Same-budget post-build repeats
**`1789491122693821913`** and **`1789491137560168940`** also timed out; the issue is
not limited to overlapping compilation. The other four application jobs remained
live, with identical observed resource checkpoints, when this record was added.
They must still reach terminal, finalized results before being counted as passes.

At the subsequent **2026-09-15 17:03 UTC** poll, x64 Release application report
`1789490978738955388` was finalized and passed all 1000 cycles with 101 matching
resource checkpoints. Its CTest process remained live in the benchmark smoke;
ARM64 Release, x64 Debug and RV64 Release application runs were still live.

A temporary first-ten-cycle diagnostic in **`1789491303073225363`** completed those
cycles with zero workflow errors and measured 1.04–1.34 seconds per cycle. It then
correctly failed resource equality because its new logging path faulted in one
additional user page. **This instrumented result is not acceptance.** The tagged
diagnostic was removed; the restored original source/hash again timed out in
**`1789491466527443519`**. Instrumentation changes the reproduction conditions;
the timeout's root cause remains unresolved. No watchdog, total budget, resource
assertion or lifecycle threshold was loosened. The four existing jobs continued
using their frozen input copies while the separate ARM64 diagnostic was built.

Remaining boundaries include these application timeouts, Uname/terminal/stdio
runtime diagnostics (getcwd diagnostics are gone in the focused BusyBox run),
general file/namespace permissions beyond chdir search checks, dirfd-relative
interfaces/fchdir, symlinks, shared-table FD concurrency and fallible metadata
allocation. The pathname buffer remains bounded to 1024 bytes; dynamic mount
changes are not implemented. Six full-profile single-kernel runs of **at least
30 minutes and 10000 cycles** and calibrated Release performance acceptance are
still required. The goal remains active; networking and persistent storage stay
excluded.

### Demand-Zero Debug Progress — Acceptance Incomplete (2026-09-16)

The four remaining application reports in the preceding table subsequently
finalized successfully: each completed 1000 core/application cycles and 101
matching resource checkpoints. Their CTest processes also exited successfully.
The ARM64/RV64 Debug failures remain retained, so that source's six-configuration
application matrix did **not** pass. Release benchmark smoke is not a calibrated
performance gate.

Diagnostics are retained under `build/application-progress-zFnxRs/`. Unmodified
frozen ARM64 Debug inputs were observed with GDB, without kernel/user logging.
The first diagnostic guest reached checkpoints 10 and 20 at 32.53 and 65.47
guest seconds; these debugger-perturbed timings are not acceptance measurements.
Other CPUs were in idle paths in the all-CPU sample, not a demonstrated deadlock.
The 80 subsequent CPU-0 samples included 26 in `try_demand_page` (24 in its
whole-page byte-zero loop and two in the backing-copy loop), eight in TLB
invalidation, and other executing paths. The corresponding userspace address
resolved to the pinned frigg slab allocator used by mlibc. `gdb-cli` startup
failed twice; direct `gdb-multiarch -readnever` avoided the startup issue. Both
failed sessions and the bounded diagnostic QEMUs were cleaned up.

The minimal kernel change clears freshly allocated, page-aligned anonymous
pages as 512 `u64` words instead of 4096 bytes. File-backed copies, permissions,
allocation rollback and TLB ordering are unchanged. The first ARM64 interval
then took 20.45 seconds in the unchanged runner; RV64 took 21.58 seconds. ARM64
report **`1789492596288994547`** and RV64 report **`1789492669251306739`** use source
**`4ce05afd82a3c6a6eb219c0e0ba389961d6096a3b9ee1faf2608b7aa44eae11b`**. The ARM64
initramfs SHA-256 remained identical to the original failing run, isolating this
change from userspace layout or runtime updates. These observations support
expensive Debug page initialization as a contributor to the missed deadline;
they do not explain all timing variance or close performance acceptance.
Both word-only runs subsequently **failed** the unchanged no-progress deadline:
ARM64 last completed cycle 100 (267.09 seconds to case failure), RV64 cycle 70
(193.68 seconds). Each retained one distinct resource state across its completed
checkpoints. The initial improvement was not a complete fix.

The existing `users.vm/private_cow` check now verifies every byte of both new
pages before its COW checks. It passed in **`1789492721199574362`**. A deliberate
nonzero final-word mutation failed with exactly **`mask=0x10`** in
**`1789492770782882882`**; that mutation was removed. The strengthened check and
word-zero implementation use source
**`2e25535ca13f93c6f64c2cb581b52b8205281a5f78767af59c8ad3114b5a677d`**.
All six builds and functional reports succeeded (**22 suites/116 cases each**),
and host tests passed **234/234**. The functional report IDs are ARM64 Debug
`1789492823464130538`, ARM64 Release `1789492843278652988`, x64 Debug
`1789492843608621904`, x64 Release `1789492844290395534`, RV64 Debug
`1789492824372525209`, and RV64 Release `1789492845610620647`. Debug ARM64's four
non-application CTests and RV64's three also passed; the four other application
jobs remain in progress on these frozen inputs.

A second, separate change removes the demand handler's duplicate TLB invalidation:
every successful `map_user_page` branch already publishes and invalidates the
entry. No required invalidation or barrier was removed from that mapping helper.
This source is **`e076c1e2b62bb307fc546f083e3f7471e2470976ec3bba7b36eab955873baa2e`**.
The ARM64 and RV64 Debug focused reports `1789493215238322680` and
`1789493215938969769` passed **five suites/38 cases** each: users.vm, users.uaccess,
users.exec, mm.permissions and mm.transactions. Nevertheless, unpinned ARM64
application report **`1789493099881112439`** failed after checkpoint 40, so the
timeout remains open. RV64 report **`1789493100318021652`** also failed after
checkpoint 120 (313.33 seconds to case failure). ARM64 report
**`1789493391507735968`**, with the existing `--host-cpus 8,9,10,11` option, failed
before checkpoint 10 (30.78 seconds to case failure). This separate environment
experiment did not resolve the issue and does not replace the unpinned failures.
Neither optimization is a claim of complete performance acceptance.

Earlier frozen results are not substitutes for the current source's complete
acceptance. No deadline, checkpoint interval, resource assertion or required
lifecycle count was relaxed. Six full-profile long runs and calibrated Release
performance gates remain outstanding.

At **2026-09-15 17:33 UTC**, source `2e25535c...` x64 Release application report
`1789493020900324532` finalized successfully with 1000 cycles and 101 matching
resource checkpoints. ARM64 Release `1789493020899932542`, x64 Debug
`1789493046160047054` and RV64 Release `1789493020901125663` were still live at
cycles 700, 710 and 800 respectively. Their CTest jobs must reach terminal state;
none establishes the newer `e076c1e2...` source's complete acceptance.

At **2026-09-15 17:41 UTC**, all four of those source `2e25535c...` CTest
processes had exited successfully. Each application report finalized with 1000
core/application cycles and 101 resource checkpoints. The ARM64/RV64 Debug
application failures are still failures; this does not close the six-config
application matrix or calibrated performance acceptance.

### Recycled Kernel Stack Initialization (2026-09-16)

Further diagnostics are retained under `build/page-progress-BbgHo3/`. Host
`perf` on the frozen failing `e076c1e2...` ARM64 Debug image attributed **69.94%**
of inclusive samples to QEMU 7.2.22's `notdirty_write`, not ordinary guest
page-zero computation. Its software MMU checks writes to physical pages still
containing translated code. An eight-second, 32 MiB-capped QEMU trace contained
506922 complete records; **494411** addressed page `0x4832f000`, with repeated
word stores near its upper end. This strongly suggests a recycled kernel stack;
the debugger did not directly identify the owner of that particular page.
Earlier guest-PC samples alone were insufficient to distinguish this host-side
cost from the guest instructions that triggered it.

Both fork and init previously allocated a 16 KiB kernel stack without clearing
its contents; only the initial context at its top was initialized. A fork-only
full-stack-zero trial, source `08348750...`, passed ARM64 Debug application report
**`1789494707417392882`**: **1000 cycles**, **101 matching resource checkpoints**,
and 661.54 host seconds. Its initramfs SHA-256 remained
`0f6831cff2199891b020dc3f46202b31171e9ebd08bdec47c48f64de217ba2dc`.
The same capped trace after that change no longer concentrated writes on one
low-address page. These are causal diagnostics, not a calibrated benchmark or
proof that all translated-code invalidation overhead is gone.

The production fix is shared by fork and init through
`Thread::allocate_kernel_stack()`: reject replacement of an existing stack,
return OOM without publishing state, and clear all four pages before publishing
the identity-mapped base and size. Exec reuses its active stack and must not
clear it. Existing process cleanup still owns stack release.

The real-kernel `scheduler/kernel_stack_initialization` regression exhausts the
PFA, verifies OOM rollback, then offers only a fully poisoned four-page block.
It checks complete zeroing, alignment/top and rejected repeated allocation.
Without zeroing, report **`1789495252276812605`** failed only its `zeroed`
assertion (527 passed/1 failed). The corrected source is
**`5666b8f0694d6f8bc14fddbdf81cf02569a480bda23bb0f36a0be254341df452`**;
ARM64 focused report **`1789495298703999823`** passed scheduler, users.frame,
users.vm and users.exec (**4 suites/21 cases**). All six builds succeeded and
host tests passed **234/234**. The final source's complete six-configuration
runtime matrix, long runs and calibrated performance gates are not established
by the earlier trial.

The final source's six functional reports subsequently all passed **22 suites /
117 cases**: ARM64 Debug `1789495547578260985`, ARM64 Release
`1789495547578869815`, x64 Debug `1789495547598471194`, x64 Release
`1789495547631998889`, RV64 Debug `1789495547643241303` and RV64 Release
`1789495547673516917`. Their separate application reports subsequently all
passed **1000 core/application cycles with 101 matching resource checkpoints**:

| Configuration | Application report | Host case seconds |
| --- | --- | ---: |
| ARM64 Debug | `1789495737127452860` | 723.81 |
| ARM64 Release | `1789495712252511308` | 536.43 |
| x64 Debug | `1789495747305202449` | 718.23 |
| x64 Release | `1789495728294840714` | 532.79 |
| RV64 Debug | `1789495742957063851` | 803.32 |
| RV64 Release | `1789495713898431604` | 536.49 |

All six application reports retain the same `5666b8f0...` source identity.
Their CTest processes also exited successfully. Follow-on CTests select their
own inputs: ARM64 rebuilding overlapped that outer CTest process, so its later
framework/console results must not be attributed to this earlier source without
checking their individual artifacts. The frozen functional/application evidence
does not prove the later terminal/access changes, long-run or performance gates.

The post-zeroing host profile captured 4162 samples and reached application
checkpoint 60 in the bounded 45-second diagnostic run. Its `perf report` step
blocked without output and a bounded retry also timed out; the raw
`stack-zero-perf.data` is retained without claiming a post-fix sample percentage.

### Terminal Query and Access Checks (2026-09-16)

Evidence is under `build/terminal-stdio-1oO5OV/`. The original BusyBox replay
`busybox-red` passed its nine existing cases but emitted **138** terminal-query
and flush diagnostics. The public libc regression `terminal-red` failed with
exit code 202 (`mask=0xca00`) on a valid duplicated console descriptor. The
old mlibc adapter returned ENOSYS for every character device, preventing mlibc
from choosing a buffering mode. [The isatty contract](https://pubs.opengroup.org/onlinepubs/009695299/functions/isatty.html)
distinguishes terminal devices, non-terminals and invalid descriptors; descriptor
numbers or character-device type alone do not determine terminal status.

Moss now exposes native `SYS_IOCTL = 132`, reusing `FileOps::ioctl`. The only
implemented command is the no-payload `MOSS_IOCTL_ISATTY = 0x4d01`: the console
driver succeeds, while ordinary files, pipes, null/zero and unsupported device
commands return ENOTTY. Invalid descriptors, oversized commands and nonzero
payloads are rejected. The static mlibc adapter uses that device-owned query.
The libc test covers a console duplicated above FD 199, stdio output/flush/close,
closed and invalid descriptors, regular files, null/zero, pipes, full-width FD
validation and invalid command/payload rejection. This does **not** implement
termios, PTYs, terminal job control or Linux ioctl compatibility.

`terminal-green` passed libc but correctly exposed a second missing interface:
BusyBox's `file_redirection` timed out at `rm: remove '/moss-data'?`. With stdin
now correctly recognized as a terminal, BusyBox interpreted the missing
`access(W_OK)` result as non-writability and requested confirmation. No `rm -f`,
fake non-terminal result or relaxed timeout was substituted. The minimized
`access-red` libc check failed with exit 216 (`mask=0xd800`).

Native `access` (38) and mlibc `Access` now use the real UID/GID, shared inode
permission checks and the existing directory-search/cwd traversal. They handle
F_OK, combined R/W/X, invalid masks and user addresses, missing paths, trailing
slashes and relative paths. Root bypasses ordinary R/W mode bits, but regular
file X_OK requires at least one executable bit; immutable initramfs regular
files remain non-writable. [POSIX access](https://pubs.opengroup.org/onlinepubs/9699919799/functions/access.html)
specifies real credentials rather than effective credentials. This query is
not authorization for a subsequent open or namespace mutation; enforcement in
those operations and supplementary groups remain separate unresolved scope.

`vfs/access_permissions` checks owner/group/other selection, owner precedence,
root execute rules, immutable files, ancestor search permissions, relative
lookup and resource recovery across **64 lifecycles**. A temporary permission
bypass caused **384 failures / 896 passes** in `access-permissions-red`; it was
removed. Corrected `access-permissions-green` passed **3 suites / 21 cases**
(VFS, libc and BusyBox), with no terminal/flush/Access diagnostics. Source:
**`defc3559057155d9cb905a948115b6e4bcb72b72b7a1a96ed2e2f3b5b8a2c3bb`**.
All six builds and their non-application CTests succeeded. Each functional
report passed **22 suites / 118 cases**:

| Configuration | Functional report |
| --- | --- |
| ARM64 Debug | `1789496813628361267` |
| ARM64 Release | `1789496860550436593` |
| x64 Debug | `1789496858905583837` |
| x64 Release | `1789496860235132354` |
| RV64 Debug | `1789496859841523950` |
| RV64 Release | `1789496860896324786` |

All six retain the `defc3559...` source identity. Framework, production boot,
ARM64 Debug console input and Release benchmark smoke also passed; smoke runs
do not establish calibrated performance acceptance. Host tests passed
**234/234**. Uname diagnostics remain, as do the broader permissions,
dirfd/symlink, shared-FD and fallible-metadata gaps already recorded above.
This source has not yet completed its own 1000-cycle application matrix, the
six full-profile long runs or calibrated Release performance gates.

### Native System Identity (2026-09-16)

Evidence is retained under `build/uname-runtime-aZS9m7/`. `busybox-red`
passed its nine cases but emitted **10 Uname diagnostics**. BusyBox 1.37.0 ash
uses the returned node name to initialize `HOSTNAME`, without checking uname's
return value. The new public-libc regression failed in `uname-red` with exit
231 (`mask=0xe700`) while the required adapter/native operation was absent.

Native syscall **110** now copies six zero-padded 65-byte identity fields through
the existing checked user-copy path. The values are `Moss`, `moss`, the top-level
CMake project release, release plus build type, the target machine name and an
empty domain. The static mlibc Uname adapter checks the pinned public ABI layout
and forwards the syscall result. [POSIX uname](https://pubs.opengroup.org/onlinepubs/009604599/functions/uname.html)
allows implementation-defined identity values; this adds neither networking,
hostname mutation nor Linux binary compatibility.

The libc check verifies the public/native results, architecture and build
identity, complete zero padding, surrounding canaries, successful cross-VMA
copyout, invalid/overflow/kernel pointers, missing and read-only output mappings,
no partial write on rejection, and recovery. BusyBox's exec/environment case and
each complete application cycle now require `HOSTNAME=moss`.

Two intermediate fixture failures are retained: `uname-green` used the nested
mlibc project's empty `PROJECT_VERSION` as the expectation (exit 232), corrected
to the top-level `CMAKE_PROJECT_VERSION`; `uname-version` tried an unsupported
partial-VMA munmap (exit 239), corrected to the existing two-VMA test pattern.
Native munmap semantics were not expanded. `uname-boundary` then passed libc
and all nine BusyBox cases without Uname diagnostics. Its source preceded the
additional no-partial-write and per-cycle hostname assertions.

All six builds succeeded for source
**`dd5be48a81c1895dd21bf7a065b4ad9d69a5f96e2f633142437323c6eeb70954`**.
Each functional report finalized with **22 suites / 118 cases passed** and the
same source identity; all six BusyBox serial logs are free of Uname diagnostics:

| Configuration | Functional report |
| --- | --- |
| ARM64 Debug | `1789498227721274297` |
| ARM64 Release | `1789498323039875249` |
| x64 Debug | `1789498323048328233` |
| x64 Release | `1789498324649373309` |
| RV64 Debug | `1789498324240030349` |
| RV64 Release | `1789498325083396541` |

All six non-application CTest sets also passed, including framework and
production boot, ARM64 Debug console input and Release benchmark smoke. Smoke
is not calibrated performance acceptance. Host tests passed **234/234**.
The ARM64 Debug 1000-cycle application run
(`arm64-debug-application/results.json` under the evidence root) also passed on
the same source: **1000 core/application cycles, 101 matching resource
checkpoints and 29152 passed / 0 failed assertions**, in 699.41 host case seconds.
This source's other five application runs have not started. The six
full-profile long runs and calibrated Release performance gates remain
incomplete, as do the broader permission, shared-FD and metadata-allocation
gaps above.

### File Authorization and Fork Credentials (2026-09-16)

Evidence is retained under `build/vfs-permissions-IFYk2X/`. Access queries were
already tested, but several actual operations still bypassed those permissions:

| Retained failing report | Observed failure |
| --- | --- |
| `open-red` | 64 unauthorized opens succeeded; 1344 assertions passed and 64 failed. |
| `create-red-corrected` | 64 unprivileged creations in the root's 0755 directory succeeded; 2944 assertions passed and 64 failed. |
| `namespace-red-catalog` | The UID/GID 99 libc child unlinked a protected file; exit 16. |
| `stat-red` | Stat traversed a non-searchable ancestor through `..`; exit 23. |
| `exec-red` | A valid copied ELF with mode 0600 executed despite lacking execute permission; the replacement image exited 90. |
| `credentials` | Fork did not preserve the unprivileged parent's IDs; exit 26. The child constructor's default IDs were root. |

Open now passes actor credentials through traversal and authorizes the requested
access before driver open/truncation. Creation and rename share the existing
parent-resolution helper's write/search check; unlink/rmdir authorize their
resolved parent before mutation. Stat and exec traverse with effective IDs;
exec also requires a regular file with execute permission. Fork copies all four
real/effective UID/GID fields before publishing a runnable child. These changes
reuse the namespace lock and existing access predicate, without introducing a
second permission engine or a production identity-management API.

The tests preserve the distinction between initial creation and reopening: the
new file's mode does not prohibit its initial descriptor, but later opens require
authorization. Namespace modifications depend on parent permissions rather than
the target file's read/write bits. Stat requires path search, not file-content
read permission. These are selected contracts from
[open](https://pubs.opengroup.org/onlinepubs/9699919799.orig/functions/open.html),
[rename](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html) and
[stat](https://pubs.opengroup.org/onlinepubs/009696799/functions/stat.html).
The [exec](https://pubs.opengroup.org/onlinepubs/9799919799/functions/exec.html)
and [fork](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fork.html)
checks also require failed exec to retain the old image and fork to retain IDs.
This static profile does not implement set-ID executables or claim full POSIX
permission conformance.

`users.libc/filesystem_permissions` exercises those syscalls through the real
static libc, using a validation-image-only child credential fixture. It checks
owner IDs on new objects, inherited authorized descriptors after the credential
drop, unmodified file contents and poisoned stat output after rejection, allowed
mutations in a writable directory, child/parent credential isolation, successful
exec preserving IDs, and root's execute-bit requirement. The production control
hook still returns ENOSYS. `vfs/access_permissions` repeats owner/group/other
precedence, traversal and rejected namespace mutations across 64 lifecycles,
checking original inode/data identity and exact populated and final pool/heap
recovery. Its final targeted `recovery` report passed VFS and libc: 13 cases,
including 3392 passed / 0 failed permission assertions.

Intermediate fixture failures are not hidden: `create-red` included 64 incorrect
group-bit expectations alongside the 64 real creation failures; `namespace-red`
initially omitted the new case from the host catalog. Existing tests that created
non-root-owned objects directly in root's 0755 directory now use an explicitly
writable arena or root creation, while the arena separately checks non-root file
and directory ownership. `credentials-green` passed libc, exec, signals and
BusyBox (38 cases) after the credential repair.

The first six-build source was
`b4ffed6969ba206efb9c9ca94d8d6a2502e9e1480dcd5e6b9ea8602bb25c514f`.
Both x64 functional runs (`1789501139104860183`, `1789501139791368076`) failed
before readiness with `case_capacity`: the full registry, including self-tests
and x64's extra SIMD fault case, grew beyond 128. Capacity is now 160, still
static and bounded; the existing `registry_limits` check must accept exactly
that capacity and reject the next entry. No test was dropped and no acceptance
deadline or performance threshold changed. Those original functional/framework
failures and the other configurations' results remain retained.

The next source, `109a315e5493a3d3636b22b50acd8f287c318a279e1c4024362f85b5ceebec2d`,
passed all six functional reports with **22 suites / 119 cases**:
ARM64 Debug `1789501443794047513`, ARM64 Release `1789501444697286392`,
x64 Debug `1789501443605344300`, x64 Release `1789501444547656776`,
RV64 Debug `1789501443956241875` and RV64 Release `1789501444726636835`.
However, x64 Debug's following framework report `1789501623681783517` failed
at `cleanup_guards` with `missing_completion`; `framework-red-repeat` reproduced
it. The preceding `registry_limits` test's aggregate reset generated a
**16688-byte stack frame**, exceeding the thread's **16384-byte** kernel stack
before its callees were included. Copying a static empty registry instead of
constructing reset temporaries reduces that frame to **368 bytes**. The unchanged
cleanup test and entire self suite then passed in `registry-stack`; before/after
disassembly is retained beside those reports. Kernel stack size and all registry
overflow assertions are unchanged.

`registry-stack-repeat-{1,2,3}` also passed all four self cases. The next six-build
source was `d47e691cd2d73b35e3d89dab14c2abc6eb1abbbf8f1f819317228e711cc0db19`.
All builds, framework checks and production boots passed, as did ARM64 Debug
console input and Release benchmark smoke. Its functional matrix is **not
accepted**:

| Configuration | Functional report | Outcome |
| --- | --- | --- |
| ARM64 Debug | `1789502202558720365` | 22 suites / 119 cases passed |
| ARM64 Release | `1789502202557612534` | 22 suites / 119 cases passed |
| x64 Debug | `1789502200441983661` | 22 suites / 119 cases passed |
| x64 Release | `1789502203246857568` | `users.signals/pipe_interrupted` failed; its next three cases did not run |
| RV64 Debug | `1789502202719976396` | `timers/dispatch` failed; timer capacity did not run |
| RV64 Release | `1789502203595781502` | 22 suites / 119 cases passed |

The RV64 failure was the combined `stopped_count >= 3` and no-early-periodic
callback assertion after the fixture's 100 ms guest-clock window. Ten subsequent
unchanged `timer-red-repeat-*` runs passed; that does not explain or erase the
matrix failure. The timer test now uses the existing typed comparison assertions
to report count and first-expiry failures independently, without changing any
count, waiting window or host deadline. That diagnostic-only revision passed
the targeted `timer-diagnostics` run; it has not completed a new matrix. Both
the timer failure and x64 pipe-interruption failure remain under investigation.

Separately, the earlier **`109a315e...`** source completed 1000 core/application
cycles in ARM64 Debug/Release, x64 Release and RV64 Debug/Release. Each finalized
`<configuration>-application/results.json` under the evidence root passed with
101 matching resource checkpoints. x64 Debug did not start that application
run because its preceding framework check failed. These five runs predate the
registry-stack repair and cannot be combined with later images into a complete
current-source matrix.

Host checks passed 234/234. Current-source routine application acceptance, all
six full-profile long runs and calibrated Release performance gates remain
incomplete. No deadline or performance threshold was relaxed. Shared-table FD
concurrency and fallible metadata allocation remain separate limits. Symlink/
dirfd and supplementary-group support are not claimed; networking and persistent
storage remain excluded.

### Timing Fixture Ordering (2026-09-16)

Evidence is retained under `build/timing-recovery-9bEBrP/`. These repairs address
reproduced fixture ordering defects, not a claim that all historical timing
failures have one cause.

`pipe-red` delays the parent for 30 ms before its blocking read/write. The old
sender's guessed 10 ms delay delivers the caught signal before the intended I/O;
both calls return one byte rather than EINTR. The shared signal fixture now
observes its own parent sleeping in the intended read/write syscall and FD,
under the existing sleep lock and after sleep handoff. It explicitly rejects
the parent's preparation nanosleep as readiness. For caught signals a second
real pipe acknowledges the interrupted call before the child releases the data
pipe. Ignored/blocked signals still require normal data completion. CPU 0/CPU 1
placement and payload, handler, EOF and exit-status assertions remain mandatory.
The child retains its existing sleep/wakeup migration step; it is not used as
parent-readiness evidence. The narrowly scoped observer is validation-only;
the production image's weak hook returns ENOSYS and the standalone demo retains
its original uncoordinated path. This does not establish immediate affinity
migration or complete deterministic SMP coverage.

Intermediate diagnostics are retained: the first build rejected a const trap
frame accessor, and `pipe-frame`, `pipe-observe` and `pipe-transition` hit the
unchanged watchdog. Merely yielding after setting affinity left the child on
CPU 0; a combined error expression then short-circuited signal delivery. Signal
delivery is now independent of the CPU assertion. A 1 ns sleep in `pipe-affinity`
still did not guarantee a sleep/wakeup handoff; the existing 10 ms migration
sleep was restored. `pipe-settled` and the log-free `pipe-clean` passed the x64
Release signal suite. Temporary diagnostic logging was removed from source.

`timer-window-red` keeps the ordinary timer phase and adds a second phase with
preemption-length setup between arming the one-shot and periodic timers. The
old observation deadline has already expired: periodic count and first callback
time are both zero (12 assertions passed, 2 failed). The fixture now starts the
same 100 ms observation window after both timers are armed. Both phases retain
one-shot count/deadline, minimum three periodic callbacks, no early callback and
no callbacks after cancellation. `timer-window-green` passed RV64 Debug timers
and signals. This is a controlled setup-window reproduction; the earlier matrix
failure's combined assertion did not record the individual values needed to
prove it was the same event.

Four two-host-CPU, concurrent RV64 timer probes with default memory produced
three startup timeouts during initialization and one pass (`timer-pinned-*`);
none of those timeouts reached a timer case. All four corresponding 256 MiB
probes (`timer-small-*`) passed. They are diagnostics, not baseline-profile or
performance acceptance. No host deadline, callback requirement or performance
threshold was widened.

The subsequent source
`5fa757c9bc91310b5dd1a6d9f2ad2c8e1e1096b34aadbeb87fc74c142f09bd93`
passed all six builds and functional reports (**22 suites / 119 cases**), with
no unreached cases. Framework self-checks and production boots also passed in
all six configurations, plus ARM64 Debug console input. The reports are:

| Configuration | Functional report |
| --- | --- |
| ARM64 Debug | `1789504484939844954` |
| ARM64 Release | `1789504615893574485` |
| x64 Debug | `1789504484660262251` |
| x64 Release | `1789504625047809192` |
| RV64 Debug | `1789504481715500402` |
| RV64 Release | `1789504615342727472` |

Reports are under `build/<configuration>/validation/<ID>/results.json`; build
and CTest logs are under the evidence root. Host checks passed 234/234, Ruff,
clang-format and `git diff --check`. This matrix excluded the separate routine
application and benchmark tests; it does not establish those acceptance gates.

### Full-Width FD Syscall Entry (2026-09-16)

The same evidence root retains an independent boundary repair. The earlier
`vfs/fd_boundaries` test checked the real FD table but did not cross the userspace
syscall entry. Read, write and close still cast their native long FD argument to
int before passing it to the already full-width VFS API. Values whose low 32 bits
name a live descriptor could therefore read, write or close that descriptor.

The existing static-libc descriptor test now issues real raw syscalls with -1,
256, and positive/negative out-of-range values sharing a live FD's low bits. It
requires EBADF, an untouched poisoned read buffer, unchanged file offset and
contents, and a still usable original descriptor followed by close and unlink.
Each entry defect was reproduced independently while repairing the preceding
one: `fd-entry-read-red` exited 143, `fd-entry-write-red` exited 144, and
`fd-entry-close-red` exited 145. The kernel repair only removes the three casts,
reusing the existing FD table validation. This is Moss's native full-width
contract, not a claim about Linux binary compatibility.

`fd-entry-green` passed x64 Release libc, users, signals and BusyBox. These
focused passes were followed by the fresh matrix below.

### Full-Width FD Routine Matrix (2026-09-16)

Source `162c79f9a97e20b0ec3f1a5c46b4df280a2431a906dea30eeb9bf9eb07457e93`
passed all six builds and **25/25 CTests**, excluding `moss-benchmark`.
Every functional report finalized **22 suites / 119 cases** with no unreached
cases. Every separate application guest completed **1000 full core/application
cycles**, with **101 ordered checkpoints** from 0 through 1000, equal core and
application counters, and all 11 owned-resource measurements exactly restored
to their warm baseline. No failed or incomplete report was replaced.

| Configuration | Functional report | Application report | Framework report | Production boot |
| --- | --- | --- | --- | --- |
| ARM64 Debug | `1789504991493823912` | `1789505088635352329` | `1789505790177847421` | `run-urka8i8g` |
| ARM64 Release | `1789505841218035664` | `1789505956548334563` | `1789506474966368994` | `run-apv3ch6w` |
| x64 Debug | `1789504990602836279` | `1789505086233631919` | `1789505753714176258` | `run-gsiw3dr0` |
| x64 Release | `1789505809818665284` | `1789505867896211941` | `1789506380640285889` | `run-uzv8rp27` |
| RV64 Debug | `1789504991573931657` | `1789505086241484493` | `1789505809338898282` | `run-8nga7u1h` |
| RV64 Release | `1789505857240414941` | `1789505997031580899` | `1789506473795502097` | `run-qie6s0nk` |

Reports are under `build/<configuration>/validation/<ID>/results.json` and
`build/<configuration>/production-boot/<run>/guest/results.json`. ARM64 Debug
console input also passed in `run-ncauljh4`. Build and CTest logs are
`build/timing-recovery-9bEBrP/<configuration>-fd-{build,ctest}.log`.
This snapshot includes the syscall-width repair, but predates the shared-table
I/O regression and repair below. It does not establish those new changes, the
six full-profile long runs, metadata-failure closure or performance acceptance.

### Shared-Table Active I/O Ownership (2026-09-16)

The earlier `vfs.smp/shared_references` case exercised separate tables retaining
one File, not a close in the same table as blocked I/O. It now additionally
observes the actual CPU 1 thread sleeping after scheduler handoff, closes and
reuses that descriptor on CPU 0, and requires the pending operation to retain
its original endpoint. These phases use real VFS calls and the existing two-CPU
driver, without a guessed sleep delay or replacement filesystem.

Evidence remains under `build/timing-recovery-9bEBrP/`:

| Run | Evidence |
| --- | --- |
| `fd-shared-read-typed` | Read-side regression failed with `-EPIPE` (-32) instead of a one-byte write; 5139 assertions passed before the failure. |
| `fd-shared-ref-count` | Diagnostic observed just one File reference while the read was blocked. |
| `fd-shared-held-probe` | Test-only retained-reference control passed the same close/reuse/read sequence; diagnostic, not a production repair. |
| `fd-shared-read-green` | Production repair passed 5145 assertions, source `7bf02e187f25fccec154ae9ac20f75494b4589ffb17e6118f7b7eb47265c9c8c`. |
| `fd-shared-write-red` | Temporarily restoring only `do_write` to borrowed lookup made the new blocked-write check fail: 5 live Files instead of the required 6. |
| `fd-shared-write-green` | Restored production ownership passed all 9379 assertions, source `762c832a76c76e0ef6b84bbdd745df759ea09964e66a2bd6b1e6cdcea9c059e0`. |

The root cause was a borrowed File pointer surviving its sole descriptor
reference. The scoped `FileRef` retains the existing intrusive reference while
holding the FD-table slot lock; descriptor removal and replacement use the same
lock. All nine production FD borrowers now keep that scoped owner throughout
their operation. Final endpoint release and blocking I/O occur outside the
slot lock. Clone copies its source slots under that lock. No heap control block
or lock held across blocking I/O was introduced.

The write-side phase fills the real 4096-byte pipe, checks that the blocked
writer remains owned after close/reuse, verifies all original bytes and the
resumed writer's byte, then requires EOF, a usable replacement descriptor and
exact pool/heap recovery. Both temporary reference controls and tagged debug
instrumentation have been removed.

The subsequent source
`b17274bff0aff650117e18443c08a9e00b6faad51139b1370bcce1c873082633`
passed all six builds and **19/19 CTests**, explicitly excluding the independent
application-repeat and benchmark tests. Every functional report finalized
**22 suites / 119 cases**, no unreached cases, and the new **9379-assertion**
shared-FD case. That case took 0.10–0.56 host seconds across the six runs,
within its unchanged 5-second deadline; these durations are not benchmarks.

| Configuration | Functional report | Framework report | Production boot |
| --- | --- | --- | --- |
| ARM64 Debug | `1789523460640082206` | `1789523588841790162` | `run-4w_1zv85` |
| ARM64 Release | `1789523671635450930` | `1789523715961368971` | `run-vf6hy12h` |
| x64 Debug | `1789523460235846107` | `1789523586891036962` | `run-yb5mfq21` |
| x64 Release | `1789523623342827460` | `1789523649130147934` | `run-iqqeqfuf` |
| RV64 Debug | `1789523460973149882` | `1789523600150386080` | `run-jxi1waqu` |
| RV64 Release | `1789523671645990395` | `1789523718099260304` | `run-u050fc4c` |

Report paths follow the matrix above; ARM64 Debug console input additionally
passed in `run-52uufcyo`. Logs are `<evidence-root>/<configuration>-shared-io-{build,ctest}.log`.
Host checks passed **234/234 pytest tests**, Ruff, clang-format and
`git diff --check`. The initial mistaken unittest invocation discovered zero
tests and exited 5; its log is retained and is not counted as a pass. Only this
evidence documentation changed after the six-configuration runs.

Multi-step open/pipe publication and rollback, atomic duplication semantics,
and fallible metadata allocation remain separate obligations; this repair does
not establish complete shared-FD concurrency. Fresh 1000-cycle full-application
runs after this repair, all six full-profile long runs, and calibrated Release
performance gates remain incomplete. Earlier source `162c79...` application
passes are historical evidence, not acceptance of the changed production path.

### Open Reservation Before Filesystem Mutation (2026-09-16)

The next `vfs.smp/shared_references` phase leaves exactly one descriptor free.
CPU 0 holds namespace exclusion while CPU 1 enters real `open(O_TRUNC)`; the
independently locked File-pool count observes its allocated, still unpublished
File. CPU 0 then attempts to duplicate an existing descriptor into the last
slot. Exactly one caller may acquire that slot. A failed open must preserve the
original inode, size and byte; a successful truncating open must empty that
same inode. The case finishes with descriptor, pool and heap recovery.

`file_pool_usage()` is a read-only, file-lock-only view of the existing resource
counter. The combined `pool_usage()` reuses it under namespace/inode exclusion.
This permits the controlled namespace barrier without recursively acquiring
the namespace lock, arbitrary sleeps, unpublished-pointer reads or a fake VFS.

Preserved evidence under `build/timing-recovery-9bEBrP/`:

| Run | Evidence |
| --- | --- |
| `fd-open-race-red`, `fd-open-race-repeat` | Both failed: a rejected open left EOF where the original one-byte file was required. |
| `fd-open-race-probe2` | `open=-24`, competing `dup=255`, inode `11/11`, size `1/0`: confirmed truncation before final EMFILE, not an offset or descriptor-identity error. |
| `fd-open-race-green` | Slot reservation repaired the controlled race; SMP, VFS, static libc and BusyBox passed on x64 Release. |
| `fd-open-boundaries`, `fd-open-exhaustion` | Expanded test incorrectly deleted a cloned table without the existing explicit `close_all` protocol; retained as test-cleanup failures, not production evidence. |
| `fd-open-cleanup` | Corrected cleanup and all expanded boundaries passed; 9646 SMP assertions and 278 FD-boundary assertions. Source `36c0fd29aa56232a88790cd3095360b09ea0f1471d1a35b48569857a7e845561`. |

The first diagnostic-stat build failed a signed/unsigned test comparison;
`fd-open-race-probe-build.log` preserves it. No runtime passed from that build.
The diagnostic tag has been removed, while inode/size assertions remain.

`FdTable::Reservation` now retains one unpublished slot before File allocation
or filesystem mutation, releases it automatically on every error return, and
publishes only a fully initialized File. Allocation skips pending slots;
targeted duplication returns EBUSY rather than stealing one. The FD lock is
not held across namespace/driver work. Tests also cover invisible pending
slots, close/flag rejection, close-on-exec handling, clone omitting pending
slots, cancellation and lowest-slot reuse, 256 failed path opens, and real
File-pool exhaustion without file creation or leaked reservations.

This closes the reproduced last-FD open/truncate race, not every descriptor
transaction. Atomic pipe-pair publication, duplication's source/target lookup
transaction and fallible heap-backed metadata remain open.

The subsequent six builds and **19/19 CTests** passed, excluding independent
application-repeat and benchmark tests. Every functional report finalized
**22 suites / 119 cases**, including **9646 SMP assertions and 278 FD-boundary
assertions**, with no unreached cases. Framework self-checks and production
boots also passed, plus ARM64 Debug console input in `run-g8c5gf0z`.

| Configuration | Functional report | Framework report | Production boot |
| --- | --- | --- | --- |
| ARM64 Debug | `1789524827932446333` | `1789524956694917247` | `run-1w1ki4f_` |
| ARM64 Release | `1789525025812935133` | `1789525142263683486` | `run-uva2bih4` |
| x64 Debug | `1789524828779170866` | `1789524952584377914` | `run-pc2u58ir` |
| x64 Release | `1789524995209007604` | `1789525081894547729` | `run-h21twt7i` |
| RV64 Debug | `1789524830745207574` | `1789524961660971947` | `run-xcfrkxgp` |
| RV64 Release | `1789525028015177114` | `1789525145365476614` | `run-q0dcl24i` |

Reports use the earlier `build/<configuration>/validation/<ID>/results.json`
and production-boot path conventions. Logs are
`<evidence-root>/<configuration>-open-reservation-{build,ctest}.log`.
Host checks passed **234/234 pytest tests**, Ruff, clang-format and
`git diff --check`; `fd-open-host-tests.log` retains the host result.

Keep the two recorded source identities distinct: Debug reports use
`24a8cf525e7a0fc3e4a0db0eb56e8da4c980881ce209f78d7de122bdebed46b9`,
and Release reports use
`7d7b20474b1bb1a110800b8225a778b586c8917f339bca3c606b0d5589f1a31c`.
Between those builds, the independent application-acceptance activity appended
the following documentation section. The source-identity calculation includes
the complete Git diff, including documentation; the worktree timestamp audit
found this document as the only input changed after the Debug builds. These
are nevertheless not six reports of one identical whole-worktree snapshot,
and no provenance was rewritten to make them so. New-source full-application
repeat, long-run and calibrated-performance acceptance remain incomplete.

### Renewed Routine Application Matrix After Shared-FD Repair (2026-09-16)

After retaining the incomplete performance outcome by user direction, the next
acceptance item reran `users.applications` on all six ARM64/RV64/x64 Debug/Release
configurations. Current build-directory provenance was not uniform, so this
campaign reused and verified the retained common-source validation artifacts
from the shared-FD functional matrix above. Every guest uses source
`b17274bff0aff650117e18443c08a9e00b6faad51139b1370bcce1c873082633`;
the result does not accept subsequent worktree changes or mix later images.

All six runners exited 0. Each guest completed **1000 core cycles and 1000 real
BusyBox workflows**, retaining **101 exactly equal resource checkpoints**:
**6000 workflows / 606 checkpoints** across the matrix, including each warm
baseline. Original serial logs were replayed through the protocol validator;
saved assertions/checkpoints, full source identity and image/fixture hashes
were verified. No selected workload was left unrun. The existing 30-second
no-progress watchdog and 3060-second total guest deadline were unchanged.

Reports are `build/application-acceptance.6du3GD/<preset>/results.json`, with
frozen inputs and per-guest serial/QEMU logs. The root `README.md` records the
commands and per-configuration durations; `audit.py` and `audit.log` retain the
offline check. All owned runner/QEMU processes were reaped. No kernel or runner
source repair was needed for this step, and no assertion was weakened.

This closes the renewed **routine application** matrix for that shared-FD
snapshot only. It does not replace the six full-application stability runs
requiring **both 10000 cycles and at least 30 minutes in one guest**, establish
general SMP correctness, or close the separately deferred performance gate.

### Atomic Pipe-Pair Publication (2026-09-16)

The next controlled shared-table interleaving reproduced a separate rollback
defect. With only FD 255 free, CPU1 published the pipe's read endpoint and
paused. CPU0 closed it and reused FD 255 for `/dev/null`; CPU1 then failed to
allocate the write endpoint with EMFILE and closed CPU0's replacement by its
old numeric FD. The replacement accepted a byte before CPU1 resumed but
returned EBADF afterward, while the original shared `/dev/null` FD still
worked. The validation image uses an observation hook outside the FD lock;
the production image retains an empty hook, not a substitute VFS operation.

Evidence remains under `build/timing-recovery-9bEBrP/`:

| Run | Outcome |
| --- | --- |
| `pipe-pair-red`, `pipe-pair-repeat` | Both failed with 9905 passing assertions and one failure: replacement write returned -9 instead of 1. |
| `pipe-pair-probe` | Failed with 9907 passing assertions and one failure; retained log records `pipe=-24 published=true anchor=0 replacement=-9`. |
| `pipe-pair-green` | All five selected suites passed after atomic pair installation; SMP case passed 9910 assertions. |
| `pipe-pair-success` | All five selected suites passed with the successful-pair and reserved-slot checks added; SMP case passed 10179 assertions. |

The red/repeat source identity is
`39242584ee5d3b723f9324c1c814cc6529f4c1fe9dd1ea536167e73f6f5a4628`.
Each run retains its own `results.json`, frozen inputs and serial logs, plus
adjacent `<run>-build.log` and `<run>.log`. The temporary diagnostic tag has
been removed; the before/after replacement writes remain assertions.

`FdTable::alloc_fd_pair` now finds both available slots under the existing
table lock before installing either reference. Insufficient capacity changes
neither the table nor the caller's output. `do_pipe` no longer publishes one
endpoint then rolls it back by number; temporary endpoint references are
released outside the table lock. Pair allocation also skips a pending `open`
reservation. With two slots available, the second CPU observes and clones both
endpoints before the publisher returns, checks access modes and descriptor
flags, transfers a byte through the clone, then verifies EOF and exact pool
and heap restoration after close.

All six builds and **19/19 CTests** passed, excluding independent application
repeat and benchmark tests. Every functional report finalized **22 suites /
119 cases**, including **10179 SMP assertions and 282 FD-boundary assertions**,
with no unreached cases. All functional and framework reports share source
`3517d244e876da8ec993971f1d838390756ec20656daca6d27823a4015454c31`.

| Configuration | Functional report | Framework report | Production boot |
| --- | --- | --- | --- |
| ARM64 Debug | `1789526234021218812` | `1789526370194437625` | `run-nwqkmqvf` |
| ARM64 Release | `1789526440707701186` | `1789526541500851424` | `run-wzkuv__9` |
| x64 Debug | `1789526233812527290` | `1789526385503220967` | `run-3071nu6a` |
| x64 Release | `1789526438878793240` | `1789526538210986263` | `run-qgq3v8ww` |
| RV64 Debug | `1789526234407674413` | `1789526381521508211` | `run-0lcn2q27` |
| RV64 Release | `1789526440627733161` | `1789526546736662383` | `run-3fkmregh` |

Reports use `build/<configuration>/validation/<ID>/results.json` and
`build/<configuration>/production-boot/<run>/guest/results.json`.
ARM64 Debug console input also passed in `run-e_en0b1t`. Matrix logs use
`<evidence-root>/<configuration>-pipe-pair-{build,ctest}.log`. Host checks
passed **234/234 pytest tests**, Ruff, clang-format and `git diff --check`;
`pipe-pair-host-tests.log` retains the pytest output.

This closes the reproduced VFS pair-allocation rollback, not the whole native
pipe syscall transaction. `sys_pipe` still publishes before copying the pair
to userspace and cleans up failed copyout by numeric descriptor; a concurrent
copyout/close/reuse interleaving remains unverified and unclosed. The existing
1000 read-only-output failures do not exercise that interleaving. Duplication's
source/target transaction, fallible heap-backed metadata, current-source full
application repeat, long-run and calibrated-performance acceptance also remain
open. No long-run or performance credit is inferred from this matrix.

### Native Pipe Copyout Transaction (2026-09-16)

The next regression exercised the previously unclosed native boundary. CPU1
returns to userspace and invokes the actual pipe syscall with a read-only
output address. At an observation point immediately before the real copyout,
CPU0 closes and reuses the two published descriptors in CPU1's process table.
Both replacements accept writes before CPU1 resumes. The syscall then returns
EFAULT; its numeric-descriptor cleanup incorrectly closes the replacement at
FD 4, whose next write returns EBADF, although the shared original FD still
accepts a write. No user-copy failure or VFS operation is simulated.

Evidence is retained under `build/timing-recovery-9bEBrP/`:

| Run | Outcome |
| --- | --- |
| `pipe-copyout-red`, `pipe-copyout-repeat` | Both failed with 10191 passing assertions and one failure: replacement write returned -9 instead of 1. |
| `pipe-copyout-green` | Original failure disappeared, but the new fixture closed FD 4 before expecting the next lowest-free allocation to return 5; preserved failure reports 4 instead of 5. |
| `pipe-copyout-cleanup` | All six selected suites passed after moving fixture close operations after both replacement checks; production repair unchanged, 10201 SMP assertions passed. |
| `pipe-copyout-boundaries` | All six suites passed with File-pool exhaustion and pending-pair clone checks; 10203 SMP and 292 FD-boundary assertions passed. |

The red/repeat source identity is
`37b00519a7cb135d8defb7fab8112cdc33a2c740adb06164c6084eab097c7f06`.
Each run retains `results.json`, frozen inputs and serial logs, with adjacent
`<run>-build.log` where a build occurred and `<run>.log`. No original failed
result was rewritten as passed.

The repair reuses `FdTable::Reservation` and `OutputBuffer`: both descriptor
slots remain reserved and invisible during the real user copy, and
`Reservation::install_pair` publishes them under one table lock only after
copyout succeeds. Copyout failure cancels the reservations and releases the
unpublished endpoint ownership, without closing numeric descriptors. The
earlier `alloc_fd_pair` helper is replaced, not left as a second implementation;
kernel-buffer callers and native `sys_pipe` use the same transaction. User
copy and final endpoint release occur outside the FD lock.

The regression verifies that pending slots reject close/replacement, are not
inherited by a cloned table, become reusable after EFAULT, and restore pool
and heap accounting. Additional real File-pool exhaustion checks cover both
endpoint allocation failures, unchanged output sentinels and subsequent reuse
of both reserved slots. Existing native read-only-output repetition, VFS,
userspace access-fault, mlibc and BusyBox checks remain enabled.

All six builds and **19/19 CTests** passed, excluding independent application
repeat and benchmark tests. Each functional report finalized **22 suites /
119 cases** with no unreached cases. Functional and framework reports all
record source
`bfba9ca7b546d30103ce2979d847135c8e12c8ccac35d4a46d0b9b857d952dae`.

| Configuration | Functional report | Framework report | Production boot |
| --- | --- | --- | --- |
| ARM64 Debug | `1789531639049533751` | `1789531791728671964` | `run-h66iwte0` |
| ARM64 Release | `1789531848230501345` | `1789531975870601384` | `run-qf2sr1tk` |
| x64 Debug | `1789531638472334356` | `1789531800532600837` | `run-p437afba` |
| x64 Release | `1789531845868189063` | `1789531974172781395` | `run-e8gmmv6w` |
| RV64 Debug | `1789531639249023549` | `1789531805006410140` | `run-s_n70sr3` |
| RV64 Release | `1789531854336087158` | `1789531984767264188` | `run-6c47er1p` |

Reports follow the preceding validation and production-boot path conventions.
ARM64 Debug console input also passed in `run-0cab851z`. Matrix logs use
`<evidence-root>/<configuration>-pipe-copyout-{build,ctest}.log`; the host log
is `pipe-copyout-host-tests.log`. All **234 pytest tests**, Ruff, clang-format
and `git diff --check` passed. This closes the reproduced native copyout
rollback, not all FD transactions: duplication's source/target transaction and
fallible heap-backed metadata remain open. Current-source application-repeat,
six full-application long runs and calibrated-performance acceptance still
require separate evidence.

### Atomic Descriptor Duplication (2026-09-16)

The descriptor-copy regression now exercises `dup`, `dup2`, `F_DUPFD` and
`F_DUPFD_CLOEXEC` at the real VFS boundary. CPU1 pauses outside the table lock;
CPU0 closes source FD 0 and installs a writable replacement at target FD 2.
The test checks results consistent with one complete table transaction, no
source-slot resurrection, preservation of the later target replacement and
exact pool/heap recovery.

Distinguish this Moss transaction guarantee from a general compatibility
claim. In the inspected [Linux implementation](https://github.com/torvalds/linux/blob/master/fs/file.c),
ordinary `dup` obtains the file reference before separately allocating a
descriptor; `dup2` looks up its source with the table lock held through target
installation. Consequently, source-number reuse during concurrent `dup` and
`close` is not by itself evidence of a Linux/POSIX violation. Moss uses its
existing table lock to provide the stronger whole-operation guarantee for all
four copy entrances. This does not claim complete POSIX conformance.

Evidence remains under `build/timing-recovery-9bEBrP/`:

| Run | Outcome |
| --- | --- |
| `fd-dup-red`, `fd-dup-repeat` | Both failed the stronger Moss transaction check: 10208 passing assertions and one failure. |
| `fd-dup-probe` | After confirming source FD 0 was empty, logged `kind=0 result=0 source_flags=0 target_flags=1`; 10209 passing assertions and one failure. |
| `fd-dup-green` | All six selected suites passed, including all four copy variants and 10255 SMP assertions. |
| `fd-dup2-negative` | Temporarily restored only the previous split `dup2` path. `dup` passed; `dup2` returned 2 but overwrote the later writable target with the old read-only file. Target write returned -9 instead of 1; 10225 passing assertions and one failure. |
| `fd-dup-restored` | Temporary old path and diagnostic tags removed, full x64 Release rebuilt, all six selected suites passed again with the identical final source identity. |

The original red/repeat source is
`cd04683b55ccc614d80c41d99d54d1874cd6d64ac595633d7a8c33e07bced9b7`.
Each run retains its `results.json`, frozen inputs and serial logs, with
adjacent `<run>-build.log` where built and `<run>.log`. No failed evidence was
discarded or relabeled.

`FdTable::duplicate` and `duplicate_to` now combine source validation and
destination installation under the existing table lock. The replaced target's
final release remains outside that lock. The old pointer-based `install_fd`
entry was removed after checking its callers, and the out-of-range target test
now uses the actual `do_dup2` boundary. Existing mlibc checks retain minimum-FD,
full-width invalid arguments, close-on-exec, same-FD no-op and fork/exec flag
behavior; pending open/pipe reservations still reject targeted replacement.

All six builds and **19/19 CTests** passed, excluding independent application
repeat and benchmark tests. Every functional report finalized **22 suites /
119 cases**, including **10255 SMP assertions**, with no unreached cases.
Functional and framework reports, plus both final focused runs, record source
`fdf3f967efbf2a4c6ef724ee53b0d4a5bb74620574ec16a7c586f2fe82eafd7b`.

| Configuration | Functional report | Framework report | Production boot |
| --- | --- | --- | --- |
| ARM64 Debug | `1789538440730626927` | `1789538555339744318` | `run-u5alen41` |
| ARM64 Release | `1789538606017324200` | `1789538709912149006` | `run-2ey1vy17` |
| x64 Debug | `1789538440563533873` | `1789538563842310428` | `run-449_4t3z` |
| x64 Release | `1789538604699291599` | `1789538714894092613` | `run-7msigcdl` |
| RV64 Debug | `1789538441541545894` | `1789538551850593439` | `run-lngm4ds5` |
| RV64 Release | `1789538597828855629` | `1789538694071319469` | `run-vwjeek5l` |

Reports follow the preceding validation and production-boot path conventions;
ARM64 Debug console input also passed in `run-_pk5qzxg`. Matrix logs use
`<evidence-root>/<configuration>-fd-dup-{build,ctest}.log`. All **234 pytest
tests**, Ruff, clang-format and `git diff --check` passed; host output is in
`fd-dup-host-tests.log`. Fallible heap-backed metadata, current-source complete
application repeats, six full-application long runs and calibrated-performance
acceptance remain open; these functional results do not replace those gates.

### Fallible FD-Table Cloning (2026-09-16)

`FdTable::clone()` promised a null result on allocation failure, but ordinary
kernel `new` panicked instead. The actual `fork` caller also ignored a null
clone. Cloning now uses the existing fallible runtime heap allocator and
placement construction; `sys_fork` checks the result, removes the unfinished
child through its existing cleanup path and returns `ENOMEM` before publishing
the child to the parent or scheduler. Ordinary `operator new` is unchanged.

`vfs/fd_boundaries` exhausts capacity for real FD-table-sized allocations,
checks that cloning fails without changing source descriptors, close-on-exec
flags or CWD/file references, releases the held allocations, and verifies a
successful clone/release and an intact source read. The fixture explicitly
calls `close_all()` before deleting a populated table, matching existing table
ownership conventions.

The new native `users/fork_fd_allocation_rollback` case repeats 16 failed forks
and successful retries. A validation-only observer temporarily exhausts the
real heap around the table-allocation boundary and releases that pressure
before normal child cleanup; it does not supply a substitute clone result.
The case requires `ENOMEM`, no remaining child, exact heap/page/process/thread/
VFS recovery, preserved descriptor flags and offsets, a writable parent COW
stack, and a subsequent successful fork/wait. This specifically covers the FD
table allocation, not every earlier heap-backed allocation in `fork`.

Original evidence is retained under `build/timing-recovery-9bEBrP/`:

| Run | Observed result |
| --- | --- |
| `fd-clone-red` | Unexpected kernel panic in `fd_boundaries`; later VFS cases explicitly remained unrun. |
| `fd-clone-green` | Panic removed; two recovery assertions failed because the new fixture omitted `close_all()` before deleting its successful copy. |
| `fd-clone-cleanup` | Fixture cleanup corrected; VFS and VFS SMP passed, with 3469 passing FD-boundary assertions. |
| `fork-fd-red` | Real fork boundary failed the required result, user error mask `0x4`; 3166 passing assertions and one failure. |
| `fork-fd-green` | VFS, VFS SMP, users, exec, mlibc and BusyBox suites passed; the native rollback case recorded 50657 passing assertions on x64 Release. |

Each run keeps its frozen inputs, `results.json` and raw serial logs; adjacent
`<run>-build.log` and `<run>.log` retain build/runner output. The first panic
source is `0f3c491ad480a9ff4ae754f4343b78c20edc64d93b7b2bd9f7ce08c1e8745628`;
the native fork red source is
`7d9d8bc53245e77bc3dea8116d2a7aea9a7ff9d78aa4ee82139c93b0242c67e7`.

The six builds record the repaired source
`167074cb96ad15a0ac1e243c871cba9135e2a3f11c2a709547992608445d3242`.
All six functional reports completed **22 suites / 120 cases** each, with no
unreached cases. Every configuration additionally completed
**1000 complete core/application lifecycle cycles**, with **101 exact
resource-recovery checkpoints** per application report. Framework self-checks,
production boot and ARM64 Debug console input also passed: **25 CTests in
aggregate**, excluding only the independent benchmarks. All **234 host pytest tests**, Ruff,
clang-format and `git diff --check` passed before this evidence update.

| Configuration | Functional report | Application report | Framework report | Production boot |
| --- | --- | --- | --- | --- |
| ARM64 Debug | `1789539651575293739` | `1789539750005825264` | `1789540506448157161` | `run-9uwy_y8u` |
| x64 Debug | `1789539651054276869` | `1789539748284227665` | `1789540475651234818` | `run-g8m60ntv` |
| RV64 Debug | `1789539652176079514` | `1789539752122922568` | `1789540532434310053` | `run-gl482oiq` |
| ARM64 Release | `1789540529166670052` | `1789540566419812423` | `1789541109827151246` | `run-9d7fdx9p` |
| x64 Release | `1789540501759849753` | `1789540523298052733` | `1789541030863483137` | `run-6ajzq8_0` |
| RV64 Release | `1789540567887016180` | `1789540732893721151` | `1789541197233638009` | `run-m8m2wmci` |

Validation paths are `build/<configuration>/validation/<report>/results.json`;
boot paths are `build/<configuration>/production-boot/<run>/guest/results.json`.
The ARM64 Debug console run is `run-qh4er5f0`. Matrix logs are
`<evidence-root>/<configuration>-fd-clone-{build,ctest}.log`; host output is
`fd-clone-host-tests.log`. The preceding descriptor-duplication matrix's
**19/19 CTests was likewise the six-configuration aggregate**.

The completed reports were checked for matching source identity, finalized
status, no unrun functional cases, and unchanged resources across all 101
application checkpoints. These routine runs do not meet the separate
30-minute-and-10000-cycle stability gate. Remaining work includes other
heap-backed process/VMA/container metadata failures, full-application long runs
on all six configurations and calibrated Release performance acceptance.
Network and persistent-storage behavior remain outside scope.

### Fallible Address-Space Metadata (2026-09-16)

`create_user_address_space()` already had page-table and ASID rollback for a
failed metadata allocation, but its `make_unique` used panic-on-OOM `new` and
never reached that branch. It now allocates through the existing runtime heap
allocator, returns `OutOfMemory` through the existing cleanup branch, and
placement-constructs the successfully allocated object under the same unique
owner. Its fork, exec and init callers already handle the returned error;
ordinary `new` and unrelated factories are unchanged.

The new `mm.transactions/address_space_heap_rollback` test uses the real heap
pressure helper, not a substitute allocator or error result. It requires 256
consecutive metadata-allocation failures to return `OutOfMemory`, with exact
physical-page recovery after each attempt. After releasing pressure, it reuses
the existing ASID capacity/reuse check to acquire all 254 available leases
(one belongs to the worker), and requires exact final heap/page recovery.

Evidence remains under `build/timing-recovery-9bEBrP/`:

| Run | Observed result |
| --- | --- |
| `as-heap-red` | Unexpected kernel panic in the new case; later ASID/unmap cases remained explicitly unrun. |
| `as-heap-green` | Six focused suites passed, including MM transactions/permissions, native users/exec, mlibc and BusyBox. |
| `as-heap-asid-negative` | Temporarily omitted ASID release: the expected error and full-capacity checks failed, with 65416 passing assertions and two failures. |
| `as-heap-pages-negative` | Restored ASID release but temporarily omitted page-table release: 65673 passing assertions and 257 physical-page recovery failures. |

Both temporary omissions were removed before the final six builds. The
original panic source is
`a2615aa856a1212e66e9be6001ddf5025cc93424c7b71d2e0677a6ca3cce9b98`.
The green run and all final functional/framework reports share source
`46209cec0a17e11e5a13d7706f0d5f1fd7e8d82ed113b1bd92b432df3f08d9d9`.
Each run retains its frozen inputs, report and raw logs; adjacent build/runner
logs use the same naming convention as the preceding slices.

All six configurations passed **22 suites / 121 functional cases**, framework
self-checks and production boot, plus ARM64 Debug console input: **19 CTests
in aggregate**, excluding application repeats and benchmarks. No functional
cases were unrun. The new case recorded 65924 passing assertions on ARM64,
65930 on x64 and 65946 on RV64, in both build types. All **234 host pytest
tests**, Ruff, clang-format and `git diff --check` passed.

| Configuration | Functional report | Framework report | Production boot |
| --- | --- | --- | --- |
| ARM64 Debug | `1789541669996636763` | `1789541700870057200` | `run-dl7u0vzl` |
| ARM64 Release | `1789541726173272641` | `1789541747083186533` | `run-0icjigkp` |
| x64 Debug | `1789541669645583558` | `1789541702870649822` | `run-gzfkgbr4` |
| x64 Release | `1789541726152445651` | `1789541750312248978` | `run-qd9ko9ye` |
| RV64 Debug | `1789541670210230077` | `1789541704883642850` | `run-zli_hi_r` |
| RV64 Release | `1789541730474731688` | `1789541751452073380` | `run-n6xf1e_p` |

Report paths follow the preceding conventions; ARM64 Debug console input is
`run-dfk6130j`. Matrix logs use `<configuration>-as-heap-{build,ctest}.log`;
host output is `as-heap-host-tests.log`. The preceding 1000-application-cycle
matrix remains evidence for its recorded source, not renewed application
repeat acceptance of this revision. Other heap-backed process/VMA/container
metadata failures, current-source application repeats, six full-application
long runs and calibrated Release performance acceptance remain open.

### Fallible VMA Admission and Native mmap Rollback (2026-09-16)

`AddressSpace::add_vma()` returned a success flag, but the conditional list
insertion allocated its node through panic-on-OOM `make_unique`. The existing
containers/MM ABI bridge now exposes fallible, aligned, zeroed runtime storage;
`LockedList::push_front_unless()` constructs a node only after allocation
succeeds. Conflict detection and publication keep their existing single lock,
and rejected nodes retain ordinary unique-owner cleanup outside that lock.
Ordinary `new`, unconditional insertion and unrelated factories are unchanged.

Two real-heap-pressure tests cover this boundary:

- `mm.transactions/vma_heap_rollback` requires failed admission to leave the
  existing VMA and permissions unchanged, then verifies successful retry/removal
  and exact heap/physical-page recovery after address-space destruction.
- `users/mmap_heap_rollback` performs four native syscall failure/retry cycles.
  It requires `ENOMEM`, exact lifecycle resources after releasing pressure,
  readable/writable existing mappings and the next successful mapping at the
  unchanged allocation cursor. It touches and unmaps the retry mapping too.
  Pressure is established before entering `mmap`; no production failure hook or
  substitute syscall result is used.

Evidence is retained under `build/timing-recovery-9bEBrP/`:

| Run | Observed result |
| --- | --- |
| `vma-heap-red` | Actual kernel panic in the direct VMA case, with later cases explicitly unrun. |
| `vma-heap-green` | Eight focused suites passed after the shared insertion fix. |
| `mmap-heap-green` | Native users and MM transaction suites passed with both new cases. |
| `mmap-heap-red` | Temporarily restored only the old node allocation; the native mmap case reproduced the kernel panic. |

The direct red source is
`ecce9351f5140e2f3ea9513e10b99e22204b781334a8aba42ce6bb6c80aee56b`;
the native negative-control source is
`1302e1054d5ef35c52818f1d63436e687e0ec2863b2939de715777107903e431`.
The temporary reversal was removed before the final six builds. Their
functional/framework reports and `mmap-heap-green` share source
`f0f98987f395b9766c92cfceaca6de9cc4c0151fcf840cd2f28f592b2dfc7ab6`.

All six configurations passed **22 suites / 123 functional cases**, framework
self-checks and production boot, plus ARM64 Debug console input: **19 CTests
in aggregate**, excluding application repeats and benchmarks. All **234 host
pytest tests**, Ruff, clang-format and `git diff --check` passed.

| Configuration | Functional report | Framework report | Production boot |
| --- | --- | --- | --- |
| ARM64 Debug | `1789542627292831341` | `1789542658750852526` | `run-7q4pftkh` |
| ARM64 Release | `1789542683138254873` | `1789542698316310798` | `run-6xd2uo_r` |
| x64 Debug | `1789542627128884629` | `1789542662962197234` | `run-0er61xcm` |
| x64 Release | `1789542684717918940` | `1789542704471452433` | `run-twn7r04h` |
| RV64 Debug | `1789542627732391885` | `1789542665629459010` | `run-154b3l1c` |
| RV64 Release | `1789542689265366566` | `1789542705614799247` | `run-9kqig3_3` |

Report paths follow the preceding conventions; ARM64 Debug console input is
`run-89r841ob`. Matrix logs use `<configuration>-vma-heap-{build,ctest}.log`;
host output is `vma-heap-host-tests.log`. This closes VMA admission, not every
heap-backed fork/process publication path. Current-source application repeats,
six full-application long runs and calibrated Release performance acceptance
remain open; older 1000-cycle reports retain only their recorded-source claim.
Network and persistent storage remain excluded.

### Fallible Process Creation and Stable SMP Test Milestones (2026-09-16)

`ProcessManager::create_process()` now propagates allocation failure for the
process object, shared-reference control block and process-table node. The
explicit `SharedPtr::try_make` receives the existing runtime allocation bridge,
keeping the core module independent of the runtime heap; its temporary unique
owner destroys a constructed object if control-block allocation fails.
`LockedHashMap::try_insert_or_update` allocates before publication and shares
the existing locked publication/unlocked retirement implementation. Failed
creation neither publishes a process nor increments the successful creation
counter. Ordinary `new`, existing infallible factories and unrelated map
callers retain their previous behavior.

The new `process/heap_rollback` case exhausts the real heap, gradually releases
small owned allocations, and retries through the public process manager until
creation succeeds. Every failed attempt requires `OutOfMemory`, unchanged
process counts and creation/exit counters, and exact heap/page recovery; the
successful attempt must be discoverable, removable and fully reclaimed.
`users/fork_process_allocation_rollback` adds four real syscall failure/retry
cycles, checking `ENOMEM`, no waitable child, exact lifecycle resources,
unchanged parent descriptor flags/offset and writable parent state. It reuses
the native fork checks and the pre-syscall real-heap-pressure controls; no
substitute allocator result or new production failure hook is used.

Evidence is retained under `build/timing-recovery-9bEBrP/`:

| Run | Observed result |
| --- | --- |
| `process-heap-red` | Actual kernel panic in process creation. |
| `process-heap-green` | Seven focused suites passed with fallible creation. |
| `fork-process-green` | Native users and process rollback suites passed. |
| `process-heap-control-negative` | Temporarily leaked the process object after control-block allocation failure: nine assertions failed, including per-attempt heap recovery. |
| `process-heap-publication-negative` | Temporarily counted creation before process-table publication: two counter assertions failed. |

Both negative controls were restored. The original panic source is
`00d483388485361b2e2ddc23ed7a86657f38b24eb751f3d585018e9ece47423d`;
the two negative-control sources are respectively
`04514dd258a20d16098a67daf02e26aaa36dc262f76e901ed4d8b7a68f37f7b1`
and `d4e9e07e4ab8a09e0b2cf12dbbd3181108448039c2b443eaf8151a3c941f546b`.
An unrelated external whole-tree build was left running. Intermediate x64
checks used `build/x64-process-heap`; each normal matrix directory was reused
only after its external workflow ended. No external process was stopped.

The first matrix passed 18/19 CTests. RV64 Release report
`1789543813585423544` retained a `vfs.smp/shared_references` case timeout, not
a process-rollback failure. Thirty-three serial replays passed, while a fixed
120-run, three-lane cohort reproduced three timeouts (runs 8, 93 and 95 under
`process-heap-rv-vfs-parallel-*`). This exposed a test synchronization defect:
the peer could miss an intermediate phase because `wait_for` required exact
equality even though the milestones advance monotonically.

The existing real dual-CPU VFS case now deliberately resumes its peer after
the owner has advanced past the intermediate release. With the old equality
condition, `vfs-phase-red`, `vfs-phase-red-2` and `vfs-phase-red-3` all timed out
at the unchanged five-second deadline, using source
`6a34769a98c95c905f2afbde8acd507eb2b78955410ca192e2d09aa4db2a6b9a`.
The shared helper now accepts an already-reached monotonic milestone; its
container, VFS and timer users were inspected for that nonwrapping contract.
All VFS ownership/resource assertions remain, and the forced interleaving is
retained. This fixes the validation handshake, not an established kernel
deadlock. No timeout was relaxed or passing replay substituted for the failure.

`vfs-phase-green` passed the five affected focused suites. The final matrix
and all **120/120** post-fix three-lane replays (`vfs-phase-green-parallel-*`)
share source
`8b4dcf9947f595d844be1749f5669f366c007e577fb96f1d19596fc01994a7e5`.
Each replay passed all 10255 VFS assertions with identical frozen kernel and
fixture hashes. The final six configurations each passed **23 suites / 125
functional cases**, framework self-checks and production boot, plus ARM64
Debug console input: **19 CTests in aggregate**, excluding application repeats
and benchmarks. All **234 host pytest tests**, Ruff, clang-format and
`git diff --check` passed.

| Configuration | Functional report | Framework report | Production boot |
| --- | --- | --- | --- |
| ARM64 Debug | `1789544466632342176` | `1789544502502335488` | `run-vhyifd7x` |
| ARM64 Release | `1789544536364801544` | `1789544551811961797` | `run-wkol0dq8` |
| x64 Debug | `1789544466443122753` | `1789544506724264620` | `run-trkevrde` |
| x64 Release | `1789544538931338676` | `1789544558838903506` | `run-kwd18ra2` |
| RV64 Debug | `1789544467251164849` | `1789544506721726819` | `run-fli571mq` |
| RV64 Release | `1789544537241952070` | `1789544553006247464` | `run-iyjr74b_` |

Report paths follow the preceding conventions; ARM64 Debug console input is
`run-_uns0_dq`. Final matrix logs use
`<configuration>-process-heap-final-{build,ctest}.log`; host output is
`process-heap-phase-final-host-tests.log`. First-matrix logs omit `-final` and
remain intact. Further heap-backed fork/thread publication paths, renewed
1000-application-cycle runs, six full-application long runs and calibrated
Release performance acceptance remain open. Network and persistent storage
remain excluded.

### Fallible Fork Metadata Publication (2026-09-16)

Native `fork` now propagates allocation failure while copying VMAs, creating
the Thread, registering it in the child process, and publishing the child in
its parent's wait list. VMA copying uses the existing checked insertion;
`LockedList::try_push_front` shares publication with ordinary insertion.
`Thread::try_create` is also used by the existing process-thread and init-thread
creation paths. `Process::register_thread` transfers ownership only on success,
and every caller checks its result. The Thread destructor now owns its kernel
stack, including when registration fails before a Process can own the Thread.
Parent publication uses an explicit fallible operation; existing orphan
reparenting and snapshot iteration retain their previous allocation contracts.
This does not claim that every process-lifecycle allocation is now fallible.

The new `users/fork_metadata_allocation_rollback` case uses real heap pressure
at those four boundaries, starting the VMA failure after one successful copy.
Validation-only overrides of no-op boundary observers acquire/release pressure;
they do not substitute allocation results. Each failure must return `ENOMEM`,
leave no waitable child, restore exact lifecycle resources, preserve parent
descriptor flags/offset and writable COW state, and permit a successful
fork/wait retry. The native helper and existing accounting are reused.

Evidence remains under `build/timing-recovery-9bEBrP/`:

| Run | Observed result |
| --- | --- |
| `fork-metadata-probe` | Visible boundary probes confirmed that VMA rollback passed before ordinary Thread allocation panicked. Earlier info-level probes had been filtered; the earlier panic logs alone did not establish the allocation site. |
| `fork-metadata-registration-red` | After fallible Thread creation, registration-node allocation panicked. |
| `fork-metadata-parent-red` | After checked registration, parent child-list allocation panicked. |
| `fork-metadata-green` | Native users, process rollback and container suites passed; the new case passed 457551 assertions on x64 Release. |
| `fork-metadata-vma-negative` | Temporarily restored ordinary VMA-node allocation; the native case panicked. |
| `fork-metadata-stack-negative` | Temporarily leaked the unpublished Thread's stack; resource recovery and native completion assertions failed. |
| `fork-metadata-parent-negative` | Temporarily retained the failed child in the process table; resource recovery and native completion assertions failed. |

All temporary negative controls and diagnostic probes were removed. The
initial `fork-metadata-vma-red` and `fork-metadata-thread-red` reports also
remain intact. No timeout or resource assertion was relaxed.

All six functional reports have passed **23 suites / 126 cases** with source
`79e730dc0b094efe064c566a0535a5726c7924edf83fba6ed527789e862a76f4`.
All **234 host pytest tests**, Ruff, clang-format and `git diff --check` passed.
After all six builds completed, a two-space continuation-indent correction
restored the exact source of the earlier focused green run,
`d9c49b3c5501c6bab5153621a2d0f21068f8c31fd09e90617a4708b803e93272`;
it does not change kernel logic. Frozen-image reports keep their original
source identity, including during the documentation updates.

All six configurations passed their complete routine matrix: **25 CTests in
aggregate**, including framework self-checks, production boot and ARM64 Debug
console input. Every application guest completed **1000 full core/application
cycles**, with **101 identical resource checkpoints**. Report completeness,
case counts, exact resources and common source identity were checked directly.

| Configuration | Functional report | Application report | Framework report | Production boot |
| --- | --- | --- | --- | --- |
| ARM64 Debug | `1789546010791244115` | `1789546050561463757` | `1789546786850503845` | `run-kfx4bmr9` |
| ARM64 Release | `1789546812533345143` | `1789546829369674439` | `1789547384166283575` | `run-q43h3bd9` |
| x64 Debug | `1789546010198733924` | `1789546054213771818` | `1789546785837964808` | `run-u00cv5vr` |
| x64 Release | `1789546810418729250` | `1789546830898645123` | `1789547369167194710` | `run-v0xv57o7` |
| RV64 Debug | `1789546011197599834` | `1789546059320844140` | `1789546867406420446` | `run-kdqi5fd8` |
| RV64 Release | `1789546896390983200` | `1789547044972266195` | `1789547590888809855` | `run-eosujmpu` |

Report paths follow the preceding conventions; ARM64 Debug console input is
`run-beps8r_3`. Matrix logs use
`<configuration>-fork-metadata-final-{build,ctest}.log`; host output is
`fork-metadata-host-tests.log`.

Six full-application long runs were started with those same frozen images
under `fork-metadata-long-<configuration>` in the evidence directory. The x64
Debug run **failed** with `unexpected_kernel_panic`: after checkpoint 680 its
serial log reported general-protection exception vector 13, error 0,
RIP `0x182504`. Symbolization against the saved ELF places that instruction in
`PerCpuData<Thread *>::data()`, comparing memory at `this + 0x400`; the cause
of the invalid access is not yet established. Other CPUs continued reporting
unchanged resources through checkpoint 760, which does not negate the fatal
exception. The runner retained the failure and exited 1. A previously passing
1000-cycle run is not evidence that this later failure is harmless.

The other five long runs were **cancelled** at the user's pause request on
2026-09-16. Their finalized reports retain `status: error`,
`observed: cancelled`; none reached 10000 cycles. The last complete checkpoints
were ARM64 Debug 4500, RV64 Debug 4110, ARM64 Release 5320, x64 Release 5590
and RV64 Release 5300 core/application cycles. Cancellation is not acceptance.
Each must satisfy both 10000 complete cycles and 30 host and guest minutes in
one instance. Saved guests retain their frozen images; any subsequent
kernel repair needs renewed acceptance of the changed kernel. Calibrated
performance acceptance also remains open; the observed external host workload
and these concurrent correctness runs are not a quiet performance environment.
No unrelated process was stopped. Network and persistent storage remain excluded.

### Scheduler Migration Diagnostic Checkpoint (2026-09-16)

The short-term diagnostic slice is complete; the scheduler repair and overall
acceptance are **not complete**. Work was paused at the user's request before
the broader context-ownership repair. Its unfinished production/assembly edits
were removed; the existing completed fixes and new failing regression remain.

The new `scheduler/migration_current_owner` regression uses the real isolated
scheduler queues, current-thread binding and `LoadBalancer::migrate_task`.
It represents the actual yield/preemption window after Ready publication but
before saving the running thread's continuation. It checks that migration
does not transfer that executing thread, and cleans up both queues before
reporting failure; it never dispatches a synthetic CPU context. The scheduler
workload now requires at least two CPUs, checked by the host before loading
artifacts.

The x64 Debug report
`build/timing-recovery-9bEBrP/scheduler-migration-owner-red/results.json`
is finalized and **failed**: the three preceding scheduler cases passed;
the new case passed three assertions and failed its ownership assertion.
Its recorded source is
`7d039766fd7964acc45aa8eb4621d89694e2d418516a6aa02e65a8346058b505`.
This establishes an executable ownership-contract violation, not proof that
it caused the earlier general-protection fault. The test remains enabled and
the current full functional matrix must not be described as green. The earlier
23-suite/126-case matrix predates this regression.

Two bounded GDB replays used the exact original failing x64 Debug kernel and
initramfs. `x64-gp-replay.12Q6QG` reached 1190 core/application cycles and
992.548 guest seconds without another GP before its 1200-host-second deadline
(which included debugger setup). `x64-gp-core.Z1Tuxj` reached 30900 core-only
cycles and 287.709 guest seconds before its 300-host-second deadline. Both
ended with host exit 124 and are **inconclusive diagnostic windows**, not
acceptance or evidence that the original fault is repaired. The longer
`x64-gp-extended.9lDTwu` replay was cancelled for the pause after checkpoint
500 at 423.064 guest seconds, also without another GP. These directories,
serial logs, debugger commands and original failures remain under the same
evidence root; no run was relabeled as passing.

All five owned long-run runners and the diagnostic QEMU/debugger were stopped
and reaped. Unrelated workloads were left running. Resume with execution
ownership through the completed context save, atomic dispatch/migration queue
ownership, then renewed matrix and long-run/performance acceptance; do not
infer safety from the mutable current-thread pointer alone.
The pause checkpoint's host suite passed **235 tests**, including rejection of
the scheduler migration workload with only one CPU. Ruff and staged whitespace
checks passed. The new regression passes clang-format; a full-file formatting
audit also found pre-existing layout differences outside the changed hunks in
the RV64 boot/page-fault code and `ut_kernel.hpp`. Those files were not broadly
reformatted for this checkpoint.

## Required Core Acceptance Scope

Confirmed on 2026-09-14: the current correctness and stability work must cover all six core paths below. Use the existing [core invariants and acceptance cases](../moss-todo.md#10-修复后必须成立的核心不变量) to derive concrete checks.

| Core Path | Required Acceptance Behavior |
| --- | --- |
| Memory and user isolation | Preserve page and heap ownership, user/kernel access boundaries, COW permissions and consistent mappings. |
| Scheduling and wakeup | Preserve exclusive task execution and queue ownership; enforce affinity and avoid lost wakeups. |
| Timers | Preserve expiry and cancellation behavior, callback lifetime and explicit capacity-exhaustion behavior. |
| Process lifecycle | Preserve fork state, the old image after failed exec, and complete exit/wait resource recovery. |
| Signals | Preserve supported delivery and masking behavior, validate restored user state and reject invalid signal frames. |
| VFS and pipes | Preserve access modes, shared offsets, data ordering, EOF and resource ownership through close and reuse. |

Each path requires normal-behavior checks, boundary and error inputs, rollback after resource exhaustion, and resource-recovery checks. Shared state additionally requires controlled concurrent interleavings. Existing kernel defects remain visible as failed results linked to repair items; missing or skipped checks do not establish acceptance of the required path.

Confirmed on 2026-09-14: necessary repairs to the real kernel paths are part of this delivery. A failing regression test establishes the defect, but delivery requires the production repair and renewed acceptance evidence. Preserve the original failed runs and required assertions throughout that work.

This confirms coverage and acceptance obligations. The stability gates below and the revised performance gate in [Result Validity and CI](#result-validity-and-ci) are confirmed; numerical performance-regression thresholds require calibration. The initial implementation and workload descriptions below do not establish completion of this expanded scope.

## Required Stability Gates

Confirmed on 2026-09-14 as initial engineering acceptance thresholds:

| Gate | Required Execution |
| --- | --- |
| Routine regression | Core functional checks, error rollback, deterministic concurrent interleavings and at least 1,000 completed kernel lifecycle cycles. |
| Nightly or stage acceptance | ARM64, x64 and RISC-V 64, each in Debug and Release. Every configuration must sustain workload execution in one kernel instance for at least 30 minutes and complete at least 10,000 kernel lifecycle cycles in that instance. |

Both duration and cycle count are required for long-run acceptance. The six configurations require at least three aggregate guest-hours, excluding build, startup and reporting. Each workload must declare a complete lifecycle cycle; reporting individual operations or polling iterations cannot satisfy the cycle threshold. Rebooting starts a new run and cannot preserve accumulated stability credit.

Periodically check physical-page, heap, file-descriptor and task ownership/accounting against the expected baseline after owned resources are released. Any unexpected panic, loss of progress or resource leak fails the run. Passing functional tests or reaching the elapsed-time target does not override these failures.

These gates are agreed requirements, not completed long-run evidence or measured runtime estimates. Workload definitions, progress deadlines and runner support must implement them explicitly; the initial short-suite timeouts described below do not cover long-run acceptance.

## Framework Reuse

Extend the existing `ut_kernel` framework for real kernel functional tests, suite-level isolation, and reliable result reporting. `ut_kernel` is the project's kernel unit-testing framework, not Unity; Unity will not be introduced for this work.

The implementation adds a distinct benchmark interface, including explicitly registered function microbenchmarks, while sharing the validation image, workload selection, and result-reporting infrastructure described here. See the [usage guide](kernel-validation-usage.md) for commands and authoring examples.

### Test Declaration and Registration

This authoring interface is implemented by extending the existing `ut_kernel` APIs; it does not introduce another testing library.

Keep the familiar `register_test(name, function)` and `expect(condition)` entry points. Add explicit `register_suite` grouping with a stable suite ID and declared runtime prerequisites. A suite's declaration callback registers its cases but does not execute their bodies. A single explicit registration entry point invokes the suite registration functions before selection and execution; correctness must not depend on global-constructor execution or static-initialization order.

The following sketch omits the existing framework namespace. `register_suite` is implemented; the named case functions represent tests of the production page allocator.

```cpp
void register_mm_tests() {
  register_suite("mm", [] {
    register_test("page_alignment", test_page_alignment);
    register_test("page_reuse", test_page_reuse);
  });
}
```

An author adds a case to its suite's registration function. Adding a new suite also requires one entry in the central suite catalog. This explicit catalog favors predictable kernel startup and auditable selection over automatic discovery of arbitrary global test objects. Tests create their runtime fixtures when their bodies execute, not while registering themselves.

Registration uses static-lifetime descriptors and bounded framework storage, without allocating registry nodes from the kernel heap. Preserve declaration order within each suite. Validate duplicate IDs, invalid metadata, and capacity overflow before executing cases; these are explicit registration failures, never silent omissions. The registered catalog and selected case list support enumeration and the host's completion accounting.

Ordinary `expect` records an assertion without panicking. When a failed precondition makes further operations unsafe, the case uses an explicit guard and normal return, allowing constructed local resource guards to run. An assertion evaluates its condition once and contributes exactly once to the assertion counts, including when its result is inspected by a guard. After the case returns and cleanup is checked, any failure prevents subsequent cases in that suite from running. Do not introduce exceptions or `setjmp`/`longjmp` to escape arbitrary test-helper call stacks; helpers must propagate failure through normal control flow.

Keep suite and case setup and cleanup explicit where needed, and validate cleanup against the real owned resources. Destructive behavior and expected panic or timeout belong to case metadata and separate guest execution, not an in-guest recovery mechanism. The current flag-based panic simulation must not count as evidence of a real kernel panic.

The separate benchmark API continues to use the confirmed named scenario and untimed preparation / timed operation / untimed validation-and-cleanup model. Registering a benchmark must not perform its measured work or create its live fixture prematurely. A registered callback can create local fixtures and pass capturing lambdas to the templated batch loop without requiring heap-backed callable storage in the registry.

Framework self-validation covers deferred case execution, catalog completeness, duplicate IDs, capacity overflow, assertion evaluation and accounting, normal-return cleanup, suite stop behavior, and the real fatal paths already required below.

## Initial Functional Coverage

| Area | Required Behavior |
| --- | --- |
| Physical memory | Allocate pages of different orders; verify alignment and non-overlap of live allocations; release pages and exercise repeated reuse. |
| Page permission boundary | Inspect production kernel and active user roots for supervisor-only kernel leaves, x86 user permission chains, kernel W^X and read-only/NX direct-map aliases. Create/clone/free address spaces without changing shared kernel tables. This is not complete user-fault containment acceptance. |
| VFS | Open and read real files; verify contents, file position, EOF, close, and error handling, including rejected writes to the current read-only ramfs. |
| Userspace and processes | Verify syscall return values, user-domain/VMA policy, bounded strings, cross-VMA copies and rejection, then process creation, execution, exit and reaping. Policy rejection is not CPU-fault recovery or complete uaccess acceptance. |

Physical-memory cases must exercise the production page allocator. VFS cases must use the real mounted filesystem environment. Tests that claim userspace syscall coverage must invoke the architecture's real user-to-kernel entry path; directly calling a syscall handler does not establish that coverage.

x86 users tests also exercise legacy FP state through yield/fork/exec and x87
exception termination; the SMP case checks inheritance on CPU1. The explicit
`users.simd_fault` workload is separate from the default set because the current
TCG execution does not deliver #XM. Its assertion remains a failure, not an
expected-pass classification; it does not establish SIMD-fault acceptance.

Framework self-validation must include intentional assertion failures, kernel panics, and timeouts, and must verify that failures reach the host and CTest. Startup and image-loading failures must remain distinguishable from completed test results, as required by ADR-0001.

## Initial Benchmark Workloads

| Workload | Measurement |
| --- | --- |
| Physical-page allocation and release | Operation duration for different allocation orders. |
| VFS file reads | Throughput while reading actual file contents. |
| Userspace `getpid` | Round-trip duration through the actual user-to-kernel syscall path. |

The current ramfs rejects writes, so the first version measures its read behavior. The expanded catalog below adds process lifecycle and the other required core paths; the initial set alone is not the acceptance target.

## Required Benchmark Coverage

Confirmed on 2026-09-14: retain the existing five scenarios (`bench.allocate`, `bench.release`, `bench.combined`, `bench.read`, and `bench.getpid`) and add representative performance coverage for each of the six required core paths.

| Core Path | Required Representative Measurements |
| --- | --- |
| Memory and user isolation | Allocation/release costs and page-fault/COW handling duration. |
| Scheduling and wakeup | Context-switch duration and latency from wakeup to actual execution. |
| Timers | Latency from timer expiry to callback execution. |
| Process lifecycle | Duration of a complete fork/exec/exit/wait lifecycle. |
| Signals | Latency from signal send to execution of the userspace handler. |
| VFS and pipes | File-read and pipe-transfer throughput. |

Each path must have at least one representative scenario, with explicit workload parameters and measurement boundaries for its declared metrics. The added scenarios must exercise the real production paths and follow the fixed-QEMU environment, measurement-validity and noise-calibration requirements. The existing `getpid` round trip remains additional syscall coverage; it does not substitute for the other lifecycle and delivery measurements.

All six paths now have implemented representative measurements: the five original
scenarios plus `bench.fault`, `bench.cow`, `bench.switch`, `bench.wakeup`,
`bench.timer`, `bench.lifecycle`, `bench.signal` and `bench.pipe`. Their exact
parameters, timing boundaries and limitations are in the
[usage guide](kernel-validation-usage.md#single-function-measurements).
All thirteen have completed real Release guest measurements on all three ISAs.
That establishes execution and measurement validity, not passage of the
repeat-based performance gate; calibration outcomes are recorded below.

## Benchmark Execution and Sampling

Each kernel benchmark scenario, identified by its operation and workload parameters, runs in a fresh QEMU instance. Scenarios execute serially and separately from functional tests, using a Release build of the validation image. All warmup and recorded batches for one scenario share that instance; individual samples do not each require a new kernel boot.

Startup, warmup, framework logging, and result validation are outside the timed interval. Each recorded batch measures repeated real kernel operations. Preserve the raw batch measurements and operation counts, derive per-operation duration or throughput, and report the median across those batch-derived values. A distribution of batch averages is not a distribution of individually timed operation latencies.

The execution, sampling, and clock-validation policies are confirmed. The requested multi-vCPU and larger-memory revision replaces the earlier single-vCPU, 256 MiB proposal. The CI policy defines how correctness, measurement validity, and performance changes affect the result.

### Initial Configurable Defaults

The resource requirement and concrete defaults of four vCPUs and 2 GiB are confirmed. These defaults are not measured accuracy or execution-time guarantees. Effective values and any overrides must appear in the report.

| Setting | Initial Default |
| --- | --- |
| Functional and benchmark guest resources | Four vCPUs and 2 GiB RAM, configurable and checked against actual kernel CPU and memory readiness. |
| Benchmark execution profile | Release validation image; explicit TCG acceleration; the architecture's existing QEMU machine and CPU model; no instruction-counting mode. |
| Warmup | Five completed, untallied measurement batches after clock validation and any batch-sizing pilot. |
| Recorded samples | Thirty completed batches for each scenario. |
| Batch sizing | For a new baseline, a bounded pilot targets approximately 1 ms per batch, subject to the scenario's declared iteration and resource capacity. The resulting operation count is fixed for the recorded batches. |
| Host deadlines | At most 30 s for validation startup, 5 s per ordinary functional case including its setup and cleanup, and 60 s total per guest. Explicit workload metadata or runner options can override these limits. |

The PFA exhaustion workload now declares a 30 s case default: on 2026-09-06,
four concurrent 2 GiB RV64 Debug guests all hit the old 5 s deadline, but the same
images completed all ownership checks in about 16 s with a diagnostic 20 s limit.
Other workloads retain 5 s; explicit `--case-timeout` wins. This changes a host
execution budget, not allocation correctness or a performance threshold. Effective
budgets and host-observed case durations are recorded, and historical failures remain
failures. Functional/framework CTest has a 600 s outer guard for up to nine guests
and cleanup. Evidence and limitations are in `moss-todo.md`, section 3.10.

A baseline comparison reuses the baseline's fixed operation count when the workload and resource capacity permit it. It must not independently change allocator occupancy or other batch preconditions and silently call the results comparable. A caller may also specify a fixed operation count directly. Pilot work and its cleanup are outside the recorded samples; any operation or cleanup failure during a pilot or warmup still fails the scenario.

The batch-duration target is a sizing heuristic, not permission to exceed a fixture's capacity or a claim of statistical precision. Record the actual counter interval and instrumentation overhead. A zero or otherwise unresolvable interval is not a valid operation-duration sample; overhead-dominated measurements must state their limitation and must not be presented as exact isolated function time. Changing these defaults changes the recorded comparison conditions.

Functional cases and benchmark scenarios retain the required real interrupt and scheduler behavior; the framework does not globally suppress interrupts to improve a result. A production function's own locking and interrupt control remain part of that function's behavior. Multi-vCPU execution is now part of the required profile. Hardware-accelerated benchmark profiles remain outside the initial acceptance matrix.

A multi-vCPU guest does not change suite isolation or make benchmark scenarios execute concurrently. Single-function measurement uses one benchmark worker pinned to CPU 0, with its real interrupt and scheduler behavior retained. Record and verify the affinity and observed CPU identity. Parallel-worker throughput is a separately declared workload, not an implicit replacement for single-function operation timing. A baseline must match the CPU count, memory profile, affinity, and worker policy.

Deadlines are enforced by the host rather than relying on the guest clock under validation. Intentionally timed-out self-validation cases declare their expected timeout separately. An outer CTest deadline must leave enough time for the runner's applicable deadlines, partial-report finalization, and QEMU termination and reaping; it must not kill the runner before that work can finish.

### Multi-vCPU and Memory Prerequisites

The initial source inspection on 2026-09-06 found the following prerequisites. They have since been repaired and exercised in the acceptance matrix; this list records why changing QEMU arguments alone was insufficient:

- `scripts/run_qemu.py::build_qemu_args` forced test mode to one vCPU and used 256 MiB; its architecture configuration also capped x64 and RISC-V 64 at one vCPU.
- `src/boot/src/arch/x64/boot_impl.cpp` and `src/boot/src/arch/riscv64/boot_impl.cpp` left secondary-CPU activation unimplemented, and their `wait_for_all_cpus_active` functions returned one.
- ARM64 already had secondary-CPU initialization, but the four-vCPU, 2 GiB profile needed real execution evidence.
- x64 hardware initialization populated RAM information from platform defaults rather than the requested QEMU RAM size. Actual boot memory discovery and usable mappings needed adaptation for the enlarged profile.

The confirmed prerequisite work is to complete the missing secondary-CPU startup and required per-CPU runtime integration, verify CPU identity and affinity behavior, and make memory discovery and mapping reflect the actual boot environment. This explicitly includes implementation beyond changing the test runner's defaults.

Before accepting the profile, verify that all requested CPUs are actually online and can each execute bounded real kernel work. An emulator CPU count, a firmware topology entry, or an incremented shared count alone is not sufficient. Report requested, detected, and online CPUs separately. CPU affinity for a microbenchmark must constrain real execution, not only annotate its result.

Host memory configuration, generated device trees or boot memory maps, kernel mappings, and allocator-managed memory must agree after accounting for legitimate reserved regions. Use bounded owned allocations and accesses to demonstrate usable memory beyond the previous 256 MiB RAM window; do not infer that capability solely from `-m 2G` or require all 2 GiB to be free. Preserve the distinction between physical RAM and allocatable RAM in reports.

Do not silently clamp the new profile to one CPU or the old memory limit. Missing SMP or memory readiness is a failed or unsupported profile with its reason, not successful multi-core validation. An explicitly requested smaller diagnostic profile can aid repair but does not satisfy this acceptance requirement.

### Clock Validation

The architecture-specific frequency sources and calibration policy below are implemented and exercised by the Release benchmark matrix.

The current `hal::timer::frequency()` in `src/hal/timer/src/timer_hal.cppm` reads the ARM64 frequency register, but the x64 path contains a 1 GHz placeholder fallback and the RISC-V 64 path uses a platform default. Those defaults are not sufficient evidence for benchmark time conversion. The existing counter reads also need architecture-appropriate ordering for measurement boundaries.

| Architecture | Counter and Frequency Source |
| --- | --- |
| ARM64 | Ordered `CNTVCT_EL0` reads with a nonzero `CNTFRQ_EL0` frequency. [Arm documents these counter and frequency registers](https://learn.arm.com/learning-paths/servers-and-cloud-computing/arm_pmu/assembly/). |
| RISC-V 64 | Ordered `time` CSR reads with a validated `/cpus/timebase-frequency` property from the actual boot device tree. [Linux's RISC-V 64 initialization](riscv64_LINUX_SOURCE) uses this property; do not substitute the current fixed 10 MHz value when it is missing. |
| x64 | Ordered TSC reads. Use CPUID leaf `0x15` only when its ratio and crystal-frequency information are complete and nonzero, as specified in the [Intel architecture manual](https://cdrdv2-public.intel.com/868137/325462-089-sdm-vol-1-2abcd-3abcd-4.pdf). Otherwise calibrate against the explicitly enabled QEMU microvm i8254 PIT. [QEMU documents the optional PIT device](https://www.qemu.org/docs/master/system/i386/microvm.html). |

Frequency discovery or calibration runs once during each scenario's guest preparation, before warmup, and is reused by its batches. It does not require an additional QEMU instance per batch. Record the frequency source and value; a calibrated frequency additionally retains its reference-counter samples and uncertainty checks. PIT calibration must use the counter facilities actually available in microvm, preserve the runtime's timer requirements, bound retries and waiting, and reject ambiguous or inconsistent samples. The existing kernel clock derived from the same unverified TSC frequency is not an independent calibration reference.

Before accepting measurements, verify counter progress and nondecreasing reads, apply the required compiler and architecture ordering, and validate tick-to-time arithmetic against zero frequency and overflow. Frequency validation does not by itself establish adequate resolution or eliminate timing overhead for a short function; the batch and overhead rules below still apply.

If the counter or frequency cannot be validated, retain any readable raw ticks as diagnostic data, mark the benchmark measurement invalid or unsupported with its reason, and return a nonzero benchmark-run result. Do not publish derived nanoseconds, throughput, or a baseline comparison as valid. Functional suites remain independently runnable. The three initial supported QEMU profiles must demonstrate valid timing to satisfy benchmark delivery; reporting an unsupported clock is not a substitute for that acceptance evidence.

## Individual Function Performance

An additional confirmed requirement is to measure selected production kernel functions, beyond the initial fixed workload list. These measurements retain the real kernel environment and the revision-comparison rules above. Explicitly registered function microbenchmarks and the preparation, measurement, and cleanup model below are implemented in `src/test/framework/benchmark.hpp`.

### Explicit Function Microbenchmarks

Register a named benchmark scenario whose measured operation invokes the selected production function with declared inputs and preconditions. Reuse the same validation image, scenario selection, warmup, batch sampling, and reporting mechanisms. A function's own callees are included in its measured operation; this does not attribute exclusive CPU time to individual functions in a call graph.

Separate untimed batch preparation, the timed operation loop, and untimed result validation and cleanup. In an allocation-only benchmark, each timed call allocates pages into a bounded result buffer, and cleanup releases the successful allocations after timing. In a release-only benchmark, preparation allocates the pages and the timed loop releases them. Allocation followed by release inside the same timed operation is a distinct combined workload.

The following uses the implemented callback interface with an author-supplied `PageAllocationBatch` fixture. The fixture prepares bounded result storage, validates results, and releases owned pages. Preparation and cleanup return `bool`, including safe cleanup after partially failed preparation. The built-in `allocation_benchmark` in `src/test/validation.cpp` is the complete production-backed example.

```cpp
bench::register_benchmark("bench.my_allocate", [](bench::Context& context) {
  PageAllocationBatch fixture;
  context.measure_batches(
      [&](usize count) { return fixture.prepare(count); },
      [&](usize index) {
        fixture.results[index] = mm::PageFrameAllocator::allocate_pages(0);
      },
      [&](usize count) { return fixture.verify_and_release(count); });
});
```

The timing loop should invoke a templated callable so registration dispatch does not add an indirect call on every operation. Retain the production Release optimization policy. Supply appropriate compiler barriers and observable inputs and outputs, and inspect optimized code for representative small-function benchmarks. A result sink alone does not establish that a call executes on every iteration: [Google Benchmark's optimization guidance](https://google.github.io/benchmark/user_guide.html#preventing-optimization) documents constant-folding and result-reuse limitations of `DoNotOptimize`.

Use validated, ordered counter reads around each batch and record timing-source metadata. Keep raw counter ticks; report time units such as nanoseconds only when the frequency and conversion are validated. A timer tick must not be labeled as a CPU cycle without evidence that it is one. Short-function measurements include residual loop, result-storage, and measurement costs; report or bound that overhead and retain unadjusted samples rather than presenting unchecked subtraction as exact function time.

Bounded batch execution changes allocator occupancy, cache state, and other preconditions across calls. Each scenario must describe that workload. Functions requiring different per-call state need an appropriate fixture or a separately labeled measurement strategy. The elapsed interval also includes any scheduling, interruption, or waiting that occurs under the declared runtime conditions; it is not automatically exclusive on-CPU execution time.

The confirmed requirement is explicitly registered function microbenchmarks. Automatic recording of function calls during normal kernel workloads is a separate profiling model outside this first delivery.

## Result Validity and CI

Correctness and measurement validity are mandatory. Functional errors, unexpected kernel panics, timeouts, and invalid timing cause the relevant run to fail and return a nonzero exit status. Framework self-validation treats intentionally injected failures according to its declared expected outcome and verifies that the underlying failure is correctly reported.

The revised performance gate was confirmed on 2026-09-14. Repeated same-revision measurements must establish scenario-specific noise ranges and regression thresholds under comparable conditions before acceptance decisions use them. A degradation beyond its calibrated threshold, confirmed by repeated measurement against a comparable baseline, must fail performance acceptance.

Missing baselines, incompatible environments or excessive noise leave performance acceptance incomplete. These outcomes must be distinguishable from a confirmed regression, and neither may satisfy the performance gate. A pass requires valid comparable evidence under the calibrated decision rule. Retain all original samples and intermediate outcomes.

The two-report runner comparison remains informational. A separate repeated-measurement gate implements the conservative `empirical-range-v1` decision rule described in the [usage guide](kernel-validation-usage.md#repeated-measurement-gate), with distinct nonzero exits for regression and incomplete evidence. Host tests validate its decisions and CLI exits. On 2026-09-15 the user accepted a 5% trial noise ceiling, applied with `--max-noise 0.05`; this does not change the empirical regression bounds. The historical five-scenario replay and current thirteen-scenario measurements are separate evidence sets. Expanded execution coverage is implemented; performance acceptance still requires valid comparable repetitions for every current scenario.

Valid measurements without a comparable baseline remain measurements, not evidence that a performance comparison passed. Failed or incomplete operations must not be presented as successful performance samples.

## Result Artifacts and Baseline Interface

The result-delivery contract below is implemented. Actual successful, deliberately failing, and timed-out guests produce the documented artifacts; the acceptance record identifies the saved runs used to verify offline comparison.

### Run Reports

The host runner produces the following artifacts in a run-specific output directory:

| Artifact | Contents and Purpose |
| --- | --- |
| `results.json` | Versioned complete report: requested and executed workloads, observed outcomes, failure reasons, cases not run, raw benchmark batches, derived metrics, clock validation, and environment provenance. |
| `junit.xml` | CI view of individual functional cases and benchmark-scenario validity, with failures and infrastructure errors visible. Cases not run are represented as skipped with their cause, never as passed. Performance changes in this view are informational; the separate repeated-measurement gate supplies the blocking exit status. |
| Per-guest serial and QEMU diagnostic logs | Original guest output and emulator diagnostics, retained on both success and failure and linked from the report. |

JSON is the canonical structured report; terminal summaries and JUnit are derived views. Raw diagnostic ticks from an invalid run remain distinguishable from accepted benchmark samples. Framework self-validation records both the expected and observed outcome so an intentional panic is not confused with a production test passing normally.

Reuse the existing Python tooling and QEMU argument construction. Parse structured events and report data with standard JSON support and the existing Pydantic dependency; generate XML through a structured XML API. No report database, web service, or dashboard is required for the first version. [CTest supports JUnit output](https://cmake.org/cmake/help/latest/manual/ctest.1.html#cmdoption-ctest-output-junit), but when one CTest invocation represents a kernel suite, the runner's case-level report supplies the individual guest-case detail. CTest still receives the runner's normalized nonzero failure status.

### Completion and Failure Evidence

The guest emits bounded, versioned machine-readable records distinguishable from ordinary serial logs. The host checks workload identity, case start and completion, expected case accounting, benchmark sample validity, and the terminal run record against architecture-specific QEMU termination. A raw QEMU exit code, a human-readable success line, or some completed cases alone cannot establish that the requested run passed.

An empty workload selection, duplicate or unknown case identity, malformed protocol record, unexpected termination, or missing completion record cannot become a successful run. The report distinguishes failure before validation startup from a failure in an identified case. A suite interrupted by a failing case retains that failure and marks its remaining selected cases not run; later suites still run in fresh guests. Intentionally fatal self-validation cases use their declared expected termination evidence instead of requiring a normal completion record.

Preserve and incrementally record available evidence so timeout handling can finish a partial report before returning failure. If an external force-kill prevents report finalization, missing finalization is not evidence of success. The runner owns its child QEMU processes and must terminate and reap them on timeout or cancellation.

### Explicit Baseline Comparison

Allow the caller to select workloads, choose an output directory, and optionally name a previous JSON report as the baseline. A separate comparison operation can compare two saved reports without booting QEMU again. Baselines are explicit inputs and are never silently selected or overwritten by a new run.

Record the kernel revision and dirty state, actual image hashes, compiler and effective build options, workload identity and definition version, parameters and batch policy, QEMU version and effective arguments, accelerator, machine and CPU configuration, memory, guest CPU count, host identity, clock source, and frequency-validation evidence. Revision and kernel-image hashes are provenance expected to differ across revisions, not fields that must match to permit comparison. Different workload definitions, fixture contents, build modes, or execution environments are not automatically comparable. Calibrated frequency estimates retain their uncertainty and are not compared as if they were exact constants.

For matching valid workloads, report both medians, an absolute and relative change where defined, the metric's improvement direction, and links to the raw samples. Missing or incompatible baseline entries are explicitly unavailable or not comparable, not a performance pass or regression. The revised gate additionally requires calibrated acceptance decisions; reporting a numerical comparison alone cannot satisfy it.

## Current Implementation Sequence

The following sequence organizes the scope confirmed on 2026-09-14. Completion requires executed evidence for the expanded requirements; the initial acceptance record does not satisfy them.

1. Repair validation infrastructure gaps: resource-profile checks, catalogue accounting and capacity, artifact-to-source provenance, and completion/reporting under failure. Retain the real-kernel execution and failure-propagation checks.
2. Complete functional and deterministic-concurrency coverage for the six core paths, using the existing invariants and acceptance cases. Reproduce each blocking defect, repair its production path and verify the affected architecture/build configurations.
3. Implement the routine and long-run stability gates with declared lifecycle cycles, resource-accounting checkpoints, progress detection and suitable host deadlines. Retain evidence when the guest fails or stops making progress.
4. Add the required representative benchmarks, calibrate scenario noise with repeated same-revision measurements, and implement the confirmed performance gate. Record the calibrated numerical thresholds and decision rule with the evidence that supports them; missing or inconclusive evidence cannot complete performance acceptance.
5. Run the full ARM64/x64/RISC-V 64 Debug/Release functional and stability matrix, production boot checks, and the Release performance acceptance scenarios under comparable environments. Collect performance evidence separately from competing workloads, preserve all reports and diagnostics, and publish the tested artifact identities and final outcomes.

### Current Delivery Status

- [x] Acceptance scope, stability thresholds, performance-gate policy, benchmark coverage and necessary kernel-repair scope confirmed and documented.
- [ ] Validation infrastructure gaps repaired and verified.
- [ ] Six core paths pass the required functional and deterministic-concurrency checks, including necessary kernel repairs.
- [x] Routine and long-run stability gates implemented and passed across the required matrix for the declared six-path lifecycle workload (not general SMP reliability).
- [ ] Representative performance coverage, noise calibration and regression gate implemented and validated.
- [ ] Complete matrix evidence and updated runnable usage delivered.

Implementation evidence so far (2026-09-14, expanded acceptance remains incomplete):

- The 256 MiB resource profile now checks real allocator accounting instead of requiring more than 256 MiB of managed pages. The default 2 GiB probe still touches memory beyond 256 MiB. ARM64 `resources`, `mm`, `pfa`, and `heap` passed at 256 MiB.
- Build-time provenance binds source revision/dirty-content identity to the validation image and initramfs hashes; changing a built artifact rejects its stale record. Missing records remain explicitly unrecorded. The host suite passed 125 tests; the original functional/framework CTest matrix passed on all three Debug architectures before adding the new signal suite.
- `users.signals` now runs real fork/exec children with individually reported cases. Original failing reports are retained: ARM64 `build/arm64-debug/validation/1789375958143808333/results.json`, RV64 `build/riscv64-debug/validation/1789375959311299318/results.json`, and x64 `build/x64-debug/validation/1789375960480971334/results.json`. They exposed missing/wrong trap-frame arguments and syscall-result ordering; the next run exposed absent SIGCHLD generation.
- After production-path repairs, the first eight signal cases (basic, nested, SIGCHLD, masks, altstack, ignore, invalid arguments, and forged-frame checks) passed three-architecture Debug runs `1789376631580004999` (ARM64), `1789376632812124682` (RV64), and `1789376633868621498` (x64). Inheritance, state lifecycle, IRQ delivery, concurrency and fault-safe copying are still being implemented/verified; this is not full signal acceptance.
- The inheritance probe initially failed in ARM64 run `1789376798628787150`. Signal dispositions are now owned by `Process`, with fork inheritance and exec reset, removing the absolute-PID array. The subsequent 300-cycle probe failed on ARM64 cycle 254; `mm.transactions/asid_leases` independently proved live-tag aliasing in run `1789377085850711126`. Tags now remain leased until address-space destruction, with broadcast invalidation before ARM64 reuse. The 255-live-address-space ceiling returns a resource-exhaustion error instead of aliasing. Both suites then passed on ARM64 (`1789377197375887077`), RV64 (`1789377198587308951`) and x64 (`1789377199640404746`). This short signal lifecycle check does not replace the required full-cycle stability workload.
- ARM64 full regression `1789377335364297550` exposed a rare exception-return panic. A real pending timer IRQ reproduced ELR/SPSR corruption deterministically in `build/arm64-debug/dispatch-irq/run-5qykmdhj`: the IRQ entered after the user return state was published, replacing it with a kernel restore PC. The common IRQ, synchronous-fault and lower-EL restore paths now mask IRQ before publishing return state. The probe passed three runs in `run-ljiq8adj`; the subsequent Debug functional/framework CTest passed. The probe uses ELF symbols without module DWARF because the installed GDB 10.2 misresolves/hangs on some module symbols. Its RV64 recheck remains incomplete with this debugger: QEMU's target description was rejected and `sstatus` was unavailable, not a kernel pass.
- On 2026-09-15, `users.lifecycle/fork_exec_recovery` added one warmup plus 1,000 fork/exec/exit/wait cycles, including an inherited descriptor left open across exec/exit. Resource checkpoints run every 100 cycles. Original ARM64 failure `1789438853301732603` measured +208,000 heap bytes and +300 standard-file references after 100 cycles. `Process` now owns and closes its descriptor table before publishing Zombie, with destructor fallback. Checkpoints recovered exactly on ARM64 (`1789439156592514987`), RV64 (`1789439155427447863`) and x64 (`1789439157770337897`). RV64's initial single-page difference was independently identified as a demand-loaded constant page used by the test's modulo-100 loop; using an incrementing checkpoint counter removed that harness artifact without relaxing resource equality. Host checks reject missing, reordered or unequal recovery evidence; 130 host tests passed before the subsequent VFS additions. This covers repeated process lifecycles, not yet the required six-path full-cycle workload or 30-minute acceptance.
- VFS probes separately reproduced full-width FD truncation (`1789439229951616914`), non-reusable pipe inodes after 247 successful cycles (`1789439297717075804`), leaked endpoints during failed FD installation (`1789439358497057492`), and missing rollback after a read-only userspace result pointer (`1789439475315460228`), all on ARM64 Debug. Repairs preserve full-width FD bounds, recycle unreferenced inode slots, share the final File release path for rollback, and close both installed descriptors when pipe result copying fails. The suite checks 1,000 normal pipe lifecycles and 1,000 rollback attempts for each FD shortage; the userspace case checks 1,000 failed output copies followed by reusable descriptors 3/4. This is not blocking-pipe/SMP acceptance; pool metadata, reference and I/O concurrency still need the planned ownership/locking work.
- The 13-suite/52-case functional catalog at that point and the framework self-checks passed all six configurations. Functional report IDs: ARM64 Debug `1789439524196012908`, RV64 Debug `1789439525321142754`, x64 Debug `1789439526533404789`, ARM64 Release `1789439577291290267`, RV64 Release `1789439578429714909`, x64 Release `1789439579622192005`. Host tests passed 130 cases after updating the JUnit test to account for the expanded VFS catalog. Later timer additions are not covered by these matrix reports.
- `timers/capacity` reproduced the heap-full-but-active timer defect on ARM64 Debug (`1789439877246820309`). Timer start now returns a checked result, publishes active state only after reserving a queue slot, rejects duplicate start/null callbacks/relative overflow, and propagates start failure through nanosleep and scheduler startup. The capacity case passed in `1789439983325281348`; cancellation/in-flight callback lifetime, remaining timing cases and the new matrix are still pending.
- After the merge, x86 timer dispatch still failed (`1789441386388539393`). GDB's LAPIC monitor showed a masked timer with vector zero: device initialization had reset the boot-owned controller after timer setup. Device setup now reuses that controller; dispatch passed (`1789441622610935894`). Real `users.timers` syscalls exposed x86/RV early return and unsupported absolute-sleep flags on ARM64. All architectures now switch through the sleep path, with checked deadlines and synchronous cancellation before releasing stack timers. A three-CPU held-callback test rejects early cancellation return and restart during a callback; deliberately removing the wait failed case 5 (`1789442428392156884`). This does not close cross-CPU scheduler ownership or signal interruption semantics.
- The expanded six-path cycle exposed an actual empty-page-table retention defect, twice losing a physical page by cycle 600 on x86 Debug (`1789443037620355908`, `1789443071063097919`). `mm.transactions/unmap_reclaims_tables` independently failed (`1789443244103070473`). Unmapping now reclaims empty private tables after walk-cache invalidation, preserving adjacent live mappings and shared kernel entries. Both the focused case and 1,000 full cycles passed (`1789443302134481959`, then the updated protocol in `1789443508216140906`). Cross-CPU x86/RV TLB shootdown remains outside this fix.
- The runner now freezes image/fixture/DTB inputs per report and rejects copies that race a rebuild. A prior matrix overlapped an x86 rebuild and is not used as single-artifact acceptance evidence. The new `--stability` mode requires one guest, advancing resource checkpoints, at least 10,000 full cycles, and at least 30 minutes measured in both guest and host time. Host negative checks reject insufficient cycles, insufficient duration, stalls and fabricated instant guest duration; 139 host tests passed. The new six-configuration matrix and long-run outcomes are not yet complete.

### Continued Acceptance on 2026-09-15

- A real x86 Release hang in `users.uaccess/sigframe_fault` was captured in GDB:
  CFS was looping on an RB node whose parent was itself. `sched_yield` enqueued
  the caller but only ARM64 actually switched away. The new `users/yield_reuse`
  case failed on the old code (`1789444457087412619`) and passed after enabling
  the IRQ-masked context switch on all three architectures (`1789444527202278304`).
- RV64 Release separately hung during the two-child cancellation case
  (`1789444016249945887`); GDB again showed a self-linked RB node. Both exit
  notification paths bypassed `task_wakeup` and unconditionally enqueued the
  parent. They now use the shared atomic blocked-to-ready transition. The
  original `users.timers` workload passed 20 consecutive runs, from
  `1789445001239148320` through `1789445164373349964`. This does not establish
  context-save/remote-migration ownership or lost-wakeup closure.
- `users/wait_status_rollback` failed with mask `0x16` on x86 Release
  (`1789445140325658197`): unsupported options and truncated PIDs were accepted,
  and EFAULT consumed the child's exit status. The shared wait path now rejects
  those inputs and commits reaping only after successful status copyout. The
  16-cycle retry/reap regression passed (`1789445213557387508`). Process-group
  wait selection remains unsupported and is rejected explicitly.
- The ensuing six-configuration functional/framework matrix passed, including
  1,000 complete six-path cycles per configuration. Functional report IDs:
  ARM64 Debug `1789445245736964232`, ARM64 Release `1789445248048902491`,
  RV64 Debug `1789445248445632056`, RV64 Release `1789445250958646855`,
  x86 Debug `1789445250781366677`, x86 Release `1789445253012227661`.
  All six production shell/ELF/second-command probes passed too; production
  startup is now a separate `moss-production-boot` CTest.
- GDB 17.2 built in a temporary directory resolved the old RV64 target-description
  problem. Three real RV64 IRQ-injection probes passed in
  `build/riscv64-debug/dispatch-irq/run-oe00sf4l`; three ARM64 probes passed in
  `build/arm64-debug/dispatch-irq/run-gcf70cf8`. Earlier tool failures remain recorded.
- The earlier frozen-image long runs passed ARM64 Debug (345,600 cycles), ARM64
  Release (744,600) and RV64 Debug (310,700), each exceeding 1,800 host seconds.
  x86 Debug `1789443811410629567` was correctly rejected: 1,800.31 guest seconds
  corresponded to only 1,797.58 host seconds. The guest now continues full cycles
  until a host completion permit arrives, while retaining both duration checks.
  A short real x86/GDB probe verified that an early permit is received but cannot
  complete the test (`build/race-probe.nZCavR/stability-input-1789445950434368075`).
  All six new long runs using this handshake passed, as detailed below; cancelled
  intermediate runs retain their reports and do not count toward acceptance.
- The repeated-measurement performance gate now rejects partial scenario reuse,
  missing/invalid/noisy evidence and inconsistent repeat outcomes; CLI tests check
  exits 0/1/2 and preservation of prior outputs. Real repeated measurements are
  recorded below; neither the host checks nor the initial five scenarios
  establish completed expanded performance acceptance.
- The handshake-image matrix passed all 18 CTests (functional, framework and
  production boot for six configurations), with 17 functional suites / 75 cases
  per configuration. Functional reports: ARM64 Debug `1789446105751489922`,
  ARM64 Release `1789446107012276918`, RV64 Debug `1789446108117579335`,
  RV64 Release `1789446109298329466`, x86 Debug `1789446110512487532`,
  x86 Release `1789446111663526749`. New ARM64/RV64 IRQ probes each passed three
  runs in `run-y13dzirq` / `run-ywfbd8up`. Host regression tests passed 171 cases
  (`build/continued-host-tests.log`); Ruff, CMake formatting and `git diff --check`
  passed too.
- Full target-specific clang-tidy checks were executed for all three Debug
  targets and failed; logs are `build/<preset>/continued-tidy-full.log`.
  Diagnostics include assembly-shared offset macros, braces/naming and other
  readability/widening warnings. This is not a clean static-analysis gate.
  The new wait PID-boundary expression was simplified without changing the
  generated runtime content. All six image payloads and fixtures were compared
  with the running stability snapshots and remained identical (x86 ELF debug
  metadata excluded from the runtime-content comparison).

#### Completed Long-Run Evidence

Historical record: the long-run report directories listed here and the old
`build/performance-calibration.CCC9D4` directory were not present in the workspace
when the expanded performance work resumed. Their recorded outcomes below are
not freshly verified artifacts or inputs to the current performance gate. This
turn did not remove them; the new evidence is identified separately below.

Each report below is finalized, passed and has no unrun selection. Each used one
frozen guest with four online/work-performing vCPUs and 2 GiB RAM, one warmup
cycle, then the complete memory/COW, scheduling/wait, timer, fork/exec/exit/reap,
userspace-signal and VFS/pipe cycle. Every 100-cycle checkpoint matched its
baseline exactly for heap bytes, free physical pages, live processes/threads,
user/stack pages, descriptors and File references. No reset between cycles or
resource tolerance was used. Host completion permits were sent only after
1,800 host seconds and at least 10,000 observed cycles.

Reports are `build/<preset>/validation/<ID>/results.json`; each retains source
and artifact identities, frozen inputs and raw serial diagnostics. Durations
below are rounded; both unrounded guest and host values exceed 1,800 seconds.

| Preset | Report ID | Complete cycles | Guest seconds | Host case seconds |
| --- | --- | ---: | ---: | ---: |
| arm64-debug | `1789445808451057370` | 340,800 | 1,800.459 | 1,800.476 |
| arm64-release | `1789445810140872896` | 751,600 | 1,800.087 | 1,800.107 |
| riscv64-debug | `1789445810802752678` | 319,100 | 1,800.572 | 1,800.653 |
| riscv64-release | `1789445812608857014` | 739,100 | 1,800.042 | 1,800.060 |
| x64-debug | `1789445812966170282` | 404,500 | 1,800.948 | 1,800.277 |
| x64-release | `1789445815051749352` | 848,600 | 1,800.021 | 1,802.428 |

This closes the declared routine/long-duration lifecycle gate. It does not close
the missing deterministic scheduler ownership/migration tests, lost-wakeup
protocols, signal interruption/stop/continue, blocking-pipe and shared-file
concurrency, or the other functional obligations above.

#### Real Repeated Performance Measurements

After all stability guests were reaped, Release architectures and scenarios
were measured sequentially, without other owned validation/build jobs. The
unrelated pre-existing Android emulator was not stopped; these are measurements
on the recorded shared host, not an isolated performance machine.
Evidence is `build/performance-calibration.CCC9D4/<preset>/run-0` through `run-7`,
each containing all five current scenarios, raw batches and frozen inputs.
Runs 1–7 reuse run 0's per-scenario operation counts. `calibration.json` uses
runs 0–4 as baseline and 5–7 as same-image confirmation, retains all eight report
hashes, and returns `incomplete` / exit 2 because no noise ceiling was supplied.

After the user confirmed the 5% trial ceiling, the same saved reports were replayed
with `--max-noise 0.05`. The separate `trial-noise-005.json` in each target's
directory records the configured result. Original calibration files, all 24 input
report hashes, samples and empirical bounds were checked unchanged. No new guest
was measured and no outlier was removed.

| Release target | Baseline range across scenarios | Confirmation range across scenarios | 5% trial gate |
| --- | ---: | ---: | --- |
| ARM64 | 1.63%–79.48% | 0.18%–4.17% | Incomplete / exit 2; allocation noise exceeds 5%, other four scenarios pass |
| RV64 | 0.61%–3.52% | 0.91%–2.50% | Incomplete / exit 2; release repeats disagree across the regression bound, other four pass |
| x64 | 0.80%–3.65% | 0.63%–2.20% | Passed / exit 0 for the five current scenarios |

RV64's release result demonstrates why a noise ceiling is not a slowdown
allowance: its repeat variability is below 5%, but the confirmation medians
straddle the unchanged empirical upper bound. It remains inconclusive rather
than passing or declaring a confirmed regression. The configured replay thus
has 13 passed scenarios and two incomplete scenarios, not full performance
acceptance. The performance-gate regression suite also passed all 27 host tests.

The ARM64 allocation outlier is the first run: 569.47 ns/op versus
317.28–331.16 ns/op in the remaining baseline runs. Its median empty-loop
overhead also rose (62 counter ticks versus roughly 35), so the data do not
establish an allocator regression. Eight subsequent diagnostic guests alternated
automatic pilot and fixed 256-operation batches (`pilot-probe-0` through `-7`
under the same evidence root). Both modes selected/executed 256 operations and
measured 319.81–345.22 ns/op with 35-tick median overhead; this did not reproduce
a pilot-specific slowdown. The exact cause of the original timing disturbance
remains unproven. These diagnostic repeats do not replace the original baseline,
erase the outlier or authorize wider thresholds.

At the time of this historical collection, COW/fault, switch/wakeup,
timer-lateness, lifecycle, signal-delivery and pipe-throughput scenarios were not
implemented. Initial-scenario calibration cannot substitute for their coverage.

Architecture return-state rules are checked against the [RISC-V 64 privileged ISA](riscv64_PRIV_DOC) and [Intel instruction reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2a-manual.pdf); userspace frames must not select supervisor mode or make privileged FP restore fault.

#### Expanded Performance Acceptance on 2026-09-15

Work resumed on `c81ce06` with the existing dirty worktree preserved, including
the committed `x64` / `riscv64` preset rename. The expanded thirteen-scenario
catalog uses real page faults/COW, saved-context transfers, timer callbacks,
process lifecycle, user signal delivery and pipe transfers. The
[measurement definitions](kernel-validation-usage.md#single-function-measurements)
state the boundaries and limits; none claims native-hardware performance.

All three Release builds succeeded. The timer deadline conversion uses an exact
bounded inverse of the existing clock conversion, without a new compiler-runtime
dependency; `timers/contracts` checks initialization and ordered-counter bounds.
Before repeated timing, all nine Release CTests (functional, framework and
production boot on each ISA) passed. Each functional report contains 17 suites,
75 cases and the 1,000-cycle six-path recovery workload.

| Release preset | Thirteen-scenario measurement smoke report | Functional CTest report |
| --- | --- | --- |
| arm64-release | `1789451501337275505` | `1789451594092304587` |
| riscv64-release | `1789451502514331785` | `1789451594128277643` |
| x64-release | `1789451438996399788` | `1789451594139905368` |

Reports are under `build/<preset>/validation/<ID>/results.json`. Build and CTest
logs are `performance-build.log` and `performance-ctest.log` under each preset.
These smoke/functional checks preceded calibration and are not substituted for
independent repeated performance reports. Host tooling passed 184 tests
(`build/performance-host-tests.log`), including negative catalog/latency semantics,
QMP bind-before-resume and incomplete/malformed saved-affinity evidence checks.
Ruff passed. Optimized userspace disassembly is retained in
`build/<preset>/performance-userspace-disassembly.log` for all three Release
targets: the actual faulting stores, yield/fork/wait/pipe syscalls and signal
send-to-handler counter reads remain inside their declared measurement intervals.

The first full-catalog campaign is `build/performance-acceptance.EKKlGw`.
It uses Release/TCG, four vCPUs, 2 GiB, host CPU eligibility 8–11, 20 warmups
and 100 recorded batches. All eight ARM64 reports completed thirteen scenarios,
but its `gate.json` exits 2: baseline variation ranges from 36.47% to 80.25%,
exceeding 5% in every scenario. The collector was stopped during RV64 run-1 to
investigate; partial reports are retained and x64 was not collected there.
This interrupted campaign is not a completed three-ISA matrix.

A separate 16-guest ARM64 allocation diagnostic keeps the same image and fixed
256-operation batches. The sole alternating change is individual vCPU affinity
via the new optional `--host-cpus 8,9,10,11`. Pinned medians span 319.97–334.31 ns
(4.48% range/min); unpinned medians span 320.81–399.47 ns (24.52%). All raw probe
reports and the sequence remain under the first campaign directory. This short
comparison motivates a full pinned campaign; it does not prove that pinning
eliminates shared-host timing noise.

The fresh full-catalog pinned campaign is `build/performance-pinned.uoPXu1`.
Its saved `collect.py` runs architectures and scenarios serially with the same
warmup/sample/resource settings, additionally binding and verifying each vCPU
to one distinct host CPU before resuming QEMU. Each architecture uses runs 0–4
as baseline and 5–7 as same-image candidates, reusing run 0's operation counts;
the gate explicitly uses `--max-noise 0.05`. No retry, sample filtering, outlier
removal or threshold widening is permitted. No owned builds or other validation
guests run during timing. The pre-existing Android emulator and other user
processes remain untouched. All 24 reports finalized successfully: 312 valid
scenario guests and 37,440 raw batches (20 warmups and 100 recorded samples per
guest). Report hashes, frozen image/fixture identity, fixed operation counts and
raw protocol/affinity evidence were rechecked. This is completed measurement
collection, not completed performance acceptance:

| Release target | Baseline noise across thirteen scenarios | Candidate noise | Gate |
| --- | ---: | ---: | --- |
| ARM64 | 48.64%–132.20% | 2.83%–113.60% | Incomplete / exit 2 |
| RV64 | 14.40%–126.94% | 0.04%–133.08% | Incomplete / exit 2 |
| x64 | 3.81%–74.01% | 15.41%–122.26% | Incomplete / exit 2 |

Every architecture/scenario pair fails at least one repeat group's 5% ceiling.
No scenario was accepted and none establishes a confirmed kernel regression.
Each target's `gate.json` retains all medians, calibrated bounds and input hashes.

Read-only host checks observed competing work: a `perception-3d` index worker
used roughly 398%–489% CPU, and a separate `/home/wu/code/fl/p` build was active
(`host-contention.log` under the pinned campaign). These are outside this task
and were not stopped. Low-rate CPU-frequency and thread scheduling snapshots
are retained in `host-probe.jsonl` / `host-thread-probe.jsonl`; the first probe
did not identify QEMU threads because this build uses the same thread name for
all of them, so it supplies frequency evidence only. The sampled frequencies
were near 4.2 GHz, and the inspected CPU cgroup had no quota or throttling.
These observations establish a shared, non-quiescent host, not a proof that any
one process or mechanism caused every timing outlier.

With explicit user approval, `build/performance-governor.6rngu5/probe.py` varied
only CPU 8–11 governors in the fixed sequence `(ondemand, performance,
performance, ondemand) * 4`, keeping the ARM64 image, per-vCPU affinity and
256-operation allocation batches unchanged. Each policy's affected/related CPU
list contained only its requested CPU. All sixteen real guests passed; original
reports include the actual governors and the log retains the complete sequence.
Ondemand medians range 323.59–583.34 ns (80.27% noise); performance medians range
321.16–500.63 ns (55.88%). Performance policy therefore did not produce a usable
5% baseline in this diagnostic. All four original ondemand settings were restored
and independently read back after the script exited. No other CPU policy or user
process was changed. New reports record pinned-CPU governors and refuse offline
comparison across different recorded policies.

The final host suite passed 185 tests
(`build/performance-governor.6rngu5/final-host-tests.log`); Ruff,
`git diff --check` and clang-format checks on the three edited benchmark/timer
source files passed. Current validation images and fixtures still match the
tested hashes when resolved through each artifact manifest (x64 uses the ELF,
not the raw `.bin`). All owned collection/probe processes and guests were reaped;
the user's pre-existing Android emulator, stash and pull backup remain intact.

Next acceptance needs a quiet host window or an independent machine, followed
by fresh complete same-image calibration and candidate groups. A deliberately
slowed real kernel image has not yet established the regression gate's exit-1
path under valid calibrated conditions; synthetic host tests cover that decision
but do not replace real evidence. Neither missing evidence nor noise is resolved
by widening the 5% ceiling, discarding runs, or repeatedly measuring until green.

#### Interim Performance Handoff on 2026-09-16

At the user's request, the next campaign started immediately without claiming
that the overnight watcher's quiet-window heuristic had been satisfied.
`build/performance-manual.gUwvZh/` retains all 24 completed reports, 312 valid
scenario guests and 37,440 raw batches. Offline protocol, hash, fixed-count and
gate replay checks passed. The unchanged `--max-noise 0.05` gate accepted 0/13
ARM64, 9/13 RV64 and 1/13 x64 scenarios; the remaining 29 pairs were incomplete
because of excessive noise. All three architecture gates returned exit 2,
with no confirmed regression. Every one of the 58 host sampling intervals had
observed interference; whole-host CPU utilization reached 62.13% and measurement
CPU SMT siblings reached 64.88%. No other user's process was stopped.

These are same-image repeatability results, not an optimization comparison.
ARM64/RV64 used source `162c79...`, while x64 used `7bf02e...`; they do not form
a common-source acceptance matrix. On 2026-09-16 the user asked to retain this
interim outcome and proceed to the next item. Performance acceptance stays
**incomplete**, including the real slowed-image negative control; this handoff
does not waive the noise ceiling or mark the gate passed. The next selected
item is renewed routine full-application acceptance after the shared-FD repair.

## Native CMake Source Migration (2026-09-16)

[ADR-0007](adr/0007-vendor-third-party-sources-with-native-cmake.md) is implemented:
the static mlibc/BusyBox runtime now compiles checked-in source through native
CMake targets. The [source inventory](third-party-sources.md) records exact
upstream inputs, archive SHA-256 values, preserved source scope and direct
adaptations. This supersedes the historical download/Meson/Make build described
in earlier checkpoints; it does not change the kernel acceptance boundary.

Fresh build directories were created under
`/dev/shm/moss-native-cmake.3Y6k9s/<preset>` rather than reusing the old `_deps`
trees. The tested working tree was based on `07a943d` plus this migration;
individual reports retain their actual source/artifact fingerprints and frozen
inputs, not a fabricated clean-commit identity. Durable copies of the reports,
serial logs, frozen inputs and command logs are under the ignored local directory
`build/native-cmake-source-migration-20260916/`. Reports retain their original
temporary absolute paths; the temporary build trees are not permanent storage.

| Configuration | Clean full build | mlibc E2E | BusyBox E2E | Application recovery | Framework | Production boot | No-op rebuild |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ARM64 Debug | pass | 2/2 | 9/9 | 1000 cycles, 101 checkpoints | pass | pass | no work |
| ARM64 Release | pass | 2/2 | 9/9 | 1000 cycles, 101 checkpoints | pass | pass | no work |
| x64 Debug | pass | 2/2 | 9/9 | 1000 cycles, 101 checkpoints | pass | pass | no work |
| x64 Release | pass | 2/2 | 9/9 | 1000 cycles, 101 checkpoints | pass | pass | no work |
| RV64 Debug | pass | 2/2 | 9/9 | 1000 cycles, 101 checkpoints | pass | pass | no work |
| RV64 Release | pass | 2/2 | 9/9 | 1000 cycles, 101 checkpoints | pass | pass | no work |

Each application report records both `cycles=1000` and
`application_cycles=1000`; resource recovery assertions passed. Framework runs
check normal success and the intentionally expected assertion, panic and timeout
outcomes. These are not performance comparisons or long-run stability results.

Commands used for each preset (substitute its fresh build directory for `DIR`):

```sh
uv run cmake --preset PRESET -B DIR
uv run cmake --build DIR --parallel 8
uv run scripts/kernel_validation.py run --manifest DIR/moss-artifacts.json \
  --workload users.libc --output LIBC_REPORT
uv run scripts/kernel_validation.py run --manifest DIR/moss-artifacts.json \
  --workload users.busybox --output BUSYBOX_REPORT
uv run ctest --test-dir DIR --no-tests=error \
  -R '^moss-(applications|production-boot|framework)$' --output-on-failure
uv run cmake --build DIR --parallel 8
```

Additional build acceptance:

- All three architectures also completed fresh Release full builds with the
  network disabled using `unshare -Urn` and `UV_OFFLINE=1` (x64 used an additional
  `x64-offline` directory). CMake, `llvm-ar` and `llvm-ranlib` were pinned to
  installed executable paths. Initial ARM64/RV64 attempts stalled in the host's
  Swift tool-manager `llvm-ranlib` wrapper; their logs are retained separately.
  Selecting the actual installed LLVM binaries resolved this host-tool issue
  without changing the system environment or downloading source.
- `uv run pytest scripts/tests -q`: **239 passed**. The three new native-runtime
  build cases copy sources into paths containing spaces, forbid Make/Meson and
  download tools, build real ELF outputs, verify a no-op rebuild, and edit Moss
  sysdeps directly to verify recompilation/relinking. Source-tree hashes remain
  unchanged by configuration and compilation. A formatter test protects the
  vendored-tree exclusion.
- Archive comparison confirms complete BusyBox/mlibc/header dependency trees
  with only the documented additions/adaptations, and unchanged compiler-rt
  builtins/supporting files. No imported source is hidden by Git ignore rules.
- Ruff and checks of the changed Python/native CMake files pass. Whole-repository
  formatting still reports two unchanged pre-existing files:
  `src/boot/src/arch/riscv64/boot_impl.cpp` and `src/mm/src/page_fault.cpp`.
  They were not reformatted as part of this migration.
  The full staged whitespace check also reports existing upstream whitespace in
  imported files; those bytes are intentionally preserved. Checks of the
  Moss-authored integration and adaptation files pass.

The known `scheduler/migration_current_owner` failure was reproduced on x64
Debug and remains a failure: three scheduler cases pass, then that case reports
one failed assertion. Its original `results.json` and serial output are retained
in `scheduler-known-red/`; no test was disabled or reclassified. **The overall
kernel functional matrix, long-duration stability and performance acceptance
remain incomplete.** Network and persistent-storage work remain excluded.

## Initial Implementation and Acceptance Plan

The following sequence records the implemented plan. The evidence checklist is satisfied for the initial workloads and default QEMU profile, not for arbitrary kernel subsystems or native hardware.

### Implementation Order

1. Establish genuine multi-vCPU and enlarged-memory readiness on all three architectures, including missing x64 and RISC-V 64 secondary-CPU startup and required memory discovery and mapping changes. Preserve existing unrelated worktree changes throughout.
2. Extend `ut_kernel` registration, assertion accounting, case lifecycle, and catalog validation. Add focused tests for the host-side event parser, report schema, timeout handling, and architecture-specific exit normalization. Host test doubles validate host tooling only, not kernel functionality.
3. Integrate the dedicated validation image with production architecture startup and real subsystem initialization. Add explicit validation dispatch at the appropriate readiness points and real guest failure reporting. Keep validation behavior out of normal production execution.
4. Connect guest execution to the host runner and CTest. Prove ordinary pass and failure, actual kernel panic, timeout, startup failure, suite stop behavior, and subsequent-suite isolation before treating kernel-suite results as trustworthy.
5. Add the initial real page-allocation, VFS, and userspace/process functional suites and the necessary userspace probes and fixtures. Exercise actual user-to-kernel entry instructions where syscall coverage is claimed.
6. Implement the architecture-specific benchmark clocks, bounded batch execution, function fixtures, and the initial benchmark scenarios. Complete structured reports, JUnit output, explicit baseline selection, and offline comparison.
7. Run the architecture and build verification matrix, inspect representative optimized microbenchmarks, record actual evidence and remaining kernel defects, and document runnable commands and authoring examples using the implemented interfaces.

### Required Acceptance Evidence

| Area | Required Evidence |
| --- | --- |
| Builds and startup | Production and validation images build for ARM64, x64, and RISC-V 64. Normal production startup remains usable. Validation startup reaches the actual subsystem prerequisites rather than a substitute test-only implementation. |
| Multi-vCPU and enlarged memory | The requested CPUs actually come online and each executes real kernel work. The enlarged memory is discovered, mapped, and usable through owned allocations, with reservations accounted for. The single-function benchmark's declared CPU affinity is enforced. Single-core or old-memory fallback does not satisfy the revised profile. |
| Framework reliability | On all three architectures, real guest pass, assertion failure, panic, and timeout produce the declared host outcomes. Launch failure, empty selection, malformed or missing completion records, duplicate registration, and registry overflow cannot report success. Expected fatal self-checks must identify their intended case and failure, not accept any unrelated crash as success. |
| Isolation and cleanup | A failing case stops its suite, remaining cases are reported not run, and a later suite starts in a fresh guest. Resource-owning cases verify cleanup. Observed process launches follow the suite/scenario isolation policy, with no new QEMU process per ordinary case or measurement batch. Timeout and cancellation leave no owned QEMU process running. |
| Real functional coverage | Each initial kernel behavior has an executed production-path test on each supported architecture. A failed test remains a failure. Cases not reached after an earlier suite failure require a separately recorded targeted run before claiming they were exercised. Arithmetic demos and host mocks do not count as this evidence. |
| Function microbenchmarks | Demonstrate allocation-only, release-only, and combined allocation/release scenarios with their different timing boundaries, real VFS reads, and the userspace `getpid` round trip. Inspect representative Release output to verify that measured work is not folded away or hoisted out of the loop. |
| Timing validity | All three initial QEMU profiles demonstrate valid counter and frequency handling and valid samples for the initial benchmark workloads. Record raw ticks, counts, frequency provenance, calibration evidence where needed, and overhead limitations. Invalid clocks, operations, or cleanup do not produce accepted performance samples. |
| Report and comparison behavior | Actual successful and failing guest runs produce JSON, case-level JUnit, and original diagnostics. Matching saved reports can be compared without launching QEMU. Missing or incompatible baselines remain explicit. Initial acceptance covered informational-only slowdowns; the revised performance gate requires separate implementation and evidence. |
| Build-mode checks | Run the functional coverage matrix in Debug, run framework self-validation and representative real functional cases in Release, and run all benchmark acceptance in Release. Do not infer Release measurement behavior from a Debug-only run. |
| Delivery record | Publish the actual commands, architecture/build coverage, artifact locations, failure classifications, and outstanding defects. Passing host tests or a successful compile alone is not end-to-end acceptance. |

The initial delivery separated framework completion from other kernel repairs. The 2026-09-14 extension in ADR-0001 includes the repairs necessary for the six required core paths to pass acceptance. Defects remain visible until repaired; do not weaken assertions, skip required cases, or replace the tested implementation to obtain a pass. A targeted diagnostic rerun is additional evidence and does not erase the original failed run.

## Initial Implementation Status

- [x] Genuine SMP and enlarged-memory readiness on all three architectures.
- [x] Production-backed validation image and bounded `ut_kernel` registration.
- [x] Host protocol, reports, isolation, and failure self-validation.
- [x] Real allocator, VFS, and userspace/process cases.
- [x] Validated clocks, function benchmarks, and baseline comparison.
- [x] Debug/Release acceptance matrix and delivery evidence.

## Production BusyBox Shell (2026-09-16)

Update (2026-09-17): the production initramfs contains only `/busybox.elf`,
launched directly by the embedded init trampoline. The validation driver is
now named `/validation.elf` and includes all 15 signal regression cases;
each case re-executes that image to retain process isolation and exec-reset
coverage. The production probe now explicitly executes `/busybox.elf ash -c`.
The reports below describe the earlier launcher-based images before cleanup.

The normal initramfs now contains `/busybox.elf`, including builds with
`MOSS_BUILD_TESTS=OFF`. Its `/shell.elf` is a small launcher for interactive ash
with `PATH=/` and the existing `moss$` prompt. BusyBox standalone shell lookup
provides the selected applets and `sh` without symlinks or `/proc/self/exe`.
The validation initramfs retains its own `/shell.elf` validation driver.

On macOS x86_64 with Clang 23.1.0 and QEMU 11.1.1, all six Debug/Release builds
completed. The production probe requires the ash banner, external `hello.elf`,
bare applet lookup, a pipeline, redirection, copy/rename/removal, nested `sh`, and
a subsequent command after child reaping. The old mini-shell image failed this
probe at its first step. The resulting production images passed all 11 steps:

| Preset | Production report directory under `production-boot/` | Passing `users.busybox` report under `validation/` |
| --- | --- | --- |
| `arm64-debug` | `run-7qf75kln/guest` | `1789566484832820000` |
| `arm64-release` | `run-1kyjc4ln/guest` | `1789566506770750000` |
| `x64-debug` | `run-i73gfu78/guest` | `1789566519407995000` |
| `x64-release` | `run-b4k4ey0t/guest` | `1789566535831716000` |
| `riscv64-debug` | `run-5xivo5u2/guest` | `1789566548819490000` |
| `riscv64-release` | `run-l5cr4on5/guest` | `1789566931581919000` |

Paths are relative to `build/<preset>/`, with `results.json` in each listed
directory. Each passing BusyBox report covers all nine cases with the unchanged
5-second case budget. Artifact hashes match the final built images and fixtures.

Host checks passed: nine production-probe tests and eight userspace build tests.
The latter build the production archive with `MOSS_BUILD_TESTS=OFF` on all three
architectures, verify its shell and BusyBox bytes, reject legacy build/download
tools, and check unchanged rebuilds, dependency-triggered regeneration and source
tree hashes. The three focused compilation/dependency cases were also rerun after
adding an assertion that linking the small programs does not build BusyBox.

Earlier timeouts remain evidence: the initial concurrent-load Debug runs were
`arm64-debug/validation/1789565973057286000`,
`x64-debug/validation/1789566022532811000`, and
`riscv64-debug/validation/1789566024906977000` (all under `build/`). RISC-V Release
also timed out in `1789566570611905000` and `1789566682810399000`. Its diagnostic
run `1789566720978969000` explicitly used 15 seconds and passed; it does not
replace the subsequent default-budget result in the table.

A comparison using the pre-change BusyBox profile from `41903c3`, the same
RISC-V Release kernel, and a fixture differing only in `busybox.elf` passed all
nine cases at 5 seconds as well. The preserved comparison is under
`build/riscv64-release/validation/busybox-shell-baseline-20260916/`. These reruns
establish functional results, not the precise cause of the earlier timeouts or
a performance comparison. No repository timeout was relaxed.

This verifies the normal shell and selected BusyBox paths. It does not establish
full kernel, long-run stability, performance, job-control, or terminal-editing
acceptance. The GDB-controlled first-read variant was not run on this host.
