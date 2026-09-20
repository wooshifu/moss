---
status: accepted
date: 2026-09-19
---

# Run Network Stacks as Userspace Services

Moss will implement Ethernet and related link protocols, ARP or NDP, IP, ICMP, transport protocols, routing, interface configuration, firewalling, NAT and network-namespace policy in one or more Network Stack Services. Socket state, congestion control and protocol timers belong to those services rather than the Mechanism Kernel Domain.

An Isolated Device Driver owns each NIC Driver Recovery Domain and exchanges capability-authorized packet buffers with a selected Network Stack Service through a Shared Memory Data Plane. Control and availability notifications use bounded IPC. Applications receive Socket Capabilities, with POSIX socket file descriptors provided by the POSIX Compatibility View. VPN and proxy services can be composed above restricted network capabilities without kernel protocol hooks.

## Considered Options

A kernel-resident network stack was rejected as the default because packet parsing, protocol state machines and mutable routing or firewall policy would enlarge the kernel failure and update boundary. Combining the NIC driver and protocol stack in one domain was rejected because a protocol fault should not require rebinding the hardware and a driver fault should not corrupt transport state. Adding a speculative generic in-kernel fast path was rejected until an end-to-end workload demonstrates a concrete latency, throughput or power violation.

## Consequences

Moss needs packet-buffer Memory Objects, ownership transitions, batching and wakeup mechanisms that preserve isolation across the NIC and stack domains. Network service death invalidates its Socket Capabilities and clients reconnect according to their application protocols; transport connections are not silently restored. Tests must cover malformed packets, buffer ownership, service and driver failure, namespace separation and backpressure. Any later in-kernel acceleration is a separately reviewed Kernel Residency Exception and must not establish a second socket or policy model.
