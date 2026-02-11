#pragma once

// 现代C++风格的错误处理 - Result<T, E>类型

#include "types.hpp"

// 使用标准库concepts
#include "moss_std.hpp"

// 内核环境下的基础 type traits 实现
namespace moss::kernel::detail {
    // 基础类型特征
    template<typename T>
    struct remove_reference { using type = T; };
    template<typename T>
    struct remove_reference<T&> { using type = T; };
    template<typename T>
    struct remove_reference<T&&> { using type = T; };

    template<typename T>
    using remove_reference_t = typename remove_reference<T>::type;

    // move 实现
    template<typename T>
    constexpr remove_reference_t<T>&& move(T&& t) noexcept {
        return static_cast<remove_reference_t<T>&&>(t);
    }

    // forward 实现
    template<typename T>
    constexpr T&& forward(remove_reference_t<T>& t) noexcept {
        return static_cast<T&&>(t);
    }

    template<typename T>
    constexpr T&& forward(remove_reference_t<T>&& t) noexcept {
        return static_cast<T&&>(t);
    }

    // 简化的 type traits (假设所有类型都是 nothrow constructible)
    template<typename T>
    struct is_nothrow_copy_constructible { static constexpr bool value = true; };
    template<typename T>
    constexpr bool is_nothrow_copy_constructible_v = is_nothrow_copy_constructible<T>::value;

    template<typename T>
    struct is_nothrow_move_constructible { static constexpr bool value = true; };
    template<typename T>
    constexpr bool is_nothrow_move_constructible_v = is_nothrow_move_constructible<T>::value;

    template<typename T>
    struct is_nothrow_copy_assignable { static constexpr bool value = true; };
    template<typename T>
    constexpr bool is_nothrow_copy_assignable_v = is_nothrow_copy_assignable<T>::value;
}

namespace moss::kernel {

// 错误代码枚举
enum class ErrorCode : u32 {
    Success = 0,
    OutOfMemory,
    InvalidParameter,
    PermissionDenied,
    NotFound,
    AlreadyExists,
    Timeout,
    DeviceBusy,
    IoError,
    NetworkError,
    InvalidState,
    Interrupted,
    TooManyFiles,
    NoSpace,
    ReadOnly,
    NotSupported,
    // IPC相关错误
    InvalidArgument,
    ResourceExhausted,
    Busy,
    InternalError,
    Unknown = 0xFFFFFFFF
};

// 内核错误类型别名
using KernelError = ErrorCode;

// 将错误码转换为字符串的函数声明
const char* error_to_string(ErrorCode error) noexcept;

// Result类型实现 - 类似Rust的Result<T, E>
template<typename T, typename E = ErrorCode>
class [[nodiscard]] Result {
private:
    union {
        T value_;
        E error_;
    };
    bool has_value_;

public:
    // 构造函数
    constexpr Result(const T& value) noexcept(is_nothrow_copy_constructible_v<T>)
        : value_(value), has_value_(true) {}

    constexpr Result(T&& value) noexcept(is_nothrow_move_constructible_v<T>)
        : value_(move(value)), has_value_(true) {}

    constexpr Result(const E& error) noexcept(is_nothrow_copy_constructible_v<E>)
        : error_(error), has_value_(false) {}

    constexpr Result(E&& error) noexcept(is_nothrow_move_constructible_v<E>)
        : error_(move(error)), has_value_(false) {}

    // 拷贝构造函数
    constexpr Result(const Result& other) noexcept(
        is_nothrow_copy_constructible_v<T> &&
        is_nothrow_copy_constructible_v<E>)
        : has_value_(other.has_value_) {
        if (has_value_) {
            new (&value_) T(other.value_);
        } else {
            new (&error_) E(other.error_);
        }
    }

    // 移动构造函数
    constexpr Result(Result&& other) noexcept(
        is_nothrow_move_constructible_v<T> &&
        is_nothrow_move_constructible_v<E>)
        : has_value_(other.has_value_) {
        if (has_value_) {
            new (&value_) T(move(other.value_));
        } else {
            new (&error_) E(move(other.error_));
        }
    }

    // 析构函数
    ~Result() noexcept {
        if (has_value_) {
            value_.~T();
        } else {
            error_.~E();
        }
    }

    // 赋值操作符
    constexpr Result& operator=(const Result& other) noexcept(
        is_nothrow_copy_constructible_v<T> &&
        is_nothrow_copy_constructible_v<E> &&
        is_nothrow_copy_assignable_v<T> &&
        is_nothrow_copy_assignable_v<E>) {
        if (this != &other) {
            if (has_value_ && other.has_value_) {
                value_ = other.value_;
            } else if (!has_value_ && !other.has_value_) {
                error_ = other.error_;
            } else {
                this->~Result();
                has_value_ = other.has_value_;
                if (has_value_) {
                    new (&value_) T(other.value_);
                } else {
                    new (&error_) E(other.error_);
                }
            }
        }
        return *this;
    }

    // 检查是否包含值
    [[nodiscard]] constexpr bool has_value() const noexcept {
        return has_value_;
    }

    [[nodiscard]] constexpr bool is_ok() const noexcept {
        return has_value_;
    }

    [[nodiscard]] constexpr bool is_error() const noexcept {
        return !has_value_;
    }

    // 获取值（不安全，需要先检查）
    [[nodiscard]] constexpr const T& value() const & noexcept {
        return value_;
    }

    [[nodiscard]] constexpr T& value() & noexcept {
        return value_;
    }

    [[nodiscard]] constexpr T&& value() && noexcept {
        return move(value_);
    }

    // 获取错误（不安全，需要先检查）
    [[nodiscard]] constexpr const E& error() const & noexcept {
        return error_;
    }

    [[nodiscard]] constexpr E& error() & noexcept {
        return error_;
    }

    [[nodiscard]] constexpr E&& error() && noexcept {
        return move(error_);
    }

    // 安全的值获取
    [[nodiscard]] constexpr T value_or(const T& default_value) const & {
        return has_value_ ? value_ : default_value;
    }

    [[nodiscard]] constexpr T value_or(T&& default_value) && {
        return has_value_ ? move(value_) : move(default_value);
    }

    // 操作符重载
    constexpr explicit operator bool() const noexcept {
        return has_value_;
    }

    [[nodiscard]] constexpr const T& operator*() const & noexcept {
        return value_;
    }

    [[nodiscard]] constexpr T& operator*() & noexcept {
        return value_;
    }

    [[nodiscard]] constexpr T&& operator*() && noexcept {
        return move(value_);
    }

    [[nodiscard]] constexpr const T* operator->() const noexcept {
        return &value_;
    }

    [[nodiscard]] constexpr T* operator->() noexcept {
        return &value_;
    }
};

// void特化 - 用于只返回错误状态的函数
template<typename E>
class [[nodiscard]] Result<void, E> {
private:
    E error_;
    bool has_value_;

public:
    // 成功构造函数
    constexpr Result() noexcept : has_value_(true) {}

    // 错误构造函数
    constexpr Result(const E& error) noexcept(is_nothrow_copy_constructible_v<E>)
        : error_(error), has_value_(false) {}

    constexpr Result(E&& error) noexcept(is_nothrow_move_constructible_v<E>)
        : error_(move(error)), has_value_(false) {}

    // 检查方法
    [[nodiscard]] constexpr bool has_value() const noexcept {
        return has_value_;
    }

    [[nodiscard]] constexpr bool is_ok() const noexcept {
        return has_value_;
    }

    [[nodiscard]] constexpr bool is_error() const noexcept {
        return !has_value_;
    }

    // 获取错误
    [[nodiscard]] constexpr const E& error() const & noexcept {
        return error_;
    }

    [[nodiscard]] constexpr E& error() & noexcept {
        return error_;
    }

    // 操作符重载
    constexpr explicit operator bool() const noexcept {
        return has_value_;
    }
};

// 便利的类型别名
template<typename T>
using KernelResult = Result<T, ErrorCode>;

using VoidResult = Result<void, ErrorCode>;

// 便利函数
template<typename T>
[[nodiscard]] constexpr Result<T> Ok(T&& value) {
    return Result<T>{move(value)};
}

[[nodiscard]] constexpr Result<void> Ok() {
    return Result<void>{};
}

template<typename E>
[[nodiscard]] constexpr Result<void, E> Error(E&& error) {
    return Result<void, E>{moss::forward<E>(error)};
}

template<typename T, typename E>
[[nodiscard]] constexpr Result<T, E> Error(E&& error) {
    return Result<T, E>{moss::forward<E>(error)};
}

} // namespace moss::kernel
