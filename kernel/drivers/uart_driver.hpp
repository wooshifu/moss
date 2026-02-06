#pragma once

// ARM64 UART (PL011) 驱动示例
// 展示设备驱动框架的使用

#include "device_manager.hpp"
#include "../include/types.hpp"
#include "../include/result.hpp"

namespace moss::kernel::drivers {

// PL011 UART寄存器偏移
namespace UartRegs {
    static constexpr u32 UARTDR = 0x000;       // 数据寄存器
    static constexpr u32 UARTRSR = 0x004;      // 接收状态寄存器
    static constexpr u32 UARTFR = 0x018;       // 标志寄存器
    static constexpr u32 UARTILPR = 0x020;     // IrDA低功耗计数器寄存器
    static constexpr u32 UARTIBRD = 0x024;     // 整数波特率寄存器
    static constexpr u32 UARTFBRD = 0x028;     // 小数波特率寄存器
    static constexpr u32 UARTLCR_H = 0x02C;    // 线路控制寄存器
    static constexpr u32 UARTCR = 0x030;       // 控制寄存器
    static constexpr u32 UARTIFLS = 0x034;     // 中断FIFO级别选择寄存器
    static constexpr u32 UARTIMSC = 0x038;     // 中断掩码设置/清除寄存器
    static constexpr u32 UARTRIS = 0x03C;      // 原始中断状态寄存器
    static constexpr u32 UARTMIS = 0x040;      // 掩码中断状态寄存器
    static constexpr u32 UARTICR = 0x044;      // 中断清除寄存器
    static constexpr u32 UARTDMACR = 0x048;    // DMA控制寄存器
}

// UART标志位
namespace UartFlags {
    static constexpr u32 UARTFR_CTS = (1 << 0);    // 清除发送
    static constexpr u32 UARTFR_DSR = (1 << 1);    // 数据设置就绪
    static constexpr u32 UARTFR_DCD = (1 << 2);    // 数据载波检测
    static constexpr u32 UARTFR_BUSY = (1 << 3);   // UART忙
    static constexpr u32 UARTFR_RXFE = (1 << 4);   // 接收FIFO空
    static constexpr u32 UARTFR_TXFF = (1 << 5);   // 发送FIFO满
    static constexpr u32 UARTFR_RXFF = (1 << 6);   // 接收FIFO满
    static constexpr u32 UARTFR_TXFE = (1 << 7);   // 发送FIFO空
}

// UART控制位
namespace UartControl {
    static constexpr u32 UARTCR_UARTEN = (1 << 0);  // UART使能
    static constexpr u32 UARTCR_SIREN = (1 << 1);   // SIR使能
    static constexpr u32 UARTCR_SIRLP = (1 << 2);   // SIR低功耗IrDA模式
    static constexpr u32 UARTCR_LBE = (1 << 7);     // 回环使能
    static constexpr u32 UARTCR_TXE = (1 << 8);     // 发送使能
    static constexpr u32 UARTCR_RXE = (1 << 9);     // 接收使能
    static constexpr u32 UARTCR_DTR = (1 << 10);    // 数据传输就绪
    static constexpr u32 UARTCR_RTS = (1 << 11);    // 请求发送
    static constexpr u32 UARTCR_OUT1 = (1 << 12);   // 输出1
    static constexpr u32 UARTCR_OUT2 = (1 << 13);   // 输出2
    static constexpr u32 UARTCR_RTSEN = (1 << 14);  // RTS硬件流控制使能
    static constexpr u32 UARTCR_CTSEN = (1 << 15);  // CTS硬件流控制使能
}

// UART线路控制
namespace UartLineControl {
    static constexpr u32 UARTLCR_H_BRK = (1 << 0);     // 发送中断
    static constexpr u32 UARTLCR_H_PEN = (1 << 1);     // 奇偶校验使能
    static constexpr u32 UARTLCR_H_EPS = (1 << 2);     // 偶校验选择
    static constexpr u32 UARTLCR_H_STP2 = (1 << 3);    // 两个停止位选择
    static constexpr u32 UARTLCR_H_FEN = (1 << 4);     // FIFO使能
    static constexpr u32 UARTLCR_H_WLEN_5 = (0 << 5);  // 字长5位
    static constexpr u32 UARTLCR_H_WLEN_6 = (1 << 5);  // 字长6位
    static constexpr u32 UARTLCR_H_WLEN_7 = (2 << 5);  // 字长7位
    static constexpr u32 UARTLCR_H_WLEN_8 = (3 << 5);  // 字长8位
}

// UART设备类
class UartDevice : public Device {
private:
    VirtAddr base_addr_;            // 寄存器基址
    u32 clock_freq_;                // 时钟频率
    u32 baud_rate_;                 // 波特率
    InterruptId irq_;               // 中断号
    bool initialized_;              // 初始化状态

    // 统计信息
    u64 bytes_sent_;
    u64 bytes_received_;
    u64 tx_errors_;
    u64 rx_errors_;

public:
    UartDevice(const char* name, const char* compatible) noexcept
        : Device(0, DeviceType::UART, name, compatible),
          base_addr_(0), clock_freq_(24000000), baud_rate_(115200),
          irq_(0), initialized_(false),
          bytes_sent_(0), bytes_received_(0), tx_errors_(0), rx_errors_(0) {}

    ~UartDevice() override = default;

    // 设备初始化
    [[nodiscard]] VoidResult initialize() override noexcept {
        if (initialized_) {
            return VoidResult{ErrorCode::InvalidState};
        }

        // 获取内存资源
        auto memory_res = get_resource(DeviceResource::Memory);
        if (!memory_res) {
            return VoidResult{ErrorCode::NotFound};
        }

        base_addr_ = memory_res->memory.mapped_addr;
        if (base_addr_ == 0) {
            return VoidResult{ErrorCode::InvalidParameter};
        }

        // 获取中断资源
        auto irq_res = get_resource(DeviceResource::IRQ);
        if (irq_res) {
            irq_ = irq_res->interrupt.irq;
        }

        // 读取设备属性
        const char* clock_freq_str = get_property("clock-frequency");
        if (clock_freq_str != nullptr) {
            clock_freq_ = string_to_u32(clock_freq_str);
        }

        const char* baud_rate_str = get_property("current-speed");
        if (baud_rate_str != nullptr) {
            baud_rate_ = string_to_u32(baud_rate_str);
        }

        // 初始化UART硬件
        auto init_result = initialize_hardware();
        if (!init_result) {
            return init_result;
        }

        // 注册中断处理函数
        if (irq_ != 0) {
            // 在实际实现中注册中断处理
            // g_gic->register_interrupt(irq_, uart_interrupt_handler, this, name_);
        }

        initialized_ = true;
        set_state(DeviceState::Active);

        return VoidResult{};
    }

    // 设备挂起
    [[nodiscard]] VoidResult suspend() override noexcept {
        if (!initialized_) {
            return VoidResult{ErrorCode::InvalidState};
        }

        // 禁用UART
        write_reg(UartRegs::UARTCR, 0);
        set_state(DeviceState::Suspended);

        return VoidResult{};
    }

    // 设备恢复
    [[nodiscard]] VoidResult resume() override noexcept {
        if (state() != DeviceState::Suspended) {
            return VoidResult{ErrorCode::InvalidState};
        }

        // 重新初始化硬件
        auto init_result = initialize_hardware();
        if (!init_result) {
            return init_result;
        }

        set_state(DeviceState::Active);
        return VoidResult{};
    }

    // 设备关闭
    void shutdown() override noexcept {
        if (initialized_) {
            // 禁用UART
            write_reg(UartRegs::UARTCR, 0);

            // 取消注册中断
            if (irq_ != 0) {
                // g_gic->unregister_interrupt(irq_);
            }

            initialized_ = false;
        }
    }

    // UART操作接口
    [[nodiscard]] VoidResult send_char(char c) noexcept {
        if (!initialized_) {
            return VoidResult{ErrorCode::InvalidState};
        }

        // 等待发送FIFO非满
        while (read_reg(UartRegs::UARTFR) & UartFlags::UARTFR_TXFF) {
            // 可以添加超时检查
        }

        // 发送字符
        write_reg(UartRegs::UARTDR, static_cast<u32>(c));
        bytes_sent_++;
        update_access();

        return VoidResult{};
    }

    [[nodiscard]] KernelResult<char> receive_char() noexcept {
        if (!initialized_) {
            return KernelResult<char>{ErrorCode::InvalidState};
        }

        // 检查接收FIFO是否为空
        if (read_reg(UartRegs::UARTFR) & UartFlags::UARTFR_RXFE) {
            return KernelResult<char>{ErrorCode::NotFound};
        }

        // 读取字符
        u32 data = read_reg(UartRegs::UARTDR);

        // 检查错误
        if (data & 0xF00) {  // 错误位在高4位
            rx_errors_++;
            return KernelResult<char>{ErrorCode::IoError};
        }

        bytes_received_++;
        update_access();

        return KernelResult<char>{static_cast<char>(data & 0xFF)};
    }

    [[nodiscard]] VoidResult send_string(const char* str) noexcept {
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

    // 获取UART统计信息
    [[nodiscard]] struct {
        u64 bytes_sent;
        u64 bytes_received;
        u64 tx_errors;
        u64 rx_errors;
        u32 current_baud_rate;
        bool is_active;
    } get_uart_statistics() const noexcept {
        return {
            bytes_sent_,
            bytes_received_,
            tx_errors_,
            rx_errors_,
            baud_rate_,
            state() == DeviceState::Active
        };
    }

    // 设置波特率
    [[nodiscard]] VoidResult set_baud_rate(u32 baud_rate) noexcept {
        if (!initialized_) {
            return VoidResult{ErrorCode::InvalidState};
        }

        baud_rate_ = baud_rate;

        // 计算波特率除数
        u32 temp = 16 * baud_rate;
        u32 divint = clock_freq_ / temp;
        u32 divfrac = ((clock_freq_ % temp) * 64 + temp / 2) / temp;

        // 设置波特率寄存器
        write_reg(UartRegs::UARTIBRD, divint);
        write_reg(UartRegs::UARTFBRD, divfrac);

        return VoidResult{};
    }

private:
    // 初始化UART硬件
    [[nodiscard]] VoidResult initialize_hardware() noexcept {
        // 禁用UART
        write_reg(UartRegs::UARTCR, 0);

        // 清除所有错误
        write_reg(UartRegs::UARTRSR, 0);

        // 设置波特率
        auto baud_result = set_baud_rate(baud_rate_);
        if (!baud_result) {
            return baud_result;
        }

        // 设置线路控制：8位数据，无奇偶校验，1个停止位，启用FIFO
        write_reg(UartRegs::UARTLCR_H,
                  UartLineControl::UARTLCR_H_WLEN_8 |
                  UartLineControl::UARTLCR_H_FEN);

        // 清除所有中断
        write_reg(UartRegs::UARTICR, 0x7FF);

        // 启用UART，发送和接收
        write_reg(UartRegs::UARTCR,
                  UartControl::UARTCR_UARTEN |
                  UartControl::UARTCR_TXE |
                  UartControl::UARTCR_RXE);

        return VoidResult{};
    }

    // 寄存器读写
    [[nodiscard]] u32 read_reg(u32 offset) const noexcept {
        return *reinterpret_cast<volatile u32*>(base_addr_ + offset);
    }

    void write_reg(u32 offset, u32 value) const noexcept {
        *reinterpret_cast<volatile u32*>(base_addr_ + offset) = value;
    }

    // 字符串转数字
    [[nodiscard]] static u32 string_to_u32(const char* str) noexcept {
        if (str == nullptr) return 0;

        u32 result = 0;
        while (*str >= '0' && *str <= '9') {
            result = result * 10 + (*str - '0');
            str++;
        }
        return result;
    }

    // 中断处理函数
    static void uart_interrupt_handler(InterruptId irq, void* context) noexcept {
        UartDevice* uart = static_cast<UartDevice*>(context);
        if (uart == nullptr) return;

        // 读取中断状态
        u32 int_status = uart->read_reg(UartRegs::UARTMIS);

        // 处理接收中断
        if (int_status & (1 << 4)) {  // RXIM
            // 处理接收到的数据
            while (!(uart->read_reg(UartRegs::UARTFR) & UartFlags::UARTFR_RXFE)) {
                u32 data = uart->read_reg(UartRegs::UARTDR);
                // 这里可以将数据放入接收缓冲区
            }
        }

        // 处理发送中断
        if (int_status & (1 << 5)) {  // TXIM
            // 处理发送完成
        }

        // 清除中断
        uart->write_reg(UartRegs::UARTICR, int_status);
    }
};

// UART驱动类
class UartDriver : public Driver {
private:
    static const char* compatible_devices[];

public:
    UartDriver() noexcept
        : Driver("pl011-uart", "1.0", compatible_devices, 2) {}

    ~UartDriver() override = default;

    // 探测设备
    [[nodiscard]] VoidResult probe(Device* device) override noexcept {
        if (device == nullptr) {
            return VoidResult{ErrorCode::InvalidParameter};
        }

        // 检查设备类型
        if (device->type() != DeviceType::UART) {
            return VoidResult{ErrorCode::NotSupported};
        }

        // 初始化设备
        auto init_result = device->initialize();
        if (!init_result) {
            return init_result;
        }

        return VoidResult{};
    }

    // 移除设备
    void remove(Device* device) override noexcept {
        if (device != nullptr) {
            device->shutdown();
        }
    }
};

// 兼容设备列表
const char* UartDriver::compatible_devices[] = {
    "arm,pl011",
    "arm,primecell"
};

// 全局UART驱动实例
extern UartDriver* g_uart_driver;

} // namespace moss::kernel::drivers