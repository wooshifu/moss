# Bootstrap hardware and driver-service boundary

Moss builds a generic image for each ISA and build configuration, then discovers supported hardware from firmware at boot. The kernel currently retains the controller, timer and diagnostic console mechanisms needed to reach the initial userspace supervisor. [ADR-0015](adr/0015-default-device-drivers-to-isolated-services.md) places ordinary device enumeration, matching, protocol state and recovery in isolated services; those services and their resource capabilities are not implemented yet.

## Source boundaries

| Module | Current role |
| --- | --- |
| `moss.drivers.uart` (`src/drivers/serial`) | PL011/16550 register access and receive controls needed by the bootstrap console |
| `moss.drivers.irqchip` (`src/drivers/irqchip`) | GICv2/v3, BCM, APIC and PLIC interrupt-controller mechanisms |
| `moss.drivers.timer` (`src/drivers/timer`) | Architectural/SBI/LAPIC timer mechanisms and PIT calibration |
| `moss.drivers.console` | Production RX buffering, IRQ delivery and blocking input |
| `moss.interrupts`, `moss.timer` | Interrupt dispatch and clocksource/hrtimer queues |
| `moss.drivers` | Former in-kernel `DeviceManager`/`Driver` registry, linked only into the validation image |
| Architecture boot/trap code | Boot protocols, mappings, CPU startup and exception entry |

The hardware modules remain separate OBJECT targets. Existing `moss.hal.uart`, `moss.hal.intc` and `moss.hal.timer` imports re-export their namespace aliases. The production image links `moss_driver_console` and the hardware mechanisms; only the validation image links `moss_drivers_validation`.

## Bootstrap ownership

Boot initializes the interrupt controller and per-CPU timers. EarlyInit initializes the clocksource and timer queue. DeviceInit borrows the boot-owned controller, verifies timer readiness and initializes console RX when firmware selected a UART. Reinitializing the controller or timer here would reset live interrupt routes, calibration or scheduler ticks. Console initialization is idempotent, and required IRQ registration failures abort DeviceInit. A missing optional UART does not create a fictitious device or driver binding.

The kernel does not publish a device registry or run `Driver::probe` to adopt already-live hardware. The former matching and callback implementation remains in validation for its ownership and rollback regressions; those tests do not establish a production device ABI. A later driver service needs capability-scoped MMIO/port and interrupt access, DMA lifetime enforcement, exclusive ownership and reset authority. Hardware able to DMA without effective IOMMU isolation needs an explicit policy before it can be treated as isolated.

## Console concurrency

ARM64 and x64 deliver UART RX through the active interrupt controller. RV64 retains WFI polling. An IRQ-safe lock protects the RX ring and waiter list across CPUs. Checking input and preparing/publishing sleep occur under that event lock. Wakeup borrows waiter nodes; only the sleeping owner unlinks its stack node, including after a signal wakeup. The ABI I/O wait bridges hand off to the scheduler without importing process into the console module.

The ring has a 255-byte usable capacity and drops newly arriving bytes when full. PL011 RX status is acknowledged before draining the FIFO, so a refill cannot lose its notification. Repeated initialization preserves the ring. Line editing, echo and Ctrl-C behavior remain in devfs; caught signals return a partial read or EINTR through its existing error conversion. Interrupt handlers preserve EOI before callbacks that can context-switch. Timer programming still happens on the receiving CPU before scheduling.

## Checks

`uv run scripts/kernel_validation.py run --manifest build/arm64-debug/moss-artifacts.json --workload drivers` exercises the validation-only registry's registration orders, matching, rollback and ownership, plus the production console's boot readiness, repeat initialization and RX ring. The `users.signals` suite covers console reads interrupted from another CPU. Host console interleaving tests execute the production queue, receive and blocking bodies with modeled IRQ/scheduler bridges; they supplement QEMU checks.

Use the matching Debug, Release and RelWithDebInfo workflows for all three ISAs to check the production boot, userspace and validation paths. These are emulator checks; physical-machine acceptance and isolated-driver crash, mapping and DMA cleanup remain separate.
