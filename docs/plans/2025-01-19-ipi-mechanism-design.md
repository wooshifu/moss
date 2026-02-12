# MOSS内核IPI(CPU间中断)机制设计文档

## 设计概述

基于Linux内核IPI架构，为MOSS内核设计和实现轻量级的CPU间中断通信机制。该设计采用渐进式方案，先实现核心功能，后续可扩展到负载均衡和任务迁移。

## 设计目标

1. **CPU间通信**: 支持CPU间的基础通信需求
2. **调度协调**: 实现跨CPU的重调度请求
3. **函数调用**: 支持在指定CPU上执行函数
4. **系统诊断**: 提供CPU状态监控和调试功能
5. **架构兼容**: 与现有MOSS SMP架构良好集成

## 架构设计

### 核心组件

#### 1. IPI类型定义
```cpp
enum class IpiType : u8 {
    Reschedule = 0,    // 触发目标CPU重新调度
    CallFunction = 1,  // 在目标CPU上执行函数
    Stop = 2,          // 停止/暂停目标CPU
    Ping = 3           // 简单的ping测试（调试用）
};
```

#### 2. IPI消息结构
```cpp
struct IpiMessage {
    IpiType type;
    u32 source_cpu;
    u32 target_cpu;
    void* data;
    u64 sequence;
};
```

#### 3. Per-CPU队列系统
- 每个CPU维护独立的IPI接收队列
- 使用无锁环形缓冲区确保线程安全
- 队列大小：64个消息

### 核心功能

#### 1. IPI发送机制
- `send_ipi()`: 基础IPI发送接口
- `request_reschedule()`: 重调度请求
- `smp_call_function_single()`: Linux风格的跨CPU函数调用
- `ping_cpu()`: CPU诊断ping

#### 2. IPI接收处理
- 基于GIC SGI中断的IPI通知
- Per-type消息分发机制
- 类型特定的处理函数

#### 3. 具体处理函数
- **Reschedule IPI**: 设置重调度标志，触发调度器检查
- **Call Function IPI**: 在目标CPU上执行指定函数，支持同步等待
- **Stop IPI**: CPU停止/暂停机制（用于调试和管理）
- **Ping IPI**: 简单的CPU可达性测试

### 与现有架构集成

#### GIC集成
- 使用现有的`send_sgi()`功能发送SGI中断
- 注册SGI 0-3作为IPI专用中断号
- 利用现有的中断处理框架

#### SMP架构集成
- 在SMP初始化完成后初始化IPI子系统
- 与Per-CPU数据结构良好集成
- 支持现有的CPU在线/离线状态管理

#### 调度器集成
- 通过重调度标志与CFS调度器协作
- 支持跨CPU的调度决策通知

## 性能特性

### 统计和监控
- Per-CPU统计计数器
- IPI类型统计
- 队列使用率监控
- 延迟和吞吐量跟踪

### 调试支持
- 自测试功能验证IPI机制
- 健康状态检查
- 详细的统计信息输出
- 消息序列号用于调试跟踪

## 实现计划

### 阶段1: 核心基础设施
1. 创建`src/interrupts/include/interrupts/ipi.hpp`
2. 实现基础IPI类和消息结构
3. 集成Per-CPU队列系统
4. 基础的发送/接收机制

### 阶段2: GIC集成
1. 扩展现有GIC驱动支持IPI注册
2. 实现SGI中断处理
3. 完成IPI消息分发机制

### 阶段3: 功能实现
1. 实现各种IPI处理函数
2. 完成跨CPU函数调用机制
3. 添加调试和测试功能

### 阶段4: 系统集成
1. 在启动流程中初始化IPI子系统
2. 与调度器集成
3. 添加统计和监控接口
4. 完整的功能测试

## 验证方案

### 功能测试
1. **基础通信测试**: 验证CPU间消息传递
2. **重调度测试**: 验证跨CPU调度请求
3. **函数调用测试**: 验证跨CPU函数执行
4. **压力测试**: 高频IPI发送测试

### 性能测试
1. **延迟测试**: 测量IPI端到端延迟
2. **吞吐量测试**: 测试最大IPI处理能力
3. **多CPU并发测试**: 验证多CPU同时发送IPI

### 稳定性测试
1. **长期运行测试**: 24小时持续IPI测试
2. **错误处理测试**: 队列溢出等异常情况
3. **CPU热插拔测试**: 验证CPU状态变化处理

## 预期效果

实现后的MOSS内核将具备：
1. **真正的多CPU协调**: CPU间可以进行有效通信
2. **调度器协作**: 支持跨CPU的调度决策
3. **系统诊断能力**: 丰富的CPU间通信监控
4. **扩展基础**: 为后续负载均衡和任务迁移打下基础

## 文件结构

```
src/interrupts/
├── include/interrupts/
│   └── ipi.hpp                 # IPI机制主头文件
└── src/
    └── ipi.cpp                 # IPI实现文件

src/interrupts/include/interrupts/
└── gic.hpp                     # 扩展现有GIC驱动
```

这个设计为MOSS内核提供了Linux风格的IPI基础设施，与现有架构完美集成，同时为未来的高级SMP功能奠定了基础。