export module moss.capability;

import moss.std;
import moss.types;
import moss.result;
import moss.smart_ptr;
import moss.containers;

export namespace moss::kernel::capability {

enum class ObjectType : u8 { Endpoint, Reply };

namespace rights {
inline constexpr u32 SEND = 1U << 0;
inline constexpr u32 RECEIVE = 1U << 1;
inline constexpr u32 TRANSFER = 1U << 2;
inline constexpr u32 DUPLICATE = 1U << 3;
} // namespace rights

class Object {
  ObjectType type_;

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
        entry.handle = next_handle_++;
        entry.rights = granted_rights;
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

  // Both tables stay locked through publication and optional withdrawal, so
  // a failed destination install never consumes source authority.
  [[nodiscard]] KernelResult<Handle> transfer_to(Table &target, Handle handle, u32 granted_rights,
                                                 bool remove_source) noexcept {
    if (this == &target)
      return KernelResult<Handle>{ErrorCode::InvalidArgument};
    shared_ptr<Object> retired;
    // The same address order on both transfer directions prevents AB/BA lock
    // inversion; retired ownership is released after both locks leave scope.
    if (reinterpret_cast<usize>(this) < reinterpret_cast<usize>(&target)) {
      containers::LockGuard<containers::IrqSpinLock> first(lock_);
      containers::LockGuard<containers::IrqSpinLock> second(target.lock_);
      return transfer_locked(target, handle, granted_rights, remove_source, retired);
    }
    containers::LockGuard<containers::IrqSpinLock> first(target.lock_);
    containers::LockGuard<containers::IrqSpinLock> second(lock_);
    return transfer_locked(target, handle, granted_rights, remove_source, retired);
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
    }
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
      }
    }
  }
};

} // namespace moss::kernel::capability
