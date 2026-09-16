// Simplified MOSS IPI (Inter-Processor Interrupt) implementation
// Focused on Ping IPI vertical slice verification

module moss.interrupts;

namespace moss::kernel::interrupts::simple {

namespace log = moss::kernel::logging;

// Global simplified IPI manager instance
SimpleInterProcessorInterrupt *g_simple_ipi_manager = nullptr;

// === SimpleInterProcessorInterrupt implementation ===

VoidResult SimpleInterProcessorInterrupt::initialize(u32 max_cpus) noexcept {
  if (initialized_) {
    return VoidResult{};
  }

  if (max_cpus == 0 || max_cpus > MAX_CPUS) {
    return VoidResult{ErrorCode::InvalidArgument};
  }

  max_cpus_ = max_cpus;
  total_pings_sent_.store(0, containers::MemoryOrder::Relaxed);
  initialized_ = true;

  log::klog::info("IPI simple subsystem initialized");

  return VoidResult{};
}

IpiResult SimpleInterProcessorInterrupt::send_ipi(u32 target_cpu, IpiType type) noexcept {
  if (!initialized_) {
    return IpiResult::NotInitialized;
  }

  if (!is_valid_cpu_id(target_cpu)) {
    return IpiResult::InvalidCpu;
  }

  if (type != IpiType::Ping) {
    return IpiResult::InvalidType;
  }

  // Reuse the initialized hardware path, including its registered Ping handler
  // and architecture-specific target checks. A successful request is accepted
  // for asynchronous delivery; it is not a remote acknowledgement.
  auto *backend = hw_simple::g_simple_hardware_ipi;
  if (backend == nullptr) {
    return IpiResult::NotInitialized;
  }
  const auto result = backend->ping_cpu(target_cpu);
  if (result != hw_simple::IpiResult::Success) {
    if (result == hw_simple::IpiResult::NotInitialized)
      return IpiResult::NotInitialized;
    if (result == hw_simple::IpiResult::InvalidCpu)
      return IpiResult::InvalidCpu;
    return IpiResult::HardwareError;
  }

  (void)total_pings_sent_.fetch_add(1, containers::MemoryOrder::Relaxed);
  return IpiResult::Success;
}

IpiResult SimpleInterProcessorInterrupt::ping_cpu(u32 target_cpu) noexcept {
  return send_ipi(target_cpu, IpiType::Ping);
}

VoidResult SimpleInterProcessorInterrupt::self_test() noexcept {
  if (!initialized_) {
    return VoidResult{ErrorCode::InvalidState};
  }

  log::klog::info("IPI simple self-test started");

  u32 current_cpu = get_current_cpu_id();
  if (!is_valid_cpu_id(current_cpu)) {
    return VoidResult{ErrorCode::InvalidState};
  }
  for (u32 target_cpu = 0; target_cpu < max_cpus_; ++target_cpu) {
    if (target_cpu != current_cpu) {
      auto result = ping_cpu(target_cpu);
      if (result != IpiResult::Success) {
        log::klog::error("IPI simple self-test failed");
        return VoidResult{ErrorCode::InvalidState};
      }
    }
  }

  log::klog::info("IPI simple self-test passed");
  return VoidResult{};
}

SimpleInterProcessorInterrupt::SystemInfo SimpleInterProcessorInterrupt::get_system_info() const noexcept {
  SystemInfo info{};
  info.initialized = initialized_;
  info.max_cpus = max_cpus_;
  info.total_pings_sent = total_pings_sent_.load(containers::MemoryOrder::Relaxed);
  return info;
}

bool SimpleInterProcessorInterrupt::is_valid_cpu_id(u32 cpu_id) const noexcept { return cpu_id < max_cpus_; }

// Self-test excludes the actual firmware-mapped logical CPU, including a
// nonzero caller; a fixed zero would send back to that caller and skip CPU 0.
u32 SimpleInterProcessorInterrupt::get_current_cpu_id() const noexcept { return arch::get_current_cpu_id(); }

// === Global initialization functions ===

VoidResult initialize_simple_ipi_system(u32 max_cpus) noexcept {
  if (g_simple_ipi_manager != nullptr) {
    return VoidResult{};
  }

  g_simple_ipi_manager = new SimpleInterProcessorInterrupt();
  if (g_simple_ipi_manager == nullptr) {
    return VoidResult{ErrorCode::OutOfMemory};
  }

  auto result = g_simple_ipi_manager->initialize(max_cpus);
  if (!result) {
    delete g_simple_ipi_manager;
    g_simple_ipi_manager = nullptr;
    return result;
  }

  return VoidResult{};
}

void shutdown_simple_ipi_system() noexcept {
  if (g_simple_ipi_manager != nullptr) {
    delete g_simple_ipi_manager;
    g_simple_ipi_manager = nullptr;
  }
}

// === Debug tool functions ===

const char *ipi_type_to_string(IpiType type) noexcept {
  switch (type) {
  case IpiType::Ping:
    return "Ping";
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
  default:
    return "Unknown";
  }
}

} // namespace moss::kernel::interrupts::simple
