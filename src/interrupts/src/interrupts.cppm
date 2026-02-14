// MOSS Interrupts Module - Interrupt Management for Kernel
// Provides GIC driver, IPI mechanisms, and interrupt handling infrastructure.

module;

// Architecture detection macros (same as containers.cppm)
#ifndef MOSS_ARCH_ARM64
#ifndef MOSS_ARCH_X86_64
#ifndef MOSS_ARCH_RISCV
#if defined(__x86_64__) || defined(__x86_64) || defined(__amd64__) ||           \
    defined(__amd64) || defined(_M_X64)
#define MOSS_ARCH_X86_64
#elif defined(__aarch64__) || defined(_M_ARM64)
#define MOSS_ARCH_ARM64
#elif defined(__riscv) && __riscv_xlen == 64
#define MOSS_ARCH_RISCV
#else
#define MOSS_ARCH_X86_64
#endif
#endif
#endif
#endif

// External debug print function
extern "C" void early_debug_print(const char *message) noexcept;

export module moss.interrupts;

import moss.std;
import moss.types;
import moss.result;
import moss.containers;

// ============================================================================
// Internal (non-exported) arch helpers
// ============================================================================
namespace moss::kernel::arch {

inline void memory_barrier() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("dsb sy" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("mfence" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("fence iorw, iorw" ::: "memory");
#else
  asm volatile("" ::: "memory");
#endif
}

inline void cpu_yield() noexcept {
#if defined(MOSS_ARCH_ARM64)
  asm volatile("yield" ::: "memory");
#elif defined(MOSS_ARCH_X86_64)
  asm volatile("pause" ::: "memory");
#elif defined(MOSS_ARCH_RISCV)
  asm volatile("" ::: "memory");
#else
  asm volatile("" ::: "memory");
#endif
}

[[nodiscard]] inline u32 get_current_cpu_id() noexcept {
#if defined(MOSS_ARCH_ARM64)
  u64 mpidr;
  asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  return static_cast<u32>(mpidr & 0xFF);
#elif defined(MOSS_ARCH_X86_64)
  return 0;
#elif defined(MOSS_ARCH_RISCV)
  u64 hart_id;
  asm volatile("csrr %0, mhartid" : "=r"(hart_id));
  return static_cast<u32>(hart_id);
#else
  return 0;
#endif
}

} // namespace moss::kernel::arch

// ============================================================================
// Exported interrupt types and classes
// ============================================================================
export namespace moss::kernel::interrupts {

// ========================================================================
// GIC (Generic Interrupt Controller) Driver
// ========================================================================

// GIC version
enum class GicVersion : u8 { GICv2 = 2, GICv3 = 3, Unknown = 0 };

// Interrupt type
enum class InterruptType : u8 {
  SGI = 0, // Software Generated Interrupt (0-15)
  PPI = 1, // Private Peripheral Interrupt (16-31)
  SPI = 2, // Shared Peripheral Interrupt (32+)
  LPI = 3  // Locality-specific Peripheral Interrupt (GICv3)
};

// Interrupt trigger type
enum class TriggerType : u8 {
  EdgeRising = 0,
  LevelHigh = 1,
  EdgeFalling = 2,
  LevelLow = 3
};

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
  u64 count;
  bool enabled;
  const char *name;

  InterruptDescriptor() noexcept
      : irq(0), type(InterruptType::SPI), trigger(TriggerType::LevelHigh),
        priority(128), target_cpu_mask(1), handler(nullptr), context(nullptr),
        count(0), enabled(false), name(nullptr) {}

  InterruptDescriptor(InterruptId id, InterruptHandler h, void *ctx,
                      const char *n) noexcept
      : irq(id), type(determine_type(id)), trigger(TriggerType::LevelHigh),
        priority(128), target_cpu_mask(1), handler(h), context(ctx), count(0),
        enabled(false), name(n) {}

private:
  static constexpr InterruptType determine_type(InterruptId id) noexcept {
    if (id < 16)
      return InterruptType::SGI;
    if (id < 32)
      return InterruptType::PPI;
    return InterruptType::SPI;
  }
};

// GIC register offsets (GICv2)
namespace GicRegs {
// Distributor registers
inline constexpr u32 GICD_CTLR = 0x000;
inline constexpr u32 GICD_TYPER = 0x004;
inline constexpr u32 GICD_IIDR = 0x008;
inline constexpr u32 GICD_IGROUPR = 0x080;
inline constexpr u32 GICD_ISENABLER = 0x100;
inline constexpr u32 GICD_ICENABLER = 0x180;
inline constexpr u32 GICD_ISPENDR = 0x200;
inline constexpr u32 GICD_ICPENDR = 0x280;
inline constexpr u32 GICD_ISACTIVER = 0x300;
inline constexpr u32 GICD_ICACTIVER = 0x380;
inline constexpr u32 GICD_IPRIORITYR = 0x400;
inline constexpr u32 GICD_ITARGETSR = 0x800;
inline constexpr u32 GICD_ICFGR = 0xC00;
inline constexpr u32 GICD_SGIR = 0xF00;

// CPU Interface registers
inline constexpr u32 GICC_CTLR = 0x000;
inline constexpr u32 GICC_PMR = 0x004;
inline constexpr u32 GICC_BPR = 0x008;
inline constexpr u32 GICC_IAR = 0x00C;
inline constexpr u32 GICC_EOIR = 0x010;
inline constexpr u32 GICC_RPR = 0x014;
inline constexpr u32 GICC_HPPIR = 0x018;
} // namespace GicRegs

// GIC driver main class
class GenericInterruptController {
private:
  GicVersion version_;
  VirtAddr distributor_base_;
  VirtAddr cpu_interface_base_;
  u32 max_interrupts_;
  u32 max_cpus_;

  containers::RcuHashMap<InterruptId, InterruptDescriptor *> interrupt_table_;
  containers::PerCpuData<u64> interrupt_counts_;

  containers::AtomicCounter<u64> total_interrupts_;
  containers::AtomicCounter<u64> spurious_interrupts_;

public:
  GenericInterruptController() noexcept
      : version_(GicVersion::Unknown), distributor_base_(0),
        cpu_interface_base_(0), max_interrupts_(0), max_cpus_(0),
        total_interrupts_(0), spurious_interrupts_(0) {}

  ~GenericInterruptController() noexcept { cleanup(); }

  // Non-copyable, non-movable
  GenericInterruptController(const GenericInterruptController &) = delete;
  GenericInterruptController &
  operator=(const GenericInterruptController &) = delete;
  GenericInterruptController(GenericInterruptController &&) = delete;
  GenericInterruptController &
  operator=(GenericInterruptController &&) = delete;

  struct GicStats {
    u64 total_interrupts;
    u64 spurious_interrupts;
    u32 registered_interrupts;
    u32 enabled_interrupts;
  };

  [[nodiscard]] VoidResult initialize(VirtAddr dist_base,
                                      VirtAddr cpu_base) noexcept {
    distributor_base_ = dist_base;
    cpu_interface_base_ = cpu_base;

    auto detect_result = detect_gic_config();
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

  [[nodiscard]] VoidResult
  register_interrupt(InterruptId irq, InterruptHandler handler, void *context,
                     const char *name = nullptr) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    if (handler == nullptr) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    if (interrupt_table_.find(irq) != nullptr) {
      return VoidResult{ErrorCode::AlreadyExists};
    }

    InterruptDescriptor *desc =
        new InterruptDescriptor(irq, handler, context, name);
    if (desc == nullptr) {
      return VoidResult{ErrorCode::OutOfMemory};
    }

    interrupt_table_.insert_or_update(irq, desc);

    return VoidResult{};
  }

  [[nodiscard]] VoidResult unregister_interrupt(InterruptId irq) noexcept {
    auto desc_ptr = interrupt_table_.find(irq);
    if (desc_ptr == nullptr) {
      return VoidResult{ErrorCode::NotFound};
    }
    InterruptDescriptor *desc = *desc_ptr;

    (void)disable_interrupt(irq);
    interrupt_table_.remove(irq);
    delete desc;

    return VoidResult{};
  }

  [[nodiscard]] VoidResult enable_interrupt(InterruptId irq) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    u32 reg_offset = GicRegs::GICD_ISENABLER + (irq / 32) * 4;
    u32 bit_pos = irq % 32;
    u32 reg_value = 1U << bit_pos;

    write_distributor_reg(reg_offset, reg_value);

    auto desc_ptr = interrupt_table_.find(irq);
    if (desc_ptr != nullptr) {
      InterruptDescriptor *desc = *desc_ptr;
      desc->enabled = true;
    }

    return VoidResult{};
  }

  [[nodiscard]] VoidResult disable_interrupt(InterruptId irq) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    u32 reg_offset = GicRegs::GICD_ICENABLER + (irq / 32) * 4;
    u32 bit_pos = irq % 32;
    u32 reg_value = 1U << bit_pos;

    write_distributor_reg(reg_offset, reg_value);

    auto desc_ptr = interrupt_table_.find(irq);
    if (desc_ptr != nullptr) {
      InterruptDescriptor *desc = *desc_ptr;
      desc->enabled = false;
    }

    return VoidResult{};
  }

  [[nodiscard]] VoidResult
  set_interrupt_priority(InterruptId irq, InterruptPriority priority) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    u32 reg_offset = GicRegs::GICD_IPRIORITYR + irq;
    write_distributor_reg(reg_offset, priority);

    auto desc_ptr = interrupt_table_.find(irq);
    if (desc_ptr != nullptr) {
      InterruptDescriptor *desc = *desc_ptr;
      desc->priority = priority;
    }

    return VoidResult{};
  }

  [[nodiscard]] VoidResult set_interrupt_target(InterruptId irq,
                                                u32 cpu_mask) noexcept {
    if (irq >= max_interrupts_) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    if (irq < 32) {
      return VoidResult{ErrorCode::NotSupported};
    }

    u32 reg_offset = GicRegs::GICD_ITARGETSR + irq;
    write_distributor_reg(reg_offset, cpu_mask);

    auto desc_ptr = interrupt_table_.find(irq);
    if (desc_ptr != nullptr) {
      InterruptDescriptor *desc = *desc_ptr;
      desc->target_cpu_mask = cpu_mask;
    }

    return VoidResult{};
  }

  [[nodiscard]] VoidResult send_sgi(InterruptId sgi,
                                    u32 target_cpu_mask) noexcept {
    if (sgi >= 16) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    u32 sgir_value = sgi | (target_cpu_mask << 16);
    write_distributor_reg(GicRegs::GICD_SGIR, sgir_value);

    return VoidResult{};
  }

  void handle_interrupt() noexcept {
    u32 cpu = get_current_cpu_id();

    u32 iar = read_cpu_interface_reg(GicRegs::GICC_IAR);
    InterruptId irq = iar & 0x3FF;

    if (irq >= 1020) {
      (void)spurious_interrupts_.fetch_add(
          1, containers::MemoryOrder::Relaxed);
      return;
    }

    (void)total_interrupts_.fetch_add(1, containers::MemoryOrder::Relaxed);
    interrupt_counts_.get_cpu(cpu)++;

    auto desc_ptr = interrupt_table_.find(irq);
    if (desc_ptr != nullptr) {
      InterruptDescriptor *desc = *desc_ptr;
      if (desc->handler != nullptr) {
        desc->handler(irq, desc->context);
        desc->count++;
      }
    }

    write_cpu_interface_reg(GicRegs::GICC_EOIR, iar);
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

    return {total_interrupts_.load(containers::MemoryOrder::Relaxed),
            spurious_interrupts_.load(containers::MemoryOrder::Relaxed),
            registered, enabled};
  }

  [[nodiscard]] const InterruptDescriptor *
  get_interrupt_info(InterruptId irq) const noexcept {
    auto desc_ptr = interrupt_table_.find(irq);
    return desc_ptr ? *desc_ptr : nullptr;
  }

  void set_priority_mask(InterruptPriority mask) noexcept {
    write_cpu_interface_reg(GicRegs::GICC_PMR, mask);
  }

  [[nodiscard]] static u32 get_current_cpu_id() noexcept {
    return arch::get_current_cpu_id();
  }

private:
  [[nodiscard]] VoidResult detect_gic_config() noexcept {
    u32 typer = read_distributor_reg(GicRegs::GICD_TYPER);

    max_interrupts_ = ((typer & 0x1F) + 1) * 32;
    max_cpus_ = ((typer >> 5) & 0x7) + 1;
    version_ = GicVersion::GICv2;

    return VoidResult{};
  }

  [[nodiscard]] VoidResult initialize_distributor() noexcept {
    write_distributor_reg(GicRegs::GICD_CTLR, 0);

    for (u32 i = 0; i < max_interrupts_; i += 32) {
      write_distributor_reg(GicRegs::GICD_ICENABLER + i / 8, 0xFFFFFFFF);
    }

    for (u32 i = 0; i < max_interrupts_; i += 32) {
      write_distributor_reg(GicRegs::GICD_ICPENDR + i / 8, 0xFFFFFFFF);
    }

    for (u32 i = 0; i < max_interrupts_; i += 4) {
      write_distributor_reg(GicRegs::GICD_IPRIORITYR + i, 0x80808080);
    }

    for (u32 i = 32; i < max_interrupts_; i += 4) {
      write_distributor_reg(GicRegs::GICD_ITARGETSR + i, 0x01010101);
    }

    write_distributor_reg(GicRegs::GICD_CTLR, 1);
    return VoidResult{};
  }

  [[nodiscard]] VoidResult initialize_cpu_interface() noexcept {
    write_cpu_interface_reg(GicRegs::GICC_PMR, 0xFF);
    write_cpu_interface_reg(GicRegs::GICC_CTLR, 1);
    return VoidResult{};
  }

  [[nodiscard]] u32 read_distributor_reg(u32 offset) const noexcept {
    return *reinterpret_cast<volatile u32 *>(distributor_base_ + offset);
  }

  void write_distributor_reg(u32 offset, u32 value) const noexcept {
    *reinterpret_cast<volatile u32 *>(distributor_base_ + offset) = value;
  }

  [[nodiscard]] u32 read_cpu_interface_reg(u32 offset) const noexcept {
    return *reinterpret_cast<volatile u32 *>(cpu_interface_base_ + offset);
  }

  void write_cpu_interface_reg(u32 offset, u32 value) const noexcept {
    *reinterpret_cast<volatile u32 *>(cpu_interface_base_ + offset) = value;
  }

  void cleanup() noexcept {
    interrupt_table_.for_each([](const auto &entry) { delete entry.value; });
  }
};

// Global GIC instance pointer
extern GenericInterruptController *g_gic;

// C-style convenience interface
extern "C" {
void interrupt_handler_entry() noexcept;
ErrorCode register_irq_handler(InterruptId irq, InterruptHandler handler,
                               void *context, const char *name);
ErrorCode enable_irq(InterruptId irq);
ErrorCode disable_irq(InterruptId irq);
}

// ========================================================================
// IPI SGI ID assignment (based on Linux kernel design)
// ========================================================================
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

enum class IpiResult : u8 {
  Success = 0,
  InvalidCpu = 2,
  NotInitialized = 3
};

struct IpiMessage {
  IpiType type;
  u32 source_cpu;
  u32 target_cpu;
  u64 sequence;
};

class SimpleInterProcessorInterrupt {
public:
  static constexpr u32 MAX_CPUS = moss::kernel::MAX_CPUS;

  SimpleInterProcessorInterrupt() noexcept = default;
  ~SimpleInterProcessorInterrupt() noexcept = default;

  SimpleInterProcessorInterrupt(const SimpleInterProcessorInterrupt &) =
      delete;
  SimpleInterProcessorInterrupt &
  operator=(const SimpleInterProcessorInterrupt &) = delete;

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
  u64 message_sequence_ = 0;
  u64 total_pings_sent_ = 0;

  bool is_valid_cpu_id(u32 cpu_id) const noexcept;
  u32 get_current_cpu_id() const noexcept;
};

extern SimpleInterProcessorInterrupt *g_simple_ipi_manager;

VoidResult initialize_simple_ipi_system(u32 max_cpus) noexcept;
void shutdown_simple_ipi_system() noexcept;

inline IpiResult simple_ipi_ping(u32 target_cpu) noexcept {
  return g_simple_ipi_manager
             ? g_simple_ipi_manager->ping_cpu(target_cpu)
             : IpiResult::NotInitialized;
}

const char *ipi_type_to_string(IpiType type) noexcept;
const char *ipi_result_to_string(IpiResult result) noexcept;

} // namespace simple

// ========================================================================
// Simple Hardware IPI (ipi_hardware_simple)
// ========================================================================
namespace hw_simple {

enum class IpiType : u8 {
  Ping = 0,
  Reschedule = 1,
  CallFunction = 2,
  Stop = 3,
  WakeUp = 4,
  Timer = 5,
  Debug = 6
};

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
  u64 total_ipis_sent_;
  u64 message_sequence_;

  u64 sgi_send_counts_[8];
  u64 sgi_receive_counts_[8];

public:
  SimpleHardwareIpi() noexcept
      : gic_(nullptr), initialized_(false), max_cpus_(0),
        total_ipis_sent_(0), message_sequence_(1000) {
    for (u32 i = 0; i < 8; ++i) {
      sgi_send_counts_[i] = 0;
      sgi_receive_counts_[i] = 0;
    }
  }

  ~SimpleHardwareIpi() noexcept { shutdown(); }

  SimpleHardwareIpi(const SimpleHardwareIpi &) = delete;
  SimpleHardwareIpi &operator=(const SimpleHardwareIpi &) = delete;

  VoidResult initialize(GenericInterruptController *gic,
                         u32 max_cpus) noexcept;
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
  VoidResult send_hardware_sgi(IpiSgiId sgi_id,
                                u32 target_cpu_mask) noexcept;
  bool is_valid_cpu_id(u32 cpu_id) const noexcept;
  static u32 get_current_cpu_id() noexcept;
  u64 generate_sequence() noexcept;
};

extern SimpleHardwareIpi *g_simple_hardware_ipi;

VoidResult initialize_simple_hardware_ipi(GenericInterruptController *gic,
                                           u32 max_cpus) noexcept;
void shutdown_simple_hardware_ipi() noexcept;

const char *ipi_type_to_string(IpiType type) noexcept;
const char *ipi_result_to_string(IpiResult result) noexcept;
const char *sgi_id_to_string(IpiSgiId sgi) noexcept;

} // namespace hw_simple

// === Module-level variable definitions ===

// Global GIC instance
GenericInterruptController *g_gic = nullptr;

} // namespace moss::kernel::interrupts
