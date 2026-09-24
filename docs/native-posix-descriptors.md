# Native POSIX Descriptor Migration

This is the implementation contract for the descriptor part of
[ADR-0018](adr/0018-move-vfs-and-filesystems-to-userspace.md). It is not a
completion claim.

## Current split

The kernel [FdTable](../src/vfs/src/vfs-file.cppm) allocates 0–255 for console,
regular files and pipes. Managed [mlibc](../third_party/mlibc/sysdeps/moss/sysdeps.cpp)
still sends `open`, `read`, `write`, `close`, `dup`, `fcntl`, `pipe` and `lseek`
to that table. The [Process Compatibility Service](../src/userspace/moss_process_service.c)
independently allocates 0–255 for its console, file and pipe view; the same
number can name different objects in the two tables. The supervisor seeds
0–2 from a supervised Console Object Service before managed children fork.
That service can write through the bootstrap kernel console and has a bounded
input ring, but physical input is not connected to it yet. A privileged probe
uses a badged input sender to exercise its read path without racing the
legacy shell for keyboard bytes. Its shared open descriptions already
preserve offsets across `dup` and `fork`, and its per-descriptor flags survive
`fork` and close before managed constructors after `exec`. A caller can import
a transferable File Object Capability into this view. Its `FD_PIPE` request
installs both pipe ends in one reply transaction; the final shared-description
close notifies the supervised Pipe Object Service. The pipe service has bounded
one-page rings, explicit `WOULD_BLOCK`, EOF and broken-write results. A direct
pipe transfer commits only after its immediate IPC reply succeeds. Managed
pipe and console reads reserve bytes until the Process Service confirms
whether its reply reached the caller; a failed finalization retires that
service epoch before another read. The supervisor also restarts
that epoch if the Pipe Object Service dies;
old endpoint capabilities cannot reach the replacement service. The protocol
now distinguishes missing paths, bad descriptors and a full per-process table;
other backend failures still need precise POSIX error mapping.
Descriptor allocation now precedes namespace `CREATE` and file `TRUNCATE`, so
a known-full table or local allocation failure cannot mutate the file first.
`FD_STAT` reports descriptor kind and asks the serving file service for current
size and a file ID scoped to that service incarnation. Pipe and console report
their kind with zero ID and size; directory entries, timestamps, credentials
and readiness metadata remain to be defined.

The process service currently waits synchronously for each object call
while processing one request at a time. A blocking console read or pipe read
would therefore delay unrelated process registration, wait and signal calls.
Console and pipe reads return `WOULD_BLOCK` instead of waiting; the existing
one-second backend deadline bounds a stalled call. Kernel `SYS_POLL` is still
unimplemented, so physical console input needs a dedicated blocking reader
domain or a native readiness mechanism before libc switches. Blocking I/O,
uncertain writes and cancellation across service death still need a complete
transaction contract before libc can use these descriptors.

## Ownership boundary

1. The Process Compatibility Service must allocate every managed POSIX
   descriptor number, including 0–2, and own its per-process table,
   per-descriptor flags and shared open-description identity. Managed libc must
   use that view for **all** descriptor operations before ordinary file I/O is
   redirected. The kernel table remains a bootstrap/legacy implementation
   during migration, not another descriptor authority for a managed process.
2. A regular file, console stream or pipe end must be represented by a
   capability to its serving userspace object. The namespace selects a file
   object on open; later operations use the returned authority without another
   pathname lookup. The supervisor supplies initial console authority when it
   starts a managed tree. Pipe creation returns both ends to one descriptor
   installation transaction; failure or an undelivered reply releases both.
3. A blocking backend may not hold the process registry's only request loop.
   The I/O contract needs pending-call cancellation, peer-death and readiness
   behavior before libc uses it for console and pipes. A shared open
   description must serialize offset-sensitive operations even when those
   calls are concurrent. `close`, `fork`, `exec` and service death must release
   object authority exactly once per final description reference.
4. POSIX errors must distinguish a missing path, bad descriptor, exhausted
   descriptor table, denied operation, interrupted call and dead backend.
   Uncertain writes need an explicit recovery result; retrying them blindly
   can duplicate bytes. A backend restart must never rebind an old object cap
   to a new object merely because its pathname or numeric badge matches.

## Delivery order and evidence

1. Add console and pipe object protocols with bounded I/O, EOF/EPIPE,
   interrupt and close semantics. Exercise concurrent readers and writers,
   cancellation, endpoint death and reclamation before exposing them to libc.
2. Seed managed descriptors 0–2 from supervisor-delegated console authority;
   create and install both pipe ends atomically. Verify `dup2` redirection,
   `fork` offset sharing, close-on-exec and final-close behavior in a real
   managed process.
3. Route the entire managed mlibc descriptor family through the one view,
   including metadata and directory operations needed by BusyBox. Run ash
   pipelines, redirection and file utilities before removing the kernel VFS
   path for managed programs.
4. For each moved family, retain fault-injection evidence and run the six
   architecture/build application and production checks, resource baselines,
   long-run stability and comparable performance measurements. QEMU results
   do not establish physical console, DMA or persistent-storage behavior.

The kernel [pipe](../src/vfs/src/vfs_init.cpp) and
[console](../src/vfs/src/vfs_init.cpp) implementations define the current
observable behavior to preserve while their authority moves out of the
kernel VFS model.
