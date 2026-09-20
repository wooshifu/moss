// MOSS Drivers Module - Device Management and Driver Framework
// Passive device descriptions and static built-in driver binding.

export module moss.drivers;

import moss.std;
import moss.types;
import moss.result;
import moss.smart_ptr;
import moss.arch;
import moss.containers;
import moss.interrupts;
import moss.abi;
import moss.platform;
import moss.timer;
import moss.drivers.console;

// ============================================================================
// Exported driver framework types and classes
// ============================================================================
export namespace moss::kernel::drivers {

// Re-export smart pointer types used by driver framework
using moss::kernel::make_shared;
using moss::kernel::make_unique;
using moss::kernel::shared_ptr;
using moss::kernel::unique_ptr;

// ========================================================================
// Device types and enums
// ========================================================================

enum class DeviceType : u8 {
  Unknown = 0,
  Block = 1,
  Character = 2,
  Network = 3,
  Input = 4,
  Display = 5,
  Audio = 6,
  GPIO = 7,
  I2C = 8,
  SPI = 9,
  UART = 10,
  Timer = 11,
  Clock = 12,
  Power = 13,
  Platform = 14,
  InterruptController = 15
};

enum class DeviceState : u8 { Uninitialized = 0, Initializing = 1, Active = 2, Suspended = 3, Error = 4, Removed = 5 };

enum class HardwareId : u8 {
  Unknown,
  Pl011,
  Ns16550,
  GicV2,
  GicV3,
  Bcm,
  Apic,
  Plic,
  ArmVirtualTimer,
  LapicTimer,
  SbiTimer
};
enum class BindMode : u8 { Initialize, AdoptBoot };
class Driver;
class DeviceManager;

// ========================================================================
// Device resource structures
// ========================================================================

struct DeviceProperty {
  // Property storage is borrowed (often firmware-backed); the caller must keep
  // both strings alive for as long as this device can be queried.
  const char *name;
  const char *value;
  usize value_size;

  DeviceProperty(const char *n, const char *v, usize size) noexcept : name(n), value(v), value_size(size) {}
};

struct DeviceMemoryInfo {
  PhysAddr start;
  PhysAddr end;
  VirtAddr mapped_addr;
};

struct DeviceIoInfo {
  u32 start;
  u32 end; // Exclusive end can be 0x10000 for a range ending at the last I/O port.
};

struct DeviceInterruptInfo {
  InterruptId irq;
  interrupts::TriggerType trigger;
};

struct DeviceDmaInfo {
  u32 channel;
  u32 request_line;
};

struct DeviceResource {
  enum Type { Memory = 0, IO = 1, IRQ = 2, DMA = 3 } type;

  union {
    DeviceMemoryInfo memory;
    DeviceIoInfo io;
    DeviceInterruptInfo interrupt;
    DeviceDmaInfo dma;
  };

  DeviceResource() noexcept : type(Memory) { memory = {.start = 0, .end = 0, .mapped_addr = 0}; }
};

// ========================================================================
// Device base class
// ========================================================================

class Device {
  friend class DeviceManager;
  DeviceManager *manager_{nullptr};
  shared_ptr<Driver> driver_;
  HardwareId hardware_id_;
  BindMode bind_mode_;
  u32 probe_error_{static_cast<u32>(ErrorCode::Success)};

protected:
  DeviceId device_id_;
  DeviceType type_;
  u8 state_;
  const char *name_;
  const char *compatible_;
  Device *parent_;
  containers::LockedList<Device *> children_;

  containers::LockedList<DeviceResource> resources_;
  containers::LockedList<DeviceProperty> properties_;

  u64 init_time_;
  u64 last_access_time_;
  u64 access_count_;

public:
  Device(DeviceId id, DeviceType type, const char *name, const char *compatible,
         HardwareId hardware_id = HardwareId::Unknown, BindMode mode = BindMode::Initialize) noexcept
      : hardware_id_(hardware_id), bind_mode_(mode), device_id_(id), type_(type),
        state_(static_cast<u8>(DeviceState::Uninitialized)), name_(name), compatible_(compatible), parent_(nullptr),
        init_time_(0), last_access_time_(0), access_count_(0) {}

  virtual ~Device() noexcept = default;

  Device(const Device &) = delete;
  Device &operator=(const Device &) = delete;
  Device(Device &&) = delete;
  Device &operator=(Device &&) = delete;

  // Basic property access
  [[nodiscard]] DeviceId device_id() const noexcept { return __atomic_load_n(&device_id_, __ATOMIC_ACQUIRE); }
  [[nodiscard]] DeviceType type() const noexcept { return type_; }
  [[nodiscard]] DeviceState state() const noexcept {
    return static_cast<DeviceState>(__atomic_load_n(&state_, __ATOMIC_ACQUIRE));
  }
  [[nodiscard]] const char *name() const noexcept { return name_; }
  [[nodiscard]] const char *compatible() const noexcept { return compatible_; }

  [[nodiscard]] HardwareId hardware_id() const noexcept { return hardware_id_; }
  [[nodiscard]] BindMode bind_mode() const noexcept { return bind_mode_; }
  [[nodiscard]] ErrorCode probe_error() const noexcept {
    return static_cast<ErrorCode>(__atomic_load_n(&probe_error_, __ATOMIC_ACQUIRE));
  }

  // Device tree relationships
  [[nodiscard]] Device *parent() const noexcept { return parent_; }
  void set_parent(Device *parent) noexcept { parent_ = parent; }
  void add_child(Device *child) noexcept {
    child->set_parent(this);
    children_.push_front(child);
  }

  // Resource management
  [[nodiscard]] bool add_resource(const DeviceResource &resource) noexcept {
    return resources_.try_push_front(resource);
  }

  [[nodiscard]] containers::Optional<DeviceResource> get_resource(DeviceResource::Type type,
                                                                  usize index = 0) const noexcept {
    usize found_count = 0;
    containers::Optional<DeviceResource> found;

    resources_.for_each([type, index, &found_count, &found](const DeviceResource &res) {
      if (!found && res.type == type) {
        if (found_count++ == index) {
          found = res;
        }
      }
    });

    return found;
  }

  // Property management
  void add_property(const char *name, const char *value, usize value_size) noexcept {
    properties_.push_front(DeviceProperty(name, value, value_size));
  }

  [[nodiscard]] const char *get_property(const char *name) const noexcept {
    auto found = properties_.find_if(
        [name](const DeviceProperty &prop) { return moss::abi::bridge::strcmp(prop.name, name) == 0; });

    return found ? found->value : nullptr;
  }

private:
  void set_state(DeviceState new_state) noexcept {
    if (new_state == DeviceState::Active) {
      init_time_ = get_current_time();
    }
    __atomic_store_n(&state_, static_cast<u8>(new_state), __ATOMIC_RELEASE);
  }

public:
  // Statistics
  void update_access() noexcept {
    last_access_time_ = get_current_time();
    access_count_++;
  }

  struct DeviceStats {
    u64 init_time;
    u64 last_access_time;
    u64 access_count;
  };

  [[nodiscard]] DeviceStats get_statistics() const noexcept {
    return {.init_time = init_time_, .last_access_time = last_access_time_, .access_count = access_count_};
  }

protected:
  [[nodiscard]] static u64 get_current_time() noexcept { return arch::get_timestamp_counter(); }
};

class Driver {
  const char *name_;
  DeviceType type_;
  HardwareId hardware_id_;
  const char *compatible_;

public:
  Driver(const char *name, DeviceType type, HardwareId hardware_id, const char *compatible = nullptr) noexcept
      : name_(name), type_(type), hardware_id_(hardware_id), compatible_(compatible) {}
  virtual ~Driver() noexcept = default;
  Driver(const Driver &) = delete;
  Driver &operator=(const Driver &) = delete;
  [[nodiscard]] const char *name() const noexcept { return name_; }
  [[nodiscard]] bool matches(const Device &device) const noexcept {
    return type_ == device.type() && hardware_id_ == device.hardware_id() &&
           (!compatible_ || (device.compatible() && moss::abi::bridge::strcmp(compatible_, device.compatible()) == 0));
  }
  // A failed probe must release only resources it acquired, preserving borrowed boot state.
  [[nodiscard]] virtual VoidResult probe(Device &device, BindMode mode) noexcept = 0;
  virtual void remove(Device &device) noexcept = 0;
  [[nodiscard]] virtual VoidResult suspend(Device &) noexcept { return VoidResult{}; }
  [[nodiscard]] virtual VoidResult resume(Device &) noexcept { return VoidResult{}; }
};

class DeviceManager {
  // ponytail: linear scans suit the three boot devices; add an index when device counts grow.
  containers::LockedList<shared_ptr<Device>> devices_;
  containers::LockedList<shared_ptr<Driver>> drivers_;
  mutable containers::IrqSpinLock registry_lock_;
  DeviceId next_id_{1};

  [[nodiscard]] shared_ptr<Device> next_device(DeviceId after) const noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(registry_lock_);
    shared_ptr<Device> result;
    devices_.for_each([&](const shared_ptr<Device> &device) {
      if (device->device_id() > after && (!result || device->device_id() < result->device_id()))
        result = device;
    });
    return result;
  }

public:
  DeviceManager() noexcept = default;
  DeviceManager(const DeviceManager &) = delete;
  DeviceManager &operator=(const DeviceManager &) = delete;
  ~DeviceManager() noexcept {
    // Destruction requires callers to have stopped registry operations. Boot devices
    // borrow hardware that still serves scheduling and logging during shutdown.
    for (DeviceId after = 0;;) {
      auto device = next_device(after);
      if (!device)
        break;
      after = device->device_id();
      if (device->driver_ && device->bind_mode() != BindMode::AdoptBoot)
        device->driver_->remove(*device);
      device->driver_.reset();
      device->set_state(DeviceState::Removed);
      __atomic_store_n(&device->manager_, static_cast<DeviceManager *>(nullptr), __ATOMIC_RELEASE);
    }
    devices_.clear();
    drivers_.clear();
  }

  [[nodiscard]] KernelResult<DeviceId> register_device(shared_ptr<Device> device) noexcept {
    if (!device || !device->name())
      return KernelResult<DeviceId>{Err<ErrorCode>(ErrorCode::InvalidParameter)};
    {
      containers::LockGuard<containers::IrqSpinLock> guard(registry_lock_);
      DeviceManager *expected = nullptr;
      if (!__atomic_compare_exchange_n(&device->manager_, &expected, this, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return KernelResult<DeviceId>{Err<ErrorCode>(ErrorCode::AlreadyExists)};
      auto duplicate = devices_.find_if([&](const shared_ptr<Device> &other) {
        return moss::abi::bridge::strcmp(other->name(), device->name()) == 0;
      });
      if (duplicate || !devices_.try_push_front(device)) {
        __atomic_store_n(&device->manager_, static_cast<DeviceManager *>(nullptr), __ATOMIC_RELEASE);
        return KernelResult<DeviceId>{Err<ErrorCode>(duplicate ? ErrorCode::AlreadyExists : ErrorCode::OutOfMemory)};
      }
      __atomic_store_n(&device->device_id_, next_id_++, __ATOMIC_RELEASE);
      device->set_state(DeviceState::Uninitialized);
    }
    (void)bind_device(device->device_id());
    return KernelResult<DeviceId>{device->device_id()};
  }

  [[nodiscard]] VoidResult register_driver(shared_ptr<Driver> driver) noexcept {
    if (!driver || !driver->name())
      return VoidResult{ErrorCode::InvalidParameter};
    {
      containers::LockGuard<containers::IrqSpinLock> guard(registry_lock_);
      if (drivers_.find_if([&](const shared_ptr<Driver> &other) {
            return other.get() == driver.get() || moss::abi::bridge::strcmp(other->name(), driver->name()) == 0;
          }))
        return VoidResult{ErrorCode::AlreadyExists};
      if (!drivers_.try_push_front(driver))
        return VoidResult{ErrorCode::OutOfMemory};
    }
    for (DeviceId after = 0;;) {
      auto device = next_device(after);
      if (!device)
        break;
      after = device->device_id();
      if (device->state() == DeviceState::Uninitialized)
        (void)bind_device(after);
    }
    return VoidResult{};
  }

  [[nodiscard]] VoidResult bind_device(DeviceId id) noexcept {
    shared_ptr<Device> device;
    shared_ptr<Driver> driver;
    {
      containers::LockGuard<containers::IrqSpinLock> guard(registry_lock_);
      auto found = devices_.find_if([&](const shared_ptr<Device> &item) { return item->device_id() == id; });
      if (!found)
        return VoidResult{ErrorCode::NotFound};
      device = *found;
      if (device->state() != DeviceState::Uninitialized)
        return VoidResult{ErrorCode::InvalidState};
      auto matched = drivers_.find_if([&](const shared_ptr<Driver> &item) { return item->matches(*device); });
      if (!matched)
        return VoidResult{ErrorCode::NotFound};
      driver = *matched;
      device->set_state(DeviceState::Initializing);
    }
    // Claim the transition under lock; callbacks may allocate, sleep or inspect the registry.
    auto result = driver->probe(*device, device->bind_mode());
    {
      containers::LockGuard<containers::IrqSpinLock> guard(registry_lock_);
      __atomic_store_n(&device->probe_error_, static_cast<u32>(result ? ErrorCode::Success : result.error()),
                       __ATOMIC_RELEASE);
      if (result)
        device->driver_ = driver;
      device->set_state(result ? DeviceState::Active : DeviceState::Error);
    }
    return result;
  }

  [[nodiscard]] VoidResult unregister_device(DeviceId id) noexcept {
    shared_ptr<Device> device;
    shared_ptr<Driver> driver;
    {
      containers::LockGuard<containers::IrqSpinLock> guard(registry_lock_);
      auto found = devices_.find_if([&](const shared_ptr<Device> &item) { return item->device_id() == id; });
      if (!found)
        return VoidResult{ErrorCode::NotFound};
      device = *found;
      if (device->bind_mode() == BindMode::AdoptBoot)
        return VoidResult{ErrorCode::PermissionDenied};
      if (device->state() == DeviceState::Initializing)
        return VoidResult{ErrorCode::ResourceBusy};
      device->set_state(DeviceState::Initializing);
      driver = device->driver_;
    }
    if (driver)
      driver->remove(*device);
    {
      containers::LockGuard<containers::IrqSpinLock> guard(registry_lock_);
      devices_.remove_if([&](const shared_ptr<Device> &item) { return item.get() == device.get(); });
      device->driver_.reset();
      device->set_state(DeviceState::Removed);
      __atomic_store_n(&device->manager_, static_cast<DeviceManager *>(nullptr), __ATOMIC_RELEASE);
    }
    return VoidResult{};
  }

  [[nodiscard]] shared_ptr<Device> get_device(DeviceId id) const noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(registry_lock_);
    auto found = devices_.find_if([&](const shared_ptr<Device> &item) { return item->device_id() == id; });
    return found ? *found : shared_ptr<Device>{};
  }
  [[nodiscard]] shared_ptr<Device> get_device_by_name(const char *name) const noexcept {
    if (!name)
      return {};
    containers::LockGuard<containers::IrqSpinLock> guard(registry_lock_);
    auto found = devices_.find_if(
        [&](const shared_ptr<Device> &item) { return moss::abi::bridge::strcmp(item->name(), name) == 0; });
    return found ? *found : shared_ptr<Device>{};
  }
  struct DeviceManagerStats {
    u32 total_devices;
    u32 active_devices;
    u32 registered_drivers;
  };
  [[nodiscard]] DeviceManagerStats get_statistics() const noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(registry_lock_);
    DeviceManagerStats stats{static_cast<u32>(devices_.size()), 0, static_cast<u32>(drivers_.size())};
    devices_.for_each([&](const shared_ptr<Device> &device) {
      if (device->state() == DeviceState::Active)
        ++stats.active_devices;
    });
    return stats;
  }
  void list_devices(void (*callback)(const Device &, void *), void *context) const noexcept {
    for (DeviceId after = 0;;) {
      auto device = next_device(after);
      if (!device)
        break;
      after = device->device_id();
      callback(*device, context);
    }
  }
  [[nodiscard]] VoidResult suspend_all_devices() noexcept { return change_power_state(false); }
  [[nodiscard]] VoidResult resume_all_devices() noexcept { return change_power_state(true); }

private:
  [[nodiscard]] VoidResult change_power_state(bool resume) noexcept {
    for (DeviceId after = 0;;) {
      auto device = next_device(after);
      if (!device)
        break;
      after = device->device_id();
      shared_ptr<Driver> driver;
      {
        containers::LockGuard<containers::IrqSpinLock> guard(registry_lock_);
        if (__atomic_load_n(&device->manager_, __ATOMIC_ACQUIRE) != this)
          continue;
        if (device->bind_mode() == BindMode::AdoptBoot)
          return VoidResult{ErrorCode::PermissionDenied};
        if (device->state() != (resume ? DeviceState::Suspended : DeviceState::Active))
          continue;
        driver = device->driver_;
        device->set_state(DeviceState::Initializing);
      }
      auto result = resume ? driver->resume(*device) : driver->suspend(*device);
      {
        containers::LockGuard<containers::IrqSpinLock> guard(registry_lock_);
        device->set_state(result ? (resume ? DeviceState::Active : DeviceState::Suspended)
                                 : (resume ? DeviceState::Suspended : DeviceState::Active));
      }
      if (!result)
        return result;
    }
    return VoidResult{};
  }
};

inline DeviceManager *g_device_manager = nullptr;

// These drivers adopt the selected platform resources, rather than inventing
// firmware-compatible strings for ACPI or architectural devices.
class BootDriver final : public Driver {
  interrupts::GenericInterruptController *controller_;

public:
  BootDriver(const char *name, DeviceType type, HardwareId id, interrupts::GenericInterruptController *controller)
      : Driver(name, type, id), controller_(controller) {}
  VoidResult probe(Device &device, BindMode mode) noexcept override {
    if (mode != BindMode::AdoptBoot || !controller_)
      return VoidResult{ErrorCode::InvalidState};
    if (device.type() == DeviceType::UART)
      return console::initialize();
    if (device.type() == DeviceType::Timer && !timer::TimerSubsystem::instance().is_initialized())
      return VoidResult{ErrorCode::InvalidState};
    return VoidResult{};
  }
  void remove(Device &) noexcept override {}
};

[[nodiscard]] inline VoidResult register_boot_devices(DeviceManager &manager,
                                                      interrupts::GenericInterruptController *controller) noexcept {
#if defined(MOSS_ARCH_ARM64)
  const auto chip = platform::hardware.intc.gic_version == 1   ? HardwareId::Bcm
                    : platform::hardware.intc.gic_version == 3 ? HardwareId::GicV3
                                                               : HardwareId::GicV2;
  const auto clock = HardwareId::ArmVirtualTimer;
#elif defined(MOSS_ARCH_X64)
  const auto chip = HardwareId::Apic;
  const auto clock = HardwareId::LapicTimer;
#else
  const auto chip = HardwareId::Plic;
  const auto clock = HardwareId::SbiTimer;
#endif
  const auto serial =
      platform::hardware.uart.kind == platform::UartKind::Pl011 ? HardwareId::Pl011 : HardwareId::Ns16550;
  const DeviceType types[] = {DeviceType::InterruptController, DeviceType::Timer, DeviceType::UART};
  const HardwareId ids[] = {chip, clock, serial};
  const char *names[] = {"irqchip", "timer", "console"};
  const usize count = platform::hardware.uart.valid ? 3 : 2;
  for (usize i = 0; i < count; ++i) {
    auto driver = shared_ptr<Driver>::try_make<BootDriver>(moss::abi::bridge::moss_heap_allocate, names[i], types[i],
                                                           ids[i], controller);
    if (!driver)
      return VoidResult{ErrorCode::OutOfMemory};
    auto registered = manager.register_driver(driver);
    if (!registered)
      return registered;
  }
  for (usize i = 0; i < count; ++i) {
    auto device = shared_ptr<Device>::try_make(moss::abi::bridge::moss_heap_allocate, 0U, types[i], names[i], nullptr,
                                               ids[i], BindMode::AdoptBoot);
    if (!device)
      return VoidResult{ErrorCode::OutOfMemory};
    DeviceResource resource;
    if (i == 0) {
      const auto &chip_info = platform::hardware.intc;
      const PhysAddr bases[] = {chip_info.dist_base, chip_info.cpu_base, chip_info.redist_base};
      u64 sizes[] = {chip_info.dist_size, chip_info.cpu_size, chip_info.redist_size};
#if defined(MOSS_ARCH_X64)
      // The existing xAPIC profile maps each register bank as one 4 KiB MMIO page.
      sizes[0] = sizes[1] = PAGE_SIZE;
#endif
      for (usize bank = 0; bank < 3; ++bank) {
        if (!bases[bank] || !sizes[bank])
          continue;
        resource.type = DeviceResource::Memory;
        resource.memory = {bases[bank], bases[bank] + sizes[bank], bases[bank]};
        if (!device->add_resource(resource))
          return VoidResult{ErrorCode::OutOfMemory};
      }
    } else if (i == 2) {
      const auto &uart = platform::hardware.uart;
      resource.type = uart.port_io ? DeviceResource::IO : DeviceResource::Memory;
      if (resource.type == DeviceResource::IO) {
        resource.io = {static_cast<u32>(uart.base_addr), static_cast<u32>(uart.base_addr + uart.size)};
      } else {
        resource.memory = {uart.base_addr, uart.base_addr + uart.size, uart.base_addr};
      }
      if (!device->add_resource(resource))
        return VoidResult{ErrorCode::OutOfMemory};
    }
    if (i != 0) {
      resource.type = DeviceResource::IRQ;
      resource.interrupt = {i == 1 ? platform::timer_irq() : platform::hardware.uart.irq,
                            interrupts::TriggerType::LevelHigh};
#if defined(MOSS_ARCH_X64)
      resource.interrupt.trigger = interrupts::TriggerType::EdgeRising;
      if (i == 2) {
        // MADT override bits 0..1 encode polarity; bits 2..3 encode trigger mode.
        const u16 flags = platform::hardware.isa_flags[platform::hardware.uart.irq];
        const bool level = (flags & 12) == 12, low = (flags & 3) == 3;
        resource.interrupt.trigger =
            level ? (low ? interrupts::TriggerType::LevelLow : interrupts::TriggerType::LevelHigh)
                  : (low ? interrupts::TriggerType::EdgeFalling : interrupts::TriggerType::EdgeRising);
      }
#endif
      if (!device->add_resource(resource))
        return VoidResult{ErrorCode::OutOfMemory};
    }
    auto registered = manager.register_device(device);
    if (!registered)
      return VoidResult{registered.error()};
    if (device->state() != DeviceState::Active)
      return VoidResult{device->probe_error()};
  }
  return VoidResult{};
}
} // namespace moss::kernel::drivers
