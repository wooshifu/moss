// MOSS Smart Pointer Module - Kernel UniquePtr and SharedPtr
// Replacement for std::unique_ptr and std::shared_ptr in freestanding environment
export module moss.smart_ptr;

import moss.std;
import moss.types;

export namespace moss::kernel {

// Kernel UniquePtr implementation
template <typename T> class UniquePtr {
private:
  T *ptr_;

public:
  // Constructors
  UniquePtr() noexcept : ptr_(nullptr) {}
  explicit UniquePtr(T *p) noexcept : ptr_(p) {}
  UniquePtr(nullptr_t) noexcept : ptr_(nullptr) {}

  // Move construct and assign
  UniquePtr(UniquePtr &&other) noexcept : ptr_(other.release()) {}

  UniquePtr &operator=(UniquePtr &&other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  // Disable copy
  UniquePtr(const UniquePtr &) = delete;
  UniquePtr &operator=(const UniquePtr &) = delete;

  // Destructor
  ~UniquePtr() noexcept {
    if (ptr_ != nullptr) {
      delete ptr_;
    }
  }

  // Access operators
  T &operator*() const noexcept { return *ptr_; }
  T *operator->() const noexcept { return ptr_; }
  T *get() const noexcept { return ptr_; }

  // Boolean conversion
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  // Release ownership
  [[nodiscard]] T *release() noexcept {
    T *result = ptr_;
    ptr_ = nullptr;
    return result;
  }

  // Reset pointer
  void reset(T *new_ptr = nullptr) noexcept {
    T *old_ptr = ptr_;
    ptr_ = new_ptr;
    if (old_ptr != nullptr) {
      delete old_ptr;
    }
  }

  // Swap
  void swap(UniquePtr &other) noexcept {
    T *temp = ptr_;
    ptr_ = other.ptr_;
    other.ptr_ = temp;
  }
};

// Kernel SharedPtr implementation (simplified for kernel use)
template <typename T> class SharedPtr {
private:
  struct ControlBlock {
    T *ptr;
    atomic<u32> ref_count;

    ControlBlock(T *p) noexcept : ptr(p), ref_count(1) {}
  };

  ControlBlock *control_;

  void release_ref() noexcept {
    if (control_ != nullptr) {
      if (control_->ref_count.fetch_sub(1, memory_order_acq_rel) == 1) {
        delete control_->ptr;
        delete control_;
      }
    }
  }

public:
  // Constructors
  constexpr SharedPtr() noexcept : control_(nullptr) {}

  explicit SharedPtr(T *ptr) noexcept
      : control_(ptr ? new ControlBlock(ptr) : nullptr) {}

  // Copy construct
  SharedPtr(const SharedPtr &other) noexcept : control_(other.control_) {
    if (control_ != nullptr) {
      (void)control_->ref_count.fetch_add(1, memory_order_relaxed);
    }
  }

  // Move construct
  SharedPtr(SharedPtr &&other) noexcept : control_(other.control_) {
    other.control_ = nullptr;
  }

  // Copy assign
  SharedPtr &operator=(const SharedPtr &other) noexcept {
    if (this != &other) {
      release_ref();
      control_ = other.control_;
      if (control_ != nullptr) {
        (void)control_->ref_count.fetch_add(1, memory_order_relaxed);
      }
    }
    return *this;
  }

  // Move assign
  SharedPtr &operator=(SharedPtr &&other) noexcept {
    if (this != &other) {
      release_ref();
      control_ = other.control_;
      other.control_ = nullptr;
    }
    return *this;
  }

  // Destructor
  ~SharedPtr() noexcept { release_ref(); }

  // Access operators
  T &operator*() const noexcept { return *control_->ptr; }
  T *operator->() const noexcept { return control_->ptr; }
  T *get() const noexcept { return control_ ? control_->ptr : nullptr; }

  // Boolean conversion
  explicit operator bool() const noexcept {
    return control_ != nullptr && control_->ptr != nullptr;
  }

  // Reference count
  [[nodiscard]] u32 use_count() const noexcept {
    return control_ ? control_->ref_count.load(memory_order_relaxed) : 0;
  }

  // Reset
  void reset(T *new_ptr = nullptr) noexcept {
    SharedPtr temp(new_ptr);
    swap(temp);
  }

  // Swap
  void swap(SharedPtr &other) noexcept {
    ControlBlock *temp = control_;
    control_ = other.control_;
    other.control_ = temp;
  }
};

// Factory functions
template <typename T, typename... Args>
[[nodiscard]] UniquePtr<T> make_unique(Args &&...args) {
  return UniquePtr<T>(new T(forward<Args>(args)...));
}

template <typename T, typename... Args>
[[nodiscard]] SharedPtr<T> make_shared(Args &&...args) {
  return SharedPtr<T>(new T(forward<Args>(args)...));
}

// Convenience type aliases (lowercase)
template <typename T> using unique_ptr = UniquePtr<T>;
template <typename T> using shared_ptr = SharedPtr<T>;

} // namespace moss::kernel
