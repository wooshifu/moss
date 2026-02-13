# OBJECT 库循环依赖的解决办法

## 问题背景

MOSS 内核使用 CMake OBJECT library 组织各模块。OBJECT library 在链接阶段直接将 `.o` 文件合并进最终的 ELF，不会像 STATIC library 那样丢弃"未引用"的符号——这对内核至关重要，因为中断处理函数、`extern "C"` 入口等符号只从汇编代码引用，链接器无法通过 C++ 符号表发现它们。

但 CMake 对 OBJECT library 有一个硬性限制：**不允许循环依赖**。

## 为什么不用 STATIC library

STATIC library (`add_library(xxx STATIC)`) 允许循环依赖，但它在内核场景下有致命缺陷：

| 特性 | OBJECT library | STATIC library |
|------|---------------|---------------|
| 未引用符号 | 全部保留 | **被丢弃** |
| 中断处理函数 | 安全 | 可能被丢弃 |
| extern "C" (汇编引用) | 安全 | 可能被丢弃 |
| 循环依赖 | 不允许 | 允许 |
| 链接行为 | 直接合并 .o | ar 归档，按需提取 |

内核中大量关键函数仅从汇编或硬件中断表引用，链接器的符号解析无法追踪这些引用。使用 STATIC library 会导致这些函数被静默丢弃，产生运行时崩溃。

## 解决方案：INTERFACE library 分离头文件依赖

循环依赖的根源通常是：**模块 A 的头文件被模块 B 的头文件 include，同时模块 B 的实现又依赖模块 A**。本质上是"编译期头文件依赖"和"链接期实现依赖"混在了一起。

解决思路：为模块创建一个 **INTERFACE library**，只传播 include 目录，不携带任何编译目标。需要头文件的模块链接 INTERFACE library，需要实现的模块链接完整的 OBJECT library。

### 模式模板

```cmake
# src/xxx/CMakeLists.txt

# 1. INTERFACE library —— 只暴露头文件路径，不编译任何源码
add_library(moss_xxx_headers INTERFACE)
target_include_directories(moss_xxx_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(moss_xxx_headers INTERFACE moss_core)  # 仅传递头文件级别的依赖

# 2. OBJECT library —— 正常编译，可以依赖其他模块的完整实现
add_library(moss_xxx OBJECT)
target_sources(moss_xxx PRIVATE src/xxx.cpp)
target_include_directories(moss_xxx PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(moss_xxx PRIVATE moss_core moss_yyy)  # 可以依赖 moss_yyy 的完整实现
```

需要 `xxx` 头文件但不需要其实现的模块：

```cmake
# src/yyy/CMakeLists.txt
target_link_libraries(moss_yyy PRIVATE moss_xxx_headers)  # 只拿头文件，不形成编译依赖
```

### 关键规则

- **INTERFACE library 的依赖链必须无环**。如果 `moss_xxx_headers` 依赖 `moss_yyy`，而 `moss_yyy` 又依赖 `moss_xxx_headers`，循环依然存在。INTERFACE library 只应链接它的头文件真正需要的最小依赖集。
- **INTERFACE library 不产生 .o 文件**，所以根 CMakeLists.txt 的 `TARGET_OBJECTS` 列表不需要添加它。
- **同一模块的两个 library 共享 include 目录**，`moss_xxx` 和 `moss_xxx_headers` 都指向 `${CMAKE_CURRENT_SOURCE_DIR}/include`。

## MOSS 中的实际案例

项目中有三处使用了此模式：

### 案例 1：kernel ↔ process

process 模块的 `process.hpp` 需要 kernel 的头文件定义（如 `kernel_main.hpp` 中的类型），而 kernel 模块的实现又依赖 process 模块。

```
moss_kernel_headers (INTERFACE)  ← 只暴露 kernel/include
    ↑
moss_process (OBJECT)            ← 链接 moss_kernel_headers 获取头文件
    ↑
moss_kernel (OBJECT)             ← 链接 moss_process 获取完整实现
```

```cmake
# src/kernel/CMakeLists.txt
add_library(moss_kernel_headers INTERFACE)
target_include_directories(moss_kernel_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(moss_kernel_headers INTERFACE moss_core)

# src/process/CMakeLists.txt
target_link_libraries(moss_process ... PUBLIC moss_kernel_headers)
```

### 案例 2：process ↔ ipc

ipc 模块的 `ipc_manager.hpp` include 了 `process/process.hpp`，而 process 模块的实现依赖 ipc。

```
moss_process_headers (INTERFACE) ← 只暴露 process/include
    ↑
moss_ipc (OBJECT)                ← 链接 moss_process_headers 获取头文件
    ↑
moss_process (OBJECT)            ← 链接 moss_ipc 获取完整实现
```

```cmake
# src/process/CMakeLists.txt
add_library(moss_process_headers INTERFACE)
target_include_directories(moss_process_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(moss_process_headers INTERFACE moss_core moss_containers)

# src/ipc/CMakeLists.txt
target_link_libraries(moss_ipc PRIVATE moss_core moss_containers moss_mm
    PUBLIC moss_process_headers)
```

### 案例 3：mm ↔ containers

containers 模块的 `slab_allocator.hpp` include 了 `mm/page_frame_allocator.hpp`，而 mm 模块的实现依赖 containers。

```
moss_mm_headers (INTERFACE)      ← 只暴露 mm/include
    ↑
moss_containers (OBJECT)         ← 链接 moss_mm_headers 获取头文件
    ↑
moss_mm (OBJECT)                 ← 链接 moss_containers 获取完整实现
```

```cmake
# src/mm/CMakeLists.txt
add_library(moss_mm_headers INTERFACE)
target_include_directories(moss_mm_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(moss_mm_headers INTERFACE moss_core)

# src/containers/CMakeLists.txt
target_link_libraries(moss_containers PUBLIC moss_core moss_mm_headers)
```

## 依赖关系全景

```
moss_core (OBJECT)
    ↑
moss_containers (OBJECT)  ←─── moss_mm_headers (INTERFACE)
    ↑                                ↑
moss_mm (OBJECT)  ───────────────────┘ (自身的 headers)
    ↑
moss_drivers (OBJECT)
moss_interrupts (OBJECT)
    ↑
moss_ipc (OBJECT)  ←──── moss_process_headers (INTERFACE)
    ↑                              ↑
moss_process (OBJECT)  ←── moss_kernel_headers (INTERFACE)
    ↑                              ↑
moss_kernel (OBJECT)  ─────────────┘ (自身的 headers)
    ↑
moss_boot (OBJECT)
    ↓
moss.elf
```

## 判断何时需要此模式

遇到以下 CMake 报错时，考虑使用 INTERFACE library：

```
The inter-target dependency graph contains the following
strongly connected component (cycle):
  "moss_A" depends on "moss_B"
  "moss_B" depends on "moss_A"
```

排查步骤：

1. 确认是哪个头文件造成了 A → B 的依赖（通常是 A 的某个 `.hpp` include 了 B 的头文件）
2. 如果 A 只需要 B 的**类型定义**（头文件），不需要 B 的**函数实现**（.cpp），就为 B 创建 `moss_B_headers` INTERFACE library
3. 将 A 对 B 的依赖从 `moss_B` 改为 `moss_B_headers`
4. 确保 `moss_B_headers` 的依赖链中不会重新引入 A，否则循环依然存在
