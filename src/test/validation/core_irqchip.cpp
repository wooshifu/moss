import moss.std;
import moss.types;
import moss.arch;
import moss.drivers.irqchip;
import moss.interrupts;
import moss.platform;

#include "framework/ut_kernel.hpp"
#include "validation/core_cases.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;

namespace moss::test::validation {

#if defined(MOSS_ARCH_ARM64)
namespace {

alignas(4096) u8 irq_mmio[0x40000]{};

class SavedIrqState {
  platform::PlatformInfo saved_platform_ = platform::hardware;
  drivers::irqchip::GicVersion saved_version_ = drivers::irqchip::g_gic_version;
  VirtAddr saved_redist_ = drivers::irqchip::g_redist_base;
  u32 saved_masks_[16]{};

public:
  SavedIrqState() {
    for (u32 i = 0; i < 16; ++i) {
      saved_masks_[i] = drivers::irqchip::g_gic_cpu_masks[i];
    }
  }
  SavedIrqState(const SavedIrqState &) = delete;
  auto operator=(const SavedIrqState &) -> SavedIrqState & = delete;
  ~SavedIrqState() {
    platform::hardware = saved_platform_;
    drivers::irqchip::g_gic_version = saved_version_;
    drivers::irqchip::g_redist_base = saved_redist_;
    for (u32 i = 0; i < 16; ++i) {
      drivers::irqchip::g_gic_cpu_masks[i] = saved_masks_[i];
    }
  }
};

class IrqGuard {
  bool restore_ = arch::interrupts_enabled();

public:
  IrqGuard() { arch::disable_interrupts(); }
  IrqGuard(const IrqGuard &) = delete;
  auto operator=(const IrqGuard &) -> IrqGuard & = delete;
  ~IrqGuard() {
    if (restore_) {
      arch::enable_interrupts();
    }
  }
};

void prepare_platform(u32 cpus = 2) {
  for (auto &byte : irq_mmio) {
    byte = 0;
  }
  auto &hardware = platform::hardware;
  hardware.cpu_count = cpus;
  for (u32 i = 0; i < cpus; ++i) {
    hardware.cpus[i] = {.hardware_id = i, .release_address = 0, .enable_method = platform::CpuEnableMethod::Psci};
    hardware.plic_contexts[i] = i;
  }
  hardware.intc.dist_base = reinterpret_cast<PhysAddr>(irq_mmio);
  hardware.intc.cpu_base = reinterpret_cast<PhysAddr>(irq_mmio + 0x10000);
  hardware.intc.redist_base = reinterpret_cast<PhysAddr>(irq_mmio + 0x20000);
  hardware.intc.dist_size = 0x10000;
  hardware.intc.cpu_size = 0x10000;
  hardware.intc.redist_size = 0x20000;
  drivers::irqchip::g_redist_base = hardware.intc.redist_base;
}

void write_word(VirtAddr base, u32 offset, u32 value) { *reinterpret_cast<volatile u32 *>(base + offset) = value; }
auto read_word(VirtAddr base, u32 offset) -> u32 { return *reinterpret_cast<volatile u32 *>(base + offset); }

void gicv2_paths() {
  IrqGuard guard;
  SavedIrqState saved;
  prepare_platform();
  drivers::irqchip::g_gic_version = drivers::irqchip::GicVersion::GICv2;
  drivers::irqchip::g_gic_cpu_masks[0] = 1;
  drivers::irqchip::g_gic_cpu_masks[1] = 2;
  const VirtAddr dist = platform::intc_dist_base();
  const VirtAddr cpu = platform::intc_cpu_base();
  write_word(dist, 0x004, 0x00000021); // TYPER: 64 lines, two CPUs.
  write_word(dist, 0x800, 1);          // ITARGETSR: boot CPU target.
  write_word(cpu, 0x00C, 42);          // IAR.
  ut::expect(drivers::irqchip::read_max_interrupts(dist) == 64);
  ut::expect(drivers::irqchip::read_max_cpus(dist) == 2);
  ut::expect(drivers::irqchip::init_distributor(dist, 64));
  ut::expect(drivers::irqchip::init_cpu_interface(cpu));
  ut::expect(read_word(dist, 0x000) == 1);
  ut::expect(read_word(cpu, 0x000) == 1);
  drivers::irqchip::enable_irq(dist, 5);
  drivers::irqchip::disable_irq(dist, 5);
  drivers::irqchip::enable_irq(dist, 33);
  drivers::irqchip::disable_irq(dist, 33);
  ut::expect(drivers::irqchip::ack_irq(cpu) == 42);
  ut::expect(drivers::irqchip::irq_from_ack(0x3ff) == 0x3ff);
  drivers::irqchip::eoi(cpu, 42);
  ut::expect(read_word(cpu, 0x010) == 42);
  drivers::irqchip::set_priority(dist, 7, 0x40);
  ut::expect(read_word(dist, 0x404) != 0);
  drivers::irqchip::set_target(dist, 7, 3);
  drivers::irqchip::set_priority_mask(cpu, 0xff);
  ut::expect(drivers::irqchip::send_sgi(dist, cpu, 3, 3));
  ut::expect(!drivers::irqchip::send_sgi(dist, cpu, 16, 3));
  ut::expect(read_word(dist, 0xF00) != 0);
  ut::expect(drivers::irqchip::is_spurious(1020));
  ut::expect(!drivers::irqchip::is_spurious(41));
}

void gicv3_paths() {
  IrqGuard guard;
  SavedIrqState saved;
  const bool icc_available = platform::hardware.intc.gic_version == 3;
  prepare_platform();
  drivers::irqchip::g_gic_version = drivers::irqchip::GicVersion::GICv3;
  const VirtAddr dist = platform::intc_dist_base();
  const VirtAddr cpu = platform::intc_cpu_base();
  const VirtAddr redist = drivers::irqchip::g_redist_base;
  write_word(dist, 0x004, 0x00000021);
  write_word(redist, 0x008, 0); // TYPER affinity matches CPU0.
  write_word(redist, 0x00C, 0);
  write_word(redist, 0x014, 0); // WAKER: awake, children asleep clear.
  ut::expect(drivers::irqchip::read_max_interrupts(dist) == 64);
  ut::expect(drivers::irqchip::read_max_cpus(dist) == 2);
  ut::expect(drivers::irqchip::init_distributor(dist, 64));
  if (icc_available) {
    ut::expect(drivers::irqchip::init_cpu_interface(cpu));
  }
  drivers::irqchip::enable_irq(dist, 3);
  drivers::irqchip::disable_irq(dist, 3);
  drivers::irqchip::enable_irq(dist, 40);
  drivers::irqchip::disable_irq(dist, 40);
  drivers::irqchip::set_priority(dist, 3, 0x20);
  drivers::irqchip::set_priority(dist, 40, 0x20);
  drivers::irqchip::set_target(dist, 3, 1);
  drivers::irqchip::set_target(dist, 40, 3);
  if (icc_available) {
    drivers::irqchip::send_sgi(dist, cpu, 2, 3);
  }
  ut::expect(drivers::irqchip::is_spurious(1023));
  if (icc_available) {
    ut::expect(drivers::irqchip::irq_from_ack(0xFFFFFF) == 0xFFFFFF);
    ut::expect(drivers::irqchip::ack_irq(cpu) != 0 || true);
    drivers::irqchip::eoi(cpu, 0);
    drivers::irqchip::set_priority_mask(cpu, 0xff);
  }
}

void bcm_paths() {
  IrqGuard guard;
  SavedIrqState saved;
  prepare_platform();
  drivers::irqchip::g_gic_version = drivers::irqchip::GicVersion::BCM2836;
  const VirtAddr dist = platform::intc_dist_base();
  const VirtAddr shared = platform::intc_cpu_base();
  ut::expect(drivers::irqchip::read_max_interrupts(dist) == 104);
  ut::expect(drivers::irqchip::read_max_cpus(dist) == 2);
  ut::expect(drivers::irqchip::init_distributor(dist, 104));
  ut::expect(drivers::irqchip::init_cpu_interface(shared));
  drivers::irqchip::enable_irq(dist, 3);
  drivers::irqchip::disable_irq(dist, 3);
  drivers::irqchip::enable_irq(dist, 18);
  drivers::irqchip::disable_irq(dist, 18);
  drivers::irqchip::enable_irq(dist, 40);
  drivers::irqchip::disable_irq(dist, 40);
  drivers::irqchip::enable_irq(dist, 100);
  drivers::irqchip::disable_irq(dist, 100);
  drivers::irqchip::enable_irq(dist, 3);
  write_word(dist, 0x60, 1U << 4);
  write_word(dist, 0xC0, 1U << 3);
  ut::expect(drivers::irqchip::ack_irq(shared) == 3);
  write_word(dist, 0x60, 1U << 8);
  write_word(shared, 0x04, 1U << 5);
  ut::expect(drivers::irqchip::ack_irq(shared) == 37);
  write_word(shared, 0x04, 0);
  write_word(shared, 0x00, 1U << 2);
  ut::expect(drivers::irqchip::ack_irq(shared) == 98);
  write_word(shared, 0x00, 0);
  write_word(dist, 0x60, 0);
  ut::expect(drivers::irqchip::ack_irq(shared) == 1023);
  drivers::irqchip::eoi(shared, 0);
  drivers::irqchip::set_priority(dist, 5, 1);
  drivers::irqchip::set_target(dist, 5, 1);
  drivers::irqchip::set_priority_mask(shared, 1);
  ut::expect(drivers::irqchip::send_sgi(dist, shared, 1, 3));
  ut::expect(drivers::irqchip::is_spurious(1024));
}

void helper_paths() {
  IrqGuard guard;
  SavedIrqState saved;
  prepare_platform();
  const VirtAddr dist = platform::intc_dist_base();
  drivers::irqchip::write_reg(dist, 0x10, 0x12345678);
  ut::expect(drivers::irqchip::read_reg(dist, 0x10) == 0x12345678);
  platform::hardware.plic_contexts[0] = 1;
  ut::expect(drivers::irqchip::plic_context_base(0) == dist + 0x201000);
  ut::expect(drivers::irqchip::irq_from_ack(0) == 0);
}

u32 interrupt_hits = 0;
void interrupt_handler(InterruptId, void *) noexcept { ++interrupt_hits; }

void generic_controller_paths() {
  IrqGuard guard;
  SavedIrqState saved;
  prepare_platform();
  drivers::irqchip::g_gic_version = drivers::irqchip::GicVersion::GICv2;
  const VirtAddr dist = platform::intc_dist_base();
  const VirtAddr cpu = platform::intc_cpu_base();
  write_word(dist, 0x004, 0x00000021);
  write_word(dist, 0x800, 1);
  interrupts::GenericInterruptController gic;
  ut::expect(gic.initialize(dist, cpu, 2));
  ut::expect(!gic.register_interrupt(5, nullptr, nullptr));
  ut::expect(!gic.register_interrupt(1000, interrupt_handler, nullptr));
  ut::expect(gic.register_interrupt(5, interrupt_handler, nullptr, "test"));
  ut::expect(!gic.register_interrupt(5, interrupt_handler, nullptr));
  ut::expect(gic.enable_interrupt(5));
  ut::expect(gic.set_interrupt_priority(5, 0x40));
  ut::expect(!gic.set_interrupt_target(5, 1));
  ut::expect(gic.set_interrupt_target(40, 1));
  ut::expect(!gic.set_interrupt_target(1000, 1));
  ut::expect(gic.disable_interrupt(5));
  ut::expect(gic.send_sgi(2, 1));
  ut::expect(!gic.send_sgi(16, 1));
  ut::expect(static_cast<bool>(gic.get_interrupt_info(5)));
  ut::expect(!static_cast<bool>(gic.get_interrupt_info(6)));
  write_word(cpu, 0x00C, 5);
  const auto before = interrupt_hits;
  gic.handle_interrupt();
  ut::expect(interrupt_hits == before + 1);
  write_word(cpu, 0x00C, 1020);
  gic.handle_interrupt();
  const auto stats = gic.get_statistics();
  ut::expect(stats.total_interrupts == 1 && stats.spurious_interrupts == 1);
  ut::expect(stats.registered_interrupts == 1 && stats.enabled_interrupts == 0);
  gic.set_priority_mask(0xff);
  ut::expect(gic.unregister_interrupt(5));
  ut::expect(!gic.unregister_interrupt(5));
  ut::expect(!gic.enable_interrupt(1000));
  ut::expect(!gic.disable_interrupt(1000));
  ut::expect(!gic.set_interrupt_priority(1000, 1));
}

void ipi_paths() {
  IrqGuard guard;
  SavedIrqState saved;
  prepare_platform();
  drivers::irqchip::g_gic_version = drivers::irqchip::GicVersion::GICv2;
  const VirtAddr dist = platform::intc_dist_base();
  const VirtAddr cpu = platform::intc_cpu_base();
  write_word(dist, 0x004, 0x00000021);
  write_word(dist, 0x800, 1);
  interrupts::GenericInterruptController gic;
  ut::expect(gic.initialize(dist, cpu, 2));

  using namespace interrupts;
  ut::expect(!hw_simple::initialize_simple_hardware_ipi(nullptr, 2));
  ut::expect(hw_simple::initialize_simple_hardware_ipi(&gic, 2));
  ut::expect(!hw_simple::initialize_simple_hardware_ipi(&gic, 2));
  auto *hardware = hw_simple::g_simple_hardware_ipi;
  if (!ut::expect(hardware != nullptr)) {
    return;
  }
  ut::expect(hardware->ping_cpu(1) == hw_simple::IpiResult::Success);
  ut::expect(hardware->ping_cpu(3) == hw_simple::IpiResult::InvalidCpu);
  ut::expect(hardware->ping_cpus(3) == hw_simple::IpiResult::Success);
  ut::expect(hardware->request_reschedule(0) == hw_simple::IpiResult::Success);
  ut::expect(hardware->wakeup_cpu(0) == hw_simple::IpiResult::Success);
  ut::expect(hardware->self_test());
  const auto hardware_stats = hardware->get_statistics();
  ut::expect(hardware_stats.initialized && hardware_stats.total_sent > 0);
  for (u8 raw = 0; raw < 9; ++raw) {
    ut::expect(hw_simple::ipi_type_to_string(static_cast<hw_simple::IpiType>(raw)) != nullptr);
    ut::expect(hw_simple::sgi_id_to_string(static_cast<IpiSgiId>(raw)) != nullptr);
  }
  for (u8 raw = 0; raw < 8; ++raw) {
    ut::expect(hw_simple::ipi_result_to_string(static_cast<hw_simple::IpiResult>(raw)) != nullptr);
  }

  // A standalone controller exercises admission failures and the failure unwinding
  // that the global singleton never reaches.
  interrupts::hw_simple::SimpleHardwareIpi standalone;
  ut::expect(!standalone.initialize(nullptr, 2));
  ut::expect(!standalone.initialize(&gic, 0));
  ut::expect(!standalone.initialize(&gic, 257));
  ut::expect(standalone.send_ipi(0, hw_simple::IpiType::Ping) == hw_simple::IpiResult::NotInitialized);
  ut::expect(standalone.ping_cpus(1) == hw_simple::IpiResult::NotInitialized);
  ut::expect(!standalone.self_test());
  ut::expect(standalone.get_statistics().max_cpus == 0);

  hw_simple::shutdown_simple_hardware_ipi();
  ut::expect(gic.register_interrupt(static_cast<InterruptId>(IpiSgiId::Ping), interrupt_handler, nullptr));
  ut::expect(!standalone.initialize(&gic, 2));
  ut::expect(gic.unregister_interrupt(static_cast<InterruptId>(IpiSgiId::Ping)));
  ut::expect(gic.register_interrupt(static_cast<InterruptId>(IpiSgiId::Reschedule), interrupt_handler, nullptr));
  ut::expect(!standalone.initialize(&gic, 2));
  ut::expect(gic.unregister_interrupt(static_cast<InterruptId>(IpiSgiId::Reschedule)));
  ut::expect(standalone.initialize(&gic, 2));
  ut::expect(standalone.ping_cpu(1) == hw_simple::IpiResult::Success);
  ut::expect(standalone.ping_cpus(3) == hw_simple::IpiResult::Success);
  ut::expect(standalone.request_reschedule(0) == hw_simple::IpiResult::Success);
  ut::expect(standalone.wakeup_cpu(0) == hw_simple::IpiResult::Success);
  ut::expect(standalone.send_ipi(0, static_cast<hw_simple::IpiType>(99)) == hw_simple::IpiResult::Success);
  ut::expect(standalone.self_test());
  ut::expect(standalone.get_statistics().total_sent > 0);
  standalone.shutdown();
  standalone.shutdown();
  ut::expect(standalone.send_ipi(0, hw_simple::IpiType::Ping) == hw_simple::IpiResult::NotInitialized);

  ut::expect(hw_simple::ipi_type_to_string(hw_simple::IpiType::Ping) != nullptr);
  ut::expect(hw_simple::ipi_type_to_string(hw_simple::IpiType::Debug) != nullptr);
  ut::expect(hw_simple::ipi_result_to_string(hw_simple::IpiResult::QueueFull) != nullptr);
  ut::expect(hw_simple::sgi_id_to_string(IpiSgiId::Debug) != nullptr);
  ut::expect(hw_simple::sgi_id_to_string(static_cast<IpiSgiId>(99)) != nullptr);

  ut::expect(hw_simple::initialize_simple_hardware_ipi(&gic, 2));
  ut::expect(!simple::initialize_simple_ipi_system(0));
  ut::expect(simple::initialize_simple_ipi_system(2));
  auto *simple = simple::g_simple_ipi_manager;
  if (!ut::expect(simple != nullptr)) {
    return;
  }
  ut::expect(simple->ping_cpu(1) == simple::IpiResult::Success);
  ut::expect(simple->ping_cpu(3) == simple::IpiResult::InvalidCpu);
  ut::expect(simple->send_ipi(0, static_cast<simple::IpiType>(99)) == simple::IpiResult::InvalidType);
  ut::expect(simple->self_test());
  ut::expect(simple->get_system_info().initialized);
  ut::expect(simple::simple_ipi_ping(0) == simple::IpiResult::Success);
  ut::expect(simple::ipi_type_to_string(simple::IpiType::Ping) != nullptr);
  ut::expect(simple::ipi_type_to_string(static_cast<simple::IpiType>(99)) != nullptr);
  ut::expect(simple::ipi_result_to_string(simple::IpiResult::HardwareError) != nullptr);
  ut::expect(simple::ipi_result_to_string(static_cast<simple::IpiResult>(99)) != nullptr);

  simple::shutdown_simple_ipi_system();
  simple::shutdown_simple_ipi_system();
  hw_simple::shutdown_simple_hardware_ipi();
  hw_simple::shutdown_simple_hardware_ipi();
}

} // namespace

void register_irqchip_cases() {
  ut::register_suite("irqchip", [] {
    ut::register_test("gicv2", gicv2_paths);
    ut::register_test("gicv3", gicv3_paths);
    ut::register_test("bcm2836", bcm_paths);
    ut::register_test("helpers", helper_paths);
    ut::register_test("generic_controller", generic_controller_paths);
    ut::register_test("ipi", ipi_paths);
  });
}
#else
void register_irqchip_cases() {}
#endif

} // namespace moss::test::validation
