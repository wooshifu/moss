// MOSS Interrupt Controller Hardware Abstraction Layer
//
// Provides architecture-specific interrupt controller register operations.
//
// What lives here (architecture-specific):
//   - GIC and BCM2836 register offsets (ARM64, runtime dispatch)
//   - APIC register constants and I/O APIC routing (x64)
//   - PLIC registers and SBI IPI delivery (RISC-V 64)
//   - Low-level init, enable/disable IRQ, ack/eoi, send SGI/IPI
//
// What stays in interrupts.cppm (architecture-independent):
//   - InterruptDescriptor, InterruptHandler, IRQ routing table
//   - GenericInterruptController high-level class (uses IntcHal ops)
//   - IPI subsystems (simple, hw_simple)

export module moss.hal.intc;

import moss.intrinsics;
import moss.std;
import moss.types;
import moss.result;
import moss.platform;
import moss.arch;

export namespace moss::kernel::hal::intc {

using moss::u32;
using moss::u64;
using moss::u8;
using moss::kernel::ErrorCode;
using moss::kernel::VirtAddr;
using moss::kernel::VoidResult;

// ============================================================================
// Low-level MMIO register access (needed early for GICR helpers below)
// ============================================================================

/// Read a 32-bit register at (base + offset).
[[nodiscard]] inline u32 read_reg(VirtAddr base, u32 offset) noexcept {
  return *reinterpret_cast<volatile u32 *>(base + offset);
}

/// Write a 32-bit value to register at (base + offset).
inline void write_reg(VirtAddr base, u32 offset, u32 value) noexcept {
  *reinterpret_cast<volatile u32 *>(base + offset) = value;
}

// ============================================================================
// GIC version runtime dispatch
// ============================================================================

#if defined(MOSS_ARCH_ARM64)
inline u32 g_gic_cpu_masks[16]{};
enum class GicVersion : u8 { Unknown = 0, BCM2836 = 1, GICv2 = 2, GICv3 = 3 };

// Set once during boot from DTB detection, read-only after.
inline GicVersion g_gic_version = GicVersion::Unknown;

// GICv3 redistributor base address (set once during boot).
inline VirtAddr g_redist_base = 0;
#endif

// ============================================================================
// Interrupt controller register offsets — architecture-specific
// ============================================================================

#if defined(MOSS_ARCH_ARM64)

namespace bcm2836 {
// BCM2836 ARM-local register map (QA7 rev 3.4) and BCM2835 ARM Peripherals §7.
// Bases come from the DTB; offsets and the four-core register strides are hardware ABI.
inline constexpr u32 GPU_ROUTE = 0x0C;
inline constexpr u32 TIMER_CONTROL = 0x40;
inline constexpr u32 MAILBOX_CONTROL = 0x50;
inline constexpr u32 IRQ_SOURCE = 0x60;
inline constexpr u32 MAILBOX_SET = 0x80;
inline constexpr u32 MAILBOX_CLEAR = 0xC0;
inline constexpr u32 PENDING_BASIC = 0x00;
inline constexpr u32 PENDING_GPU = 0x04;
inline constexpr u32 FIQ_CONTROL = 0x0C;
inline constexpr u32 ENABLE_GPU = 0x10;
inline constexpr u32 ENABLE_BASIC = 0x18;
inline constexpr u32 DISABLE_GPU = 0x1C;
inline constexpr u32 DISABLE_BASIC = 0x24;
inline constexpr u32 CORE_COUNT = 4;
inline constexpr u32 CORE_STRIDE = 4;
inline constexpr u32 MAILBOX_STRIDE = 16; // Four mailboxes per physical core; mailbox zero carries IPIs.
inline constexpr u32 MAILBOX_SOURCE = 1U << 4;
inline constexpr u32 GPU_SOURCE = 1U << 8;

// Preserve the generic dispatch contract: 16 software IPIs, local timer IDs
// 16..19, GPU bank IDs 32..95 and the eight basic IRQs at 96..103.
inline constexpr u32 IPI_COUNT = 16;
inline constexpr u32 TIMER_BASE = 16;
inline constexpr u32 TIMER_COUNT = 4;
inline constexpr u32 GPU_BASE = 32;
inline constexpr u32 BASIC_BASE = 96;
inline constexpr u32 IRQ_COUNT = 104;
inline u32 ipi_masks[CORE_COUNT]{}; // Only the owning CPU changes its local enable mask.

[[nodiscard]] inline u32 core_id() noexcept {
  return static_cast<u32>(platform::hardware.cpus[arch::get_current_cpu_id()].hardware_id);
}

inline void set_enabled(VirtAddr local, u32 irq, bool enable) noexcept {
  u32 cpu = core_id();
  if (irq < IPI_COUNT) {
    u32 &mask = ipi_masks[arch::get_current_cpu_id()];
    mask = enable ? mask | (1U << irq) : mask & ~(1U << irq);
    write_reg(local, MAILBOX_CONTROL + cpu * CORE_STRIDE, mask ? 1U : 0U);
  } else if (irq >= TIMER_BASE && irq < TIMER_BASE + TIMER_COUNT) {
    u32 offset = TIMER_CONTROL + cpu * CORE_STRIDE;
    u32 mask = 1U << (irq - TIMER_BASE);
    u32 value = read_reg(local, offset);
    write_reg(local, offset, enable ? value | mask : value & ~mask);
  } else if (irq >= GPU_BASE && irq < IRQ_COUNT) {
    VirtAddr shared = platform::intc_cpu_base();
    u32 offset = irq >= BASIC_BASE ? (enable ? ENABLE_BASIC : DISABLE_BASIC)
                                   : (enable ? ENABLE_GPU : DISABLE_GPU) + ((irq - GPU_BASE) / 32) * 4;
    write_reg(shared, offset, 1U << ((irq - GPU_BASE) % 32));
  }
}

[[nodiscard]] inline u32 acknowledge() noexcept {
  VirtAddr local = platform::intc_dist_base();
  u32 cpu = core_id();
  u32 sources = read_reg(local, IRQ_SOURCE + cpu * CORE_STRIDE);
  if (sources & MAILBOX_SOURCE) {
    u32 offset = MAILBOX_CLEAR + cpu * MAILBOX_STRIDE;
    u32 pending = read_reg(local, offset);
    u32 enabled = pending & ipi_masks[arch::get_current_cpu_id()];
    if (enabled) {
      u32 irq = static_cast<u32>(intrinsics::bitops::ctz(enabled));
      // Clear before dispatch: clearing at EOI could lose a new IPI posted by
      // another CPU while its handler runs. Other pending IPI bits stay set.
      write_reg(local, offset, 1U << irq);
      asm volatile("dmb ish" ::: "memory");
      return irq;
    }
    write_reg(local, offset, pending); // Disabled IPI types must not keep mailbox zero asserted.
  }
  u32 timers = sources & ((1U << TIMER_COUNT) - 1);
  if (timers) {
    return TIMER_BASE + static_cast<u32>(intrinsics::bitops::ctz(timers));
  }
  if (sources & GPU_SOURCE) {
    VirtAddr shared = platform::intc_cpu_base();
    for (u32 bank = 0; bank < 2; ++bank) {
      u32 pending = read_reg(shared, PENDING_GPU + bank * 4);
      if (pending) {
        return GPU_BASE + bank * 32 + static_cast<u32>(intrinsics::bitops::ctz(pending));
      }
    }
    // Basic-pending's upper bits duplicate GPU sources and are not basic IRQs.
    u32 pending = read_reg(shared, PENDING_BASIC) & 0xFF;
    if (pending) {
      return BASIC_BASE + static_cast<u32>(intrinsics::bitops::ctz(pending));
    }
  }
  return 1023; // Existing ARM64 spurious-IRQ sentinel; BCM has no claim register.
}
} // namespace bcm2836

// GIC register offsets below are byte offsets from the relevant MMIO frame,
// fixed by the Arm GIC register map rather than kernel tuning choices.
// GIC Distributor registers (shared between v2 and v3)
namespace dist_regs {
inline constexpr u32 CTLR = 0x000;       // Distributor Control
inline constexpr u32 TYPER = 0x004;      // Interrupt Controller Type
inline constexpr u32 IIDR = 0x008;       // Distributor Implementer ID
inline constexpr u32 IGROUPR = 0x080;    // Interrupt Group (base)
inline constexpr u32 ISENABLER = 0x100;  // Interrupt Set-Enable (base)
inline constexpr u32 ICENABLER = 0x180;  // Interrupt Clear-Enable (base)
inline constexpr u32 ISPENDR = 0x200;    // Interrupt Set-Pending (base)
inline constexpr u32 ICPENDR = 0x280;    // Interrupt Clear-Pending (base)
inline constexpr u32 ISACTIVER = 0x300;  // Interrupt Set-Active (base)
inline constexpr u32 ICACTIVER = 0x380;  // Interrupt Clear-Active (base)
inline constexpr u32 IPRIORITYR = 0x400; // Interrupt Priority (base)
inline constexpr u32 ITARGETSR = 0x800;  // GICv2: Interrupt Processor Targets (base)
inline constexpr u32 ICFGR = 0xC00;      // Interrupt Configuration (base)
inline constexpr u32 SGIR = 0xF00;       // GICv2: Software Generated Interrupt
// GICv3-specific distributor registers
inline constexpr u32 IROUTER = 0x6100; // GICv3: Interrupt Routing (64-bit per SPI)
inline constexpr u32 PIDR2 = 0xFFE8;   // Peripheral ID 2 (ArchRev in bits [7:4])
// GICv3 GICD_CTLR bit definitions
inline constexpr u32 CTLR_ENABLE_GRP1_NS = (1U << 1);
inline constexpr u32 CTLR_ARE_S = (1U << 4);
} // namespace dist_regs

// GICv2 CPU Interface registers (offset from CPU interface base, MMIO only)
namespace cpu_regs {
inline constexpr u32 CTLR = 0x000;  // CPU Interface Control
inline constexpr u32 PMR = 0x004;   // Priority Mask
inline constexpr u32 BPR = 0x008;   // Binary Point
inline constexpr u32 IAR = 0x00C;   // Interrupt Acknowledge
inline constexpr u32 EOIR = 0x010;  // End of Interrupt
inline constexpr u32 RPR = 0x014;   // Running Priority
inline constexpr u32 HPPIR = 0x018; // Highest Priority Pending Interrupt
} // namespace cpu_regs

// GICv3 Redistributor registers (offset from per-CPU GICR frame)
namespace redist_regs {
inline constexpr u32 CTLR = 0x000;
inline constexpr u32 IIDR = 0x004;
inline constexpr u32 TYPER_LO = 0x008; // GICR_TYPER low 32 bits
inline constexpr u32 TYPER_HI = 0x00C; // GICR_TYPER high 32 bits
inline constexpr u32 WAKER = 0x014;
inline constexpr u32 WAKER_PROCESSOR_SLEEP = (1U << 1);
inline constexpr u32 WAKER_CHILDREN_ASLEEP = (1U << 2);
// SGI/PPI frame offsets (GICR_base + SGI_OFFSET)
inline constexpr u32 SGI_OFFSET = 0x10000;
inline constexpr u32 IGROUPR0 = SGI_OFFSET + 0x080;
inline constexpr u32 ISENABLER0 = SGI_OFFSET + 0x100;
inline constexpr u32 ICENABLER0 = SGI_OFFSET + 0x180;
inline constexpr u32 IPRIORITYR0 = SGI_OFFSET + 0x400;
// Each redistributor frame = 2 × 64KB pages (RD_base + SGI_base)
inline constexpr u32 FRAME_SIZE = 0x20000;
// GICR_TYPER bit definitions
inline constexpr u64 TYPER_LAST = (1ULL << 4);
} // namespace redist_regs

// ============================================================================
// GICv3 ICC system register wrappers (ARM64 inline asm)
// ============================================================================
// Uses raw SysReg encodings (S<op0>_<op1>_C<CRn>_C<CRm>_<op2>)
// for maximum toolchain compatibility.

namespace icc {

// ICC_IAR1_EL1 = S3_0_C12_C12_0 — Interrupt Acknowledge (Group 1)
[[nodiscard]] inline u32 read_iar1() noexcept {
  u64 val;
  asm volatile("mrs %0, S3_0_C12_C12_0" : "=r"(val));
  return static_cast<u32>(val);
}

// ICC_EOIR1_EL1 = S3_0_C12_C12_1 — End of Interrupt (Group 1)
inline void write_eoir1(u32 val) noexcept { asm volatile("msr S3_0_C12_C12_1, %0" ::"r"(static_cast<u64>(val))); }

// ICC_PMR_EL1 = S3_0_C4_C6_0 — Priority Mask
inline void write_pmr(u32 val) noexcept { asm volatile("msr S3_0_C4_C6_0, %0" ::"r"(static_cast<u64>(val))); }

// ICC_BPR1_EL1 = S3_0_C12_C12_3 — Binary Point (Group 1)
inline void write_bpr1(u32 val) noexcept { asm volatile("msr S3_0_C12_C12_3, %0" ::"r"(static_cast<u64>(val))); }

// ICC_CTLR_EL1 = S3_0_C12_C12_4 — Control
inline void write_ctlr(u32 val) noexcept { asm volatile("msr S3_0_C12_C12_4, %0" ::"r"(static_cast<u64>(val))); }

// ICC_SRE_EL1 = S3_0_C12_C12_5 — System Register Enable
[[nodiscard]] inline u32 read_sre() noexcept {
  u64 val;
  asm volatile("mrs %0, S3_0_C12_C12_5" : "=r"(val));
  return static_cast<u32>(val);
}

inline void write_sre(u32 val) noexcept { asm volatile("msr S3_0_C12_C12_5, %0" ::"r"(static_cast<u64>(val))); }

// ICC_IGRPEN1_EL1 = S3_0_C12_C12_7 — Interrupt Group 1 Enable
inline void write_igrpen1(u32 val) noexcept { asm volatile("msr S3_0_C12_C12_7, %0" ::"r"(static_cast<u64>(val))); }

// ICC_SGI1R_EL1 = S3_0_C12_C11_5 — SGI Generation (Group 1, 64-bit)
inline void write_sgi1r(u64 val) noexcept { asm volatile("msr S3_0_C12_C11_5, %0" ::"r"(val)); }

} // namespace icc

// ============================================================================
// GICv3 Redistributor helpers
// ============================================================================

/// Find the GICR frame by matching all four MPIDR affinity bytes.
/// Standard RD+SGI frame pairs occupy 2 * 64 KiB = 128 KiB.
[[nodiscard]] inline VirtAddr find_my_redist_frame() noexcept {
  u64 mpidr;
  asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  u32 my_affinity = static_cast<u32>(mpidr) & 0xFFFFFF;
  my_affinity |= static_cast<u32>((mpidr >> 32) & 0xFF) << 24;

  VirtAddr frame = g_redist_base;
  for (u64 remaining = platform::hardware.intc.redist_size; remaining >= redist_regs::FRAME_SIZE;
       remaining -= redist_regs::FRAME_SIZE) {
    u64 typer = static_cast<u64>(read_reg(frame, redist_regs::TYPER_LO)) |
                (static_cast<u64>(read_reg(frame, redist_regs::TYPER_HI)) << 32);

    if (typer & (1ULL << 1)) {
      return 0; // GICv4 VLPIS changes the redistributor stride; unsupported.
    }
    // GICR_TYPER[63:32] contains Aff3:Aff2:Aff1:Aff0.
    u32 affinity = static_cast<u32>(typer >> 32);
    if (affinity == my_affinity) {
      return frame;
    }

    if (typer & redist_regs::TYPER_LAST) {
      break;
    }
    frame += redist_regs::FRAME_SIZE;
  }
  return 0; // Never configure another CPU's frame when the description is incomplete.
}

/// Wake the redistributor for this CPU (clear ProcessorSleep in GICR_WAKER)
inline void wake_redistributor(VirtAddr redist_frame) noexcept {
  u32 waker = read_reg(redist_frame, redist_regs::WAKER);
  waker &= ~redist_regs::WAKER_PROCESSOR_SLEEP;
  write_reg(redist_frame, redist_regs::WAKER, waker);

  // Wait for ChildrenAsleep to clear
  while (read_reg(redist_frame, redist_regs::WAKER) & redist_regs::WAKER_CHILDREN_ASLEEP) {
    asm volatile("yield" ::: "memory");
  }
}

#elif defined(MOSS_ARCH_X64)
// x64 Local APIC registers (MMIO offsets from APIC base, or MSR addresses)
namespace dist_regs {
// I/O APIC uses an indirect select/window pair, not direct per-IRQ MMIO.
inline constexpr u32 IOREGSEL = 0x00; // I/O Register Select
inline constexpr u32 IOWIN = 0x10;    // I/O Window
} // namespace dist_regs

namespace cpu_regs {
// Local APIC register offsets (relative to the discovered MMIO base)
inline constexpr u32 ID = 0x020;         // Local APIC ID
inline constexpr u32 VERSION = 0x030;    // Local APIC Version
inline constexpr u32 TPR = 0x080;        // Task Priority
inline constexpr u32 EOI = 0x0B0;        // End of Interrupt
inline constexpr u32 SVR = 0x0F0;        // Spurious Interrupt Vector
inline constexpr u32 LVT_TIMER = 0x320;  // LVT Timer Register
inline constexpr u32 LVT_LINT0 = 0x350;  // LVT LINT0 Register
inline constexpr u32 LVT_LINT1 = 0x360;  // LVT LINT1 Register
inline constexpr u32 LVT_ERROR = 0x370;  // LVT Error Register
inline constexpr u32 ICR_LOW = 0x300;    // Interrupt Command (low 32 bits)
inline constexpr u32 ICR_HIGH = 0x310;   // Interrupt Command (high 32 bits)
inline constexpr u32 LVT_MASK = 0x10000; // Mask bit for LVT entries
} // namespace cpu_regs

#elif defined(MOSS_ARCH_RISCV64)
// RISC-V 64 PLIC registers (Platform-Level Interrupt Controller)
namespace dist_regs {
inline constexpr u32 PRIORITY_BASE = 0x000000; // Priority for each source
inline constexpr u32 PENDING_BASE = 0x001000;  // Pending bits
inline constexpr u32 ENABLE_BASE = 0x002000;   // Enable bits per context
} // namespace dist_regs

namespace cpu_regs {
// PLIC context windows start at 0x200000 with a 4 KiB stride; the DTB
// interrupts-extended ordering identifies each supervisor context, not hart ID.
inline constexpr u32 THRESHOLD_OFFSET = 0x200000; // Priority threshold
inline constexpr u32 CLAIM_OFFSET = 0x200004;     // Claim/Complete
} // namespace cpu_regs
#endif

// ============================================================================
// Spurious interrupt threshold — architecture-specific
// ============================================================================
#if defined(MOSS_ARCH_ARM64)
// GIC INTIDs 1020..1023 are special acknowledge responses, not normal IRQs;
// they must not enter descriptor dispatch or receive a normal EOI.
inline constexpr u32 SPURIOUS_IRQ_THRESHOLD = 1020; // Same for GICv2 and GICv3
#elif defined(MOSS_ARCH_X64)
inline constexpr u32 SPURIOUS_IRQ_THRESHOLD = 0xFF; // APIC spurious vector
#elif defined(MOSS_ARCH_RISCV64)
inline constexpr u32 SPURIOUS_IRQ_THRESHOLD = 0; // PLIC: claim=0 means no pending
#endif

// ============================================================================
// Interrupt controller configuration query
// ============================================================================

/// Read the maximum number of supported interrupts from the controller.
/// GICD_TYPER ITLinesNumber field is the same for both GICv2 and GICv3.
[[nodiscard]] inline u32 read_max_interrupts(VirtAddr dist_base) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (g_gic_version == GicVersion::BCM2836) {
    return bcm2836::IRQ_COUNT;
  }
  u32 typer = read_reg(dist_base, dist_regs::TYPER);
  // ITLinesNumber[4:0] encodes one less than the count of 32-interrupt banks.
  return ((typer & 0x1F) + 1) * 32;
#elif defined(MOSS_ARCH_X64)
  (void)dist_base;
  return 256; // x86 supports up to 256 interrupt vectors
#elif defined(MOSS_ARCH_RISCV64)
  (void)dist_base;
  return 1024; // Fixed source-scan policy, not a discovered per-device source count.
#endif
}

/// Read the maximum number of supported CPUs from the controller.
[[nodiscard]] inline u32 read_max_cpus(VirtAddr dist_base) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (g_gic_version == GicVersion::GICv3 || g_gic_version == GicVersion::BCM2836) {
    (void)dist_base;
    return platform::hardware.cpu_count; // Enumerated from firmware, not MMIO guesses.
  }
  // GICv2: CPUNumber field
  u32 typer = read_reg(dist_base, dist_regs::TYPER);
  return ((typer >> 5) & 0x7) + 1;
#elif defined(MOSS_ARCH_X64)
  (void)dist_base;
  return 256; // APIC ID space
#elif defined(MOSS_ARCH_RISCV64)
  (void)dist_base;
  return platform::hardware.cpu_count;
#endif
}

// ============================================================================
// Distributor initialization
// ============================================================================

/// Initialize the interrupt distributor/router.
/// Disables all interrupts, clears pending, sets default priority and targets.
inline VoidResult init_distributor(VirtAddr dist_base, u32 max_interrupts) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (g_gic_version == GicVersion::BCM2836) {
    // Shared peripheral interrupts have one hardware destination, unlike GIC SPIs.
    write_reg(dist_base, bcm2836::GPU_ROUTE, static_cast<u32>(platform::hardware.cpus[0].hardware_id));
    VirtAddr shared = platform::intc_cpu_base();
    write_reg(shared, bcm2836::FIQ_CONTROL, 0);
    write_reg(shared, bcm2836::DISABLE_GPU, ~0U);
    write_reg(shared, bcm2836::DISABLE_GPU + 4, ~0U);
    write_reg(shared, bcm2836::DISABLE_BASIC, 0xFF);
    return VoidResult{};
  }
  // Enable/pending/group banks hold 32 one-bit IRQ fields per 4-byte word:
  // a bank beginning at IRQ i is therefore at i/8 bytes. Priority/target banks
  // pack four 8-bit fields; 0x80808080 gives each IRQ midpoint priority 0x80
  // under PMR=0xff. The exact default-priority policy has no recorded derivation.
  if (g_gic_version == GicVersion::GICv3) {
    // GICv3 distributor init — only handles SPIs (IRQ 32+)
    // SGI/PPI (0-31) are configured via GICR in init_cpu_interface()
    write_reg(dist_base, dist_regs::CTLR, 0);

    // Disable SPIs and clear stale pending state before publishing new routing.
    for (u32 i = 32; i < max_interrupts; i += 32) {
      write_reg(dist_base, dist_regs::ICENABLER + i / 8, 0xFFFFFFFF);
      write_reg(dist_base, dist_regs::ICPENDR + i / 8, 0xFFFFFFFF);
    }

    // Set SPI default priority
    for (u32 i = 32; i < max_interrupts; i += 4) {
      write_reg(dist_base, dist_regs::IPRIORITYR + i, 0x80808080);
    }

    // Set all SPIs to Group 1 NS
    for (u32 i = 32; i < max_interrupts; i += 32) {
      write_reg(dist_base, dist_regs::IGROUPR + i / 8, 0xFFFFFFFF);
    }

    // Route SPIs to logical CPU 0's firmware affinity, which need not be zero.
    // IROUTER starts with SPI 32 and uses 8 bytes per entry; +4 selects its high word.
    for (u32 i = 32; i < max_interrupts; i++) {
      u32 irouter_off = dist_regs::IROUTER + (i - 32) * 8;
      u64 affinity = platform::hardware.cpus[0].hardware_id;
      write_reg(dist_base, irouter_off, static_cast<u32>(affinity));
      write_reg(dist_base, irouter_off + 4, static_cast<u32>(affinity >> 32));
    }

    // Enable distributor with affinity routing
    write_reg(dist_base, dist_regs::CTLR, dist_regs::CTLR_ARE_S | dist_regs::CTLR_ENABLE_GRP1_NS);
  } else {
    // GICv2 distributor init
    write_reg(dist_base, dist_regs::CTLR, 0);

    for (u32 i = 0; i < max_interrupts; i += 32) {
      write_reg(dist_base, dist_regs::ICENABLER + i / 8, 0xFFFFFFFF);
    }

    for (u32 i = 0; i < max_interrupts; i += 32) {
      write_reg(dist_base, dist_regs::ICPENDR + i / 8, 0xFFFFFFFF);
    }

    for (u32 i = 0; i < max_interrupts; i += 4) {
      write_reg(dist_base, dist_regs::IPRIORITYR + i, 0x80808080);
    }

    u32 boot_mask = read_reg(dist_base, dist_regs::ITARGETSR) & 0xFF;
    // Multiplication by 0x01010101 replicates the boot CPU's 8-bit target mask
    // into all four IRQ bytes; using logical bit zero could route to another CPU.
    for (u32 i = 32; i < max_interrupts; i += 4) {
      write_reg(dist_base, dist_regs::ITARGETSR + i, boot_mask * 0x01010101);
    }

    write_reg(dist_base, dist_regs::CTLR, 1);
  }

#elif defined(MOSS_ARCH_X64)
  {
    // IOWIN=base+0x10; IOAPICVER register 1 encodes max entry in bits 23:16.
    // Redirection entries begin at select 0x10, two words per GSI; low bit 16
    // masks delivery while high bits 31:24 select the physical destination APIC.
    auto *select = reinterpret_cast<volatile u32 *>(platform::intc_cpu_base());
    auto *window = reinterpret_cast<volatile u32 *>(platform::intc_cpu_base() + 0x10);
    *select = 1; // IOAPICVER: maximum redirection entry.
    u32 count = ((*window >> 16) & 0xFF) + 1;
    for (u32 i = 0; i < count; ++i) {
      *select = 0x10 + i * 2;
      *window = 1U << 16;
      *select = 0x11 + i * 2;
      *window = static_cast<u32>(platform::hardware.cpus[0].hardware_id) << 24;
    }
    u32 irq = platform::hardware.uart.irq;
    if (platform::hardware.uart.valid && irq < 16 && platform::hardware.isa_gsi[irq] < count) {
      // MADT flags: polarity[1:0]=3 is active-low, trigger[3:2]=3 is level.
      // Map those to I/O APIC bits 13/15; reserve IDT vectors 0..31 for CPU
      // exceptions, so legacy IRQ n is delivered as vector 32+n.
      u32 flags = platform::hardware.isa_flags[irq];
      u32 mode = ((flags & 3) == 3 ? 1U << 13 : 0) | ((flags & 12) == 12 ? 1U << 15 : 0);
      *select = 0x10 + platform::hardware.isa_gsi[irq] * 2;
      *window = (32 + irq) | (1U << 16) | mode;
    }
  }
  (void)dist_base;
  (void)max_interrupts;

#elif defined(MOSS_ARCH_RISCV64)
  // PLIC source zero is reserved; sources use one 4-byte priority register each.
  // Priority zero disables delivery. 1024 is the HAL's fixed scan bound, not
  // firmware proof that this PLIC implements every scanned source.
  for (u32 i = 1; i <= max_interrupts && i <= 1024; i++) {
    write_reg(dist_base, dist_regs::PRIORITY_BASE + i * 4, 0);
  }
  (void)max_interrupts;
#endif

  return VoidResult{};
}

// ============================================================================
// CPU interface initialization
// ============================================================================

/// Initialize the per-CPU interrupt interface.
/// GICv2: MMIO writes to GICC registers.
/// GICv3: ICC system registers + GICR redistributor wakeup.
inline VoidResult init_cpu_interface(VirtAddr cpu_base) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (g_gic_version == GicVersion::BCM2836) {
    VirtAddr local = platform::intc_dist_base();
    u32 cpu = bcm2836::core_id();
    write_reg(local, bcm2836::TIMER_CONTROL + cpu * bcm2836::CORE_STRIDE, 0);
    write_reg(local, bcm2836::MAILBOX_CONTROL + cpu * bcm2836::CORE_STRIDE, 0);
    write_reg(local, bcm2836::MAILBOX_CLEAR + cpu * bcm2836::MAILBOX_STRIDE, ~0U);
    bcm2836::ipi_masks[arch::get_current_cpu_id()] = 0;
    return VoidResult{};
  }
  if (g_gic_version == GicVersion::GICv3) {
    // 1. Enable SRE at EL1 (should already be set from EL2 setup in start_arm64.S)
    u32 sre = icc::read_sre();
    sre |= 0x7; // SRE | DFB | DIB
    icc::write_sre(sre);
    asm volatile("isb" ::: "memory");

    // 2. PMR=0xff admits priorities numerically below 255, including default 128.
    icc::write_pmr(0xFF);

    // 3. BPR=0 minimizes the binary-point split (subject to implemented bits).
    icc::write_bpr1(0);

    // 4. Enable Group 1 interrupts
    icc::write_igrpen1(1);
    asm volatile("isb" ::: "memory");

    // 5. Wake and configure this CPU's redistributor
    VirtAddr my_redist = find_my_redist_frame();
    if (!my_redist) {
      return VoidResult{ErrorCode::InvalidArgument};
    }
    wake_redistributor(my_redist);

    // 6. Set SGI/PPI (IRQ 0-31) to Group 1 NS in the redistributor
    write_reg(my_redist, redist_regs::IGROUPR0, 0xFFFFFFFF);

    // 7. Set default priority for SGI/PPI (0-31) in redistributor
    for (u32 i = 0; i < 32; i += 4) {
      write_reg(my_redist, redist_regs::IPRIORITYR0 + i, 0x80808080);
    }

    (void)cpu_base;
  } else {
    // GICv2: banked ITARGETSR identifies this CPU independently of MPIDR.
    g_gic_cpu_masks[arch::get_current_cpu_id()] = read_reg(platform::intc_dist_base(), dist_regs::ITARGETSR) & 0xFF;
    // GICv2: MMIO CPU interface
    write_reg(cpu_base, cpu_regs::PMR, 0xFF);
    write_reg(cpu_base, cpu_regs::CTLR, 1);
  }

#elif defined(MOSS_ARCH_X64)
  // cpu_base is the discovered I/O APIC; the Local APIC has its own resource.
  (void)cpu_base;
  const VirtAddr LAPIC_BASE = platform::intc_dist_base();

  // Mask all LVT entries to prevent spurious interrupts before proper setup
  write_reg(LAPIC_BASE, cpu_regs::LVT_TIMER, cpu_regs::LVT_MASK);
  write_reg(LAPIC_BASE, cpu_regs::LVT_LINT0, cpu_regs::LVT_MASK);
  write_reg(LAPIC_BASE, cpu_regs::LVT_LINT1, cpu_regs::LVT_MASK);
  write_reg(LAPIC_BASE, cpu_regs::LVT_ERROR, cpu_regs::LVT_MASK);

  // Enable Local APIC via SVR with spurious vector 0xFF
  u32 svr = read_reg(LAPIC_BASE, cpu_regs::SVR);
  svr |= 0x100; // APIC Enable bit
  svr |= 0xFF;  // Spurious vector = 255
  write_reg(LAPIC_BASE, cpu_regs::SVR, svr);

#elif defined(MOSS_ARCH_RISCV64)
  // PLIC: set threshold to 0 (accept all priorities)
  write_reg(cpu_base, 0, 0); // threshold register at context base
#endif

  return VoidResult{};
}

// ============================================================================
// IRQ enable / disable
// ============================================================================

/// Enable a specific interrupt line.
/// GICv3: SGI/PPI (irq < 32) use GICR, SPI (irq >= 32) use GICD.
inline void enable_irq(VirtAddr dist_base, u32 irq) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (g_gic_version == GicVersion::BCM2836) {
    bcm2836::set_enabled(dist_base, irq, true);
    return;
  }
  if (g_gic_version == GicVersion::GICv3 && irq < 32) {
    VirtAddr my_redist = find_my_redist_frame();
    if (!my_redist) {
      return;
    }
    write_reg(my_redist, redist_regs::ISENABLER0, 1U << irq);
    return;
  }
  // SPI (irq >= 32) or GICv2: use GICD (same register layout)
  u32 reg_offset = dist_regs::ISENABLER + (irq / 32) * 4;
  write_reg(dist_base, reg_offset, 1U << (irq % 32));

#elif defined(MOSS_ARCH_X64)
  // I/O APIC: unmask redirection table entry for the given IRQ line
  {
    const VirtAddr IOAPIC_BASE = platform::intc_cpu_base();
    auto *ioregsel = reinterpret_cast<volatile u32 *>(IOAPIC_BASE);
    auto *iowin = reinterpret_cast<volatile u32 *>(IOAPIC_BASE + 0x10);
    u32 gsi = irq < 16 ? platform::hardware.isa_gsi[irq] : irq;
    u32 reg_low = 0x10 + gsi * 2;
    *ioregsel = reg_low;
    u32 val = *iowin;
    val &= ~(1U << 16); // clear mask bit
    *ioregsel = reg_low;
    *iowin = val;
  }
  (void)dist_base;

#elif defined(MOSS_ARCH_RISCV64)
  // PLIC enable bitmaps reserve 0x80 bytes per context (32 words * 32 IRQs).
  // Use the current CPU's firmware-described supervisor context and select its
  // word/bit by irq/32 and irq%32; the 4-byte stride is the MMIO register width.
  u32 reg_offset =
      dist_regs::ENABLE_BASE + platform::hardware.plic_contexts[arch::get_current_cpu_id()] * 0x80 + (irq / 32) * 4;
  u32 val = read_reg(dist_base, reg_offset);
  val |= (1U << (irq % 32));
  write_reg(dist_base, reg_offset, val);
#endif
}

/// Disable a specific interrupt line.
inline void disable_irq(VirtAddr dist_base, u32 irq) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (g_gic_version == GicVersion::BCM2836) {
    bcm2836::set_enabled(dist_base, irq, false);
    return;
  }
  if (g_gic_version == GicVersion::GICv3 && irq < 32) {
    VirtAddr my_redist = find_my_redist_frame();
    if (!my_redist) {
      return;
    }
    write_reg(my_redist, redist_regs::ICENABLER0, 1U << irq);
    return;
  }
  u32 reg_offset = dist_regs::ICENABLER + (irq / 32) * 4;
  write_reg(dist_base, reg_offset, 1U << (irq % 32));

#elif defined(MOSS_ARCH_X64)
  // I/O APIC: mask redirection table entry for the given IRQ line
  {
    const VirtAddr IOAPIC_BASE = platform::intc_cpu_base();
    auto *ioregsel = reinterpret_cast<volatile u32 *>(IOAPIC_BASE);
    auto *iowin = reinterpret_cast<volatile u32 *>(IOAPIC_BASE + 0x10);
    u32 gsi = irq < 16 ? platform::hardware.isa_gsi[irq] : irq;
    u32 reg_low = 0x10 + gsi * 2;
    *ioregsel = reg_low;
    u32 val = *iowin;
    val |= (1U << 16); // set mask bit
    *ioregsel = reg_low;
    *iowin = val;
  }
  (void)dist_base;

#elif defined(MOSS_ARCH_RISCV64)
  // PLIC: clear enable bit
  u32 reg_offset =
      dist_regs::ENABLE_BASE + platform::hardware.plic_contexts[arch::get_current_cpu_id()] * 0x80 + (irq / 32) * 4;
  u32 val = read_reg(dist_base, reg_offset);
  val &= ~(1U << (irq % 32));
  write_reg(dist_base, reg_offset, val);
#endif
}

// ============================================================================
// Interrupt acknowledge / end-of-interrupt
// ============================================================================

/// Acknowledge an interrupt: read the IRQ number from the controller.
/// Returns the raw acknowledge register value (contains IRQ ID + source info).
[[nodiscard]] inline u32 ack_irq(VirtAddr cpu_base) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (g_gic_version == GicVersion::BCM2836) {
    return bcm2836::acknowledge();
  }
  if (g_gic_version == GicVersion::GICv3) {
    return icc::read_iar1();
  }
  return read_reg(cpu_base, cpu_regs::IAR);

#elif defined(MOSS_ARCH_X64)
  // APIC: interrupt vector is delivered via IDT, not read from a register.
  // The vector number is passed by the CPU hardware to the ISR.
  (void)cpu_base;
  return 0;

#elif defined(MOSS_ARCH_RISCV64)
  // PLIC: claim the highest-priority pending interrupt
  return read_reg(cpu_base, 4); // claim register at context base + 4
#endif
}

/// Extract the IRQ number from the raw acknowledge value.
[[nodiscard]] inline u32 irq_from_ack(u32 ack_value) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (g_gic_version == GicVersion::GICv3) {
    return ack_value & 0xFFFFFF; // GICv3: bits [23:0] (supports LPI)
  }
  return ack_value & 0x3FF; // GICv2: bits [9:0]
#elif defined(MOSS_ARCH_X64)
  return ack_value; // APIC: vector number directly
#elif defined(MOSS_ARCH_RISCV64)
  return ack_value; // PLIC: source ID directly
#endif
}

/// Signal end-of-interrupt to the controller.
inline void eoi(VirtAddr cpu_base, u32 ack_value) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (g_gic_version == GicVersion::BCM2836) {
    return; // Mailboxes clear at acknowledge; timer/UART handlers deassert their level sources.
  }
  if (g_gic_version == GicVersion::GICv3) {
    icc::write_eoir1(ack_value);
    return;
  }
  write_reg(cpu_base, cpu_regs::EOIR, ack_value);

#elif defined(MOSS_ARCH_X64)
  // Local APIC EOI: write any value to EOI register
  write_reg(cpu_base, cpu_regs::EOI, 0);
  (void)ack_value;

#elif defined(MOSS_ARCH_RISCV64)
  // PLIC: complete by writing the source ID back
  write_reg(cpu_base, 4, ack_value); // complete register at context base + 4
#endif
}

// ============================================================================
// Priority and target configuration
// ============================================================================

/// Set the priority of a specific interrupt.
/// IPRIORITYR registers are byte-accessible (4 IRQs per 32-bit register).
/// We must read-modify-write to avoid corrupting adjacent IRQ priorities.
inline void set_priority(VirtAddr dist_base, u32 irq, u8 priority) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (g_gic_version == GicVersion::BCM2836) {
    return; // BCM2836 has no programmable interrupt priorities.
  }
  // For GICv3, SGI/PPI priorities are in the redistributor, but we use
  // the same offset calculation — the base differs for irq < 32.
  VirtAddr base = dist_base;
  u32 reg_base = dist_regs::IPRIORITYR;
  if (g_gic_version == GicVersion::GICv3 && irq < 32) {
    base = find_my_redist_frame();
    if (!base) {
      return;
    }
    reg_base = redist_regs::IPRIORITYR0;
    // irq offset within the GICR IPRIORITYR is the same (byte per IRQ)
  }
  u32 reg_offset = reg_base + (irq & ~3U);
  u32 byte_shift = (irq & 3U) * 8;
  u32 val = read_reg(base, reg_offset);
  val &= ~(0xFFU << byte_shift);
  val |= (static_cast<u32>(priority) << byte_shift);
  write_reg(base, reg_offset, val);

#elif defined(MOSS_ARCH_X64)
  // APIC: priority is embedded in the vector number (upper 4 bits)
  (void)dist_base;
  (void)irq;
  (void)priority;

#elif defined(MOSS_ARCH_RISCV64)
  // PLIC: write priority register for source
  write_reg(dist_base, dist_regs::PRIORITY_BASE + irq * 4, priority);
#endif
}

/// Set the CPU target mask for a specific interrupt (SPIs only).
/// GICv2: ITARGETSR with 8-bit per-IRQ CPU mask.
/// GICv3: IROUTER with 64-bit affinity routing per SPI.
inline void set_target(VirtAddr dist_base, u32 irq, u32 cpu_mask) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (g_gic_version == GicVersion::BCM2836) {
    return; // Per-IRQ routing is unsupported; the shared cascade stays on the boot CPU.
  }
  if (g_gic_version == GicVersion::GICv3) {
    if (irq < 32) {
      return; // SGI/PPI have no target routing in GICv3
    }
    // Route to the lowest-numbered CPU in the mask
    u32 target_cpu = static_cast<u32>(intrinsics::bitops::ctz(cpu_mask));
    // IROUTER uses the target CPU's full hardware affinity.
    u32 irouter_off = dist_regs::IROUTER + (irq - 32) * 8;
    u64 affinity = platform::hardware.cpus[target_cpu].hardware_id;
    write_reg(dist_base, irouter_off, static_cast<u32>(affinity));
    write_reg(dist_base, irouter_off + 4, static_cast<u32>(affinity >> 32));
    return;
  }
  u32 physical_mask = 0;
  for (u32 cpu = 0; cpu < platform::hardware.cpu_count; ++cpu) {
    if (cpu_mask & (1U << cpu)) {
      physical_mask |= g_gic_cpu_masks[cpu];
    }
  }
  // GICv2: ITARGETSR
  u32 reg_offset = dist_regs::ITARGETSR + (irq & ~3U);
  u32 byte_shift = (irq & 3U) * 8;
  u32 val = read_reg(dist_base, reg_offset);
  val &= ~(0xFFU << byte_shift);
  val |= ((physical_mask & 0xFFU) << byte_shift);
  write_reg(dist_base, reg_offset, val);

#elif defined(MOSS_ARCH_X64)
  // I/O APIC: destination field in redirection table entry
  (void)dist_base;
  (void)irq;
  (void)cpu_mask;

#elif defined(MOSS_ARCH_RISCV64)
  // PLIC: enable bits per context control routing
  (void)dist_base;
  (void)irq;
  (void)cpu_mask;
#endif
}

/// Set the CPU priority mask using the controller's native priority convention.
/// GIC admits priorities below PMR; PLIC admits priorities above its threshold.
inline void set_priority_mask(VirtAddr cpu_base, u8 mask) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (g_gic_version == GicVersion::BCM2836) {
    return; // IRQ masking uses per-source controls or architectural DAIF, not a priority threshold.
  }
  if (g_gic_version == GicVersion::GICv3) {
    icc::write_pmr(mask);
    return;
  }
  write_reg(cpu_base, cpu_regs::PMR, mask);

#elif defined(MOSS_ARCH_X64)
  // Local APIC: Task Priority Register
  write_reg(cpu_base, cpu_regs::TPR, mask);

#elif defined(MOSS_ARCH_RISCV64)
  // PLIC: threshold register
  write_reg(cpu_base, 0, mask);
#endif
}

// ============================================================================
// Software Generated Interrupt (SGI) / Inter-Processor Interrupt (IPI)
// ============================================================================

/// Send a software-generated interrupt (IPI) to target CPUs.
/// ARM64 GICv2: sgi_id 0-15, target_cpu_mask = 8-bit CPU bitmask via GICD_SGIR.
/// ARM64 GICv3: logical CPU bits are translated into affinity and TargetList.
/// x64 APIC: sends IPI via ICR register.
/// RISC-V 64: software interrupts via the SBI IPI extension.
inline VoidResult send_sgi(VirtAddr dist_base, [[maybe_unused]] VirtAddr cpu_base, u32 sgi_id,
                           u32 target_cpu_mask) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (sgi_id >= 16) {
    return VoidResult{ErrorCode::InvalidParameter};
  }

  if (g_gic_version == GicVersion::BCM2836) {
    // Publish shared data before setting a mailbox bit. Hardware set/clear
    // aliases coalesce repeated IPIs of the same type without overwriting others.
    asm volatile("dmb ishst" ::: "memory");
    for (u32 cpu = 0; cpu < platform::hardware.cpu_count; ++cpu) {
      if (target_cpu_mask & (1U << cpu)) {
        u32 physical = static_cast<u32>(platform::hardware.cpus[cpu].hardware_id);
        write_reg(dist_base, bcm2836::MAILBOX_SET + physical * bcm2836::MAILBOX_STRIDE, 1U << sgi_id);
      }
    }
    return VoidResult{};
  }

  if (g_gic_version == GicVersion::GICv3) {
    // ICC_SGI1R_EL1 format:
    //   bits [15:0]  = TargetList (one bit per Aff0 value 0-15)
    //   bits [23:16] = Aff1
    //   bits [27:24] = INTID = sgi_id
    //   bits [39:32] = Aff2
    //   bit  [40]    = IRM = 0 (use target list)
    //   bits [47:44] = RS (range selector) = 0
    //   bits [55:48] = Aff3
    for (u32 cpu = 0; cpu < platform::hardware.cpu_count; ++cpu) {
      if (!(target_cpu_mask & (1U << cpu))) {
        continue;
      }
      u64 id = platform::hardware.cpus[cpu].hardware_id;
      u64 sgi1r = (static_cast<u64>(sgi_id) << 24) | (1ULL << (id & 15)) | ((id & 0xFF00) << 8) |
                  ((id & 0xFF0000) << 16) | ((id & 0xFF00000000) << 16);
      icc::write_sgi1r(sgi1r);
    }
    asm volatile("isb" ::: "memory");
  } else {
    u32 physical_mask = 0;
    for (u32 cpu = 0; cpu < platform::hardware.cpu_count; ++cpu) {
      if (target_cpu_mask & (1U << cpu)) {
        physical_mask |= g_gic_cpu_masks[cpu];
      }
    }
    u32 sgir_value = sgi_id | (physical_mask << 16);
    write_reg(dist_base, dist_regs::SGIR, sgir_value);
  }

#elif defined(MOSS_ARCH_X64)
  // Local APIC ICR: send fixed IPI
  // ICR high: destination APIC ID
  // ICR low: vector | delivery mode
  (void)dist_base;
  if (target_cpu_mask != 0) {
    // Find first target CPU from mask
    u32 target = static_cast<u32>(intrinsics::bitops::ctz(target_cpu_mask));
    u32 dest_apic_id = static_cast<u32>(platform::hardware.cpus[target].hardware_id);
    // ICR destination is bits 31:24; fixed IPI vectors use 64+SGI, above the
    // exception/legacy IRQ region. Bit 14 requests level assert delivery.
    write_reg(dist_base, cpu_regs::ICR_HIGH, dest_apic_id << 24);
    write_reg(dist_base, cpu_regs::ICR_LOW, (64 + sgi_id) | (1U << 14));
  }

#elif defined(MOSS_ARCH_RISCV64)
  // SBI v0.2+: EID 0x735049 (sPI), FID 0=sbi_send_ipi. A mask of one with
  // hart_mask_base=hardware ID targets one hart even when IDs are sparse;
  // passing the logical CPU mask directly would notify the wrong physical harts.
  (void)dist_base;
  (void)cpu_base;
  (void)sgi_id;
  for (u32 cpu = 0; cpu < platform::hardware.cpu_count; ++cpu) {
    if (!(target_cpu_mask & (1U << cpu))) {
      continue;
    }
    register u64 a0 asm("a0") = 1;
    register u64 a1 asm("a1") = arch::riscv64_hart_id(cpu);
    register u64 a6 asm("a6") = 0;
    register u64 a7 asm("a7") = 0x735049;
    asm volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory");
    if (a0 != 0) {
      return VoidResult{ErrorCode::InvalidState};
    }
  }
#endif

  return VoidResult{};
}

// ============================================================================
// Query: is an IRQ spurious?
// ============================================================================

/// Check if the acknowledged IRQ number indicates a spurious interrupt.
[[nodiscard]] inline bool is_spurious(u32 irq_num) noexcept {
#if defined(MOSS_ARCH_ARM64)
  return irq_num >= SPURIOUS_IRQ_THRESHOLD; // Both GICv2 and GICv3: 1020-1023
#elif defined(MOSS_ARCH_X64)
  return irq_num == SPURIOUS_IRQ_THRESHOLD; // APIC spurious vector
#elif defined(MOSS_ARCH_RISCV64)
  return irq_num == 0; // PLIC: claim=0 means no pending interrupt
#endif
}

} // namespace moss::kernel::hal::intc
