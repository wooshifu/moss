import moss.std;
import moss.types;
import moss.abi;
import moss.boot;
import moss.mm;
import moss.timer;
import moss.drivers;
import moss.smart_ptr;
import moss.platform;
import moss.interrupts;
import moss.drivers.console;
import moss.result;

#include "validation_internal.hpp"

using namespace moss::kernel;
namespace ut = boost::ut;

namespace moss::test::validation {
namespace driver_tests {
using namespace drivers;
constexpr auto allocate = moss::abi::bridge::moss_heap_allocate;
struct Counts {
  unsigned probes{0}, removes{0}, adopted{0};
};
class MockDriver final : public Driver {
  Counts &counts_;
  DeviceManager &manager_;
  bool fail_;
  void *resource_{nullptr};

public:
  MockDriver(Counts &counts, DeviceManager &manager, bool fail = false)
      : Driver("mock", DeviceType::UART, HardwareId::Pl011, "test,uart"), counts_(counts), manager_(manager),
        fail_(fail) {}
  VoidResult probe(Device &device, BindMode mode) noexcept override {
    ++counts_.probes;
    counts_.adopted += mode == BindMode::AdoptBoot ? 1U : 0U;
    ut::expect(manager_.get_device(device.device_id()).get() == &device);
    ut::expect(manager_.bind_device(device.device_id()).error() == ErrorCode::InvalidState);
    ut::expect(manager_.unregister_device(device.device_id()).error() ==
               (mode == BindMode::AdoptBoot ? ErrorCode::PermissionDenied : ErrorCode::ResourceBusy));
    if (mode == BindMode::AdoptBoot) {
      return VoidResult{};
    }
    // A small arbitrary allocation stands in for probe-owned driver storage;
    // no device layout depends on its 64-byte size, only acquire/release does.
    auto allocation = mm::RuntimeHeapAllocator::allocate(64);
    if (!allocation) {
      return VoidResult{ErrorCode::OutOfMemory};
    }
    resource_ = *allocation;
    if (fail_) {
      ut::expect(mm::RuntimeHeapAllocator::deallocate(resource_, 0).has_value());
      resource_ = nullptr;
      return VoidResult{ErrorCode::DeviceError};
    }
    return VoidResult{};
  }
  void remove(Device & /*device*/) noexcept override {
    ++counts_.removes;
    (void)manager_.get_statistics();
    if (resource_) {
      ut::expect(mm::RuntimeHeapAllocator::deallocate(resource_, 0).has_value());
      resource_ = nullptr;
    }
  }
};
auto device(const char *name = "mock-uart", BindMode mode = BindMode::Initialize) {
  return shared_ptr<Device>::try_make(allocate, 0U, DeviceType::UART, name, "test,uart", HardwareId::Pl011, mode);
}
void registration() {
  const bool orders[] = {false, true};
  for (bool driver_first : orders) {
    Counts counts;
    DeviceManager manager;
    auto uart = device();
    auto driver = shared_ptr<Driver>::try_make<MockDriver>(allocate, counts, manager);
    if (driver_first) {
      ut::expect(manager.register_driver(driver).has_value());
    }
    auto id = manager.register_device(uart);
    if (!ut::expect(id.has_value())) {
      return;
    }
    ut::expect(*id != 0 && uart->device_id() == *id);
    char name[] = "mock-uart";
    ut::expect(manager.get_device_by_name(name).get() == uart.get());
    if (!driver_first) {
      ut::expect(uart->state() == DeviceState::Uninitialized);
      ut::expect(manager.register_driver(driver).has_value());
    }
    ut::expect(uart->state() == DeviceState::Active && counts.probes == 1);
    ut::expect(manager.register_device(uart).error() == ErrorCode::AlreadyExists);
    ut::expect(manager.register_device(device()).error() == ErrorCode::AlreadyExists);
    ut::expect(manager.register_driver(driver).error() == ErrorCode::AlreadyExists);
    ut::expect(manager.bind_device(*id).error() == ErrorCode::InvalidState);
    ut::expect(counts.probes == 1);
    ut::expect(manager.suspend_all_devices().has_value() && uart->state() == DeviceState::Suspended);
    ut::expect(manager.resume_all_devices().has_value() && uart->state() == DeviceState::Active);
    const auto stats = manager.get_statistics();
    ut::expect(stats.total_devices == 1 && stats.active_devices == 1 && stats.registered_drivers == 1);
    ut::expect(manager.unregister_device(*id).has_value());
    ut::expect(manager.unregister_device(*id).error() == ErrorCode::NotFound);
    ut::expect(counts.removes == 1 && !manager.get_device(*id) && !manager.get_device_by_name(name));
    ut::expect(uart->state() == DeviceState::Removed);
  }
}
void matching_failure() {
  Counts counts;
  DeviceManager manager;
  ut::expect(
      manager.register_driver(shared_ptr<Driver>::try_make<MockDriver>(allocate, counts, manager, true)).has_value());
  auto wrong_type =
      shared_ptr<Device>::try_make(allocate, 0U, DeviceType::Timer, "wrong-type", "test,uart", HardwareId::Pl011);
  auto wrong_chip =
      shared_ptr<Device>::try_make(allocate, 0U, DeviceType::UART, "wrong-chip", "test,uart", HardwareId::Ns16550);
  auto wrong_compatible =
      shared_ptr<Device>::try_make(allocate, 0U, DeviceType::UART, "wrong-compatible", "test,other", HardwareId::Pl011);
  ut::expect(manager.register_device(wrong_type).has_value());
  ut::expect(manager.register_device(wrong_chip).has_value());
  ut::expect(manager.register_device(wrong_compatible).has_value());
  ut::expect(counts.probes == 0);
  auto uart = device();
  const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
  auto id = manager.register_device(uart);
  if (!ut::expect(id.has_value())) {
    return;
  }
  ut::expect(uart->state() == DeviceState::Error && uart->probe_error() == ErrorCode::DeviceError);
  ut::expect(counts.probes == 1 && manager.get_statistics().active_devices == 0);
  ut::expect(manager.unregister_device(*id).has_value());
  ut::expect(counts.removes == 0);
  ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
}
void ownership() {
  Counts counts;
  {
    DeviceManager manager;
    auto driver = shared_ptr<Driver>::try_make<MockDriver>(allocate, counts, manager);
    ut::expect(manager.register_driver(driver).has_value());
    driver.reset();
    ut::expect(manager.register_device(device()).has_value());
  }
  ut::expect(counts.probes == 1 && counts.removes == 1);
  Counts boot_counts;
  auto boot = device("boot-uart", BindMode::AdoptBoot);
  {
    DeviceManager manager;
    ut::expect(
        manager.register_driver(shared_ptr<Driver>::try_make<MockDriver>(allocate, boot_counts, manager)).has_value());
    auto id = manager.register_device(boot);
    if (!ut::expect(id.has_value())) {
      return;
    }
    ut::expect(manager.unregister_device(*id).error() == ErrorCode::PermissionDenied);
    ut::expect(manager.suspend_all_devices().error() == ErrorCode::PermissionDenied);
  }
  ut::expect(boot_counts.probes == 1 && boot_counts.adopted == 1 && boot_counts.removes == 0);
}
void registration_rollback() {
  Counts counts;
  DeviceManager manager;
  auto uart = device();
  auto driver = shared_ptr<Driver>::try_make<MockDriver>(allocate, counts, manager);
  {
    HeapPressure pressure;
    if (!ut::expect(pressure.acquire(sizeof(void *)))) {
      return;
    }
    ut::expect(manager.register_driver(driver).error() == ErrorCode::OutOfMemory);
    ut::expect(manager.register_device(uart).error() == ErrorCode::OutOfMemory);
    const auto stats = manager.get_statistics();
    ut::expect(stats.total_devices == 0 && stats.registered_drivers == 0 && uart->device_id() == 0);
  }
  ut::expect(manager.register_driver(driver).has_value());
  ut::expect(manager.register_device(uart).has_value());
  ut::expect(counts.probes == 1 && uart->state() == DeviceState::Active);
}
void irq_registration_rollback() {
  auto *controller = moss::boot::g_gic_controller;
  if (!ut::expect(controller != nullptr)) {
    return;
  }
  // The supported profiles leave the source beside their console UART unused.
  // Never enable it: live bootstrap timer, UART and IPI routing stay untouched.
  const u32 irq = platform::hardware.uart.irq + 1;
  if (!ut::expect(!controller->get_interrupt_info(irq))) {
    return;
  }
  const auto registered = controller->get_statistics().registered_interrupts;
  bool recovered = false;
  unsigned failures = 0;
  {
    HeapPressure pressure;
    if (!ut::expect(pressure.acquire(sizeof(void *)))) {
      return;
    }
    for (;;) {
      const auto heap = mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes;
      auto result = controller->register_interrupt(irq, +[](u32, void *) noexcept {}, nullptr, "allocation-check");
      if (result) {
        recovered = true;
        ut::expect(static_cast<bool>(controller->get_interrupt_info(irq)));
        ut::expect(controller->unregister_interrupt(irq).has_value());
      } else {
        ++failures;
        ut::expect(result.error() == ErrorCode::OutOfMemory && !controller->get_interrupt_info(irq));
      }
      ut::expect(mm::RuntimeHeapAllocator::get_heap_stats().allocated_bytes == heap);
      ut::expect(controller->get_statistics().registered_interrupts == registered);
      if (recovered || !ut::expect(pressure.release_one())) {
        break;
      }
    }
  }
  ut::expect(failures > 0 && recovered);
}
void console_ring() {
  drivers::console::RxRing ring;
  // Eight rounds provide bounded wrap/reuse coverage, not a timing guarantee.
  // RxRing reserves one of its 256 slots to distinguish full from empty, so
  // each round must accept exactly 255 bytes and reject the following byte.
  for (unsigned round = 0; round < 8; ++round) {
    ut::expect(ring.empty() && ring.get() == -1);
    for (unsigned i = 0; i < 255; ++i) {
      ut::expect(ring.put(static_cast<u8>(i)));
    }
    ut::expect(!ring.put(255));
    for (unsigned i = 0; i < 255; ++i) {
      ut::expect(ring.get() == static_cast<int>(i));
    }
  }
  auto *manager = drivers::g_device_manager;
  if (!ut::expect(manager != nullptr)) {
    return;
  }
  const auto stats = manager->get_statistics();
  // The bootstrap registry contains irqchip, timer and console (listed below).
  ut::expect(stats.total_devices == 3 && stats.active_devices == 3);
  auto *controller = moss::boot::g_gic_controller;
  const auto clock = moss::kernel::timer::TimerSubsystem::instance().clocksource().frequency_hz();
  ut::expect(drivers::console::initialize().has_value());
  moss::abi::bridge::console_rx_init();
  ut::expect(drivers::console::is_initialized());
  ut::expect(moss::boot::g_gic_controller == controller);
  ut::expect(moss::kernel::timer::TimerSubsystem::instance().clocksource().frequency_hz() == clock);
  const char *names[] = {"irqchip", "timer", "console"};
  for (const char *name : names) {
    auto boot = manager->get_device_by_name(name);
    if (!ut::expect(static_cast<bool>(boot))) {
      return;
    }
    ut::expect(boot->state() == DeviceState::Active && boot->bind_mode() == BindMode::AdoptBoot);
    ut::expect(manager->unregister_device(boot->device_id()).error() == ErrorCode::PermissionDenied);
  }
}
} // namespace driver_tests

void register_driver_cases() {
  ut::register_suite("drivers", [] {
    ut::register_test("registration", driver_tests::registration);
    ut::register_test("matching_failure", driver_tests::matching_failure);
    ut::register_test("ownership", driver_tests::ownership);
    ut::register_test("registration_rollback", driver_tests::registration_rollback);
    ut::register_test("irq_registration_rollback", driver_tests::irq_registration_rollback);
    ut::register_test("console_ring", driver_tests::console_ring);
  });
}
} // namespace moss::test::validation
