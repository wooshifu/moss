#pragma once

// MOSS Boot系统全局变量和接口
// 用于内核访问boot阶段初始化的硬件实例

#include "../../interrupts/include/interrupts/gic.hpp"

// === Linux风格全局硬件实例声明 ===

/// 全局GIC控制器实例 - 在boot阶段初始化
/// 用于内核阶段的硬件IPI和中断管理
extern moss::kernel::interrupts::GenericInterruptController* g_gic_controller;

/// GIC硬件可用性标志 - runtime检查
/// true: GIC硬件初始化成功，可以使用真实硬件IPI
/// false: GIC硬件不可用，需要使用概念验证模式
extern bool g_gic_hardware_available;
