// MOSS Interrupt Controller Hardware Abstraction Layer
//
// Provides architecture-specific interrupt controller register operations.
//
// What lives here (architecture-specific):
//   - GIC register offsets (ARM64: GICv2/GICv3)
//   - APIC register constants (x86_64) [placeholder]
//   - PLIC register constants (RISC-V) [placeholder]
//   - Low-level init, enable/disable IRQ, ack/eoi, send SGI/IPI
//
// What stays in interrupts.cppm (architecture-independent):
//   - InterruptDescriptor, InterruptHandler, IRQ routing table
//   - GenericInterruptController high-level class (uses IntcHal ops)
//   - IPI subsystems (simple, hw_simple)

module;

#include "arch_detect.h"

export module moss.hal.intc;

import moss.std;
import moss.types;
import moss.result;
import moss.platform;

export namespace moss::kernel::hal::intc {

using moss::u8;
using moss::u32;
using moss::u64;
using moss::kernel::VirtAddr;
using moss::kernel::VoidResult;
using moss::kernel::ErrorCode;

// ============================================================================
// Interrupt controller register offsets — architecture-specific
// ============================================================================

#if defined(MOSS_ARCH_ARM64)
// GICv2 Distributor registers (offset from distributor base)
namespace DistRegs {
inline constexpr u32 CTLR       = 0x000;  // Distributor Control
inline constexpr u32 TYPER      = 0x004;  // Interrupt Controller Type
inline constexpr u32 IIDR       = 0x008;  // Distributor Implementer ID
inline constexpr u32 IGROUPR    = 0x080;  // Interrupt Group (base)
inline constexpr u32 ISENABLER  = 0x100;  // Interrupt Set-Enable (base)
inline constexpr u32 ICENABLER  = 0x180;  // Interrupt Clear-Enable (base)
inline constexpr u32 ISPENDR    = 0x200;  // Interrupt Set-Pending (base)
inline constexpr u32 ICPENDR    = 0x280;  // Interrupt Clear-Pending (base)
inline constexpr u32 ISACTIVER  = 0x300;  // Interrupt Set-Active (base)
inline constexpr u32 ICACTIVER  = 0x380;  // Interrupt Clear-Active (base)
inline constexpr u32 IPRIORITYR = 0x400;  // Interrupt Priority (base)
inline constexpr u32 ITARGETSR  = 0x800;  // Interrupt Processor Targets (base)
inline constexpr u32 ICFGR      = 0xC00;  // Interrupt Configuration (base)
inline constexpr u32 SGIR       = 0xF00;  // Software Generated Interrupt
} // namespace DistRegs

// GICv2 CPU Interface registers (offset from CPU interface base)
namespace CpuRegs {
inline constexpr u32 CTLR  = 0x000;  // CPU Interface Control
inline constexpr u32 PMR   = 0x004;  // Priority Mask
inline constexpr u32 BPR   = 0x008;  // Binary Point
inline constexpr u32 IAR   = 0x00C;  // Interrupt Acknowledge
inline constexpr u32 EOIR  = 0x010;  // End of Interrupt
inline constexpr u32 RPR   = 0x014;  // Running Priority
inline constexpr u32 HPPIR = 0x018;  // Highest Priority Pending Interrupt
} // namespace CpuRegs

#elif defined(MOSS_ARCH_X86_64)
// x86_64 Local APIC registers (MMIO offsets from APIC base, or MSR addresses)
namespace DistRegs {
// Placeholder — I/O APIC registers for SPI routing
inline constexpr u32 IOREGSEL = 0x00;  // I/O Register Select
inline constexpr u32 IOWIN    = 0x10;  // I/O Window
} // namespace DistRegs

namespace CpuRegs {
// Local APIC MMIO offsets
inline constexpr u32 ID       = 0x020;  // Local APIC ID
inline constexpr u32 VERSION  = 0x030;  // Local APIC Version
inline constexpr u32 TPR      = 0x080;  // Task Priority
inline constexpr u32 EOI      = 0x0B0;  // End of Interrupt
inline constexpr u32 SVR      = 0x0F0;  // Spurious Interrupt Vector
inline constexpr u32 ICR_LOW  = 0x300;  // Interrupt Command (low 32 bits)
inline constexpr u32 ICR_HIGH = 0x310;  // Interrupt Command (high 32 bits)
} // namespace CpuRegs

#elif defined(MOSS_ARCH_RISCV)
// RISC-V PLIC registers (Platform-Level Interrupt Controller)
namespace DistRegs {
inline constexpr u32 PRIORITY_BASE = 0x000000;  // Priority for each source
inline constexpr u32 PENDING_BASE  = 0x001000;  // Pending bits
inline constexpr u32 ENABLE_BASE   = 0x002000;  // Enable bits per context
} // namespace DistRegs

namespace CpuRegs {
// PLIC per-hart context (context = hart_id * 2 + 1 for M-mode)
inline constexpr u32 THRESHOLD_OFFSET = 0x200000;  // Priority threshold
inline constexpr u32 CLAIM_OFFSET     = 0x200004;  // Claim/Complete
} // namespace CpuRegs
#endif

// ============================================================================
// Spurious interrupt threshold — architecture-specific
// ============================================================================
#if defined(MOSS_ARCH_ARM64)
inline constexpr u32 SPURIOUS_IRQ_THRESHOLD = 1020;
#elif defined(MOSS_ARCH_X86_64)
inline constexpr u32 SPURIOUS_IRQ_THRESHOLD = 0xFF;  // APIC spurious vector
#elif defined(MOSS_ARCH_RISCV)
inline constexpr u32 SPURIOUS_IRQ_THRESHOLD = 0;     // PLIC: claim=0 means no pending
#endif

// ============================================================================
// Low-level MMIO register access
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
// Interrupt controller configuration query
// ============================================================================

/// Read the maximum number of supported interrupts from the controller.
[[nodiscard]] inline u32 read_max_interrupts(VirtAddr dist_base) noexcept {
#if defined(MOSS_ARCH_ARM64)
  u32 typer = read_reg(dist_base, DistRegs::TYPER);
  return ((typer & 0x1F) + 1) * 32;
#elif defined(MOSS_ARCH_X86_64)
  (void)dist_base;
  return 256;  // x86 supports up to 256 interrupt vectors
#elif defined(MOSS_ARCH_RISCV)
  (void)dist_base;
  return 1024; // PLIC can support up to 1024 sources
#endif
}

/// Read the maximum number of supported CPUs from the controller.
[[nodiscard]] inline u32 read_max_cpus(VirtAddr dist_base) noexcept {
#if defined(MOSS_ARCH_ARM64)
  u32 typer = read_reg(dist_base, DistRegs::TYPER);
  return ((typer >> 5) & 0x7) + 1;
#elif defined(MOSS_ARCH_X86_64)
  (void)dist_base;
  return 256;  // APIC ID space
#elif defined(MOSS_ARCH_RISCV)
  (void)dist_base;
  return 64;   // Typical PLIC hart limit
#endif
}

// ============================================================================
// Distributor initialization
// ============================================================================

/// Initialize the interrupt distributor/router.
/// Disables all interrupts, clears pending, sets default priority and targets.
inline VoidResult init_distributor(VirtAddr dist_base,
                                   u32 max_interrupts) noexcept {
#if defined(MOSS_ARCH_ARM64)
  // Disable distributor
  write_reg(dist_base, DistRegs::CTLR, 0);

  // Clear all enables
  for (u32 i = 0; i < max_interrupts; i += 32) {
    write_reg(dist_base, DistRegs::ICENABLER + i / 8, 0xFFFFFFFF);
  }

  // Clear all pending
  for (u32 i = 0; i < max_interrupts; i += 32) {
    write_reg(dist_base, DistRegs::ICPENDR + i / 8, 0xFFFFFFFF);
  }

  // Set default priority (0x80 = medium)
  for (u32 i = 0; i < max_interrupts; i += 4) {
    write_reg(dist_base, DistRegs::IPRIORITYR + i, 0x80808080);
  }

  // Route all SPIs to CPU 0
  for (u32 i = 32; i < max_interrupts; i += 4) {
    write_reg(dist_base, DistRegs::ITARGETSR + i, 0x01010101);
  }

  // Enable distributor
  write_reg(dist_base, DistRegs::CTLR, 1);

#elif defined(MOSS_ARCH_X86_64)
  // I/O APIC initialization placeholder
  (void)dist_base;
  (void)max_interrupts;

#elif defined(MOSS_ARCH_RISCV)
  // PLIC: set all priorities to 0 (disabled)
  for (u32 i = 1; i <= max_interrupts && i <= 1024; i++) {
    write_reg(dist_base, DistRegs::PRIORITY_BASE + i * 4, 0);
  }
  (void)max_interrupts;
#endif

  return VoidResult{};
}

// ============================================================================
// CPU interface initialization
// ============================================================================

/// Initialize the per-CPU interrupt interface.
inline VoidResult init_cpu_interface(VirtAddr cpu_base) noexcept {
#if defined(MOSS_ARCH_ARM64)
  // Accept all priorities
  write_reg(cpu_base, CpuRegs::PMR, 0xFF);
  // Enable CPU interface
  write_reg(cpu_base, CpuRegs::CTLR, 1);

#elif defined(MOSS_ARCH_X86_64)
  // Local APIC: enable via SVR
  u32 svr = read_reg(cpu_base, CpuRegs::SVR);
  svr |= 0x100;  // APIC Enable bit
  svr |= 0xFF;   // Spurious vector
  write_reg(cpu_base, CpuRegs::SVR, svr);

#elif defined(MOSS_ARCH_RISCV)
  // PLIC: set threshold to 0 (accept all priorities)
  write_reg(cpu_base, 0, 0); // threshold register at context base
#endif

  return VoidResult{};
}

// ============================================================================
// IRQ enable / disable
// ============================================================================

/// Enable a specific interrupt line.
inline void enable_irq(VirtAddr dist_base, u32 irq) noexcept {
#if defined(MOSS_ARCH_ARM64)
  u32 reg_offset = DistRegs::ISENABLER + (irq / 32) * 4;
  write_reg(dist_base, reg_offset, 1U << (irq % 32));

#elif defined(MOSS_ARCH_X86_64)
  // I/O APIC: unmask redirection table entry
  (void)dist_base;
  (void)irq;

#elif defined(MOSS_ARCH_RISCV)
  // PLIC: set enable bit for context 1 (S-mode, hart 0)
  u32 reg_offset = DistRegs::ENABLE_BASE + 0x80 + (irq / 32) * 4;
  u32 val = read_reg(dist_base, reg_offset);
  val |= (1U << (irq % 32));
  write_reg(dist_base, reg_offset, val);
#endif
}

/// Disable a specific interrupt line.
inline void disable_irq(VirtAddr dist_base, u32 irq) noexcept {
#if defined(MOSS_ARCH_ARM64)
  u32 reg_offset = DistRegs::ICENABLER + (irq / 32) * 4;
  write_reg(dist_base, reg_offset, 1U << (irq % 32));

#elif defined(MOSS_ARCH_X86_64)
  // I/O APIC: mask redirection table entry
  (void)dist_base;
  (void)irq;

#elif defined(MOSS_ARCH_RISCV)
  // PLIC: clear enable bit
  u32 reg_offset = DistRegs::ENABLE_BASE + 0x80 + (irq / 32) * 4;
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
  return read_reg(cpu_base, CpuRegs::IAR);

#elif defined(MOSS_ARCH_X86_64)
  // APIC: interrupt vector is delivered via IDT, not read from a register.
  // The vector number is passed by the CPU hardware to the ISR.
  (void)cpu_base;
  return 0;

#elif defined(MOSS_ARCH_RISCV)
  // PLIC: claim the highest-priority pending interrupt
  return read_reg(cpu_base, 4); // claim register at context base + 4
#endif
}

/// Extract the IRQ number from the raw acknowledge value.
[[nodiscard]] inline u32 irq_from_ack(u32 ack_value) noexcept {
#if defined(MOSS_ARCH_ARM64)
  return ack_value & 0x3FF;  // GICv2: bits [9:0]
#elif defined(MOSS_ARCH_X86_64)
  return ack_value;  // APIC: vector number directly
#elif defined(MOSS_ARCH_RISCV)
  return ack_value;  // PLIC: source ID directly
#endif
}

/// Signal end-of-interrupt to the controller.
inline void eoi(VirtAddr cpu_base, u32 ack_value) noexcept {
#if defined(MOSS_ARCH_ARM64)
  write_reg(cpu_base, CpuRegs::EOIR, ack_value);

#elif defined(MOSS_ARCH_X86_64)
  // Local APIC EOI: write any value to EOI register
  write_reg(cpu_base, CpuRegs::EOI, 0);
  (void)ack_value;

#elif defined(MOSS_ARCH_RISCV)
  // PLIC: complete by writing the source ID back
  write_reg(cpu_base, 4, ack_value); // complete register at context base + 4
#endif
}

// ============================================================================
// Priority and target configuration
// ============================================================================

/// Set the priority of a specific interrupt.
inline void set_priority(VirtAddr dist_base, u32 irq, u8 priority) noexcept {
#if defined(MOSS_ARCH_ARM64)
  write_reg(dist_base, DistRegs::IPRIORITYR + irq, priority);

#elif defined(MOSS_ARCH_X86_64)
  // APIC: priority is embedded in the vector number (upper 4 bits)
  (void)dist_base;
  (void)irq;
  (void)priority;

#elif defined(MOSS_ARCH_RISCV)
  // PLIC: write priority register for source
  write_reg(dist_base, DistRegs::PRIORITY_BASE + irq * 4, priority);
#endif
}

/// Set the CPU target mask for a specific interrupt (SPIs only).
inline void set_target(VirtAddr dist_base, u32 irq, u32 cpu_mask) noexcept {
#if defined(MOSS_ARCH_ARM64)
  write_reg(dist_base, DistRegs::ITARGETSR + irq, cpu_mask);

#elif defined(MOSS_ARCH_X86_64)
  // I/O APIC: destination field in redirection table entry
  (void)dist_base;
  (void)irq;
  (void)cpu_mask;

#elif defined(MOSS_ARCH_RISCV)
  // PLIC: enable bits per context control routing
  (void)dist_base;
  (void)irq;
  (void)cpu_mask;
#endif
}

/// Set the CPU priority mask (minimum priority to deliver).
inline void set_priority_mask(VirtAddr cpu_base, u8 mask) noexcept {
#if defined(MOSS_ARCH_ARM64)
  write_reg(cpu_base, CpuRegs::PMR, mask);

#elif defined(MOSS_ARCH_X86_64)
  // Local APIC: Task Priority Register
  write_reg(cpu_base, CpuRegs::TPR, mask);

#elif defined(MOSS_ARCH_RISCV)
  // PLIC: threshold register
  write_reg(cpu_base, 0, mask);
#endif
}

// ============================================================================
// Software Generated Interrupt (SGI) / Inter-Processor Interrupt (IPI)
// ============================================================================

/// Send a software-generated interrupt (IPI) to target CPUs.
/// For ARM64 GICv2, sgi_id is 0-15 and target_cpu_mask selects destination CPUs.
/// For x86_64 APIC, this sends an IPI via the ICR register.
/// For RISC-V, software interrupts are triggered via SIP CSR.
inline VoidResult send_sgi(VirtAddr dist_base, [[maybe_unused]] VirtAddr cpu_base,
                           u32 sgi_id, u32 target_cpu_mask) noexcept {
#if defined(MOSS_ARCH_ARM64)
  if (sgi_id >= 16) {
    return VoidResult{ErrorCode::InvalidParameter};
  }
  u32 sgir_value = sgi_id | (target_cpu_mask << 16);
  write_reg(dist_base, DistRegs::SGIR, sgir_value);

#elif defined(MOSS_ARCH_X86_64)
  // Local APIC ICR: send fixed IPI
  // ICR high: destination APIC ID
  // ICR low: vector | delivery mode
  (void)dist_base;
  if (target_cpu_mask != 0) {
    // Find first target CPU from mask
    u32 dest_apic_id = static_cast<u32>(__builtin_ctz(target_cpu_mask));
    write_reg(cpu_base, CpuRegs::ICR_HIGH, dest_apic_id << 24);
    write_reg(cpu_base, CpuRegs::ICR_LOW, sgi_id | (1U << 14)); // Fixed delivery
  }

#elif defined(MOSS_ARCH_RISCV)
  // RISC-V: software interrupts via SBI or direct CSR write
  // Placeholder — SBI ecall for IPI
  (void)dist_base;
  (void)cpu_base;
  (void)sgi_id;
  (void)target_cpu_mask;
#endif

  return VoidResult{};
}

// ============================================================================
// Query: is an IRQ spurious?
// ============================================================================

/// Check if the acknowledged IRQ number indicates a spurious interrupt.
[[nodiscard]] inline bool is_spurious(u32 irq_num) noexcept {
#if defined(MOSS_ARCH_ARM64)
  return irq_num >= SPURIOUS_IRQ_THRESHOLD;  // GICv2: 1020-1023 are spurious
#elif defined(MOSS_ARCH_X86_64)
  return irq_num == SPURIOUS_IRQ_THRESHOLD;  // APIC spurious vector
#elif defined(MOSS_ARCH_RISCV)
  return irq_num == 0;  // PLIC: claim=0 means no pending interrupt
#endif
}

} // namespace moss::kernel::hal::intc
