// MOSS Drivers Module - Device Management and Driver Framework
// Provides device tree, driver matching, lifecycle management, and UART driver.

module;

// Macro for disabling copy and move (macros do not cross module boundaries)
#define NON_COPYABLE(ClassName)                                                \
  ClassName(const ClassName &) = delete;                                       \
  ClassName &operator=(const ClassName &) = delete;

#define NON_MOVABLE(ClassName)                                                 \
  ClassName(ClassName &&) = delete;                                            \
  ClassName &operator=(ClassName &&) = delete;

#define NON_COPYABLE_NON_MOVABLE(ClassName)                                    \
  NON_COPYABLE(ClassName)                                                      \
  NON_MOVABLE(ClassName)

export module moss.drivers;

import moss.std;
import moss.types;
import moss.result;
import moss.smart_ptr;
import moss.arch;
import moss.containers;
import moss.interrupts;
import moss.abi;

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
  Platform = 14
};

enum class DeviceState : u8 {
  Uninitialized = 0,
  Initializing = 1,
  Active = 2,
  Suspended = 3,
  Error = 4,
  Removed = 5
};

// ========================================================================
// Device resource structures
// ========================================================================

struct DeviceProperty {
  const char *name;
  const char *value;
  usize value_size;

  DeviceProperty(const char *n, const char *v, usize size) noexcept
      : name(n), value(v), value_size(size) {}
};

struct DeviceMemoryInfo {
  PhysAddr start;
  PhysAddr end;
  VirtAddr mapped_addr;
};

struct DeviceIoInfo {
  u16 start;
  u16 end;
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

  DeviceResource() noexcept : type(Memory) { memory = {0, 0, 0}; }
};

// ========================================================================
// Device base class
// ========================================================================

class Device {
protected:
  DeviceId device_id_;
  DeviceType type_;
  DeviceState state_;
  const char *name_;
  const char *compatible_;
  Device *parent_;
  containers::RcuList<Device *> children_;

  containers::RcuList<DeviceResource> resources_;
  containers::RcuList<DeviceProperty> properties_;

  u64 init_time_;
  u64 last_access_time_;
  u64 access_count_;

public:
  Device(DeviceId id, DeviceType type, const char *name,
         const char *compatible) noexcept
      : device_id_(id), type_(type), state_(DeviceState::Uninitialized),
        name_(name), compatible_(compatible), parent_(nullptr), init_time_(0),
        last_access_time_(0), access_count_(0) {}

  virtual ~Device() noexcept = default;

  NON_COPYABLE_NON_MOVABLE(Device)

  // Device lifecycle interface
  [[nodiscard]] virtual VoidResult initialize() noexcept = 0;
  [[nodiscard]] virtual VoidResult suspend() noexcept { return VoidResult{}; }
  [[nodiscard]] virtual VoidResult resume() noexcept { return VoidResult{}; }
  virtual void shutdown() noexcept {}

  // Basic property access
  [[nodiscard]] DeviceId device_id() const noexcept { return device_id_; }
  [[nodiscard]] DeviceType type() const noexcept { return type_; }
  [[nodiscard]] DeviceState state() const noexcept { return state_; }
  [[nodiscard]] const char *name() const noexcept { return name_; }
  [[nodiscard]] const char *compatible() const noexcept { return compatible_; }

  // Device tree relationships
  [[nodiscard]] Device *parent() const noexcept { return parent_; }
  void set_parent(Device *parent) noexcept { parent_ = parent; }
  void add_child(Device *child) noexcept {
    child->set_parent(this);
    children_.push_front(child);
  }

  // Resource management
  void add_resource(const DeviceResource &resource) noexcept {
    resources_.push_front(resource);
  }

  [[nodiscard]] containers::Optional<DeviceResource>
  get_resource(DeviceResource::Type type, usize index = 0) const noexcept {
    usize found_count = 0;
    const DeviceResource *found = nullptr;

    resources_.for_each(
        [type, index, &found_count, &found](const DeviceResource &res) {
          if (res.type == type) {
            if (found_count == index) {
              found = &res;
              return;
            }
            found_count++;
          }
        });

    return found ? containers::Optional<DeviceResource>{*found}
                 : containers::Optional<DeviceResource>{};
  }

  // Property management
  void add_property(const char *name, const char *value,
                    usize value_size) noexcept {
    properties_.push_front(DeviceProperty(name, value, value_size));
  }

  [[nodiscard]] const char *get_property(const char *name) const noexcept {
    const DeviceProperty *found =
        properties_.find_if([name](const DeviceProperty &prop) {
          return moss::abi::bridge::strcmp(prop.name, name) == 0;
        });

    return found ? found->value : nullptr;
  }

  // State management
  void set_state(DeviceState new_state) noexcept {
    state_ = new_state;
    if (new_state == DeviceState::Active) {
      init_time_ = get_current_time();
    }
  }

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
    return {init_time_, last_access_time_, access_count_};
  }

protected:
  [[nodiscard]] static u64 get_current_time() noexcept {
    return arch::get_timestamp_counter();
  }
};

// ========================================================================
// Driver base class
// ========================================================================

class Driver {
protected:
  const char *name_;
  const char *version_;
  const char **compatible_list_;
  usize compatible_count_;

public:
  Driver(const char *name, const char *version, const char **compatible_list,
         usize compatible_count) noexcept
      : name_(name), version_(version), compatible_list_(compatible_list),
        compatible_count_(compatible_count) {}

  virtual ~Driver() noexcept = default;

  NON_COPYABLE_NON_MOVABLE(Driver)

  // Driver interface
  [[nodiscard]] virtual VoidResult probe(Device *device) noexcept = 0;
  virtual void remove(Device *device) noexcept = 0;

  // Power management
  [[nodiscard]] virtual VoidResult
  suspend([[maybe_unused]] Device *device) noexcept {
    return VoidResult{};
  }
  [[nodiscard]] virtual VoidResult
  resume([[maybe_unused]] Device *device) noexcept {
    return VoidResult{};
  }

  // Basic properties
  [[nodiscard]] const char *name() const noexcept { return name_; }
  [[nodiscard]] const char *version() const noexcept { return version_; }

  // Device matching
  [[nodiscard]] bool
  is_compatible(const char *device_compatible) const noexcept {
    for (usize i = 0; i < compatible_count_; ++i) {
      if (moss::abi::bridge::strcmp(compatible_list_[i], device_compatible) == 0) {
        return true;
      }
    }
    return false;
  }

};

// ========================================================================
// Device Manager
// ========================================================================

class DeviceManager {
private:
  containers::RcuHashMap<DeviceId, shared_ptr<Device>> devices_;
  containers::RcuHashMap<const char *, DeviceId> device_name_map_;
  containers::RcuList<Driver *> drivers_;
  containers::RcuHashMap<DeviceId, Driver *> device_driver_map_;

  containers::AtomicCounter<DeviceId> next_device_id_;
  containers::AtomicCounter<u32> total_devices_;
  containers::AtomicCounter<u32> active_devices_;
  containers::AtomicCounter<u32> registered_drivers_;

public:
  DeviceManager() noexcept
      : next_device_id_(1), total_devices_(0), active_devices_(0),
        registered_drivers_(0) {}

  ~DeviceManager() noexcept { cleanup(); }

  NON_COPYABLE_NON_MOVABLE(DeviceManager)

  [[nodiscard]] KernelResult<DeviceId>
  register_device(shared_ptr<Device> device) noexcept {
    if (!device) {
      return KernelResult<DeviceId>{Err<ErrorCode>(ErrorCode::InvalidParameter)};
    }

    DeviceId device_id =
        next_device_id_.fetch_add(1, containers::MemoryOrder::Relaxed);

    devices_.insert_or_update(device_id, device);
    if (device->name() != nullptr) {
      device_name_map_.insert_or_update(device->name(), device_id);
    }

    (void)total_devices_.fetch_add(1, containers::MemoryOrder::Relaxed);

    auto match_result = match_driver(device.get());
    if (match_result) {
      device->set_state(DeviceState::Active);
      (void)active_devices_.fetch_add(1, containers::MemoryOrder::Relaxed);
    }

    return KernelResult<DeviceId>{device_id};
  }

  [[nodiscard]] VoidResult unregister_device(DeviceId device_id) noexcept {
    auto device_ptr = devices_.find(device_id);
    if (device_ptr == nullptr) {
      return VoidResult{ErrorCode::NotFound};
    }

    shared_ptr<Device> device = *device_ptr;

    auto driver_ptr = device_driver_map_.find(device_id);
    if (driver_ptr != nullptr) {
      Driver *driver = *driver_ptr;
      driver->remove(device.get());
      device_driver_map_.remove(device_id);
    }

    devices_.remove(device_id);
    if (device->name() != nullptr) {
      device_name_map_.remove(device->name());
    }

    if (device->state() == DeviceState::Active) {
      (void)active_devices_.fetch_sub(1, containers::MemoryOrder::Relaxed);
    }
    (void)total_devices_.fetch_sub(1, containers::MemoryOrder::Relaxed);

    device->shutdown();
    return VoidResult{};
  }

  [[nodiscard]] VoidResult register_driver(Driver *driver) noexcept {
    if (driver == nullptr) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    drivers_.push_front(driver);
    (void)registered_drivers_.fetch_add(1, containers::MemoryOrder::Relaxed);

    devices_.for_each([this, driver](const auto &entry) {
      Device *device = entry.value.get();
      if (device->state() == DeviceState::Uninitialized &&
          driver->is_compatible(device->compatible())) {

        auto probe_result = driver->probe(device);
        if (probe_result) {
          device_driver_map_.insert_or_update(device->device_id(), driver);
          device->set_state(DeviceState::Active);
          (void)active_devices_.fetch_add(1, containers::MemoryOrder::Relaxed);
        }
      }
    });

    return VoidResult{};
  }

  [[nodiscard]] shared_ptr<Device>
  get_device(DeviceId device_id) const noexcept {
    const auto *device_ptr = devices_.find(device_id);
    return device_ptr ? *device_ptr : shared_ptr<Device>{};
  }

  [[nodiscard]] shared_ptr<Device>
  get_device_by_name(const char *name) const noexcept {
    const DeviceId *device_id_ptr = device_name_map_.find(name);
    if (device_id_ptr == nullptr) {
      return shared_ptr<Device>{};
    }

    return get_device(*device_id_ptr);
  }

  struct DeviceManagerStats {
    u32 total_devices;
    u32 active_devices;
    u32 registered_drivers;
  };

  [[nodiscard]] DeviceManagerStats get_statistics() const noexcept {
    return {total_devices_.load(containers::MemoryOrder::Relaxed),
            active_devices_.load(containers::MemoryOrder::Relaxed),
            registered_drivers_.load(containers::MemoryOrder::Relaxed)};
  }

  void list_devices(void (*callback)(const Device &, void *),
                    void *context) const noexcept {
    devices_.for_each([callback, context](const auto &entry) {
      const Device &device = *entry.value;
      callback(device, context);
    });
  }

  [[nodiscard]] VoidResult suspend_all_devices() noexcept {
    bool success = true;

    devices_.for_each([&success](const auto &entry) {
      Device *device = entry.value.get();
      if (device->state() == DeviceState::Active) {
        auto result = device->suspend();
        if (result) {
          device->set_state(DeviceState::Suspended);
        } else {
          success = false;
        }
      }
    });

    return success ? VoidResult{} : VoidResult{ErrorCode::IoError};
  }

  [[nodiscard]] VoidResult resume_all_devices() noexcept {
    bool success = true;

    devices_.for_each([&success](const auto &entry) {
      Device *device = entry.value.get();
      if (device->state() == DeviceState::Suspended) {
        auto result = device->resume();
        if (result) {
          device->set_state(DeviceState::Active);
        } else {
          success = false;
        }
      }
    });

    return success ? VoidResult{} : VoidResult{ErrorCode::IoError};
  }

private:
  [[nodiscard]] VoidResult match_driver(Device *device) noexcept {
    auto matched_driver_ptr = drivers_.find_if([device](const Driver *driver) {
      return driver->is_compatible(device->compatible());
    });

    if (matched_driver_ptr == nullptr) {
      return VoidResult{ErrorCode::NotFound};
    }

    Driver *matched_driver = *matched_driver_ptr;

    auto probe_result = matched_driver->probe(device);
    if (!probe_result) {
      return probe_result;
    }

    device_driver_map_.insert_or_update(device->device_id(),
                                        const_cast<Driver *>(matched_driver));
    return VoidResult{};
  }

  void cleanup() noexcept {
    devices_.for_each([](const auto &entry) { entry.value->shutdown(); });
  }
};

// Global device manager
extern DeviceManager *g_device_manager;

// ========================================================================
// PL011 UART Driver
// ========================================================================

// PL011 UART register offsets
namespace UartRegs {
inline constexpr u32 UARTDR = 0x000;
inline constexpr u32 UARTRSR = 0x004;
inline constexpr u32 UARTFR = 0x018;
inline constexpr u32 UARTILPR = 0x020;
inline constexpr u32 UARTIBRD = 0x024;
inline constexpr u32 UARTFBRD = 0x028;
inline constexpr u32 UARTLCR_H = 0x02C;
inline constexpr u32 UARTCR = 0x030;
inline constexpr u32 UARTIFLS = 0x034;
inline constexpr u32 UARTIMSC = 0x038;
inline constexpr u32 UARTRIS = 0x03C;
inline constexpr u32 UARTMIS = 0x040;
inline constexpr u32 UARTICR = 0x044;
inline constexpr u32 UARTDMACR = 0x048;
} // namespace UartRegs

// UART flag bits
namespace UartFlags {
inline constexpr u32 UARTFR_CTS = (1 << 0);
inline constexpr u32 UARTFR_DSR = (1 << 1);
inline constexpr u32 UARTFR_DCD = (1 << 2);
inline constexpr u32 UARTFR_BUSY = (1 << 3);
inline constexpr u32 UARTFR_RXFE = (1 << 4);
inline constexpr u32 UARTFR_TXFF = (1 << 5);
inline constexpr u32 UARTFR_RXFF = (1 << 6);
inline constexpr u32 UARTFR_TXFE = (1 << 7);
} // namespace UartFlags

// UART control bits
namespace UartControl {
inline constexpr u32 UARTCR_UARTEN = (1 << 0);
inline constexpr u32 UARTCR_SIREN = (1 << 1);
inline constexpr u32 UARTCR_SIRLP = (1 << 2);
inline constexpr u32 UARTCR_LBE = (1 << 7);
inline constexpr u32 UARTCR_TXE = (1 << 8);
inline constexpr u32 UARTCR_RXE = (1 << 9);
inline constexpr u32 UARTCR_DTR = (1 << 10);
inline constexpr u32 UARTCR_RTS = (1 << 11);
inline constexpr u32 UARTCR_OUT1 = (1 << 12);
inline constexpr u32 UARTCR_OUT2 = (1 << 13);
inline constexpr u32 UARTCR_RTSEN = (1 << 14);
inline constexpr u32 UARTCR_CTSEN = (1 << 15);
} // namespace UartControl

// UART line control bits
namespace UartLineControl {
inline constexpr u32 UARTLCR_H_BRK = (1 << 0);
inline constexpr u32 UARTLCR_H_PEN = (1 << 1);
inline constexpr u32 UARTLCR_H_EPS = (1 << 2);
inline constexpr u32 UARTLCR_H_STP2 = (1 << 3);
inline constexpr u32 UARTLCR_H_FEN = (1 << 4);
inline constexpr u32 UARTLCR_H_WLEN_5 = (0 << 5);
inline constexpr u32 UARTLCR_H_WLEN_6 = (1 << 5);
inline constexpr u32 UARTLCR_H_WLEN_7 = (2 << 5);
inline constexpr u32 UARTLCR_H_WLEN_8 = (3 << 5);
} // namespace UartLineControl

// UART device class
class UartDevice : public Device {
private:
  VirtAddr base_addr_;
  u32 clock_freq_;
  u32 baud_rate_;
  InterruptId irq_;
  bool initialized_;

  u64 bytes_sent_;
  u64 bytes_received_;
  u64 tx_errors_;
  u64 rx_errors_;

public:
  UartDevice(const char *name, const char *compatible) noexcept
      : Device(0, DeviceType::UART, name, compatible), base_addr_(0),
        clock_freq_(24000000), baud_rate_(115200), irq_(0), initialized_(false),
        bytes_sent_(0), bytes_received_(0), tx_errors_(0), rx_errors_(0) {}

  ~UartDevice() override = default;

  [[nodiscard]] VoidResult initialize() noexcept override {
    if (initialized_) {
      return VoidResult{ErrorCode::InvalidState};
    }

    auto memory_res = get_resource(DeviceResource::Memory);
    if (!memory_res) {
      return VoidResult{ErrorCode::NotFound};
    }

    base_addr_ = memory_res->memory.mapped_addr;
    if (base_addr_ == 0) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    auto irq_res = get_resource(DeviceResource::IRQ);
    if (irq_res) {
      irq_ = irq_res->interrupt.irq;
    }

    const char *clock_freq_str = get_property("clock-frequency");
    if (clock_freq_str != nullptr) {
      clock_freq_ = string_to_u32(clock_freq_str);
    }

    const char *baud_rate_str = get_property("current-speed");
    if (baud_rate_str != nullptr) {
      baud_rate_ = string_to_u32(baud_rate_str);
    }

    auto init_result = initialize_hardware();
    if (!init_result) {
      return init_result;
    }

    initialized_ = true;
    set_state(DeviceState::Active);

    return VoidResult{};
  }

  [[nodiscard]] VoidResult suspend() noexcept override {
    if (!initialized_) {
      return VoidResult{ErrorCode::InvalidState};
    }

    write_reg(UartRegs::UARTCR, 0);
    set_state(DeviceState::Suspended);

    return VoidResult{};
  }

  [[nodiscard]] VoidResult resume() noexcept override {
    if (state() != DeviceState::Suspended) {
      return VoidResult{ErrorCode::InvalidState};
    }

    auto init_result = initialize_hardware();
    if (!init_result) {
      return init_result;
    }

    set_state(DeviceState::Active);
    return VoidResult{};
  }

  void shutdown() noexcept override {
    if (initialized_) {
      write_reg(UartRegs::UARTCR, 0);
      initialized_ = false;
    }
  }

  // UART operations
  [[nodiscard]] VoidResult send_char(char c) noexcept {
    if (!initialized_) {
      return VoidResult{ErrorCode::InvalidState};
    }

    while (read_reg(UartRegs::UARTFR) & UartFlags::UARTFR_TXFF) {
      // Wait for TX FIFO to be non-full
    }

    write_reg(UartRegs::UARTDR, static_cast<u32>(c));
    bytes_sent_++;
    update_access();

    return VoidResult{};
  }

  [[nodiscard]] KernelResult<char> receive_char() noexcept {
    if (!initialized_) {
      return KernelResult<char>{Err<ErrorCode>(ErrorCode::InvalidState)};
    }

    if (read_reg(UartRegs::UARTFR) & UartFlags::UARTFR_RXFE) {
      return KernelResult<char>{Err<ErrorCode>(ErrorCode::NotFound)};
    }

    u32 data = read_reg(UartRegs::UARTDR);

    if (data & 0xF00) {
      rx_errors_++;
      return KernelResult<char>{Err<ErrorCode>(ErrorCode::IoError)};
    }

    bytes_received_++;
    update_access();

    return KernelResult<char>{static_cast<char>(data & 0xFF)};
  }

  [[nodiscard]] VoidResult send_string(const char *str) noexcept {
    if (str == nullptr) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    while (*str) {
      auto result = send_char(*str++);
      if (!result) {
        return result;
      }
    }

    return VoidResult{};
  }

  struct UartStatistics {
    u64 bytes_sent;
    u64 bytes_received;
    u64 tx_errors;
    u64 rx_errors;
    u32 current_baud_rate;
    bool is_active;
  };

  [[nodiscard]] UartStatistics get_uart_statistics() const noexcept {
    return {bytes_sent_, bytes_received_, tx_errors_,
            rx_errors_,  baud_rate_,      state() == DeviceState::Active};
  }

  [[nodiscard]] VoidResult set_baud_rate(u32 baud_rate) noexcept {
    if (!initialized_) {
      return VoidResult{ErrorCode::InvalidState};
    }

    baud_rate_ = baud_rate;

    u32 temp = 16 * baud_rate;
    u32 divint = clock_freq_ / temp;
    u32 divfrac = ((clock_freq_ % temp) * 64 + temp / 2) / temp;

    write_reg(UartRegs::UARTIBRD, divint);
    write_reg(UartRegs::UARTFBRD, divfrac);

    return VoidResult{};
  }

private:
  [[nodiscard]] u32 read_reg(u32 offset) const noexcept {
    return *reinterpret_cast<volatile u32 *>(base_addr_ + offset);
  }

  void write_reg(u32 offset, u32 value) const noexcept {
    *reinterpret_cast<volatile u32 *>(base_addr_ + offset) = value;
  }

  [[nodiscard]] static u32 string_to_u32(const char *str) noexcept {
    if (str == nullptr)
      return 0;

    u32 result = 0;
    while (*str >= '0' && *str <= '9') {
      result = result * 10 + static_cast<u32>(*str - '0');
      str++;
    }
    return result;
  }

  [[nodiscard]] VoidResult initialize_hardware() noexcept {
    write_reg(UartRegs::UARTCR, 0);
    write_reg(UartRegs::UARTRSR, 0);

    auto baud_result = set_baud_rate(baud_rate_);
    if (!baud_result) {
      return baud_result;
    }

    write_reg(UartRegs::UARTLCR_H, UartLineControl::UARTLCR_H_WLEN_8 |
                                        UartLineControl::UARTLCR_H_FEN);

    write_reg(UartRegs::UARTICR, 0x7FF);

    write_reg(UartRegs::UARTCR, UartControl::UARTCR_UARTEN |
                                    UartControl::UARTCR_TXE |
                                    UartControl::UARTCR_RXE);

    return VoidResult{};
  }

  static void uart_interrupt_handler(InterruptId irq, void *context) noexcept {
    (void)irq;
    UartDevice *uart = static_cast<UartDevice *>(context);
    if (uart == nullptr)
      return;

    u32 int_status = uart->read_reg(UartRegs::UARTMIS);

    if (int_status & (1 << 4)) {
      while (!(uart->read_reg(UartRegs::UARTFR) & UartFlags::UARTFR_RXFE)) {
        (void)uart->read_reg(UartRegs::UARTDR);
      }
    }

    uart->write_reg(UartRegs::UARTICR, int_status);
  }
};

// UART driver class
class UartDriver : public Driver {
private:
  static const char *compatible_devices[];

public:
  UartDriver() noexcept : Driver("pl011-uart", "1.0", compatible_devices, 2) {}

  ~UartDriver() override = default;

  [[nodiscard]] VoidResult probe(Device *device) noexcept override {
    if (device == nullptr) {
      return VoidResult{ErrorCode::InvalidParameter};
    }

    if (device->type() != DeviceType::UART) {
      return VoidResult{ErrorCode::NotSupported};
    }

    auto init_result = device->initialize();
    if (!init_result) {
      return init_result;
    }

    return VoidResult{};
  }

  void remove(Device *device) noexcept override {
    if (device != nullptr) {
      device->shutdown();
    }
  }
};

// Global UART driver instance
extern UartDriver *g_uart_driver;

// === Module-level variable definitions ===

// Global device manager instance
DeviceManager *g_device_manager = nullptr;

// Global UART driver instance
UartDriver *g_uart_driver = nullptr;

// UartDriver compatible device list (static data member)
const char *UartDriver::compatible_devices[] = {"arm,pl011", "arm,primecell"};

} // namespace moss::kernel::drivers
