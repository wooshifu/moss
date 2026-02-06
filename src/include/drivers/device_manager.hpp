#pragma once

// 设备管理器和驱动框架
// 支持设备树解析、驱动匹配和生命周期管理

#include "../include/types.hpp"
#include "../include/result.hpp"
#include "../include/smart_ptr.hpp"
#include "../containers/containers.hpp"
#include "../interrupts/gic.hpp"

namespace moss::kernel::drivers {

// 使用内核智能指针
using moss::kernel::unique_ptr;
using moss::kernel::make_unique;
using moss::kernel::shared_ptr;
using moss::kernel::make_shared;

// 设备类型
enum class DeviceType : u8 {
    Unknown = 0,
    Block = 1,          // 块设备
    Character = 2,      // 字符设备
    Network = 3,        // 网络设备
    Input = 4,          // 输入设备
    Display = 5,        // 显示设备
    Audio = 6,          // 音频设备
    GPIO = 7,           // GPIO控制器
    I2C = 8,            // I2C总线
    SPI = 9,            // SPI总线
    UART = 10,          // UART串口
    Timer = 11,         // 定时器
    Clock = 12,         // 时钟管理
    Power = 13,         // 电源管理
    Platform = 14       // 平台设备
};

// 设备状态
enum class DeviceState : u8 {
    Uninitialized = 0,
    Initializing = 1,
    Active = 2,
    Suspended = 3,
    Error = 4,
    Removed = 5
};

// 设备属性
struct DeviceProperty {
    const char* name;
    const char* value;
    usize value_size;

    DeviceProperty(const char* n, const char* v, usize size) noexcept
        : name(n), value(v), value_size(size) {}
};

// 设备资源
struct DeviceResource {
    enum Type {
        Memory = 0,
        IO = 1,
        IRQ = 2,
        DMA = 3
    } type;

    union {
        struct {
            PhysAddr start;
            PhysAddr end;
            VirtAddr mapped_addr;
        } memory;

        struct {
            u16 start;
            u16 end;
        } io;

        struct {
            InterruptId irq;
            interrupts::TriggerType trigger;
        } interrupt;

        struct {
            u32 channel;
            u32 request_line;
        } dma;
    };

    DeviceResource() noexcept : type(Memory) {
        memory = {0, 0, 0};
    }
};

// 设备基类
class Device {
protected:
    DeviceId device_id_;                    // 设备ID
    DeviceType type_;                       // 设备类型
    DeviceState state_;                     // 设备状态
    const char* name_;                      // 设备名称
    const char* compatible_;                // 兼容字符串
    Device* parent_;                        // 父设备
    containers::RcuList<Device*> children_; // 子设备列表

    // 设备资源
    containers::RcuList<DeviceResource> resources_;
    containers::RcuList<DeviceProperty> properties_;

    // 统计信息
    u64 init_time_;
    u64 last_access_time_;
    u64 access_count_;

public:
    Device(DeviceId id, DeviceType type, const char* name, const char* compatible) noexcept
        : device_id_(id), type_(type), state_(DeviceState::Uninitialized),
          name_(name), compatible_(compatible), parent_(nullptr),
          init_time_(0), last_access_time_(0), access_count_(0) {}

    virtual ~Device() noexcept = default;

    // 禁用拷贝，允许移动
    NON_COPYABLE(Device)

    Device(Device&& other) noexcept = default;
    Device& operator=(Device&& other) noexcept = default;

    // 设备生命周期接口
    [[nodiscard]] virtual VoidResult initialize() noexcept = 0;
    [[nodiscard]] virtual VoidResult suspend() noexcept { return VoidResult{}; }
    [[nodiscard]] virtual VoidResult resume() noexcept { return VoidResult{}; }
    virtual void shutdown() noexcept {}

    // 基本属性访问
    [[nodiscard]] DeviceId device_id() const noexcept { return device_id_; }
    [[nodiscard]] DeviceType type() const noexcept { return type_; }
    [[nodiscard]] DeviceState state() const noexcept { return state_; }
    [[nodiscard]] const char* name() const noexcept { return name_; }
    [[nodiscard]] const char* compatible() const noexcept { return compatible_; }

    // 设备树关系
    [[nodiscard]] Device* parent() const noexcept { return parent_; }
    void set_parent(Device* parent) noexcept { parent_ = parent; }
    void add_child(Device* child) noexcept {
        child->set_parent(this);
        children_.push_front(child);
    }

    // 资源管理
    void add_resource(const DeviceResource& resource) noexcept {
        resources_.push_front(resource);
    }

    [[nodiscard]] containers::Optional<DeviceResource> get_resource(DeviceResource::Type type, usize index = 0) const noexcept {
        usize found_count = 0;
        const DeviceResource* found = nullptr;

        resources_.for_each([type, index, &found_count, &found](const DeviceResource& res) {
            if (res.type == type) {
                if (found_count == index) {
                    found = &res;
                    return;  // 提前退出
                }
                found_count++;
            }
        });

        return found ? containers::Optional<DeviceResource>{*found}
                     : containers::Optional<DeviceResource>{};
    }

    // 属性管理
    void add_property(const char* name, const char* value, usize value_size) noexcept {
        properties_.push_front(DeviceProperty(name, value, value_size));
    }

    [[nodiscard]] const char* get_property(const char* name) const noexcept {
        const DeviceProperty* found = properties_.find_if([name](const DeviceProperty& prop) {
            return string_compare(prop.name, name) == 0;
        });

        return found ? found->value : nullptr;
    }

    // 状态管理
    void set_state(DeviceState new_state) noexcept {
        state_ = new_state;
        if (new_state == DeviceState::Active) {
            init_time_ = get_current_time();
        }
    }

    // 统计信息
    void update_access() noexcept {
        last_access_time_ = get_current_time();
        access_count_++;
    }

    [[nodiscard]] struct {
        u64 init_time;
        u64 last_access_time;
        u64 access_count;
    } get_statistics() const noexcept {
        return {init_time_, last_access_time_, access_count_};
    }

protected:
    // 辅助函数
    [[nodiscard]] static int string_compare(const char* s1, const char* s2) noexcept {
        if (s1 == nullptr || s2 == nullptr) return -1;
        while (*s1 && *s2 && *s1 == *s2) {
            s1++;
            s2++;
        }
        return *s1 - *s2;
    }

    [[nodiscard]] static u64 get_current_time() noexcept {
        u64 count;
        asm volatile("mrs %0, cntvct_el0" : "=r"(count));
        return count;
    }
};

// 驱动基类
class Driver {
protected:
    const char* name_;                      // 驱动名称
    const char* version_;                   // 驱动版本
    const char** compatible_list_;          // 兼容设备列表
    usize compatible_count_;                // 兼容设备数量

public:
    Driver(const char* name, const char* version,
           const char** compatible_list, usize compatible_count) noexcept
        : name_(name), version_(version),
          compatible_list_(compatible_list), compatible_count_(compatible_count) {}

    virtual ~Driver() noexcept = default;

    // 禁用拷贝和移动
    NON_COPYABLE_NON_MOVABLE(Driver)

    // 驱动接口
    [[nodiscard]] virtual VoidResult probe(Device* device) noexcept = 0;
    virtual void remove(Device* device) noexcept = 0;

    // 电源管理
    [[nodiscard]] virtual VoidResult suspend(Device* device) noexcept { return VoidResult{}; }
    [[nodiscard]] virtual VoidResult resume(Device* device) noexcept { return VoidResult{}; }

    // 基本属性
    [[nodiscard]] const char* name() const noexcept { return name_; }
    [[nodiscard]] const char* version() const noexcept { return version_; }

    // 设备匹配
    [[nodiscard]] bool is_compatible(const char* device_compatible) const noexcept {
        for (usize i = 0; i < compatible_count_; ++i) {
            if (string_compare(compatible_list_[i], device_compatible) == 0) {
                return true;
            }
        }
        return false;
    }

private:
    [[nodiscard]] static int string_compare(const char* s1, const char* s2) noexcept {
        if (s1 == nullptr || s2 == nullptr) return -1;
        while (*s1 && *s2 && *s1 == *s2) {
            s1++;
            s2++;
        }
        return *s1 - *s2;
    }
};

// 设备管理器
class DeviceManager {
private:
    // 设备注册表
    containers::RcuHashMap<DeviceId, shared_ptr<Device>> devices_;
    containers::RcuHashMap<const char*, DeviceId> device_name_map_;

    // 驱动注册表
    containers::RcuList<Driver*> drivers_;

    // 设备-驱动绑定
    containers::RcuHashMap<DeviceId, Driver*> device_driver_map_;

    // ID分配器
    containers::AtomicCounter<DeviceId> next_device_id_;

    // 统计信息
    containers::AtomicCounter<u32> total_devices_;
    containers::AtomicCounter<u32> active_devices_;
    containers::AtomicCounter<u32> registered_drivers_;

public:
    DeviceManager() noexcept
        : next_device_id_(1), total_devices_(0), active_devices_(0), registered_drivers_(0) {}

    ~DeviceManager() noexcept {
        cleanup();
    }

    // 禁用拷贝和移动
    NON_COPYABLE_NON_MOVABLE(DeviceManager)

    // 注册设备
    [[nodiscard]] KernelResult<DeviceId> register_device(shared_ptr<Device> device) noexcept {
        if (!device) {
            return KernelResult<DeviceId>{ErrorCode::InvalidParameter};
        }

        DeviceId device_id = next_device_id_.fetch_add(1, containers::MemoryOrder::Relaxed);

        // 注册到设备表
        devices_.insert_or_update(device_id, device);
        if (device->name() != nullptr) {
            device_name_map_.insert_or_update(device->name(), device_id);
        }

        (void)total_devices_.fetch_add(1, containers::MemoryOrder::Relaxed);

        // 尝试匹配驱动
        auto match_result = match_driver(device.get());
        if (match_result) {
            device->set_state(DeviceState::Active);
            (void)active_devices_.fetch_add(1, containers::MemoryOrder::Relaxed);
        }

        return KernelResult<DeviceId>{device_id};
    }

    // 注销设备
    [[nodiscard]] VoidResult unregister_device(DeviceId device_id) noexcept {
        auto device_ptr = devices_.find(device_id);
        if (device_ptr == nullptr) {
            return VoidResult{ErrorCode::NotFound};
        }

        shared_ptr<Device> device = *device_ptr;

        // 移除驱动绑定
        Driver* driver = const_cast<Driver*>(device_driver_map_.find(device_id));
        if (driver != nullptr) {
            driver->remove(device.get());
            device_driver_map_.remove(device_id);
        }

        // 移除设备
        devices_.remove(device_id);
        if (device->name() != nullptr) {
            device_name_map_.remove(device->name());
        }

        if (device->state() == DeviceState::Active) {
            active_devices_.fetch_sub(1, containers::MemoryOrder::Relaxed);
        }
        total_devices_.fetch_sub(1, containers::MemoryOrder::Relaxed);

        device->shutdown();
        return VoidResult{};
    }

    // 注册驱动
    [[nodiscard]] VoidResult register_driver(Driver* driver) noexcept {
        if (driver == nullptr) {
            return VoidResult{ErrorCode::InvalidParameter};
        }

        // 添加到驱动列表
        drivers_.push_front(driver);
        (void)registered_drivers_.fetch_add(1, containers::MemoryOrder::Relaxed);

        // 为现有设备匹配驱动
        devices_.for_each([this, driver](const auto& entry) {
            Device* device = entry.value.get();
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

    // 通过ID查找设备
    [[nodiscard]] shared_ptr<Device> get_device(DeviceId device_id) const noexcept {
        const auto* device_ptr = devices_.find(device_id);
        return device_ptr ? *device_ptr : shared_ptr<Device>{};
    }

    // 通过名称查找设备
    [[nodiscard]] shared_ptr<Device> get_device_by_name(const char* name) const noexcept {
        const DeviceId* device_id_ptr = device_name_map_.find(name);
        if (device_id_ptr == nullptr) {
            return shared_ptr<Device>{};
        }

        return get_device(*device_id_ptr);
    }

    // 获取设备统计信息
    [[nodiscard]] struct {
        u32 total_devices;
        u32 active_devices;
        u32 registered_drivers;
    } get_statistics() const noexcept {
        return {
            total_devices_.load(containers::MemoryOrder::Relaxed),
            active_devices_.load(containers::MemoryOrder::Relaxed),
            registered_drivers_.load(containers::MemoryOrder::Relaxed)
        };
    }

    // 列出所有设备
    void list_devices(void (*callback)(const Device&, void*), void* context) const noexcept {
        devices_.for_each([callback, context](const auto& entry) {
            const Device& device = *entry.value;
            callback(device, context);
        });
    }

    // 系统挂起/恢复
    [[nodiscard]] VoidResult suspend_all_devices() noexcept {
        bool success = true;

        devices_.for_each([&success](const auto& entry) {
            Device* device = entry.value.get();
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

        devices_.for_each([&success](const auto& entry) {
            Device* device = entry.value.get();
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
    // 为设备匹配驱动
    [[nodiscard]] VoidResult match_driver(Device* device) noexcept {
        const Driver* matched_driver = drivers_.find_if([device](const Driver* driver) {
            return driver->is_compatible(device->compatible());
        });

        if (matched_driver == nullptr) {
            return VoidResult{ErrorCode::NotFound};
        }

        // 尝试探测设备
        auto probe_result = const_cast<Driver*>(matched_driver)->probe(device);
        if (!probe_result) {
            return probe_result;
        }

        // 绑定设备和驱动
        device_driver_map_.insert_or_update(device->device_id(), const_cast<Driver*>(matched_driver));
        return VoidResult{};
    }

    // 清理资源
    void cleanup() noexcept {
        // 关闭所有设备
        devices_.for_each([](const auto& entry) {
            entry.value->shutdown();
        });

        devices_ = {};
        device_name_map_ = {};
        device_driver_map_ = {};
    }
};

// 全局设备管理器
extern DeviceManager* g_device_manager;

// 便利的设备注册宏
#define REGISTER_DEVICE(device_class, name, compatible) \
    do { \
        auto device = make_shared<device_class>(name, compatible); \
        if (device) { \
            g_device_manager->register_device(device); \
        } \
    } while(0)

#define REGISTER_DRIVER(driver_ptr) \
    g_device_manager->register_driver(driver_ptr)

} // namespace moss::kernel::drivers