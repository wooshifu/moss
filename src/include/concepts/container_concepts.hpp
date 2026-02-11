#pragma once

// 简化的容器概念定义
// 为freestanding环境提供基本类型约束

namespace moss::concepts {

// 简化的容器概念 - 不使用复杂的concepts语法
template<typename T>
struct Container {
    // 只是一个标记类型，实际约束在使用时检查
    using type = T;
};

template<typename T>
struct IterableContainer {
    using type = T;
};

template<typename T>
struct SequenceContainer {
    using type = T;
};

template<typename T>
struct Stack {
    using type = T;
};

template<typename T>
struct Queue {
    using type = T;
};

template<typename T>
struct LockFree {
    using type = T;
};

template<typename T>
struct MemoryPool {
    using type = T;
};

template<typename T>
struct SlabAllocator {
    using type = T;
};

template<typename T>
struct RCUDataStructure {
    using type = T;
};

} // namespace moss::concepts
