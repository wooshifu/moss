// src/modules/result.cppm
// MOSS Result Type Module - Error Handling for Kernel Operations
// Provides Result<T, E> type for safe error propagation without exceptions

module;

export module moss.result;

import moss.std;
import moss.types;

export namespace moss::kernel {

// Forward declarations for helper functions
template <typename T> class Ok;
template <typename E> class Err;

// Primary Result template for value/error handling
template <typename T, typename E = ErrorCode> class Result {
private:
  union Storage {
    T value_;
    E error_;

    // Default constructor - does nothing
    constexpr Storage() noexcept {}

    // Value constructor
    template <typename... Args>
    constexpr Storage(bool /*unused*/, Args &&...args) noexcept(noexcept(T(forward<Args>(args)...)))
        : value_(forward<Args>(args)...) {}

    // Error constructor
    template <typename... Args>
    constexpr Storage(int /*unused*/, Args &&...args) noexcept(noexcept(E(forward<Args>(args)...)))
        : error_(forward<Args>(args)...) {}

    // Destructor - does nothing (Result handles destruction)
    ~Storage() {}
  } storage_;

  bool has_value_;

  // Helper to destroy current value
  constexpr void destroy() noexcept {
    if (has_value_) {
      if constexpr (!is_void_v<T>) {
        storage_.value_.~T();
      }
    } else {
      storage_.error_.~E();
    }
  }

public:
  using value_type = T;
  using error_type = E;

  // Constructors

  // Default constructor - creates error state with default error
  constexpr Result() noexcept(noexcept(E())) : storage_(1, E()), has_value_(false) {}

  // Value constructor
  template <typename U = T>
  constexpr Result(const U &value) noexcept(noexcept(T(value)))
    requires(!is_same_v<remove_cv_t<remove_reference_t<U>>, Result> &&
             !is_same_v<remove_cv_t<remove_reference_t<U>>, Ok<T>> &&
             !is_same_v<remove_cv_t<remove_reference_t<U>>, Err<E>> &&
             !is_same_v<remove_cv_t<remove_reference_t<U>>, E>)
      : storage_(true, value), has_value_(true) {}

  template <typename U = T>
  constexpr Result(U &&value) noexcept(noexcept(T(forward<U>(value))))
    requires(!is_same_v<remove_cv_t<remove_reference_t<U>>, Result> &&
             !is_same_v<remove_cv_t<remove_reference_t<U>>, Ok<T>> &&
             !is_same_v<remove_cv_t<remove_reference_t<U>>, Err<E>> &&
             !is_same_v<remove_cv_t<remove_reference_t<U>>, E>)
      : storage_(true, forward<U>(value)), has_value_(true) {}

  // Ok constructor
  constexpr Result(const Ok<T> &ok) noexcept(noexcept(T(ok.value_))) : storage_(true, ok.value_), has_value_(true) {}

  constexpr Result(Ok<T> &&ok) noexcept(noexcept(T(move(ok.value_))))
      : storage_(true, move(ok.value_)), has_value_(true) {}

  // Direct error constructor (allows Result{ErrorType::value})
  constexpr Result(const E &error) noexcept(noexcept(E(error)))
    requires(!is_same_v<E, T>)
      : storage_(1, error), has_value_(false) {}

  constexpr Result(E &&error) noexcept(noexcept(E(move(error))))
    requires(!is_same_v<E, T>)
      : storage_(1, move(error)), has_value_(false) {}

  // Err constructor
  constexpr Result(const Err<E> &err) noexcept(noexcept(E(err.error_))) : storage_(1, err.error_), has_value_(false) {}

  constexpr Result(Err<E> &&err) noexcept(noexcept(E(move(err.error_))))
      : storage_(1, move(err.error_)), has_value_(false) {}

  // Copy constructor
  constexpr Result(const Result &other) noexcept(noexcept(T(other.storage_.value_)) &&
                                                 noexcept(E(other.storage_.error_)))
      : has_value_(other.has_value_) {
    if (has_value_) {
      new (&storage_.value_) T(other.storage_.value_);
    } else {
      new (&storage_.error_) E(other.storage_.error_);
    }
  }

  // Move constructor
  constexpr Result(Result &&other) noexcept(noexcept(T(move(other.storage_.value_))) &&
                                            noexcept(E(move(other.storage_.error_))))
      : has_value_(other.has_value_) {
    if (has_value_) {
      new (&storage_.value_) T(move(other.storage_.value_));
    } else {
      new (&storage_.error_) E(move(other.storage_.error_));
    }
  }

  // Destructor
  constexpr ~Result() noexcept { destroy(); }

  // Assignment operators

  constexpr Result &operator=(const Result &other) noexcept(noexcept(T(other.storage_.value_)) &&
                                                            noexcept(E(other.storage_.error_))) {
    if (this != &other) {
      destroy();
      has_value_ = other.has_value_;
      if (has_value_) {
        new (&storage_.value_) T(other.storage_.value_);
      } else {
        new (&storage_.error_) E(other.storage_.error_);
      }
    }
    return *this;
  }

  constexpr Result &operator=(Result &&other) noexcept(noexcept(T(move(other.storage_.value_))) &&
                                                       noexcept(E(move(other.storage_.error_)))) {
    if (this != &other) {
      destroy();
      has_value_ = other.has_value_;
      if (has_value_) {
        new (&storage_.value_) T(move(other.storage_.value_));
      } else {
        new (&storage_.error_) E(move(other.storage_.error_));
      }
    }
    return *this;
  }

  // Value assignment
  template <typename U = T>
  constexpr Result &operator=(const U &value) noexcept(noexcept(T(value)))
    requires(!is_same_v<remove_cv_t<remove_reference_t<U>>, Result> &&
             !is_same_v<remove_cv_t<remove_reference_t<U>>, Ok<T>> &&
             !is_same_v<remove_cv_t<remove_reference_t<U>>, Err<E>> &&
             !is_same_v<remove_cv_t<remove_reference_t<U>>, E>)
  {
    destroy();
    has_value_ = true;
    new (&storage_.value_) T(value);
    return *this;
  }

  template <typename U = T>
  constexpr Result &operator=(U &&value) noexcept(noexcept(T(forward<U>(value))))
    requires(!is_same_v<remove_cv_t<remove_reference_t<U>>, Result> &&
             !is_same_v<remove_cv_t<remove_reference_t<U>>, Ok<T>> &&
             !is_same_v<remove_cv_t<remove_reference_t<U>>, Err<E>> &&
             !is_same_v<remove_cv_t<remove_reference_t<U>>, E>)
  {
    destroy();
    has_value_ = true;
    new (&storage_.value_) T(forward<U>(value));
    return *this;
  }

  // Ok assignment
  constexpr Result &operator=(const Ok<T> &ok) noexcept(noexcept(T(ok.value_))) {
    destroy();
    has_value_ = true;
    new (&storage_.value_) T(ok.value_);
    return *this;
  }

  constexpr Result &operator=(Ok<T> &&ok) noexcept(noexcept(T(move(ok.value_)))) {
    destroy();
    has_value_ = true;
    new (&storage_.value_) T(move(ok.value_));
    return *this;
  }

  // Err assignment
  constexpr Result &operator=(const Err<E> &err) noexcept(noexcept(E(err.error_))) {
    destroy();
    has_value_ = false;
    new (&storage_.error_) E(err.error_);
    return *this;
  }

  constexpr Result &operator=(Err<E> &&err) noexcept(noexcept(E(move(err.error_)))) {
    destroy();
    has_value_ = false;
    new (&storage_.error_) E(move(err.error_));
    return *this;
  }

  // State checking
  constexpr bool has_value() const noexcept { return has_value_; }
  constexpr bool is_ok() const noexcept { return has_value_; }
  constexpr bool is_err() const noexcept { return !has_value_; }
  constexpr operator bool() const noexcept { return has_value_; }

  // Value access (const reference versions)
  constexpr const T &operator*() const & noexcept { return storage_.value_; }

  constexpr const T *operator->() const noexcept { return &storage_.value_; }

  constexpr const T &value() const & {
    if (!has_value_) {
      __builtin_unreachable();
    }
    return storage_.value_;
  }

  // Value access (mutable lvalue reference versions)
  constexpr T &operator*() & noexcept { return storage_.value_; }

  constexpr T *operator->() noexcept { return &storage_.value_; }

  constexpr T &value() & {
    if (!has_value_) {
      __builtin_unreachable();
    }
    return storage_.value_;
  }

  // Value access (rvalue reference versions)
  constexpr T &&operator*() && noexcept { return move(storage_.value_); }

  constexpr T &&value() && {
    if (!has_value_) {
      __builtin_unreachable();
    }
    return move(storage_.value_);
  }

  // Error access
  constexpr const E &error() const & noexcept { return storage_.error_; }

  constexpr E &&error() && noexcept { return move(storage_.error_); }

  // Value with fallback
  template <typename U>
  constexpr T value_or(U &&default_value) const & noexcept(noexcept(T(forward<U>(default_value)))) {
    return has_value_ ? storage_.value_ : static_cast<T>(forward<U>(default_value));
  }

  template <typename U> constexpr T value_or(U &&default_value) && noexcept(noexcept(T(forward<U>(default_value)))) {
    return has_value_ ? move(storage_.value_) : static_cast<T>(forward<U>(default_value));
  }
};

// Specialization for Result<void, E> - no value storage needed
template <typename E> class Result<void, E> {
private:
  E error_;
  bool has_value_;

public:
  using value_type = void;
  using error_type = E;

  // Constructors
  constexpr Result() noexcept : has_value_(true) {}

  constexpr Result(const Ok<void> & /*unused*/) noexcept : has_value_(true) {}
  constexpr Result(Ok<void> && /*unused*/) noexcept : has_value_(true) {}

  // Direct error constructor
  constexpr Result(const E &error) noexcept(noexcept(E(error))) : error_(error), has_value_(false) {}

  constexpr Result(E &&error) noexcept(noexcept(E(move(error)))) : error_(move(error)), has_value_(false) {}

  constexpr Result(const Err<E> &err) noexcept(noexcept(E(err.error_))) : error_(err.error_), has_value_(false) {}

  constexpr Result(Err<E> &&err) noexcept(noexcept(E(move(err.error_))))
      : error_(move(err.error_)), has_value_(false) {}

  // Copy constructor
  constexpr Result(const Result &other) noexcept(noexcept(E(other.error_)))
      : error_(other.error_), has_value_(other.has_value_) {}

  // Move constructor
  constexpr Result(Result &&other) noexcept(noexcept(E(move(other.error_))))
      : error_(move(other.error_)), has_value_(other.has_value_) {}

  // Assignment operators
  constexpr Result &operator=(const Result &other) noexcept(noexcept(E(other.error_))) {
    if (this != &other) {
      error_ = other.error_;
      has_value_ = other.has_value_;
    }
    return *this;
  }

  constexpr Result &operator=(Result &&other) noexcept(noexcept(E(move(other.error_)))) {
    if (this != &other) {
      error_ = move(other.error_);
      has_value_ = other.has_value_;
    }
    return *this;
  }

  constexpr Result &operator=(const Ok<void> & /*unused*/) noexcept {
    has_value_ = true;
    return *this;
  }

  constexpr Result &operator=(Ok<void> && /*unused*/) noexcept {
    has_value_ = true;
    return *this;
  }

  constexpr Result &operator=(const Err<E> &err) noexcept(noexcept(E(err.error_))) {
    error_ = err.error_;
    has_value_ = false;
    return *this;
  }

  constexpr Result &operator=(Err<E> &&err) noexcept(noexcept(E(move(err.error_)))) {
    error_ = move(err.error_);
    has_value_ = false;
    return *this;
  }

  // State checking
  constexpr bool has_value() const noexcept { return has_value_; }
  constexpr bool is_ok() const noexcept { return has_value_; }
  constexpr bool is_err() const noexcept { return !has_value_; }
  constexpr operator bool() const noexcept { return has_value_; }

  // Error access
  constexpr const E &error() const & noexcept { return error_; }

  constexpr E &&error() && noexcept { return move(error_); }

  // Void access (no-op)
  constexpr void operator*() const noexcept {}
  constexpr void value() const {
    if (!has_value_) {
      __builtin_unreachable();
    }
  }
};

// Helper classes for construction

template <typename T> class Ok {
public:
  T value_;

  constexpr Ok() noexcept(noexcept(T())) : value_() {}

  template <typename U = T>
  constexpr Ok(U &&value) noexcept(noexcept(T(forward<U>(value)))) : value_(forward<U>(value)) {}
};

// Specialization for void
template <> class Ok<void> {
public:
  constexpr Ok() noexcept = default;
};

template <typename E> class Err {
public:
  E error_;

  template <typename U = E>
  constexpr Err(U &&error) noexcept(noexcept(E(forward<U>(error)))) : error_(forward<U>(error)) {}
};

// Helper functions for construction - following exact specification naming
// Note: These functions cannot have the same names as the classes in the same
// namespace We provide them through explicit template instantiation for better
// API compliance

// Factory functions with specification-compliant signatures
// These create Result<T, E> directly as per spec requirement

// Ok helper functions - create Result<T, ErrorCode>
template <typename T>
constexpr auto make_result_ok(T &&value) noexcept(noexcept(
    Result<remove_cv_t<remove_reference_t<T>>, ErrorCode>(Ok<remove_cv_t<remove_reference_t<T>>>(forward<T>(value)))))
    -> Result<remove_cv_t<remove_reference_t<T>>, ErrorCode> {
  return Result<remove_cv_t<remove_reference_t<T>>, ErrorCode>(
      Ok<remove_cv_t<remove_reference_t<T>>>(forward<T>(value)));
}

constexpr Result<void, ErrorCode> make_result_ok() noexcept { return Result<void, ErrorCode>(Ok<void>()); }

// Err helper functions - create Result<void, E>
template <typename E>
constexpr auto make_result_err(E &&error) noexcept(noexcept(Result<void, remove_cv_t<remove_reference_t<E>>>(
    Err<remove_cv_t<remove_reference_t<E>>>(forward<E>(error))))) -> Result<void, remove_cv_t<remove_reference_t<E>>> {
  return Result<void, remove_cv_t<remove_reference_t<E>>>(Err<remove_cv_t<remove_reference_t<E>>>(forward<E>(error)));
}

// Convenience type aliases
template <typename T> using KernelResult = Result<T, ErrorCode>;
using VoidResult = Result<void, ErrorCode>;

} // namespace moss::kernel

// Specification-compliant helper functions in global namespace to match exact
// API These functions have the exact signatures specified in the requirements

export template <typename T> constexpr moss::kernel::Result<T, moss::kernel::ErrorCode> Ok(T &&value) {
  return moss::kernel::Result<T, moss::kernel::ErrorCode>(moss::kernel::Ok<T>(forward<T>(value)));
}

export constexpr moss::kernel::Result<void, moss::kernel::ErrorCode> Ok() {
  return moss::kernel::Result<void, moss::kernel::ErrorCode>(moss::kernel::Ok<void>());
}

export template <typename E> constexpr moss::kernel::Result<void, E> Err(E &&error) {
  return moss::kernel::Result<void, E>(moss::kernel::Err<E>(forward<E>(error)));
}

export template <typename T, typename E> constexpr moss::kernel::Result<T, E> Err(E &&error) {
  return moss::kernel::Result<T, E>(moss::kernel::Err<E>(forward<E>(error)));
}
