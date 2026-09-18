// MOSS Interrupts Module - Interrupt Management for Kernel
// Provides GIC driver, IPI mechanisms, and interrupt handling infrastructure.

export module moss.interrupts;

import moss.intrinsics;
import moss.std;
import moss.types;
import moss.result;
import moss.arch;
import moss.platform;
import moss.hal.intc;
import moss.containers;
import moss.smart_ptr;
import moss.logging;

// ============================================================================
// Exported interrupt types and classes
// ============================================================================
export namespace moss::kernel::interrupts {

// Import necessary types from moss.types
using moss::kernel::u32;
using InterruptId = u32;

namespace log = moss::kernel::logging;

// ========================================================================
// GIC (Generic Interrupt Controller) Driver
// ========================================================================

// GIC version
enum class GicVersion : u8 { BCM2836 = 1, GICv2 = 2, GICv3 = 3, Unknown = 0 };

// Interrupt type
enum class InterruptType : u8 {
  SGI = 0, // Software Generated Interrupt (0-15)
  PPI = 1, // Private Peripheral Interrupt (16-31)
  SPI = 2, // Shared Peripheral Interrupt (32+)
  LPI = 3  // Locality-specific Peripheral Interrupt (GICv3)
};

// Interrupt trigger type
enum class TriggerType : u8 { EdgeRising = 0, LevelHigh = 1, EdgeFalling = 2, LevelLow = 3 };

// Interrupt priority
using InterruptPriority = u8;

// Interrupt handler function type
using InterruptHandler = void (*)(InterruptId irq, void *context);

// Interrupt descriptor
struct InterruptDescriptor {
  InterruptId irq;
  InterruptType type;
  TriggerType trigger;
  InterruptPriority priority;
  u32 target_cpu_mask;
  InterruptHandler handler;
  void *context;
  containers::AtomicU64 count;
  bool enabled;
  const char *name;

  // Priority 128 matches HAL's 0x80 default under PMR=0xff; target mask 1
  // selects logical boot CPU zero. These are software defaults, not firmware IDs.
  InterruptDescriptor() noexcept
      : irq(0), type(InterruptType::SPI), trigger(TriggerType::LevelHigh), priority(128), target_cpu_mask(1),
        handler(nullptr), context(nullptr), count{}, enabled(false), name(nullptr) {}

  InterruptDescriptor(InterruptId id, InterruptHandler h, void *ctx, const char *n) noexcept
      : irq(id), type(determine_type(id)), trigger(TriggerType::LevelHigh), priority(128), target_cpu_mask(1),
        handler(h), context(ctx), count{}, enabled(false), name(n) {}

private:
  static constexpr InterruptType determine_type(InterruptId id) noexcept {
    if (id < 16) {
      return InterruptType::SGI;
    }
    if (id < 32) {
      return InterruptType::PPI;
    }
    return InterruptType::SPI;
  }
};

// GIC register offsets are now provided by moss.hal.intc (DistRegs/CpuRegs).
// All GIC-specific register operations are delegated to the HAL layer.

// GIC driver main class
class GenericInterruptController {
private:
  GicVersion version_;
  VirtAddr distributor_base_;
  VirtAddr cpu_interface_base_; // GICv2: GICC base; GICv3: unused (0)
  VirtAddr redistributor_base_; // GICv3: GICR base; GICv2: unused (0)
  u32 max_interrupts_;
  u32 max_cpus_;

  containers::LockedHashMap<InterruptId, shared_ptr<InterruptDescriptor>> interrupt_table_;
  containers::IrqSpinLock table_write_lock_; // Protects interrupt_table_ mutations
  containers::PerCpuData<u64> interrupt_counts_;

  containers::AtomicCounter<u64> total_interrupts_;
  containers::AtomicCounter<u64> spurious_interrupts_;

public:
  GenericInterruptController() noexcept
      : version_(GicVersion::Unknown), distributor_base_(0), cpu_interface_base_(0), redistributor_base_(0),
        max_interrupts_(0), max_cpus_(0), total_interrupts_(0), spurious_interrupts_(0) {}

  ~GenericInterruptController() noexcept { cleanup(); }

  // Non-copyable, non-movable
  GenericInterruptController(const GenericInterruptController &) = delete;
  GenericInterruptController &operator=(const GenericInterruptController &) = delete;
  GenericInterruptController(GenericInterruptController &&) = delete;
  GenericInterruptController &operator=(GenericInterruptController &&) = delete;

  struct GicStats {
    u64 total_interrupts;
    u64 spurious_interrupts;
    u32 registered_interrupts;
    u32 enabled_interrupts;
  };

  /// Two-argument initialize (backward compatible, assumes GICv2)
  [[nodiscard]] VoidResult initialize(VirtAddr dist_base, VirtAddr cpu_base) noexcept {
    return initialize(dist_base, cpu_base, 2);
  }

  /// Three-argument initialize with GIC version hint from DTB.
  /// @param dist_base      GICD or BCM2836 ARM-local base address
  /// @param second_base    GICv2: GICC; GICv3: GICR; BCM2836: cascaded ARMCTRL
  /// @param gic_version_hint  Legacy tag: 1 for BCM2836, 2 for GICv2, 3 for GICv3.
  [[nodiscard]] VoidResult initialize(VirtAddr dist_base, VirtAddr second_base, u8 gic_version_hint) noexcept {
    distributor_base_ = dist_base;

    if (gic_version_hint == 3) {
      cpu_interface_base_ = 0;
      redistributor_base_ = second_base;
    } else {
      cpu_interface_base_ = second_base;
      redistributor_base_ = 0;
    }

    auto detect_result = detect_gic_config(gic_version_hint);
    if (!detect_result) {
      return detect_result;
    }

    auto dist_result = initialize_distributor();
    if (!dist_result) {
      return dist_result;
    }

    auto cpu_result = initialize_cpu_interface();
    if (!cpu_result) {
      return cpu_result;
    }

    return VoidResult{};
  }

  [[nodiscard]] VoidResult register_interrupt(InterruptId irq, InterruptHandler handler, void *context,
                                              const char *name = nullptr) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    if (handler == nullptr) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    containers::LockGuard<containers::IrqSpinLock> guard(table_write_lock_);

    if (static_cast<bool>(interrupt_table_.find(irq))) {
      return VoidResult{ErrorCode::AlreadyExists};
    }

    auto desc = make_shared<InterruptDescriptor>(irq, handler, context, name);
    if (!desc) {
      return VoidResult{ErrorCode::OutOfMemory};
    }

    interrupt_table_.insert_or_update(irq, desc);

    return VoidResult{};
  }

  [[nodiscard]] VoidResult unregister_interrupt(InterruptId irq) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(table_write_lock_);

    auto desc_ptr = interrupt_table_.find(irq);
    if (!desc_ptr) {
      return VoidResult{ErrorCode::NotFound};
    }
    auto desc = *desc_ptr;

    (void)disable_interrupt(irq);
    interrupt_table_.remove(irq);

    return VoidResult{};
  }

  [[nodiscard]] VoidResult enable_interrupt(InterruptId irq) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    ::moss::kernel::hal::intc::enable_irq(distributor_base_, irq);

    auto desc_ptr = interrupt_table_.find(irq);
    if (static_cast<bool>(desc_ptr)) {
      auto desc = *desc_ptr;
      desc->enabled = true;
    }

    return VoidResult{};
  }

  [[nodiscard]] VoidResult disable_interrupt(InterruptId irq) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    ::moss::kernel::hal::intc::disable_irq(distributor_base_, irq);

    auto desc_ptr = interrupt_table_.find(irq);
    if (static_cast<bool>(desc_ptr)) {
      auto desc = *desc_ptr;
      desc->enabled = false;
    }

    return VoidResult{};
  }

  [[nodiscard]] VoidResult set_interrupt_priority(InterruptId irq, InterruptPriority priority) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    ::moss::kernel::hal::intc::set_priority(distributor_base_, irq, priority);

    auto desc_ptr = interrupt_table_.find(irq);
    if (static_cast<bool>(desc_ptr)) {
      auto desc = *desc_ptr;
      desc->priority = priority;
    }

    return VoidResult{};
  }

  [[nodiscard]] VoidResult set_interrupt_target(InterruptId irq, u32 cpu_mask) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    if (irq < 32) {
      return VoidResult{ErrorCode::NotSupported};
    }

    if (version_ == GicVersion::BCM2836) {
      return VoidResult{ErrorCode::NotSupported}; // ARMCTRL routes its entire cascade, never individual IRQs.
    }

    ::moss::kernel::hal::intc::set_target(distributor_base_, irq, cpu_mask);

    auto desc_ptr = interrupt_table_.find(irq);
    if (static_cast<bool>(desc_ptr)) {
      auto desc = *desc_ptr;
      desc->target_cpu_mask = cpu_mask;
    }

    return VoidResult{};
  }

  [[nodiscard]] VoidResult send_sgi(InterruptId sgi, u32 target_cpu_mask) const noexcept {
    return ::moss::kernel::hal::intc::send_sgi(distributor_base_, cpu_interface_base_, sgi, target_cpu_mask);
  }

  void handle_interrupt() noexcept {
    namespace intc_hal = ::moss::kernel::hal::intc;
    u32 cpu = get_current_cpu_id();

    u32 ack_val = intc_hal::ack_irq(cpu_interface_base_);
    InterruptId irq = intc_hal::irq_from_ack(ack_val);

    if (intc_hal::is_spurious(irq)) {
      (void)spurious_interrupts_.fetch_add(1, containers::MemoryOrder::Relaxed);
      return;
    }

    (void)total_interrupts_.fetch_add(1, containers::MemoryOrder::Relaxed);
    interrupt_counts_.get_cpu(cpu)++;

    // Hold a shared descriptor through the callback so concurrent table removal
    // cannot free it; the callback context itself remains the registrant's responsibility.
    auto desc_ptr = interrupt_table_.find(irq);
    if (static_cast<bool>(desc_ptr)) {
      auto desc = *desc_ptr;
      if (desc->handler != nullptr) {
        desc->handler(irq, desc->context);
        (void)desc->count.fetch_add(1, containers::MemoryOrder::Relaxed);
      }
    }

    intc_hal::eoi(cpu_interface_base_, ack_val);
  }

  [[nodiscard]] GicStats get_statistics() const noexcept {
    u32 registered = 0;
    u32 enabled = 0;

    interrupt_table_.for_each([&registered, &enabled](const auto &entry) {
      registered++;
      if (entry.value->enabled) {
        enabled++;
      }
    });

    return {.total_interrupts = total_interrupts_.load(containers::MemoryOrder::Relaxed),
            .spurious_interrupts = spurious_interrupts_.load(containers::MemoryOrder::Relaxed),
            .registered_interrupts = registered,
            .enabled_interrupts = enabled};
  }

  [[nodiscard]] shared_ptr<InterruptDescriptor> get_interrupt_info(InterruptId irq) const noexcept {
    auto desc_ptr = interrupt_table_.find(irq);
    return desc_ptr ? *desc_ptr : shared_ptr<InterruptDescriptor>{};
  }

  void set_priority_mask(InterruptPriority mask) const noexcept {
    ::moss::kernel::hal::intc::set_priority_mask(cpu_interface_base_, mask);
  }

  [[nodiscard]] static u32 get_current_cpu_id() noexcept { return arch::get_current_cpu_id(); }

private:
  [[nodiscard]] VoidResult detect_gic_config(u8 gic_version_hint) noexcept {
    namespace hal = ::moss::kernel::hal::intc;

#if defined(MOSS_ARCH_ARM64)
    // Use DTB hint as the primary version source.
    // GICD_PIDR2 (offset 0xFFE8) is only reliably accessible on GICv3/v4
    // distributors; QEMU GICv2 does not map that offset and will abort.
    // So we only read PIDR2 as a sanity check when the hint says v3+.
    if (gic_version_hint == 1) {
      version_ = GicVersion::BCM2836;
      hal::g_gic_version = hal::GicVersion::BCM2836;
      log::klog::info("Interrupt controller: BCM2836 local + ARMCTRL");
    } else if (gic_version_hint >= 3) {
      u32 pidr2 = hal::read_reg(distributor_base_, hal::dist_regs::PIDR2);
      u8 arch_rev = static_cast<u8>((pidr2 >> 4) & 0xF);

      version_ = GicVersion::GICv3;
      hal::g_gic_version = hal::GicVersion::GICv3;
      hal::g_redist_base = redistributor_base_;
      log::klog::info("GIC: GICv3 detected (PIDR2 ArchRev={}, hint={}), GICR={:#x}", arch_rev, gic_version_hint,
                      redistributor_base_);
    } else {
      version_ = GicVersion::GICv2;
      hal::g_gic_version = hal::GicVersion::GICv2;
      log::klog::info("GIC: GICv2 (hint={})", gic_version_hint);
    }
#else
    version_ = GicVersion::GICv2;
    (void)gic_version_hint;
#endif

    max_interrupts_ = hal::read_max_interrupts(distributor_base_);
    max_cpus_ = hal::read_max_cpus(distributor_base_);
    return VoidResult{};
  }

  [[nodiscard]] VoidResult initialize_distributor() const noexcept {
    return ::moss::kernel::hal::intc::init_distributor(distributor_base_, max_interrupts_);
  }

  [[nodiscard]] VoidResult initialize_cpu_interface() const noexcept {
    return ::moss::kernel::hal::intc::init_cpu_interface(cpu_interface_base_);
  }

  // Thin wrappers over HAL register access (preserves call sites)
  [[nodiscard]] u32 read_distributor_reg(u32 offset) const noexcept {
    return ::moss::kernel::hal::intc::read_reg(distributor_base_, offset);
  }

  void write_distributor_reg(u32 offset, u32 value) const noexcept {
    ::moss::kernel::hal::intc::write_reg(distributor_base_, offset, value);
  }

  [[nodiscard]] u32 read_cpu_interface_reg(u32 offset) const noexcept {
    return ::moss::kernel::hal::intc::read_reg(cpu_interface_base_, offset);
  }

  void write_cpu_interface_reg(u32 offset, u32 value) const noexcept {
    ::moss::kernel::hal::intc::write_reg(cpu_interface_base_, offset, value);
  }

  void cleanup() noexcept { interrupt_table_.clear(); }
};

// Global GIC instance pointer
extern GenericInterruptController *g_gic;

// ========================================================================
// IPI SGI ID assignment (based on Linux kernel design)
// ========================================================================
// This is Moss's shared SGI-number contract (0..7), used by registration,
// send mapping and per-SGI statistics; enum order changes must update all three.
enum class IpiSgiId : u8 {
  Reschedule = 0,
  CallFunction = 1,
  CallFunctionSingle = 2,
  Timer = 3,
  Ping = 4,
  WakeUp = 5,
  Stop = 6,
  Debug = 7,
};

// ========================================================================
// Simplified IPI types (ipi_simple)
// ========================================================================
namespace simple {

enum class IpiType : u8 { Ping = 3 };

// Preserve existing result values; 1/4 distinguish invalid types and failed
// hardware requests so Success never merely means metadata was constructed.
enum class IpiResult : u8 { Success = 0, InvalidType = 1, InvalidCpu = 2, NotInitialized = 3, HardwareError = 4 };

struct IpiMessage {
  IpiType type;
  u32 source_cpu;
  u32 target_cpu;
  u64 sequence;
};

class SimpleInterProcessorInterrupt {
public:
  static constexpr u32 MAX_CPUS = moss::kernel::BOOT_MAX_CPUS;

  SimpleInterProcessorInterrupt() noexcept = default;
  ~SimpleInterProcessorInterrupt() noexcept = default;

  SimpleInterProcessorInterrupt(const SimpleInterProcessorInterrupt &) = delete;
  SimpleInterProcessorInterrupt &operator=(const SimpleInterProcessorInterrupt &) = delete;

  VoidResult initialize(u32 max_cpus) noexcept;
  IpiResult send_ipi(u32 target_cpu, IpiType type) noexcept;
  IpiResult ping_cpu(u32 target_cpu) noexcept;
  VoidResult self_test() noexcept;

  struct SystemInfo {
    bool initialized;
    u32 max_cpus;
    u64 total_pings_sent;
  };
  SystemInfo get_system_info() const noexcept;

private:
  bool initialized_ = false;
  u32 max_cpus_ = 0;
  containers::AtomicU64 total_pings_sent_;

  bool is_valid_cpu_id(u32 cpu_id) const noexcept;
  u32 get_current_cpu_id() const noexcept;
};

extern SimpleInterProcessorInterrupt *g_simple_ipi_manager;

VoidResult initialize_simple_ipi_system(u32 max_cpus) noexcept;
void shutdown_simple_ipi_system() noexcept;

inline IpiResult simple_ipi_ping(u32 target_cpu) noexcept {
  return g_simple_ipi_manager ? g_simple_ipi_manager->ping_cpu(target_cpu) : IpiResult::NotInitialized;
}

const char *ipi_type_to_string(IpiType type) noexcept;
const char *ipi_result_to_string(IpiResult result) noexcept;

} // namespace simple

// ========================================================================
// Simple Hardware IPI (ipi_hardware_simple)
// ========================================================================
namespace hw_simple {

enum class IpiType : u8 { Ping = 0, Reschedule = 1, CallFunction = 2, Stop = 3, WakeUp = 4, Timer = 5, Debug = 6 };

enum class IpiResult : u8 {
  Success = 0,
  InvalidCpu = 1,
  InvalidType = 2,
  NotInitialized = 3,
  HardwareError = 4,
  QueueFull = 5,
  Timeout = 6
};

constexpr IpiSgiId ipi_type_to_sgi(IpiType type) noexcept {
  switch (type) {
  case IpiType::Ping:
    return IpiSgiId::Ping;
  case IpiType::Reschedule:
    return IpiSgiId::Reschedule;
  case IpiType::CallFunction:
    return IpiSgiId::CallFunction;
  case IpiType::Stop:
    return IpiSgiId::Stop;
  case IpiType::WakeUp:
    return IpiSgiId::WakeUp;
  case IpiType::Timer:
    return IpiSgiId::Timer;
  case IpiType::Debug:
    return IpiSgiId::Debug;
  default:
    return IpiSgiId::Ping;
  }
}

class SimpleHardwareIpi {
private:
  GenericInterruptController *gic_;
  bool initialized_;
  u32 max_cpus_;
  containers::AtomicU64 total_ipis_sent_;
  containers::AtomicU64 message_sequence_;

  // Eight slots correspond to the IpiSgiId range 0..7 above.
  containers::AtomicU64 sgi_send_counts_[8];
  containers::AtomicU64 sgi_receive_counts_[8];

public:
  SimpleHardwareIpi() noexcept
      : gic_(nullptr), initialized_(false), max_cpus_(0), total_ipis_sent_{}, message_sequence_{}, sgi_send_counts_{},
        sgi_receive_counts_{} {
    // Constructor seed only; initialize() resets to 1 before use. The choice
    // of 1000 has no recorded rationale and is not a hardware protocol value.
    message_sequence_.store(1000, containers::MemoryOrder::Relaxed);
  }

  ~SimpleHardwareIpi() noexcept { shutdown(); }

  SimpleHardwareIpi(const SimpleHardwareIpi &) = delete;
  SimpleHardwareIpi &operator=(const SimpleHardwareIpi &) = delete;

  VoidResult initialize(GenericInterruptController *gic, u32 max_cpus) noexcept;
  void shutdown() noexcept;
  bool is_initialized() const noexcept { return initialized_; }

  IpiResult send_ipi(u32 target_cpu, IpiType type) noexcept;
  IpiResult ping_cpu(u32 target_cpu) noexcept;
  IpiResult ping_cpus(u32 cpu_mask) noexcept;
  IpiResult request_reschedule(u32 target_cpu) noexcept;
  IpiResult wakeup_cpu(u32 target_cpu) noexcept;

  struct Statistics {
    u64 total_sent;
    u64 ping_count;
    u64 reschedule_count;
    u32 max_cpus;
    bool initialized;
  };

  Statistics get_statistics() const noexcept;
  VoidResult self_test() noexcept;

private:
  VoidResult send_hardware_sgi(IpiSgiId sgi_id, u32 target_cpu_mask) noexcept;
  bool is_valid_cpu_id(u32 cpu_id) const noexcept;
  static u32 get_current_cpu_id() noexcept;
  u64 generate_sequence() noexcept;
};

extern SimpleHardwareIpi *g_simple_hardware_ipi;

VoidResult initialize_simple_hardware_ipi(GenericInterruptController *gic, u32 max_cpus) noexcept;
void shutdown_simple_hardware_ipi() noexcept;

const char *ipi_type_to_string(IpiType type) noexcept;
const char *ipi_result_to_string(IpiResult result) noexcept;
const char *sgi_id_to_string(IpiSgiId sgi) noexcept;

} // namespace hw_simple

// === Module-level variable definitions ===

// Global GIC instance
GenericInterruptController *g_gic = nullptr;

} // namespace moss::kernel::interrupts
