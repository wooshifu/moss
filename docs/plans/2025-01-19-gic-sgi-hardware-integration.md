# GIC SGI硬件集成方案
*从IPI概念验证到真正硬件实现的架构设计*

## 项目目标

将之前成功的IPI概念验证系统转换为基于ARM64 GIC SGI的真正硬件实现，建立Linux兼容的CPU间中断通信机制。

## 现有基础分析

### GIC驱动优势
- ✅ **完整SGI支持**: `send_sgi(sgi_id, target_cpu_mask)` 函数已实现
- ✅ **中断处理框架**: 描述符、处理函数注册、统计机制完善
- ✅ **多CPU目标**: target_cpu_mask支持任意CPU组合
- ✅ **中断类型分类**: SGI(0-15) 已正确识别和处理

### IPI系统优势
- ✅ **Linux风格API**: send_ipi(), ping_cpu(), smp_call_function_single()
- ✅ **消息结构**: IpiMessage with type, source, target, sequence
- ✅ **错误处理**: IpiResult枚举和统计机制
- ✅ **垂直切片验证**: Ping IPI概念已成功验证

## 硬件集成架构

### 1. SGI中断号分配策略

```cpp
namespace moss::kernel::interrupts {

// IPI专用SGI中断号分配 (基于Linux内核设计)
enum class IpiSgiId : u8 {
    // 核心IPI功能 (0-3: Linux兼容)
    Reschedule = 0,      // 触发目标CPU重新调度
    CallFunction = 1,    // 在目标CPU上执行函数
    CallFunctionSingle = 2,  // 单CPU函数调用(优化版)
    Timer = 3,           // 定时器同步

    // MOSS扩展 (4-7: 自定义功能)
    Ping = 4,            // Ping测试和心跳检测
    WakeUp = 5,          // 唤醒idle CPU
    Stop = 6,            // CPU停止/暂停
    Debug = 7,           // 调试和性能分析

    // 保留 (8-15: 未来扩展)
    Reserved8 = 8,
    // ...
    Reserved15 = 15
};

// IPI类型到SGI ID的映射
constexpr IpiSgiId ipi_type_to_sgi(IpiType type) noexcept {
    switch (type) {
        case IpiType::Ping: return IpiSgiId::Ping;
        case IpiType::Reschedule: return IpiSgiId::Reschedule;
        case IpiType::CallFunction: return IpiSgiId::CallFunction;
        case IpiType::Stop: return IpiSgiId::Stop;
        default: return IpiSgiId::Ping; // 默认fallback
    }
}

}
```

### 2. 硬件集成IPI管理器

```cpp
class HardwareInterProcessorInterrupt {
private:
    GenericInterruptController* gic_;  // GIC驱动实例
    bool initialized_;
    u32 max_cpus_;

    // IPI统计 (Per-SGI ID)
    containers::PerCpuData<u64> sgi_send_counts_[16];
    containers::PerCpuData<u64> sgi_receive_counts_[16];

    // 函数调用队列 (CallFunction IPI支持)
    struct IpiCallData {
        void (*function)(void*);
        void* data;
        volatile bool completed;
        u32 sequence;
    };
    containers::PerCpuData<containers::LockfreeQueue<IpiCallData*>> call_queues_;

public:
    // 初始化硬件IPI系统
    [[nodiscard]] VoidResult initialize(GenericInterruptController* gic, u32 max_cpus) noexcept;

    // Linux风格IPI发送接口
    [[nodiscard]] IpiResult send_ipi(u32 target_cpu, IpiType type, void* data = nullptr) noexcept;
    [[nodiscard]] IpiResult ping_cpu(u32 target_cpu) noexcept;

    // Linux兼容的跨CPU函数调用
    [[nodiscard]] IpiResult smp_call_function_single(u32 cpu, void (*func)(void*), void* data, bool wait = true) noexcept;
    [[nodiscard]] IpiResult smp_call_function_many(u32 cpu_mask, void (*func)(void*), void* data, bool wait = true) noexcept;

    // 调度器协作
    [[nodiscard]] IpiResult request_reschedule(u32 target_cpu) noexcept;

private:
    // SGI中断处理函数 (注册到GIC)
    static void handle_ping_sgi(InterruptId irq, void* context) noexcept;
    static void handle_reschedule_sgi(InterruptId irq, void* context) noexcept;
    static void handle_call_function_sgi(InterruptId irq, void* context) noexcept;
    static void handle_stop_sgi(InterruptId irq, void* context) noexcept;

    // 硬件SGI发送封装
    [[nodiscard]] VoidResult send_hardware_sgi(IpiSgiId sgi_id, u32 target_cpu_mask) noexcept;
};
```

### 3. 集成实现策略

#### Phase 1: 基础硬件集成 (1-2天)
```cpp
// 替换概念验证代码为真正硬件调用
IpiResult HardwareInterProcessorInterrupt::send_ipi(u32 target_cpu, IpiType type, void* data) noexcept {
    if (!initialized_ || target_cpu >= max_cpus_) {
        return IpiResult::InvalidCpu;
    }

    // 将IPI类型映射到SGI中断号
    IpiSgiId sgi_id = ipi_type_to_sgi(type);
    u32 cpu_mask = 1U << target_cpu;

    // 🔥 关键：调用真正的硬件SGI发送
    auto result = gic_->send_sgi(static_cast<InterruptId>(sgi_id), cpu_mask);
    if (!result) {
        return IpiResult::HardwareError;
    }

    // 更新统计
    sgi_send_counts_[static_cast<u8>(sgi_id)].get_current_cpu()++;

    return IpiResult::Success;
}
```

#### Phase 2: 中断处理集成 (1天)
```cpp
// 在GIC中注册SGI处理函数
VoidResult HardwareInterProcessorInterrupt::initialize(GenericInterruptController* gic, u32 max_cpus) noexcept {
    gic_ = gic;
    max_cpus_ = max_cpus;

    // 注册所有IPI SGI处理函数
    auto ping_result = gic_->register_interrupt(
        static_cast<InterruptId>(IpiSgiId::Ping),
        handle_ping_sgi,
        this,
        "IPI-Ping"
    );
    if (!ping_result) return ping_result;

    auto reschedule_result = gic_->register_interrupt(
        static_cast<InterruptId>(IpiSgiId::Reschedule),
        handle_reschedule_sgi,
        this,
        "IPI-Reschedule"
    );
    if (!reschedule_result) return reschedule_result;

    // 启用所有IPI中断
    (void)gic_->enable_interrupt(static_cast<InterruptId>(IpiSgiId::Ping));
    (void)gic_->enable_interrupt(static_cast<InterruptId>(IpiSgiId::Reschedule));
    // ... 其他SGI

    initialized_ = true;
    return VoidResult{};
}
```

#### Phase 3: CallFunction IPI实现 (2-3天)
```cpp
// 实现真正的跨CPU函数调用
IpiResult HardwareInterProcessorInterrupt::smp_call_function_single(u32 cpu, void (*func)(void*), void* data, bool wait) noexcept {
    // 创建调用数据结构
    IpiCallData* call_data = new IpiCallData{
        .function = func,
        .data = data,
        .completed = false,
        .sequence = generate_sequence()
    };

    // 将调用放入目标CPU的队列
    auto& target_queue = call_queues_.get_cpu(cpu);
    if (!target_queue.enqueue(call_data)) {
        delete call_data;
        return IpiResult::QueueFull;
    }

    // 发送CallFunction SGI到目标CPU
    auto sgi_result = send_hardware_sgi(IpiSgiId::CallFunction, 1U << cpu);
    if (!sgi_result) {
        return IpiResult::HardwareError;
    }

    // 等待完成 (如果需要)
    if (wait) {
        while (!call_data->completed) {
            asm volatile("yield" ::: "memory");
        }
    }

    return IpiResult::Success;
}

// CallFunction SGI处理函数
void HardwareInterProcessorInterrupt::handle_call_function_sgi(InterruptId irq, void* context) noexcept {
    auto* ipi_manager = static_cast<HardwareInterProcessorInterrupt*>(context);
    u32 current_cpu = get_current_cpu_id();

    // 处理当前CPU队列中的所有函数调用
    auto& queue = ipi_manager->call_queues_.get_cpu(current_cpu);
    IpiCallData* call_data;

    while (queue.dequeue(call_data)) {
        if (call_data->function) {
            // 🔥 关键：在目标CPU上执行函数
            call_data->function(call_data->data);
        }

        // 标记完成并清理
        call_data->completed = true;
        delete call_data;
    }

    // 更新统计
    ipi_manager->sgi_receive_counts_[static_cast<u8>(IpiSgiId::CallFunction)].get_cpu(current_cpu)++;
}
```

### 4. 验证和测试策略

#### 硬件验证测试套件
```cpp
class IpiHardwareValidationSuite {
public:
    // 基础SGI硬件测试
    [[nodiscard]] VoidResult test_basic_sgi_functionality() noexcept;

    // Ping IPI往返延迟测试
    [[nodiscard]] VoidResult test_ping_latency(u32 iterations = 1000) noexcept;

    // 跨CPU函数调用测试
    [[nodiscard]] VoidResult test_cross_cpu_function_calls() noexcept;

    // 高频率IPI压力测试
    [[nodiscard]] VoidResult test_ipi_storm(u32 duration_ms = 1000) noexcept;

    // 多CPU协调测试
    [[nodiscard]] VoidResult test_multi_cpu_coordination() noexcept;

    // 性能基准测试
    [[nodiscard]] VoidResult benchmark_ipi_performance() noexcept;
};
```

## 预期成果

### 技术突破
1. **真正的硬件IPI**: 从概念验证到ARM64 GIC SGI硬件实现
2. **Linux兼容性**: 完全兼容Linux SMP IPI API模式
3. **生产级性能**: 微秒级跨CPU通信延迟
4. **完整功能集**: Ping、Reschedule、CallFunction全支持

### 验证指标
- **延迟性能**: Ping IPI < 5μs往返时间
- **吞吐量**: > 100K IPI/秒/CPU
- **可靠性**: 99.99%成功率
- **CPU效率**: < 1% CPU开销

### 输出效果预期
```
=== MOSS硬件IPI系统验证 ===
🔧 GIC SGI硬件集成: ✅ 成功
📡 跨CPU Ping测试: ✅ 延迟2.1μs
🏓 函数调用测试: ✅ 跨CPU执行成功
📊 性能基准: ✅ 125K IPI/秒
🚀 Linux兼容性: ✅ API完全兼容

=== 系统状态 ===
✅ 4CPU SMP + 真正硬件IPI
✅ Linux风格API完全工作
✅ 生产级性能和可靠性
🏆 MOSS内核SMP架构完成！
```

## 下一阶段预览

完成GIC SGI硬件集成后，将为以下高级功能奠定基础：
- **调度器协作**: 基于IPI的负载均衡和任务迁移
- **实时性能**: 微秒级跨CPU响应时间
- **可扩展性**: 支持更多CPU核心的线性扩展

这将标志着MOSS内核从概念验证转向生产级Linux兼容SMP操作系统的重大里程碑。