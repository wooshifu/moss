// MOSS Simple Hardware IPI implementation - Based on ARM64 GIC SGI
// Core SGI hardware integration, minimal dependencies

module moss.interrupts;

namespace moss::kernel::interrupts::hw_simple {

namespace log = moss::kernel::logging;

// Forward declare static SGI handler functions
static void handle_ping_sgi(InterruptId irq, void *context) noexcept;
static void handle_reschedule_sgi(InterruptId irq, void *context) noexcept;

// Global simple hardware IPI manager instance
SimpleHardwareIpi *g_simple_hardware_ipi = nullptr;

// === SimpleHardwareIpi core implementation ===

VoidResult SimpleHardwareIpi::initialize(GenericInterruptController *gic, u32 max_cpus) noexcept {
  if (initialized_) {
    return VoidResult{ErrorCode::AlreadyExists};
  }

  if (gic == nullptr) {
    return VoidResult{ErrorCode::InvalidParameter};
  }

  if (max_cpus == 0 || max_cpus > 256) {
    return VoidResult{ErrorCode::InvalidParameter};
  }

  gic_ = gic;
  max_cpus_ = max_cpus;
  message_sequence_.store(1, containers::MemoryOrder::Relaxed);

  log::klog::info("Hardware IPI system initializing...");

  // Register core IPI SGI handlers to GIC
  auto ping_result =
      gic_->register_interrupt(static_cast<InterruptId>(IpiSgiId::Ping), handle_ping_sgi, this, "IPI-Ping");
  if (!ping_result) {
    log::klog::error("Failed to register Ping SGI");
    return ping_result;
  }

  auto reschedule_result = gic_->register_interrupt(static_cast<InterruptId>(IpiSgiId::Reschedule),
                                                    handle_reschedule_sgi, this, "IPI-Reschedule");
  if (!reschedule_result) {
    log::klog::error("Failed to register Reschedule SGI");
    return reschedule_result;
  }

  // Enable core IPI SGI interrupts
  auto enable_ping = gic_->enable_interrupt(static_cast<InterruptId>(IpiSgiId::Ping));
  auto enable_reschedule = gic_->enable_interrupt(static_cast<InterruptId>(IpiSgiId::Reschedule));

  if (!enable_ping || !enable_reschedule) {
    log::klog::error("Failed to enable SGI interrupts");
    return VoidResult{ErrorCode::InvalidState};
  }

  initialized_ = true;
  log::klog::info("Hardware IPI system initialized successfully");

  return VoidResult{};
}

void SimpleHardwareIpi::shutdown() noexcept {
  if (!initialized_) {
    return;
  }

  if (gic_) {
    (void)gic_->disable_interrupt(static_cast<InterruptId>(IpiSgiId::Ping));
    (void)gic_->disable_interrupt(static_cast<InterruptId>(IpiSgiId::Reschedule));
    (void)gic_->unregister_interrupt(static_cast<InterruptId>(IpiSgiId::Ping));
    (void)gic_->unregister_interrupt(static_cast<InterruptId>(IpiSgiId::Reschedule));
  }

  initialized_ = false;
  gic_ = nullptr;
}

// === Core IPI send implementation ===

IpiResult SimpleHardwareIpi::send_ipi(u32 target_cpu, IpiType type) noexcept {
  if (!initialized_) {
    return IpiResult::NotInitialized;
  }

  if (!is_valid_cpu_id(target_cpu)) {
    return IpiResult::InvalidCpu;
  }

  // GICv2 SGIR only supports 8-bit CPU target mask (CPUs 0-7).
  if (target_cpu >= 8) {
    return IpiResult::InvalidCpu;
  }
  IpiSgiId sgi_id = ipi_type_to_sgi(type);
  u32 target_cpu_mask = 1U << target_cpu;

  auto hw_result = send_hardware_sgi(sgi_id, target_cpu_mask);
  if (!hw_result) {
    return IpiResult::HardwareError;
  }

  u8 sgi_index = static_cast<u8>(sgi_id);
  if (sgi_index < 8) {
    (void)sgi_send_counts_[sgi_index].fetch_add(1, containers::MemoryOrder::Relaxed);
  }
  (void)total_ipis_sent_.fetch_add(1, containers::MemoryOrder::Relaxed);

  return IpiResult::Success;
}

IpiResult SimpleHardwareIpi::ping_cpu(u32 target_cpu) noexcept { return send_ipi(target_cpu, IpiType::Ping); }

IpiResult SimpleHardwareIpi::ping_cpus(u32 cpu_mask) noexcept {
  if (!initialized_) {
    return IpiResult::NotInitialized;
  }

  auto hw_result = send_hardware_sgi(IpiSgiId::Ping, cpu_mask);
  if (!hw_result) {
    return IpiResult::HardwareError;
  }

  u32 cpu_count = static_cast<u32>(__builtin_popcount(cpu_mask));
  (void)sgi_send_counts_[static_cast<u8>(IpiSgiId::Ping)].fetch_add(cpu_count, containers::MemoryOrder::Relaxed);
  (void)total_ipis_sent_.fetch_add(cpu_count, containers::MemoryOrder::Relaxed);

  return IpiResult::Success;
}

IpiResult SimpleHardwareIpi::request_reschedule(u32 target_cpu) noexcept {
  return send_ipi(target_cpu, IpiType::Reschedule);
}

IpiResult SimpleHardwareIpi::wakeup_cpu(u32 target_cpu) noexcept { return send_ipi(target_cpu, IpiType::WakeUp); }

// === SGI interrupt handler implementations ===

static void handle_ping_sgi(InterruptId /* irq */, void * /* context */) noexcept {
  // Ping received on current CPU
  log::klog::debug("Ping SGI received");
}

static void handle_reschedule_sgi(InterruptId /* irq */, void * /* context */) noexcept {
  // Reschedule request received
  log::klog::debug("Reschedule SGI received");
}

// === Internal helper implementations ===

VoidResult SimpleHardwareIpi::send_hardware_sgi(IpiSgiId sgi_id, u32 target_cpu_mask) noexcept {
  if (!gic_) {
    return VoidResult{ErrorCode::InvalidState};
  }

  InterruptId sgi_interrupt_id = static_cast<InterruptId>(sgi_id);
  return gic_->send_sgi(sgi_interrupt_id, target_cpu_mask);
}

bool SimpleHardwareIpi::is_valid_cpu_id(u32 cpu_id) const noexcept { return cpu_id < max_cpus_; }

u32 SimpleHardwareIpi::get_current_cpu_id() noexcept { return GenericInterruptController::get_current_cpu_id(); }

u64 SimpleHardwareIpi::generate_sequence() noexcept {
  return message_sequence_.fetch_add(1, containers::MemoryOrder::Relaxed);
}

// === Statistics ===

SimpleHardwareIpi::Statistics SimpleHardwareIpi::get_statistics() const noexcept {
  Statistics stats{};
  stats.total_sent = total_ipis_sent_.load(containers::MemoryOrder::Relaxed);
  stats.ping_count = sgi_send_counts_[static_cast<u8>(IpiSgiId::Ping)].load(containers::MemoryOrder::Relaxed);
  stats.reschedule_count =
      sgi_send_counts_[static_cast<u8>(IpiSgiId::Reschedule)].load(containers::MemoryOrder::Relaxed);
  stats.max_cpus = max_cpus_;
  stats.initialized = initialized_;
  return stats;
}

VoidResult SimpleHardwareIpi::self_test() noexcept {
  if (!initialized_) {
    return VoidResult{ErrorCode::InvalidState};
  }

  log::klog::info("Hardware IPI self-test started");

  for (u32 target_cpu = 1; target_cpu < max_cpus_; ++target_cpu) {
    auto result = ping_cpu(target_cpu);
    if (result != IpiResult::Success) {
      log::klog::error("Hardware IPI self-test failed");
      return VoidResult{ErrorCode::InvalidState};
    }
  }

  log::klog::info("Hardware IPI self-test passed");
  return VoidResult{};
}

// === Global functions ===

VoidResult initialize_simple_hardware_ipi(GenericInterruptController *gic, u32 max_cpus) noexcept {
  if (g_simple_hardware_ipi != nullptr) {
    return VoidResult{ErrorCode::AlreadyExists};
  }

  g_simple_hardware_ipi = new SimpleHardwareIpi();
  if (g_simple_hardware_ipi == nullptr) {
    return VoidResult{ErrorCode::OutOfMemory};
  }

  auto init_result = g_simple_hardware_ipi->initialize(gic, max_cpus);
  if (!init_result) {
    delete g_simple_hardware_ipi;
    g_simple_hardware_ipi = nullptr;
    return init_result;
  }

  return VoidResult{};
}

void shutdown_simple_hardware_ipi() noexcept {
  if (g_simple_hardware_ipi != nullptr) {
    delete g_simple_hardware_ipi;
    g_simple_hardware_ipi = nullptr;
  }
}

// === Debug tool functions ===

const char *ipi_type_to_string(IpiType type) noexcept {
  switch (type) {
  case IpiType::Ping:
    return "Ping";
  case IpiType::Reschedule:
    return "Reschedule";
  case IpiType::CallFunction:
    return "CallFunction";
  case IpiType::Stop:
    return "Stop";
  case IpiType::WakeUp:
    return "WakeUp";
  case IpiType::Timer:
    return "Timer";
  case IpiType::Debug:
    return "Debug";
  default:
    return "Unknown";
  }
}

const char *ipi_result_to_string(IpiResult result) noexcept {
  switch (result) {
  case IpiResult::Success:
    return "Success";
  case IpiResult::InvalidCpu:
    return "InvalidCpu";
  case IpiResult::InvalidType:
    return "InvalidType";
  case IpiResult::NotInitialized:
    return "NotInitialized";
  case IpiResult::HardwareError:
    return "HardwareError";
  case IpiResult::QueueFull:
    return "QueueFull";
  case IpiResult::Timeout:
    return "Timeout";
  default:
    return "Unknown";
  }
}

const char *sgi_id_to_string(IpiSgiId sgi) noexcept {
  switch (sgi) {
  case IpiSgiId::Reschedule:
    return "SGI0-Reschedule";
  case IpiSgiId::CallFunction:
    return "SGI1-CallFunction";
  case IpiSgiId::CallFunctionSingle:
    return "SGI2-CallFunctionSingle";
  case IpiSgiId::Timer:
    return "SGI3-Timer";
  case IpiSgiId::Ping:
    return "SGI4-Ping";
  case IpiSgiId::WakeUp:
    return "SGI5-WakeUp";
  case IpiSgiId::Stop:
    return "SGI6-Stop";
  case IpiSgiId::Debug:
    return "SGI7-Debug";
  default:
    return "SGI-Unknown";
  }
}

} // namespace moss::kernel::interrupts::hw_simple
