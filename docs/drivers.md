# Built-in drivers and boot adoption

Moss statically links its supported drivers into one image per architecture. Firmware chooses the active resources at boot; selecting a QEMU machine does not select another kernel build. Loadable modules, hotplug and additional bus enumeration are outside the current implementation.

## Source boundaries

| Module | Responsibility |
| --- | --- |
| `moss.drivers.uart` (`src/drivers/serial`) | PL011/16550 register access, transmit serialization and receive controls |
| `moss.drivers.irqchip` (`src/drivers/irqchip`) | GICv2/v3, BCM, APIC and PLIC hardware operations |
| `moss.drivers.timer` (`src/drivers/timer`) | Architectural/SBI/LAPIC timer operations and PIT calibration |
| `moss.drivers.console` | RX buffering, IRQ delivery and blocking input |
| `moss.drivers` | Passive device descriptions, driver matching and binding lifecycle |
| `moss.interrupts`, `moss.timer` | Interrupt descriptors/dispatch and clocksource/hrtimer queues |
| Architecture boot/trap code | Boot protocols, mappings, CPU startup and exception entry |

The three hardware modules have no allocator or scheduler dependency. Existing `moss.hal.uart`, `moss.hal.intc` and `moss.hal.timer` imports re-export them through namespace aliases, preserving existing callers. Separate OBJECT targets prevent a HAL-to-device-manager dependency cycle. Production and validation images link the same driver objects.

## Discovery and binding

DeviceInit registers drivers first, then the active interrupt controller, timer and firmware-selected console. An unavailable optional UART is omitted. Matching requires both `DeviceType` and `HardwareId`; a driver may additionally require a compatible string. DTB parsing and the existing ACPI/architectural discovery select these identities. ACPI and architectural devices have no fabricated DTB compatible strings.

The manager assigns the device ID before calling probe, stores that ID in the device, compares names by content and owns shared references to drivers. Either registration order can bind a device. Probe and remove execute outside registry locks. `Initializing` reserves a transition, preventing a concurrent second probe, removal or power transition. Failed probes record an error and remain registered in `Error`, without becoming active. A failed probe must roll back its own new resources; remove is called only for successful bindings.

The current registry uses linear scans: the production boot set contains three devices. Add an index when enumeration materially increases this set, rather than introducing another registry now. Manager destruction requires all registry users to have stopped.

## Hardware ownership

| Binding mode | Ownership and teardown |
| --- | --- |
| `Initialize` | The driver owns resources acquired by successful probe; unregister or manager destruction calls remove once. |
| `AdoptBoot` | Hardware remains borrowed for kernel lifetime. Unregister and suspend are rejected; registry destruction releases descriptors without resetting hardware or calling remove. |

Boot already initializes the interrupt controller and per-CPU timers. EarlyInit initializes the clocksource and timer queue. DeviceInit adopts those exact objects and calibration, rather than resetting interrupt routes, timer queues or scheduler ticks. UART probe adopts boot output and initializes RX once. Later devfs initialization uses the idempotent C ABI bridge and does not reset buffered input. Required RX registration errors propagate through probe and fail DeviceInit. IRQ descriptor, shared control-block and table-node allocations are fallible; failure leaves no registered descriptor or new hardware state.

`drivers::g_device_manager` refers to the Kernel-owned manager and is cleared during shutdown. Shutdown does not suspend the controller or timer while scheduling still needs them. Borrowed bootstrap hardware is not deleted by the registry.

## Console concurrency

ARM64 and x64 deliver UART RX through the active interrupt controller. RV64 retains WFI polling. An IRQ-safe lock protects the RX ring and waiter list across CPUs. Checking input and preparing/publishing sleep occur under that event lock. Wakeup borrows waiter nodes; only the sleeping owner unlinks its stack node, including after a signal wakeup. The existing ABI I/O wait bridges provide the scheduler continuation handoff without importing process into the console module.

The ring preserves the existing 255-byte capacity and drops newly arriving bytes when full. PL011 RX status is acknowledged before draining the FIFO, so a refill cannot lose its notification. Repeated initialization preserves the ring. Line editing, echo and Ctrl-C behavior remain in devfs; caught signals return a partial read or EINTR through its existing error conversion.

Interrupt handlers preserve EOI before callbacks that can context-switch. Timer programming still happens on the receiving CPU before scheduling.

## Checks

`uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --workload drivers` covers both registration orders, IDs, content names, matching, duplicate rejection, probe rollback, allocation failure, owned remove, borrowed boot adoption and RX ring wrap/overflow. The `users.signals` suite includes console reads interrupted by a signal from another CPU, both empty-input EINTR and partial input delivered through the real UART. Host console interleaving tests execute the production queue, receive and blocking bodies with modeled IRQ/scheduler bridges; they supplement real QEMU checks.

Build all three architecture presets in Debug, Release and RelWithDebInfo. Run the matching Debug/Release CTest presets, including functional, applications, framework, production boot, ARM64 Debug first-read GDB input and Release benchmarks. ARM64 GICv3 and Raspberry Pi profiles must reuse an unchanged image hash. These are emulator checks; physical-machine acceptance remains separate.
