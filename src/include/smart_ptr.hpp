#pragma once

// 内核专用智能指针实现
// 替代std::unique_ptr和std::shared_ptr

#include "containers/atomic_types.hpp"
#include "moss_std.hpp" // 裸机环境基础定义
#include "result.hpp"
#include "types.hpp"

// 包含concepts约束
#include "concepts/memory_concepts.hpp"

namespace moss::kernel {

// 内核专用unique_ptr实现
template <typename T> class UniquePtr {
private:
  T *ptr_;

public:
  // 构造函数
  UniquePtr() noexcept : ptr_(nullptr) {}
  UniquePtr(T *p) noexcept : ptr_(p) {}
  UniquePtr(nullptr_t) noexcept : ptr_(nullptr) {}

  // 移动构造和赋值
  UniquePtr(UniquePtr &&other) noexcept : ptr_(other.release()) {}

  UniquePtr &operator=(UniquePtr &&other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  // 禁用拷贝
  UniquePtr(const UniquePtr &) = delete;
  UniquePtr &operator=(const UniquePtr &) = delete;

  // 析构函数
  ~UniquePtr() noexcept {
    if (ptr_ != nullptr) {
      delete ptr_;
    }
  }

  // 访问操作符
  T &operator*() const noexcept { return *ptr_; }
  T *operator->() const noexcept { return ptr_; }
  T *get() const noexcept { return ptr_; }

  // 布尔转换
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  // 释放所有权
  [[nodiscard]] T *release() noexcept {
    T *result = ptr_;
    ptr_ = nullptr;
    return result;
  }

  // 重置指针
  void reset(T *new_ptr = nullptr) noexcept {
    T *old_ptr = ptr_;
    ptr_ = new_ptr;
    if (old_ptr != nullptr) {
      delete old_ptr;
    }
  }

  // 交换
  void swap(UniquePtr &other) noexcept {
    T *temp = ptr_;
    ptr_ = other.ptr_;
    other.ptr_ = temp;
  }
};

// 创建unique_ptr的便利函数
template <typename T, typename... Args>
[[nodiscard]] UniquePtr<T> make_unique(Args &&...args) {
  return UniquePtr<T>(new T(moss::forward<Args>(args)...));
}

// 内核专用shared_ptr实现（简化版本）
template <typename T> class SharedPtr {
private:
  struct ControlBlock {
    T *ptr;
    containers::AtomicCounter<u32> ref_count;

    ControlBlock(T *p) noexcept : ptr(p), ref_count(1) {}
  };

  ControlBlock *control_;

  void release() noexcept {
    if (control_ != nullptr) {
      if (control_->ref_count.fetch_sub(1, containers::MemoryOrder::AcqRel) ==
          1) {
        delete control_->ptr;
        delete control_;
      }
    }
  }

public:
  // 构造函数
  constexpr SharedPtr() noexcept : control_(nullptr) {}

  explicit SharedPtr(T *ptr) noexcept
      : control_(ptr ? new ControlBlock(ptr) : nullptr) {}

  // 拷贝构造
  SharedPtr(const SharedPtr &other) noexcept : control_(other.control_) {
    if (control_ != nullptr) {
      (void)control_->ref_count.fetch_add(1, containers::MemoryOrder::Relaxed);
    }
  }

  // 移动构造
  SharedPtr(SharedPtr &&other) noexcept : control_(other.control_) {
    other.control_ = nullptr;
  }

  // 赋值操作符
  SharedPtr &operator=(const SharedPtr &other) noexcept {
    if (this != &other) {
      release();
      control_ = other.control_;
      if (control_ != nullptr) {
        (void)control_->ref_count.fetch_add(1,
                                            containers::MemoryOrder::Relaxed);
      }
    }
    return *this;
  }

  SharedPtr &operator=(SharedPtr &&other) noexcept {
    if (this != &other) {
      release();
      control_ = other.control_;
      other.control_ = nullptr;
    }
    return *this;
  }

  // 析构函数
  ~SharedPtr() noexcept { release(); }

  // 访问操作符
  T &operator*() const noexcept { return *control_->ptr; }
  T *operator->() const noexcept { return control_->ptr; }
  T *get() const noexcept { return control_ ? control_->ptr : nullptr; }

  // 布尔转换
  explicit operator bool() const noexcept {
    return control_ != nullptr && control_->ptr != nullptr;
  }

  // 引用计数
  [[nodiscard]] u32 use_count() const noexcept {
    return control_ ? control_->ref_count.load(containers::MemoryOrder::Relaxed)
                    : 0;
  }

  // 重置
  void reset(T *new_ptr = nullptr) noexcept {
    SharedPtr temp(new_ptr);
    swap(temp);
  }

  // 交换
  void swap(SharedPtr &other) noexcept {
    ControlBlock *temp = control_;
    control_ = other.control_;
    other.control_ = temp;
  }
};

// 创建shared_ptr的便利函数
template <typename T, typename... Args>
[[nodiscard]] SharedPtr<T> make_shared(Args &&...args) {
  return SharedPtr<T>(new T(moss::forward<Args>(args)...));
}

// 便利的类型别名
template <typename T> using unique_ptr = UniquePtr<T>;

template <typename T> using shared_ptr = SharedPtr<T>;

} // namespace moss::kernel
