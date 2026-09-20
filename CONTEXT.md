# Moss Kernel

The vocabulary used to describe Moss system architecture and evidence about kernel behavior and performance.

## Language

### System Architecture

**Moss Native System**:
An operating system whose kernel/user ABI and system-service contracts are owned by Moss, with source compatibility used selectively for application portability. Linux or Android binary and driver compatibility do not constrain the primary architecture.
_Avoid_: Android-compatible Moss, Linux-compatible Moss

**Mechanism Kernel Domain**:
The privileged Moss protection domain that owns machine-wide execution, memory-protection, interrupt and authority invariants. Functionality belongs here only when isolation cannot satisfy an accepted system contract.
_Avoid_: Monolithic side, macro-kernel module

**Isolated System Service**:
A Moss system component that runs in its own protection domain and accesses resources only through explicit kernel-mediated authority. Its failure may disrupt the service and its clients but must not corrupt the Mechanism Kernel Domain.
_Avoid_: User-space helper, trusted daemon

**Kernel Residency Exception**:
An explicit decision to place a subsystem path in the Mechanism Kernel Domain because it is required for bootstrap, depends on privilege that cannot be safely mediated, or violates a measured latency, throughput or power budget across the service boundary.
_Avoid_: Performance-sensitive code, temporary kernel code

**Moss Capability Handle**:
A process-local reference to a kernel object carrying explicit, non-amplifiable rights. Possession authorizes only those operations, and transfer or revocation is mediated by the Mechanism Kernel Domain.
_Avoid_: Global object ID, permission-bearing PID, file descriptor

**POSIX Compatibility View**:
A source-compatibility projection of file descriptors, paths, process identifiers and credentials over Moss capability-backed services. It does not define the underlying cross-domain authority model.
_Avoid_: Native kernel authority model, Linux ABI

**Bounded Control Message**:
A kernel-mediated cross-domain message whose payload has a protocol-independent upper bound and may atomically transfer Moss Capability Handles. It carries commands and metadata rather than arbitrary bulk data; the exact bound and call semantics are separate decisions.
_Avoid_: Universal IPC packet, zero-copy message

**Moss Memory Object**:
A capability-backed kernel object that owns or references pages which may be mapped into authorized protection domains. The kernel controls mapping rights, isolation and lifetime while system-service protocols define the data layout placed in those pages.
_Avoid_: Globally shared buffer, implicitly trusted shared memory

**Shared Memory Data Plane**:
A bulk-data path built by system services over Moss Memory Objects, such as a ring or buffer pool. Control messages transfer the required handles and range or synchronization metadata, but the kernel IPC queue does not carry the bulk payload.
_Avoid_: Kernel-defined universal ring buffer, unbounded control message

**Synchronous Control Call**:
A native IPC operation that sends a Bounded Control Message and blocks the caller until the kernel commits exactly one terminal outcome: a reply, caller cancellation or deadline expiry, or peer closure or death. Priority propagation and call-chain limits are separate scheduling decisions.
_Avoid_: Indefinitely blocking send, userspace-correlated RPC

**Reply Capability**:
Kernel-managed, unforgeable one-shot authority to complete one specific pending Synchronous Control Call. A successful reply consumes the authority, and every other terminal outcome invalidates it so that late or duplicate replies cannot complete the call.
_Avoid_: Response correlation ID, reusable reply port

**IPC Priority Inheritance**:
Temporary kernel propagation of the highest effective priority among blocked synchronous callers to the thread servicing their call, including through nested Synchronous Control Calls. Removing a wait dependency recomputes the inherited priority without changing any thread's configured base priority.
_Avoid_: Permanent priority boost, userspace priority repair

**Service Incarnation**:
One running instance of an Isolated System Service with a distinct identity and lifetime. Capabilities that target its objects remain bound to that incarnation and become permanently invalid when it dies, even if a service with the same discovery name is restarted.
_Avoid_: Stable service PID, automatically rebound service

**Explicit Service Reconnection**:
Client recovery that discovers a replacement Service Incarnation, obtains new capabilities and reconstructs protocol state after peer death. A client library may retry only operations whose protocol explicitly defines them as safe to repeat.
_Avoid_: Kernel replay, transparent capability rebinding

**Initial System Supervisor**:
The first userspace process created by the Mechanism Kernel Domain. It receives the Bootstrap Capability Set and owns system-service construction, resource and authority delegation, dependency ordering, discovery policy, failure response and recovery policy.
_Avoid_: Kernel service manager, privileged background helper

**Bootstrap Capability Set**:
The finite set of initial capabilities supplied to the Initial System Supervisor so it can construct the userspace system and delegate narrower authority. It is the root of userspace authority, not an ambient privilege mode inherited by every service.
_Avoid_: Userspace kernel privilege, unrestricted root process

**Isolated Device Driver**:
An Isolated System Service that implements device protocol and policy using capability-scoped access to its hardware resources. It may receive bounded MMIO or I/O-port mappings, interrupt events and DMA mappings, but it does not execute callbacks in the Mechanism Kernel Domain.
_Avoid_: Loadable kernel driver, userspace helper for a kernel driver

**Device Authority Primitive**:
A kernel mechanism that enforces device ownership and safely mediates privileged hardware access, including resource mapping, interrupt routing, DMA/IOMMU mappings and reset authority. It provides no device-specific protocol policy beyond what bootstrap or hardware enforcement requires.
_Avoid_: Kernel driver framework, device-class policy

**Driver Recovery Domain**:
The smallest set of device functions that Moss can independently authorize, isolate, reset and reconstruct after failure. One Isolated Device Driver protection domain owns that unit; functions are grouped only when hardware isolation or recovery is inseparable, or when an explicit measurement justifies a larger failure boundary.
_Avoid_: One process per source module, shared driver host by default

**DMA Confinement**:
Hardware-enforced restriction of a bus-mastering device to the physical pages explicitly mapped for its Driver Recovery Domain, normally through an IOMMU, SMMU or device DMA window. Allocating bounce buffers without preventing arbitrary DMA address programming is not confinement.
_Avoid_: Process isolation, DMA-compatible allocation

**DMA-Trusted Driver Domain**:
An explicitly declared Driver Recovery Domain that controls a bus-mastering device without enforceable DMA Confinement. Although its CPU execution remains in userspace, its ability to overwrite arbitrary physical memory makes the domain part of the system trusted computing base.
_Avoid_: Isolated Device Driver, safe userspace DMA

**Filesystem Namespace Service**:
An Isolated System Service that owns pathname interpretation, mount namespaces, current-directory context and routing to filesystem services. Successful resolution returns an object capability; the Mechanism Kernel Domain does not interpret paths or mount relationships.
_Avoid_: Kernel VFS, global path authority

**Filesystem Service**:
An Isolated System Service that implements an in-memory, persistent or remote filesystem's object model, format parsing, directory operations and access policy. It exposes capability-authorized file and directory protocols rather than kernel inode or dentry callbacks.
_Avoid_: Filesystem kernel module, userspace helper for kernel VFS

**File Object Capability**:
A Moss Capability Handle authorizing operations on one open file or directory object supplied by a Filesystem Service. A POSIX file descriptor is a compatibility-layer number referring to such authority, not the native object identity.
_Avoid_: Kernel `File *`, globally meaningful file descriptor

**Pager-Backed Memory Object**:
A Moss Memory Object whose absent contents are supplied and whose dirty contents are reconciled by an authorized Pager Service. The kernel manages physical residency, mappings, access checks and memory accounting without interpreting the external content as a file or other domain object.
_Avoid_: Kernel file mapping, userspace-controlled page table

**Pager Service**:
An Isolated System Service that interprets offsets in a Pager-Backed Memory Object, supplies requested contents and implements coherence, truncation and writeback policy. It does not choose physical frames or install process page-table entries.
_Avoid_: Kernel page cache policy, unrestricted external page-fault handler

**Network Stack Service**:
An Isolated System Service that implements packet protocols, routing, transport state and network policy above an Isolated Device Driver. It exchanges packet buffers through a Shared Memory Data Plane and exposes capability-authorized network objects to clients.
_Avoid_: Kernel network stack, NIC driver with embedded TCP/IP

**Socket Capability**:
A Moss Capability Handle authorizing operations on one endpoint owned by a Network Stack Service. POSIX socket descriptors and operations are compatibility projections over this object and its data plane.
_Avoid_: Kernel socket object, globally addressable connection ID

**Graphics Execution Service**:
An Isolated Device Driver role that owns GPU execution contexts, firmware and command-submission or GPU-scheduling policy. It operates only on capability-authorized buffers and synchronization objects; hardware that cannot confine GPU access makes the service an explicitly trusted domain.
_Avoid_: Kernel graphics stack, application MMIO access

**Display Driver Service**:
An Isolated Device Driver role that owns display modes, scanout, refresh behavior, hardware planes and connector state. It may share a Driver Recovery Domain with GPU execution only when their hardware isolation or recovery is inseparable.
_Avoid_: Kernel modesetting policy, framebuffer as window system

**Compositor Service**:
An Isolated System Service that owns window, layer, surface-placement and presentation policy while accessing only explicitly transferred surface Memory Objects and display capabilities.
_Avoid_: Kernel window manager, compositor with ambient process-memory access

**Power Policy Service**:
An Isolated System Service that selects performance, energy, ordinary thermal and system-suspend policy from workload and platform constraints. It coordinates dependent services and drivers but cannot bypass kernel-enforced safety or resource limits.
_Avoid_: Kernel power governor, privileged policy thread

**Atomic Power Transition**:
A race-sensitive kernel mechanism that commits CPU idle, wakeup, final system suspend or early resume only after validating current scheduler, timer and wake-source state. It implements a requested safe transition without owning product power policy.
_Avoid_: Kernel suspend policy, userspace direct idle instruction

**Kernel Scheduling Core**:
The Mechanism Kernel Domain facility that owns runnable state, preemption, blocking and wakeup, context switching, CPU-time accounting, resource enforcement and admission decisions. Its boundary is stable even when its scheduling classes or selection algorithms change.
_Avoid_: Userspace final dispatcher, CFS as native ABI

**Scheduling Profile Capability**:
Delegated authority to request a bounded scheduling configuration for selected threads or groups. The Kernel Scheduling Core validates and enforces the request; possession does not grant direct run-queue control or permission to exceed its resource limits.
_Avoid_: Unrestricted priority setting, scheduler admin mode

**Execution Domain**:
A capability-addressed kernel protection object containing an address space, capability table, resource accounting and termination state. Kernel-internal identifiers may support diagnostics but do not grant authority or define POSIX process identity.
_Avoid_: Globally addressable PID, POSIX process object in the native ABI

**Process Compatibility Service**:
An Isolated System Service that builds POSIX process identities and relationships over Execution Domain and Thread capabilities. It owns PID namespaces, parent and child policy, process groups, sessions, compatibility credentials, wait state and signal semantics.
_Avoid_: Kernel PID manager, native authority through uid or PID

**Loader Service**:
An Isolated System Service that parses executable formats and constructs a new Execution Domain from capability-authorized Memory Objects, mappings, initial thread state and explicitly inherited handles. The kernel validates generic execution invariants without parsing the executable format.
_Avoid_: Kernel ELF loader, executable parser in a syscall

### Validation

**General-Purpose Kernel Capability Completeness**:
Completeness of the explicitly agreed Moss capability profile, assessed through command-line applications under declared workloads and supported environments. Acceptance covers that profile's required system behaviors, resource lifecycles and performance obligations.
_Avoid_: Passing the existing core test catalog described as complete general-purpose kernel acceptance.

**Moss Application Source Compatibility**:
The ability to rebuild agreed existing C/POSIX command-line applications for Moss, with C-library or platform adaptation, while preserving their required observable behavior.
_Avoid_: Linux binary compatibility; complete POSIX conformance inferred from selected applications.

**ut_kernel**:
Moss's kernel unit-testing framework, abbreviated from the project's term `unit_kenel`.
_Avoid_: Unity.

**Kernel Functional Test**:
A check of an actual Moss kernel facility under the conditions required by that facility. Its result provides evidence only for the behavior and conditions it exercises.
_Avoid_: Language demonstrations described as kernel functional coverage.

**Kernel Stability Test**:
A check that Moss preserves its required behavior and resource ownership across repeated lifecycle operations or concurrent execution under declared workloads and resource limits.
_Avoid_: A successful boot described as kernel stability acceptance.

**Kernel Lifecycle Cycle**:
A complete execution of a declared Moss resource lifecycle, from creation through use and release to verification of the expected post-release state.
_Avoid_: Individual operations or polling iterations counted as completed lifecycles.

**Kernel Benchmark**:
A repeatable measurement of an operation performed by an actual Moss kernel facility under a specified workload and execution environment.
_Avoid_: General arithmetic measurements described as kernel performance.

**Kernel Benchmark Scenario**:
A named Moss kernel operation together with fixed workload parameters that identify a single performance comparison target.

**Kernel Benchmark Baseline**:
An explicitly selected set of prior kernel benchmark measurements used as the reference for matching workloads under comparable conditions.

**Kernel Performance Regression**:
A repeatable worsening of a Moss kernel operation's measured performance relative to its benchmark baseline under matching workloads and comparable execution conditions.
_Avoid_: A single slower sample described as a confirmed regression.

**Kernel Function Microbenchmark**:
A kernel benchmark that repeatedly invokes one selected production kernel function using declared inputs and preconditions.
_Avoid_: Runtime function profiling.

**Kernel Validation Image**:
A bootable Moss kernel dedicated to executing kernel functional tests and kernel benchmarks.

**Kernel Validation Report**:
A record of kernel test outcomes, benchmark measurements, and their execution conditions, including failures and selected workloads that did not run.

**Kernel Test Suite**:
A declared group of kernel functional tests with compatible runtime requirements and resource-cleanup expectations. Its cases can share one kernel lifetime.

**Destructive Kernel Test**:
A kernel functional test whose expected failure or persistent state changes prevent trustworthy subsequent tests in the same kernel lifetime.
