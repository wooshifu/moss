export module moss.capability;

import moss.std;
import moss.types;
import moss.result;
import moss.smart_ptr;
import moss.containers;

export namespace moss::kernel::capability {

enum class ObjectType : u8 { Endpoint, Receiver, Reply };

namespace rights {
inline constexpr u32 SEND = 1U << 0;
inline constexpr u32 RECEIVE = 1U << 1;
inline constexpr u32 TRANSFER = 1U << 2;
inline constexpr u32 DUPLICATE = 1U << 3;
} // namespace rights

class Object {
  ObjectType type_;
  atomic<u32> installed_handles_{0};

  friend class Table;
  void acquire_handle() noexcept { (void)installed_handles_.fetch_add(1, memory_order_relaxed); }
  void release_handle() noexcept {
    if (installed_handles_.fetch_sub(1, memory_order_acq_rel) == 1)
      on_last_handle_closed();
  }

protected:
  // A syscall lookup may still retain this object after its last handle is
  // closed. Services use this hook to report peer closure at handle lifetime,
  // rather than waiting for the final transient lookup reference to vanish.
  virtual void on_last_handle_closed() noexcept {}

public:
  explicit Object(ObjectType type) noexcept : type_(type) {}
  virtual ~Object() = default;
  [[nodiscard]] ObjectType type() const noexcept { return type_; }
};

class Table {
  // ponytail: fixed slots bound per-process memory and lock hold time; grow
  // this table when real service workloads need more simultaneous handles.
  static constexpr usize CAPACITY = 64;

  struct Entry {
    Handle handle{INVALID_HANDLE};
    u32 rights{0};
    bool inheritable{false};
    shared_ptr<Object> object{};
  };

  mutable containers::IrqSpinLock lock_;
  Entry entries_[CAPACITY]{};
  // Never reuse a handle value within one table: closing a slot cannot make a
  // stale handle name a later object. Exhaustion is safer than wraparound.
  Handle next_handle_{1};

  [[nodiscard]] Entry *find_locked(Handle handle) noexcept {
    if (handle == INVALID_HANDLE)
      return nullptr;
    for (auto &entry : entries_) {
      if (entry.handle == handle)
        return &entry;
    }
    return nullptr;
  }

  [[nodiscard]] const Entry *find_locked(Handle handle) const noexcept {
    if (handle == INVALID_HANDLE)
      return nullptr;
    for (const auto &entry : entries_) {
      if (entry.handle == handle)
        return &entry;
    }
    return nullptr;
  }

  [[nodiscard]] KernelResult<Handle> install_locked(shared_ptr<Object> &object, u32 granted_rights) noexcept {
    if (!object || granted_rights == 0)
      return KernelResult<Handle>{ErrorCode::InvalidArgument};
    if (next_handle_ == INVALID_HANDLE)
      return KernelResult<Handle>{ErrorCode::ResourceExhausted};
    for (auto &entry : entries_) {
      if (entry.handle == INVALID_HANDLE) {
        object->acquire_handle();
        entry.handle = next_handle_++;
        entry.rights = granted_rights;
        entry.inheritable = false;
        entry.object = moss::move(object);
        return KernelResult<Handle>{entry.handle};
      }
    }
    return KernelResult<Handle>{ErrorCode::ResourceExhausted};
  }

  [[nodiscard]] KernelResult<Handle> transfer_locked(Table &target, Handle handle, u32 granted_rights,
                                                     bool remove_source, shared_ptr<Object> &retired) noexcept {
    Entry *source = find_locked(handle);
    if (!source)
      return KernelResult<Handle>{ErrorCode::NotFound};
    const u32 required = rights::TRANSFER | (remove_source ? 0U : rights::DUPLICATE);
    if (granted_rights == 0 || (source->rights & required) != required ||
        (source->rights & granted_rights) != granted_rights)
      return KernelResult<Handle>{ErrorCode::PermissionDenied};
    shared_ptr<Object> copy = source->object;
    auto installed = target.install_locked(copy, granted_rights);
    if (installed && remove_source) {
      retired = moss::move(source->object);
      source->handle = INVALID_HANDLE;
      source->rights = 0;
      source->inheritable = false;
    }
    return installed;
  }

public:
  Table() noexcept = default;
  ~Table() noexcept { clear(); }
  Table(const Table &) = delete;
  Table &operator=(const Table &) = delete;

  [[nodiscard]] KernelResult<Handle> install(shared_ptr<Object> object, u32 granted_rights) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    return install_locked(object, granted_rights);
  }

  [[nodiscard]] KernelResult<shared_ptr<Object>> lookup(Handle handle, ObjectType type,
                                                        u32 required_rights) const noexcept {
    if (handle == INVALID_HANDLE)
      return KernelResult<shared_ptr<Object>>{ErrorCode::NotFound};
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    const Entry *entry = find_locked(handle);
    if (!entry)
      return KernelResult<shared_ptr<Object>>{ErrorCode::NotFound};
    if (entry->object->type() != type)
      return KernelResult<shared_ptr<Object>>{ErrorCode::InvalidArgument};
    if ((entry->rights & required_rights) != required_rights)
      return KernelResult<shared_ptr<Object>>{ErrorCode::PermissionDenied};
    return KernelResult<shared_ptr<Object>>{entry->object};
  }

  [[nodiscard]] KernelResult<Handle> duplicate(Handle handle, u32 granted_rights) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    Entry *source = find_locked(handle);
    if (!source)
      return KernelResult<Handle>{ErrorCode::NotFound};
    if (!(source->rights & rights::DUPLICATE) || granted_rights == 0 ||
        (source->rights & granted_rights) != granted_rights)
      return KernelResult<Handle>{ErrorCode::PermissionDenied};
    shared_ptr<Object> copy = source->object;
    return install_locked(copy, granted_rights);
  }

  [[nodiscard]] VoidResult set_inheritable(Handle handle, bool inheritable) noexcept {
    containers::LockGuard<containers::IrqSpinLock> guard(lock_);
    Entry *entry = find_locked(handle);
    if (!entry)
      return VoidResult{ErrorCode::NotFound};
    // Fork creates another reference; a holder without DUPLICATE authority
    // cannot arrange for that duplication through a later fork.
    if (inheritable && !(entry->rights & rights::DUPLICATE))
      return VoidResult{ErrorCode::PermissionDenied};
    entry->inheritable = inheritable;
    return {};
  }

  // Fork copies only opted-in handles with the same numeric values. The child
  // inherits a copy of the parent's address space, so renumbering would turn
  // its already-stored handle values into stale references.
  [[nodiscard]] VoidResult clone_inheritable_to(Table &target) const noexcept {
    if (this == &target)
      return VoidResult{ErrorCode::InvalidArgument};
    auto copy_locked = [&]() -> VoidResult {
      // A recycled empty table may have issued these numbers before; only a
      // fresh child table can safely preserve the parent's numeric handles.
      if (target.next_handle_ != 1)
        return VoidResult{ErrorCode::AlreadyExists};
      for (const auto &entry : target.entries_) {
        if (entry.handle != INVALID_HANDLE)
          return VoidResult{ErrorCode::AlreadyExists};
      }
      for (usize i = 0; i < CAPACITY; ++i) {
        if (entries_[i].handle != INVALID_HANDLE && entries_[i].inheritable) {
          entries_[i].object->acquire_handle();
          target.entries_[i] = entries_[i];
        }
      }
      target.next_handle_ = next_handle_;
      return {};
    };
    // Match transfer_to's address ordering so concurrent table operations
    // cannot acquire the same two locks in opposite orders.
    if (reinterpret_cast<usize>(this) < reinterpret_cast<usize>(&target)) {
      containers::LockGuard<containers::IrqSpinLock> first(lock_);
      containers::LockGuard<containers::IrqSpinLock> second(target.lock_);
      return copy_locked();
    }
    containers::LockGuard<containers::IrqSpinLock> first(target.lock_);
    containers::LockGuard<containers::IrqSpinLock> second(lock_);
    return copy_locked();
  }

  void close_uninheritable() noexcept {
    shared_ptr<Object> retired[CAPACITY];
    {
      containers::LockGuard<containers::IrqSpinLock> guard(lock_);
      for (usize i = 0; i < CAPACITY; ++i) {
        if (entries_[i].handle != INVALID_HANDLE && !entries_[i].inheritable) {
          retired[i] = moss::move(entries_[i].object);
          entries_[i].handle = INVALID_HANDLE;
          entries_[i].rights = 0;
        }
      }
    }
    for (auto &object : retired) {
      if (object)
        object->release_handle();
    }
  }

  // Both tables stay locked through publication and optional withdrawal, so
  // a failed destination install never consumes source authority.
  [[nodiscard]] KernelResult<Handle> transfer_to(Table &target, Handle handle, u32 granted_rights,
                                                 bool remove_source) noexcept {
    if (this == &target)
      return KernelResult<Handle>{ErrorCode::InvalidArgument};
    shared_ptr<Object> retired;
    // The same address order on both transfer directions prevents AB/BA lock
    // inversion; retired ownership is released after both locks leave scope.
    const auto transferred = [&]() -> KernelResult<Handle> {
      if (reinterpret_cast<usize>(this) < reinterpret_cast<usize>(&target)) {
        containers::LockGuard<containers::IrqSpinLock> first(lock_);
        containers::LockGuard<containers::IrqSpinLock> second(target.lock_);
        return transfer_locked(target, handle, granted_rights, remove_source, retired);
      }
      containers::LockGuard<containers::IrqSpinLock> first(target.lock_);
      containers::LockGuard<containers::IrqSpinLock> second(lock_);
      return transfer_locked(target, handle, granted_rights, remove_source, retired);
    }();
    if (retired)
      retired->release_handle();
    return transferred;
  }

  [[nodiscard]] VoidResult close(Handle handle) noexcept {
    shared_ptr<Object> retired;
    {
      containers::LockGuard<containers::IrqSpinLock> guard(lock_);
      Entry *entry = find_locked(handle);
      if (!entry)
        return VoidResult{ErrorCode::NotFound};
      retired = moss::move(entry->object);
      entry->handle = INVALID_HANDLE;
      entry->rights = 0;
      entry->inheritable = false;
    }
    retired->release_handle();
    return {};
  }

  void clear() noexcept {
    shared_ptr<Object> retired[CAPACITY];
    {
      containers::LockGuard<containers::IrqSpinLock> guard(lock_);
      for (usize i = 0; i < CAPACITY; ++i) {
        retired[i] = moss::move(entries_[i].object);
        entries_[i].handle = INVALID_HANDLE;
        entries_[i].rights = 0;
        entries_[i].inheritable = false;
      }
    }
    for (auto &object : retired) {
      if (object)
        object->release_handle();
    }
  }
};

} // namespace moss::kernel::capability
