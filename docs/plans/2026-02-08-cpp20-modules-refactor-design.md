# MOSS 内核 C++20 Modules 重构设计

**设计日期**: 2026-02-08
**状态**: 已批准
**目标**: 将 MOSS 微内核从 C++26 头文件系统重构为 C++20 modules 系统

## 概述

将 MOSS 内核从传统的头文件系统迁移到 C++20 modules，同时升级构建系统使用 uv 管理的 CMake 4.2.1，以获得更好的模块支持、编译性能和模块化架构。

## 设计决策

### 1. 模块化粒度
**选择**: 细粒度模块
- 每个主要组件都是独立的模块
- 符合当前的分层设计哲学
- 提供最佳的编译并行性和增量编译
- 便于多架构支持的条件编译

### 2. 架构特定代码处理
**选择**: 混合方式
- 公共接口用统一模块 (`moss.arch`)
- 架构特定实现用独立模块 (`moss.arch.arm64` 等)
- 保持架构抽象层的清晰性
- 与现有 `arch_abstraction.hpp` 设计理念一致

### 3. 构建系统升级
**选择**: 模块支持优先
- 使用 CMake 4.2.1 的 C++20 modules 支持
- 自动模块依赖扫描
- 对 Clang 21 的模块编译优化

## 模块架构设计

### 核心基础模块
- `moss.std` - 自实现的 freestanding 标准库替代
- `moss.types` - 基础类型定义和架构无关的核心类型
- `moss.result` - 错误处理和 Result 类型系统

### 功能层模块
- `moss.containers` - 无锁队列、RCU链表、Slab分配器等容器
- `moss.memory` - 页表管理、物理内存管理
- `moss.interrupts` - 中断控制器和中断处理
- `moss.drivers` - 设备管理器、UART驱动等硬件驱动
- `moss.ipc` - 进程间通信、共享内存、零拷贝通道
- `moss.process` - CFS调度器、负载均衡器
- `moss.kernel` - 内核核心功能集成
- `moss.boot` - 启动相关代码

### 架构抽象模块
- `moss.arch` - 统一的架构抽象接口
- `moss.arch.arm64`、`moss.arch.x86_64`、`moss.arch.riscv` - 架构特定实现

## 模块依赖关系

```
moss.std (无依赖)
    ↓
moss.types (依赖: moss.std)
    ↓
moss.result (依赖: moss.types)
    ↓
moss.arch + moss.arch.* (依赖: moss.types, moss.std)
    ↓
moss.containers (依赖: moss.arch, moss.result, moss.types)
    ↓
moss.memory (依赖: moss.containers, moss.arch)
    ↓
moss.interrupts, moss.drivers (依赖: moss.arch, moss.memory)
    ↓
moss.ipc, moss.process (依赖: moss.memory, moss.containers)
    ↓
moss.kernel (依赖: 所有上层模块)
    ↓
moss.boot (依赖: moss.kernel)
```

## 模块导出策略

- **接口优先**: 每个模块只导出公共接口，隐藏实现细节
- **架构透明**: 通过 `moss.arch` 统一导出架构相关功能
- **最小导出**: 采用显式导出，只公开必要的类型和函数
- **分区支持**: 为调试和发布版本提供不同的导出分区

## 构建系统升级

### uv 集成
创建 `pyproject.toml`:
```toml
[tool.uv]
python = ">=3.11"

[tool.uv.dependencies]
cmake = "4.2.1"
ninja = "1.11.1"

[tool.uv.scripts]
configure = "cmake --preset"
build = "cmake --build"
clean = "cmake --build --target clean"
```

### CMake Modules 配置
- 启用 `CMAKE_CXX_SCAN_FOR_MODULES=ON` 自动模块依赖扫描
- 配置 `CMAKE_CXX_MODULE_STD=ON` 支持标准库模块
- 设置模块编译缓存目录
- 为每个架构配置独立的模块缓存

### 构建脚本重构
- 将 `build.sh` 改为调用 `uv run cmake`
- 保持相同的命令行接口
- 添加模块特定的构建选项

## 实现策略

### 渐进式迁移路径

**阶段一: 基础设施搭建**
- 设置 uv 环境和 CMake 4.2.1
- 创建模块接口文件（.cppm）框架
- 建立模块编译规则

**阶段二: 核心模块转换**（按依赖顺序）
1. `moss.std` - 将 moss_std.hpp 转换为模块
2. `moss.types` - 转换基础类型定义
3. `moss.result` - 转换错误处理系统
4. `moss.arch` - 重构架构抽象层

**阶段三: 功能模块转换**
- 逐个转换 containers、memory、drivers 等模块
- 完全替换头文件系统，无需向后兼容

**阶段四: 完全迁移**
- 移除旧的头文件依赖
- 优化模块边界和导出
- 验证所有架构的编译性能提升

## 质量保证

- 每个阶段都保持现有的所有单元测试通过
- 确保三个架构（ARM64/x86_64/RISC-V）的兼容性
- 性能基准测试验证编译时间改善
- 保持 freestanding 环境的完全兼容性

## 预期收益

- **编译性能**: 模块缓存和并行编译将显著提升构建速度
- **代码质量**: 更清晰的模块边界和依赖关系
- **可维护性**: 减少头文件包含地狱，更好的封装
- **现代化**: 使用最新的 C++20 特性，为未来发展奠定基础

## 风险评估

- **兼容性风险**: 需要确保 Clang 21 对所有目标架构的模块支持
- **工具链依赖**: 依赖 uv 和新版本 CMake，需要文档化环境要求
- **迁移复杂性**: 大型代码库的模块化可能遇到循环依赖等问题

## 成功标准

1. 所有现有功能保持不变
2. 三个架构的编译成功率 100%
3. 编译时间相比现状有显著改善（目标 20% 提升）
4. 代码覆盖率保持或提升
5. 模块依赖关系清晰无循环依赖