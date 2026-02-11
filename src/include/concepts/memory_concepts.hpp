#pragma once

// 简化的内存管理概念定义
// 为freestanding环境提供基本类型约束

namespace moss::concepts {

// 简化的内存管理概念 - 不使用复杂的concepts语法
template<typename T>
struct UniquePtrCompatible {
    using type = T;
};

template<typename T>
struct SmartPointer {
    using type = T;
};

template<typename Alloc>
struct Allocator {
    using type = Alloc;
};

template<typename T, typename... Args>
struct Constructible {
    using type = T;
};

template<typename T>
struct MoveConstructible {
    using type = T;
};

template<typename T>
struct CopyConstructible {
    using type = T;
};

template<typename T>
struct Destructible {
    using type = T;
};

template<typename T>
struct RAIIResource {
    using type = T;
};

} // namespace moss::concepts
