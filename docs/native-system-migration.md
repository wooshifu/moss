# Native System Migration Status

This is an implementation snapshot of the accepted architecture in
[ADRs 0008–0033](adr/0008-default-to-isolated-system-services.md). An accepted
ADR defines the destination; a passing boot or focused probe establishes only
the path that it exercised. Keep the [general-purpose BusyBox profile](adr/0006-validate-general-purpose-kernel-capabilities.md)
separate from the wider native-system design: that profile currently excludes
networking and persistent storage.

| Boundary | Current source and observed behavior | Work before the accepted boundary is reached |
| --- | --- | --- |
| Native authority and IPC | Capability handles, bounded calls, Memory Objects, domain observation and termination are exercised by [production validation](kernel-validation-usage.md). The [supervisor](../src/userspace/moss_init.c) starts and restarts isolated process, namespace and file services. | Close service-specific recovery and resource ownership under caller timeout, concurrent exit and service death. A restarted endpoint must not inherit authority from the old incarnation. |
| POSIX processes | The [Process Compatibility Service](../src/userspace/moss_process_service.c) owns managed IDs, parentage, groups, sessions, wait records and scoped signals. [Managed mlibc](../third_party/mlibc/sysdeps/moss/sysdeps.cpp) uses badged sessions for these operations. The [supervisor](../src/userspace/moss_init.c) places its shell tree in a native scope and drains that scope before replacing a failed process service. The service checks native scope membership for child registration and attachment. After replacement, the supervisor checks that both its old session and a session delegated to the old shell return `EPIPE`. The [kernel syscall table](../src/kernel/src/syscall_table.cpp) still supplies the legacy PID, credential, signal and wait paths. | Exercise pending calls and orphaned exit records under concurrent service loss. Move compatibility credentials and complete child notification, terminal job control and signal policy. Remove legacy POSIX policy only after its remaining callers migrate. |
| Files and paths | The [native namespace service](../src/userspace/moss_namespace_service.c) handles a flat root; the [file service](../src/userspace/moss_file_service.c) keeps volatile file contents and returns file capabilities. Opened file capabilities support size queries, atomic append and exclusive create within one service request. The [process service](../src/userspace/moss_process_service.c) holds an internal 0–255 descriptor view for file, pipe and console objects, with shared open descriptions, duplication, status queries and mutable close-on-exec. It can import a caller's transferable File Object Capability without repeating namespace lookup. Supervised [pipe](../src/userspace/moss_pipe_service.c) and [console](../src/userspace/moss_console_service.c) services serve bounded object I/O; the console input ring has no physical reader yet. The [mlibc descriptor path](../third_party/mlibc/sysdeps/moss/sysdeps.cpp) still calls the kernel, whose [open handler](../src/kernel/src/syscall_table.cpp) invokes kernel VFS. | Complete blocking, cancellation and lost-reply semantics, physical console input, current directories, traversal and remaining metadata before redirecting mlibc. Migrate selected BusyBox workflows before deleting the kernel VFS path. Define filesystem-service restart and file-object lifetime separately from namespace restart. |
| Executable images and memory | The kernel accepts explicit data pages, approved immutable code versions and an initial stack through a factory-authorized native domain spawn. A spawn made inside a recovery scope inherits that scope, including when factory authority was delegated. Code snapshot and approval are separate capabilities; a reviewer can query a version's kernel-held page count before approval, and native spawn rejects unapproved executable pages. Its [exec path](../src/kernel/src/syscall_table.cpp) still parses and maps ELF. Anonymous executable `mmap` is denied and `mprotect` returns `ENOSYS`. This implements only the initial authority boundary in [ADR-0026](adr/0026-require-explicit-authority-for-executable-memory.md). | Introduce the userspace loader and pager contracts, then enforce approval-instance rights across aliases, repaging, revocation and service failure before moving ordinary loading out of the kernel. Verified bootstrap authority is a separate [ADR-0027](adr/0027-establish-initial-authority-through-verified-boot.md) gate. |
| Devices and higher services | Production still links [console and platform drivers](../src/drivers/) in the kernel. The kernel socket handlers return `ENOSYS`; this does not provide the Network Stack Service in [ADR-0020](adr/0020-run-network-stacks-as-userspace-services.md). | Isolate a driver only after interrupt, MMIO, DMA and reset authority are scoped to its recovery domain, with hardware DMA confinement or an explicit trusted exception. Network, graphics and power policy services follow their own device and recovery prerequisites. |

## Implementation order

1. Close process-service recovery first. Fault injection showed that an old
   managed grandchild survived process-service restart because the old
   registry disappeared and the supervisor only retained its direct shell
   handle. Native fork now inherits the supervisor's shell scope, and the
   supervisor closes, terminates and drains that scope before starting a new
   compatibility namespace. The production boot probe waits for an adopted,
   live grandchild before killing the process service and rejects survival after restart.
   The process service now requires that child registration and attachment
   target domains in the same scope. After a restart, the supervisor confirms
   both its previous badged session and the session delegated to its old shell
   cannot call the replacement endpoint. Exercise orphaned exit records under
   concurrent service loss. Kernel IPC validation checks that receiver closure wakes a claimed
   call even while its reply handle is still alive; exercise the same race
   against the process service before treating its recovery as complete.
2. Establish one POSIX descriptor view for regular files, pipes and console
   under the [descriptor migration contract](native-posix-descriptors.md)
   before redirecting mlibc's `open`/`read`/`write`/`close` calls. Its interface
   must preserve shared open-description offsets across `dup` and `fork`,
   per-descriptor close-on-exec behavior, and capability cleanup on close,
   exit and service death. Then move operation families through namespace and
   file-object capabilities. Run real ash and file-utility workflows on the
   new path in all six architecture/build combinations. Keep the old VFS path
   until those workflows and failure cleanup pass on the replacement.
   The current managed process session survives `exec` through the startup
   auxiliary vector; a userspace descriptor table needs an equally explicit
   handoff. During the transition, kernel-backed pipes and console handles
   still share POSIX descriptor numbers with capability-backed files. A table
   copied only in libc memory would split offsets after `fork` and disappear
   after `exec`.
   An internal file, pipe and console descriptor view now shares offsets and
   object references through the process service and clones references for
   managed children. Managed libc reports successful exec from the preinit
   array so the service closes marked entries before program constructors.
   The production probe checks that
   `dup` clears that flag, the duplicate survives fork and exec, and the
   parent's marked entry remains open. A caller can explicitly request a
   transferable file capability from the namespace and install it into this
   view; a send-only lookup remains nondelegable. The supervisor seeds 0–2
   from a console service and the process service installs pipe pairs in the
   same table. Managed libc still uses the kernel descriptor table.
3. Move ordinary image parsing and external page supply to userspace after
   their kernel authority and immutable-content invariants are testable. Use
   separate checks for approval, revocation, repaging and failed services.
4. Isolate device recovery domains and add network, graphics and power policy
   services when their platform mechanisms exist. QEMU results alone do not
   establish physical DMA confinement, display isolation or power behavior.

For each migrated boundary, retain the original failure evidence and require
six-configuration build/application/production checks, targeted fault
injection, resource recovery and comparable performance evidence. Report
physical-board validation separately from QEMU and cross-compilation.
